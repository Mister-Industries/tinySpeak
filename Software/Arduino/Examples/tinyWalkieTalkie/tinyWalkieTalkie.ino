/*
 * Project: tinySpeak - tinyWalkieTalkie
 * Author: Geoff McIntyre (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * Push-to-Talk Intercom using ESP-NOW.
 * - Low-latency audio streaming between units (No Router/Internet needed).
 * - Broadcasts to all nearby tinySpeak devices on the same channel.
 *
 * Requirements:
 * - Libraries: WiFi, esp_now (Built-in)
 * - Hardware: 2+ x tinyCore ESP32-S3 + tinySpeak HAT
 * - Note: No SD card or internet required.
 *
 * Controls:
 * [Hold RX Button 150ms] Start Talking (Transmit)
 * [Release RX Button]    Stop Talking after 200ms (Receive)
 * [t]  Toggle Talk via Serial
 * [c]  Cycle WiFi Channel (1-11)
 * [?]  Show Menu
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "driver/i2s_std.h"

// --- PINS ---
// I2S Speaker (MAX98357A)
#define I2S_SPKR_DOUT  8
#define I2S_SPKR_BCLK  9
#define I2S_SPKR_LRC   10

// I2S Microphone (SPH0645)
#define I2S_MIC_WS     11
#define I2S_MIC_SD     12
#define I2S_MIC_SCK    13

// User Button
#define PIN_BUTTON     RX

// --- AUDIO CONFIG ---
// 8kHz is phone quality and keeps ESP-NOW packets comfortably under the 250-byte limit.
// At 8kHz 16-bit mono = 16000 bytes/sec; 120 samples = 240 bytes per packet.
#define SAMPLE_RATE     8000
#define PACKET_SAMPLES  120           // 120 x int16 = 240 bytes, fits in one ESP-NOW frame
#define PACKET_SIZE     (PACKET_SAMPLES * 2)

// Shift the SPH0645's 24-bit left-justified data down to 16-bit PCM.
// Must be >= 16 to avoid overflow/clipping. Higher = quieter.
#define MIC_SHIFT       16

// Software gain applied after shifting. 1.0 = unity. Raise to taste —
// output is hard-clamped to int16 range so it can't clip the DAC.
#define MIC_GAIN        4.0f

// If no audio packet arrives for this many ms, hard-reset the speaker DMA.
// Prevents the DMA ring buffer from looping its last chunk when TX stops.
#define RECEIVER_TIMEOUT_MS  80

// How long the button must be held continuously before transmission starts (ms).
// Filters out brief contact bounces from loose jumpers that would falsely trigger TX.
#define BUTTON_HOLD_MS    150

// How long the button must be continuously released before transmission stops (ms).
// Prevents a momentary dropout in a loose connection from cutting the transmission.
#define BUTTON_RELEASE_MS 200

// --- GLOBALS ---
int  wifiChannel  = 1;
bool isTalking    = false;

// New I2S driver channel handles
i2s_chan_handle_t tx_handle = NULL; // Speaker (I2S_NUM_0)
i2s_chan_handle_t rx_handle = NULL; // Microphone (I2S_NUM_1)

// Global TX buffer — fixed size avoids VLA stack allocation in the audio loop
int32_t raw_samples[PACKET_SAMPLES];   // 32-bit mic read (SPH0645 stereo Philips)
int16_t tx_buffer[PACKET_SAMPLES];     // Downconverted 16-bit PCM for transmission

// Broadcast address — sends to every ESP-NOW device on the same channel
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Tracks when the last audio packet arrived — used to detect stream end
volatile unsigned long lastPacketTime    = 0;
// True while actively receiving audio from remote unit
volatile bool          isReceivingAudio  = false;

// Button state tracking for two-stage hold/release debounce
unsigned long buttonPressedAt  = 0;  // When the button first went LOW
unsigned long buttonReleasedAt = 0;  // When the button first went HIGH
bool buttonArmed = false;            // True once hold threshold has been met

// ---------------------------------------------------------------
// I2S Setup
// ---------------------------------------------------------------

void setupI2S() {
    // --- Speaker (I2S_NUM_0, TX) ---
    // Raw 16-bit PCM output at 8kHz. We drive this manually via i2s_channel_write()
    // rather than through the Audio library, since there's no file playback here.
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&spk_chan_cfg, &tx_handle, NULL);

    i2s_std_config_t spk_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SPKR_BCLK,
            .ws   = (gpio_num_t)I2S_SPKR_LRC,
            .dout = (gpio_num_t)I2S_SPKR_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    i2s_channel_init_std_mode(tx_handle, &spk_cfg);
    i2s_channel_enable(tx_handle);

    // --- Microphone (I2S_NUM_1, RX) ---
    // SPH0645 requires stereo Philips mode with 32-bit slots.
    // We extract the left channel only when building the TX packet.
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&mic_chan_cfg, NULL, &rx_handle);

    i2s_std_config_t mic_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_MIC_SCK,
            .ws   = (gpio_num_t)I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    i2s_channel_init_std_mode(rx_handle, &mic_cfg);
    i2s_channel_enable(rx_handle);
}

// Hard-resets the speaker I2S channel by disabling and re-enabling it.
// This is the only reliable way to clear the DMA ring buffer — simply
// writing zeros over it doesn't reset the playback pointer.
void resetSpeaker() {
    i2s_channel_disable(tx_handle);
    i2s_channel_enable(tx_handle);
}

// ---------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------

// Received audio packet -> write directly to speaker DMA buffer.
// Called from the ESP-NOW receive task (not loop()), so keep it lean.
// Note: IDF v5 changed the callback signature — first arg is now esp_now_recv_info_t*
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    if (isTalking) return; // Suppress incoming audio while transmitting to prevent feedback

    isReceivingAudio = true;
    lastPacketTime   = millis();

    // Short timeout instead of portMAX_DELAY — blocking indefinitely in a callback
    // can starve the ESP-NOW task and cause cascading packet drops.
    size_t bytes_written;
    i2s_channel_write(tx_handle, incomingData, len, &bytes_written, pdMS_TO_TICKS(20));
}

void setupESPNOW() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW init failed.");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = wifiChannel;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[ERROR] Failed to add broadcast peer.");
        return;
    }

    Serial.printf("ESP-NOW ready on channel %d\n", wifiChannel);
}

void changeChannel(int newChan) {
    wifiChannel = constrain(newChan, 1, 11);

    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    setupESPNOW();
    Serial.printf("Switched to channel %d\n", wifiChannel);
}

// ---------------------------------------------------------------
// Talk State
// ---------------------------------------------------------------

void startTalking() {
    isTalking = true;
    Serial.println("Transmitting...");
}

void stopTalking() {
    isTalking = false;
    Serial.println("Listening...");
    // The receiver handles cleanup locally via the timeout + resetSpeaker().
    // Sending silence packets over ESP-NOW is unreliable (packets can be dropped)
    // and the delay() calls here would block the loop during a critical transition.
}

// ---------------------------------------------------------------
// Menu
// ---------------------------------------------------------------

void printMenu() {
    Serial.println("\n--- tinyWalkieTalkie Menu ---");
    Serial.println("[Hold RX 150ms] Start Talking");
    Serial.println("[Release RX]    Stop Talking (after 200ms)");
    Serial.println("[t]  Toggle Talk via Serial");
    Serial.println("[c]  Cycle Channel (1-11)");
    Serial.println("[?]  Show Menu");
    Serial.printf( "Current Channel: %d\n", wifiChannel);
    Serial.println("-----------------------------");
}

// ---------------------------------------------------------------
// Setup
// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Wait for Web Serial to connect

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    setupI2S();
    setupESPNOW();

    printMenu();
}

// ---------------------------------------------------------------
// Loop
// ---------------------------------------------------------------

void loop() {
    // 1. Serial Commands
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input.length() == 0) return;

        if (input[0] == '?') { printMenu(); return; }

        if (input[0] == 't') {
            if (!isTalking) startTalking();
            else            stopTalking();
            return;
        }

        if (input[0] == 'c') {
            int nextChan = (wifiChannel >= 11) ? 1 : wifiChannel + 1;
            changeChannel(nextChan);
            return;
        }
    }

    // 2. Button handling — two-stage debounce for loose/noisy connections.
    //
    //    START: button must be held LOW for BUTTON_HOLD_MS continuously.
    //           Any interruption resets the hold timer, so a brief contact
    //           bounce never triggers a transmission.
    //
    //    STOP:  button must be released HIGH for BUTTON_RELEASE_MS continuously.
    //           A momentary dropout in a loose jumper won't cut the transmission.
    //
    int reading = digitalRead(PIN_BUTTON);

    if (reading == LOW) {
        // Button is being held down
        buttonReleasedAt = 0; // Reset release timer

        if (buttonPressedAt == 0) {
            buttonPressedAt = millis(); // Mark when press started
        }

        // Start talking once held long enough, but only once per press
        if (!isTalking && !buttonArmed && (millis() - buttonPressedAt >= BUTTON_HOLD_MS)) {
            buttonArmed = true;
            startTalking();
        }
    } else {
        // Button is released (or bounced back HIGH)
        buttonPressedAt = 0; // Reset hold timer — any bounce restarts the hold count
        buttonArmed = false; // Re-arm for next press only after full release

        if (isTalking) {
            if (buttonReleasedAt == 0) {
                buttonReleasedAt = millis(); // Mark when release started
            }
            // Only stop once released continuously for BUTTON_RELEASE_MS
            if (millis() - buttonReleasedAt >= BUTTON_RELEASE_MS) {
                buttonReleasedAt = 0;
                stopTalking();
            }
        } else {
            buttonReleasedAt = 0;
        }
    }

    // 3. Speaker DMA management — the core fix for clicking/looping.
    //
    // I2S DMA is a streaming ring buffer. If you stop feeding it, it replays
    // whatever was last written, forever. Two things are needed:
    //
    //   a) HARD RESET on stream end: disable+enable clears the ring buffer
    //      completely. Writing silence over it is not sufficient.
    //
    //   b) CONTINUOUS SILENCE FEED when idle: keeps the DMA fed with zeros
    //      every loop tick so it never has stale audio to replay.
    //
    if (!isTalking) {
        if (isReceivingAudio && (millis() - lastPacketTime) > RECEIVER_TIMEOUT_MS) {
            // Stream ended — hard reset the speaker DMA to clear the ring buffer
            isReceivingAudio = false;
            resetSpeaker();
        }

        if (!isReceivingAudio) {
            // Continuously pump silence so the DMA is always fed with zeros.
            // Non-blocking (timeout=0) — if the buffer is full we skip this tick,
            // which is fine because it means it's already filled with silence.
            static uint8_t silence[PACKET_SIZE] = {0};
            size_t bytes_written;
            i2s_channel_write(tx_handle, silence, sizeof(silence), &bytes_written, 0);
        }
    }

    // 4. Transmission loop
    if (isTalking) {
        size_t bytes_read = 0;

        // Read one packet's worth of stereo 32-bit frames from the mic.
        if (i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples), &bytes_read, 0) == ESP_OK && bytes_read > 0) {
            int stereo_frames = bytes_read / 8;
            for (int i = 0; i < stereo_frames; i++) {
                // Shift by MIC_SHIFT (>=16) to correctly downscale the SPH0645's
                // 24-bit left-justified data to 16-bit PCM without overflow.
                // Then apply MIC_GAIN and hard-clamp to prevent DAC clipping.
                float sample = (raw_samples[2 * i] >> MIC_SHIFT) * MIC_GAIN;
                if      (sample >  32767.0f) sample =  32767.0f;
                else if (sample < -32768.0f) sample = -32768.0f;
                tx_buffer[i] = (int16_t)sample;
            }

            esp_now_send(broadcastAddress, (uint8_t*)tx_buffer, stereo_frames * 2);
            // Note: don't Serial.print on TX errors here — it would corrupt audio timing
        }
    }
}