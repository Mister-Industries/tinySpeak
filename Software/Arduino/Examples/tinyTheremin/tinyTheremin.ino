/*
 * Project: tinySpeak - tinyTheremin
 * Author: Geoff McIntyre (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * A motion-controlled musical instrument.
 * 1. Tilt Left/Right (X axis)   -> Changes pitch (200-1000 Hz).
 * 2. Tilt Forward/Back (Y axis) -> Changes volume.
 * 3. Uses the on-board 6-DOF IMU (LSM6DS3TR-C).
 * 4. Press [c] while holding device still to zero the center position.
 *
 * Requirements:
 * - Libraries: Adafruit LSM6DS, Adafruit Unified Sensor, Wire
 * - Hardware: tinyCore ESP32-S3 + tinySpeak HAT (IMU required)
 * - Note: No WiFi or SD card required.
 *
 * Controls:
 * [Button]  Mute / Unmute
 * [m]  Mute / Unmute
 * [c]  Calibrate (hold device flat and still first)
 * [?]  Show Menu
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include "driver/i2s_std.h"

// --- PINS ---
// I2S Speaker (MAX98357A)
#define I2S_SPKR_DOUT  8
#define I2S_SPKR_BCLK  9
#define I2S_SPKR_LRC   10

// IMU I2C (tinyCore default)
#define IMU_SDA  3
#define IMU_SCL  4

// User Button
#define PIN_BUTTON  RX

// --- AUDIO CONFIG ---
#define SAMPLE_RATE  44100
#define BUF_LEN      128    // Samples per write — keep small for low control latency

// --- THEREMIN CONFIG ---
#define FREQ_MIN     200.0f   // Hz at full left tilt
#define FREQ_MAX     1000.0f  // Hz at full right tilt
#define TILT_RANGE   9.0f     // m/s² — max tilt considered full deflection
#define AMP_MIN      2000     // Minimum audible amplitude
#define AMP_MAX      25000    // Maximum amplitude (hard ceiling before clipping)
#define AMP_SMOOTH   0.1f     // Volume smoothing factor (0=frozen, 1=instant)

// --- GLOBALS ---
Adafruit_LSM6DSOX lsm6ds;
i2s_chan_handle_t    tx_handle = NULL;

bool    isMuted   = false;
float   frequency = 440.0f;
int32_t amplitude = 10000;
float   phase     = 0.0f;

// Calibration offsets — subtracted from raw IMU readings each tick
float cal_x = 0.0f;
float cal_y = 0.0f;

// Debounce globals for mute button
unsigned long lastDebounceTime = 0;
int lastButtonState = HIGH;

// Rate limit for Serial Plotter output
unsigned long lastPrintTime = 0;

// ---------------------------------------------------------------
// I2S Setup
// ---------------------------------------------------------------

void setupI2S() {
    // auto_clear = true: outputs silence on DMA underflow instead of
    // replaying the last buffer — prevents glitches if loop() stalls briefly.
    // Small buffer depth keeps control-to-audio latency low.
    i2s_chan_config_t chan_cfg = {
        .id            = I2S_NUM_0,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = 4,
        .dma_frame_num = BUF_LEN,
        .auto_clear    = true,
    };
    i2s_new_channel(&chan_cfg, &tx_handle, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // Stereo 16-bit — we write identical L+R samples for a centered mono output
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
    i2s_channel_init_std_mode(tx_handle, &std_cfg);
    i2s_channel_enable(tx_handle);
}

// ---------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------

// Averages 20 IMU readings over ~200ms to find the resting offset.
// Call while the device is flat and still.
void calibrate() {
    Serial.println("Calibrating — hold still...");

    float sum_x = 0.0f;
    float sum_y = 0.0f;
    const int samples = 20;

    for (int i = 0; i < samples; i++) {
        sensors_event_t a, g, temp;
        lsm6ds.getEvent(&a, &g, &temp);
        sum_x += a.acceleration.x;
        sum_y += a.acceleration.y;
        delay(10);
    }

    cal_x = sum_x / samples;
    cal_y = sum_y / samples;

    Serial.printf("Calibration done. Offsets: X=%.3f Y=%.3f\n", cal_x, cal_y);
}

// ---------------------------------------------------------------
// Menu
// ---------------------------------------------------------------

void printMenu() {
    Serial.println("\n--- tinyTheremin Menu ---");
    Serial.println("[Button]  Mute / Unmute");
    Serial.println("[m]  Mute / Unmute");
    Serial.println("[c]  Calibrate (hold flat + still first)");
    Serial.println("[?]  Show Menu");
    Serial.println("Plotter output: Pitch (Hz), Volume (amplitude)");
    Serial.println("-------------------------");
}

// ---------------------------------------------------------------
// Setup
// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Wait for Web Serial to connect

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    Wire.begin(IMU_SDA, IMU_SCL);

    if (!lsm6ds.begin_I2C()) {
        Serial.println("[ERROR] Failed to find LSM6DS3TR-C. Check wiring.");
        while (1) delay(10);
    }
    Serial.println("IMU found.");

    lsm6ds.setAccelRange(LSM6DS_ACCEL_RANGE_2_G);
    lsm6ds.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS);
    lsm6ds.setAccelDataRate(LSM6DS_RATE_104_HZ);
    lsm6ds.setGyroDataRate(LSM6DS_RATE_104_HZ);

    setupI2S();

    // Auto-calibrate on boot so it's zeroed out of the box
    calibrate();

    printMenu();
}

// ---------------------------------------------------------------
// Loop
// ---------------------------------------------------------------

void loop() {
    // 1. Read IMU and apply calibration offsets
    sensors_event_t a, g, temp;
    lsm6ds.getEvent(&a, &g, &temp);

    float ax = a.acceleration.x - cal_x;
    float ay = a.acceleration.y - cal_y;

    // 2. Map motion to audio parameters
    //    X axis (tilt left/right) -> Pitch
    float pitchInput = constrain(ax, -TILT_RANGE, TILT_RANGE);
    // Map linearly: -TILT_RANGE = FREQ_MIN, +TILT_RANGE = FREQ_MAX
    frequency = FREQ_MIN + ((pitchInput + TILT_RANGE) / (2.0f * TILT_RANGE)) * (FREQ_MAX - FREQ_MIN);

    //    Y axis (tilt forward/back) -> Volume
    //    Use absolute value so both directions increase volume from center
    float volInput = constrain(fabsf(ay), 0.0f, TILT_RANGE);
    int32_t targetAmp = (int32_t)(AMP_MIN + (volInput / TILT_RANGE) * (AMP_MAX - AMP_MIN));

    // Exponential smoothing prevents abrupt volume steps (zipper noise)
    amplitude = (int32_t)(amplitude * (1.0f - AMP_SMOOTH) + targetAmp * AMP_SMOOTH);

    // 3. Generate and write one buffer of sine wave audio
    int16_t samples[BUF_LEN * 2]; // *2 for stereo interleave (L+R pairs)
    float phaseIncrement = (2.0f * PI * frequency) / SAMPLE_RATE;

    if (!isMuted) {
        for (int i = 0; i < BUF_LEN; i++) {
            int16_t val = (int16_t)(sinf(phase) * amplitude);
            samples[i * 2]     = val; // Left
            samples[i * 2 + 1] = val; // Right

            phase += phaseIncrement;
            if (phase >= 2.0f * PI) phase -= 2.0f * PI;
        }
    } else {
        memset(samples, 0, sizeof(samples));
    }

    size_t bytes_written;
    i2s_channel_write(tx_handle, samples, sizeof(samples), &bytes_written, portMAX_DELAY);

    // 4. Serial Plotter output (rate-limited to 10Hz to avoid flooding)
    if (millis() - lastPrintTime > 100) {
        Serial.printf("Pitch:%.1f,Volume:%d\n", frequency, amplitude);
        lastPrintTime = millis();
    }

    // 5. Serial Commands
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input.length() == 0) return;

        if (input[0] == '?') { printMenu(); return; }

        if (input[0] == 'm') {
            isMuted = !isMuted;
            Serial.println(isMuted ? "Muted" : "Unmuted");
            return;
        }

        if (input[0] == 'c') {
            calibrate();
            return;
        }
    }

    // 6. Button handling (debounced) — mute toggle
    int reading = digitalRead(PIN_BUTTON);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }
    if ((millis() - lastDebounceTime) > 50) {
        static int buttonState = HIGH;
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
                isMuted = !isMuted;
                Serial.println(isMuted ? "Muted" : "Unmuted");
            }
        }
    }
    lastButtonState = reading;
}
