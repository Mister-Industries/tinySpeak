/*
 * Project: tinySpeak - tinyRecorder
 * Author: Geoff McIntyre (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * A standalone voice recorder.
 * 1. Hold Button (RX) -> Records audio to SD card (/recording.wav).
 * 2. Release Button   -> Stops and saves recording.
 * 3. Press [p]        -> Plays back the last recording.
 *
 * Requirements:
 * - Libraries: ESP32-audioI2S by schreibfaul1, SD, SPI
 * - Hardware: tinyCore ESP32-S3 + tinySpeak HAT (SD card required)
 * - Note: No WiFi required.
 *
 * Controls:
 * [Hold RX Button]    Start Recording
 * [Release RX Button] Stop & Save Recording
 * [r]  Toggle Recording via Serial
 * [p]  Play Last Recording
 * [s]  Stop Playback
 * [?]  Show Menu
 */

#include <Arduino.h>
#include "Audio.h"
#include "SD.h"
#include "SPI.h"
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

// --- GLOBALS ---
Audio audio;

bool isRecording = false;
bool sdReady     = false;  // Set true only after SD.begin() succeeds
File wavFile;

// New I2S driver channel handle for the microphone (I2S_NUM_1)
i2s_chan_handle_t rx_handle = NULL;

// Global mic buffers — allocated once to avoid heap churn during recording.
// Stereo 32-bit read buffer (new driver always reads stereo for SPH0645).
// 512 stereo frames = 1024 int32 values.
#define I2S_READ_LEN 1024
int32_t raw_samples[I2S_READ_LEN];
int16_t wav_buffer_mic[I2S_READ_LEN / 2]; // Mono 16-bit output (half the stereo frames)

// Debounce globals for record button
unsigned long lastDebounceTime = 0;
int lastButtonState = HIGH;

// ---------------------------------------------------------------
// Microphone
// ---------------------------------------------------------------

void setupMic() {
    // New driver: create a channel on I2S_NUM_1 in master/RX role.
    // Using I2S_NUM_1 keeps the mic separate from the Audio library,
    // which claims I2S_NUM_0 for the speaker via the new driver internally.
    // Mixing legacy and new drivers on the same chip causes a hard abort.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    // SPH0645 requires stereo Philips mode with 32-bit slots.
    // We read both channels and extract the left channel in captureAudio().
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
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

    i2s_channel_init_std_mode(rx_handle, &std_cfg);
    i2s_channel_enable(rx_handle);
}

// ---------------------------------------------------------------
// WAV Header
// ---------------------------------------------------------------

void writeWavHeader(File &file, int fileSize) {
    int sampleRate    = 16000;
    int numChannels   = 1;
    int bitsPerSample = 16;
    int byteRate      = sampleRate * numChannels * (bitsPerSample / 8);

    uint8_t header[44];
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    unsigned int fileSizeMinus8 = fileSize - 8;
    header[4]  = (uint8_t)(fileSizeMinus8 & 0xFF);
    header[5]  = (uint8_t)((fileSizeMinus8 >> 8)  & 0xFF);
    header[6]  = (uint8_t)((fileSizeMinus8 >> 16) & 0xFF);
    header[7]  = (uint8_t)((fileSizeMinus8 >> 24) & 0xFF);
    header[8]  = 'W'; header[9]  = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1;  header[21] = 0;
    header[22] = (uint8_t)numChannels; header[23] = 0;
    header[24] = (uint8_t)(sampleRate & 0xFF);
    header[25] = (uint8_t)((sampleRate >> 8)  & 0xFF);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
    header[28] = (uint8_t)(byteRate & 0xFF);
    header[29] = (uint8_t)((byteRate >> 8)  & 0xFF);
    header[30] = (uint8_t)((byteRate >> 16) & 0xFF);
    header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
    header[32] = (uint8_t)(numChannels * (bitsPerSample / 8)); header[33] = 0;
    header[34] = (uint8_t)bitsPerSample; header[35] = 0;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    unsigned int dataSize = fileSize - 44;
    header[40] = (uint8_t)(dataSize & 0xFF);
    header[41] = (uint8_t)((dataSize >> 8)  & 0xFF);
    header[42] = (uint8_t)((dataSize >> 16) & 0xFF);
    header[43] = (uint8_t)((dataSize >> 24) & 0xFF);

    file.seek(0);
    file.write(header, 44);
}

// ---------------------------------------------------------------
// Recording
// ---------------------------------------------------------------

void startRecording() {
    if (!sdReady) {
        Serial.println("[ERROR] SD card not available. Cannot record.");
        return;
    }
    if (audio.isRunning()) audio.stopSong();

    SD.remove("/recording.wav");
    wavFile = SD.open("/recording.wav", FILE_WRITE);
    if (!wavFile) {
        Serial.println("[ERROR] Could not open /recording.wav for writing.");
        return;
    }

    // Write blank placeholder header — finalized in stopRecording()
    uint8_t blank[44] = {0};
    wavFile.write(blank, 44);

    isRecording = true;
    Serial.println("Recording... Release button to stop.");
}

// Reads pending I2S samples into wavFile. Called every loop() tick while recording.
// Uses global buffers to avoid heap allocation on every call.
void captureAudio() {
    size_t bytes_read = 0;

    // Read stereo 32-bit frames into global raw_samples buffer
    if (i2s_channel_read(rx_handle, raw_samples, sizeof(raw_samples), &bytes_read, 1000) == ESP_OK) {
        int stereo_frames = bytes_read / 8; // 8 bytes per stereo frame (2 x int32)

        // Extract left channel only and shift to 16-bit PCM.
        // SPH0645 outputs 24-bit data left-justified in a 32-bit slot;
        // shifting right by 14 normalizes the output level correctly.
        for (int i = 0; i < stereo_frames; i++) {
            wav_buffer_mic[i] = (int16_t)(raw_samples[2 * i] >> 14);
        }

        wavFile.write((uint8_t*)wav_buffer_mic, stereo_frames * 2);
    }
}

void stopRecording() {
    isRecording = false;

    // Finalize WAV header with real file size
    writeWavHeader(wavFile, wavFile.size());
    wavFile.close();
    Serial.println("Recording saved to /recording.wav");
}

// ---------------------------------------------------------------
// Playback
// ---------------------------------------------------------------

void playRecording() {
    if (isRecording) return;
    if (!sdReady) {
        Serial.println("[ERROR] SD card not available.");
        return;
    }
    if (!SD.exists("/recording.wav")) {
        Serial.println("No recording found. Hold button to record first.");
        return;
    }
    Serial.println("Playing /recording.wav...");
    audio.connecttoFS(SD, "/recording.wav");
}

void stopSpeaking() {
    if (audio.isRunning()) {
        audio.stopSong();
        Serial.println("Stopped.");
    }
}

// ---------------------------------------------------------------
// Menu
// ---------------------------------------------------------------

void printMenu() {
    Serial.println("\n--- tinyRecorder Menu ---");
    Serial.println("[Hold RX]    Start Recording");
    Serial.println("[Release RX] Stop & Save");
    Serial.println("[r]  Toggle Recording via Serial");
    Serial.println("[p]  Play Last Recording");
    Serial.println("[s]  Stop Playback");
    Serial.println("[?]  Show Menu");
    Serial.println("-------------------------");
}

// ---------------------------------------------------------------
// Setup
// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Wait for Web Serial to connect

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // SD card must be mounted before audio library starts,
    // since playback reads from it. No CS arg: tinySpeak HAT uses default SPI CS.
    if (!SD.begin()) {
        Serial.println("[ERROR] SD card mount failed. Recording and playback will not work.");
        Serial.println("Check that an SD card is inserted in the tinySpeak HAT.");
    } else {
        sdReady = true;
        Serial.print("SD card mounted. Type: ");
        switch (SD.cardType()) {
            case CARD_MMC:  Serial.println("MMC");   break;
            case CARD_SD:   Serial.println("SDSC");  break;
            case CARD_SDHC: Serial.println("SDHC");  break;
            default:        Serial.println("Unknown");
        }
        Serial.printf("SD size: %llu MB\n", SD.cardSize() / (1024 * 1024));
    }

    // Audio library (speaker) must init before setupMic() so the new I2S driver
    // claims I2S_NUM_0 for the speaker first, leaving I2S_NUM_1 clean for the mic.
    audio.setPinout(I2S_SPKR_BCLK, I2S_SPKR_LRC, I2S_SPKR_DOUT);
    audio.setVolume(21);

    setupMic();

    printMenu();
}

// ---------------------------------------------------------------
// Loop
// ---------------------------------------------------------------

void loop() {
    audio.loop();

    // Drain mic samples to SD every tick while recording
    if (isRecording) {
        captureAudio();
    }

    // Button handling (debounced): hold to record, release to stop
    int reading = digitalRead(PIN_BUTTON);
    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }
    if ((millis() - lastDebounceTime) > 50) {
        static int buttonState = HIGH;
        if (reading != buttonState) {
            buttonState = reading;
            // Pressed (LOW) -> start recording
            if (buttonState == LOW && !isRecording) {
                startRecording();
            }
            // Released (HIGH) -> stop and save
            else if (buttonState == HIGH && isRecording) {
                stopRecording();
            }
        }
    }
    lastButtonState = reading;

    // Serial Commands
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input.length() == 0) return;

        if (input[0] == '?') { printMenu(); return; }
        if (input[0] == 'p') { playRecording(); return; }
        if (input[0] == 's') { stopSpeaking(); return; }

        if (input[0] == 'r') {
            if (!isRecording) {
                startRecording();
            } else {
                stopRecording();
            }
            return;
        }
    }
}

// ---------------------------------------------------------------
// Audio Callbacks
// ---------------------------------------------------------------

void audio_eof_wav(const char *info) {
    Serial.print("Finished: "); Serial.println(info);
}