/*
 * Project: tinySpeak - tinySoundboard
 * Author: Geoff McIntyre  (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * A motion-triggered soundboard.
 * Reads the onboard IMU to detect a "shake" gesture and triggers
 * a sound effect from the SD card. Files are read from the SD root;
 * any .mp3 or .wav file will be picked up automatically.
 *
 * Requirements:
 * - Libraries: ESP32-audioI2S by schreibfaul1, Adafruit LSM6DS,
 *              Adafruit Unified Sensor, SD, Wire
 * - Hardware: tinyCore ESP32-S3 + tinySpeak HAT (SD card required)
 *
 * Controls:
 * [Button]  Play Current Sound
 * [s]  Simulate Shake / Play Current Sound
 * [p]  Play Current Sound
 * [n]  Next Sound Effect
 * [+]  Volume Up
 * [-]  Volume Down
 * [l]  List Sound Files
 * [?]  Show Menu
 */

#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include "Audio.h"
#include <vector>

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

// --- MOTION CONFIG ---
// Total acceleration magnitude (in G) above resting (1G) needed to trigger.
// Raise if accidental triggers occur; lower if shakes aren't registering.
#define SHAKE_THRESHOLD  2.5f
#define TRIGGER_COOLDOWN 1000  // ms between allowed triggers

// --- GLOBALS ---
Audio audio;
Adafruit_LSM6DSOX lsm6ds;

std::vector<String> sfxFiles;
int  currentFileIndex = 0;
int  volume           = 21;
bool sdReady          = false;
bool imuReady         = false;

unsigned long lastTriggerTime = 0;

// Debounce globals for play button
unsigned long lastDebounceTime = 0;
int lastButtonState = HIGH;

// ---------------------------------------------------------------
// Sound File Management
// ---------------------------------------------------------------

void listSFXFiles() {
    if (!sdReady) { Serial.println("[ERROR] SD not available."); return; }

    sfxFiles.clear();
    File root = SD.open("/");
    if (!root) { Serial.println("[ERROR] Failed to open SD root."); return; }

    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        // Accept both lowercase and uppercase extensions
        name.toLowerCase();
        if (name.endsWith(".mp3") || name.endsWith(".wav")) {
            sfxFiles.push_back(String("/") + file.name());
            Serial.print("Found: "); Serial.println(file.name());
        }
        file = root.openNextFile();
    }
    root.close();

    if (sfxFiles.empty()) {
        Serial.println("No .mp3 or .wav files found in SD root.");
    } else {
        Serial.printf("%d file(s) loaded. Current: %s\n",
                      sfxFiles.size(), sfxFiles[currentFileIndex].c_str());
    }
}

void playSoundEffect() {
    if (!sdReady) { Serial.println("[ERROR] SD not available."); return; }
    if (sfxFiles.empty()) { Serial.println("No sound files. Put .mp3/.wav in SD root."); return; }

    if (audio.isRunning()) audio.stopSong();

    Serial.print("Playing: "); Serial.println(sfxFiles[currentFileIndex]);
    audio.connecttoFS(SD, sfxFiles[currentFileIndex].c_str());
    lastTriggerTime = millis();
}

void nextTrack() {
    if (sfxFiles.empty()) return;
    currentFileIndex = (currentFileIndex + 1) % sfxFiles.size();
    Serial.print("Selected: "); Serial.println(sfxFiles[currentFileIndex]);
}

// ---------------------------------------------------------------
// Menu
// ---------------------------------------------------------------

void printMenu() {
    Serial.println("\n--- tinySoundboard Menu ---");
    Serial.println("[Button]  Play Current Sound");
    Serial.println("[s] / [p] Play Current Sound");
    Serial.println("[n]  Next Sound Effect");
    Serial.println("[+]  Volume Up");
    Serial.println("[-]  Volume Down");
    Serial.println("[l]  List Sound Files");
    Serial.println("[?]  Show Menu");
    Serial.printf( "Current volume: %d / 21\n", volume);
    if (!sfxFiles.empty())
        Serial.printf("Current file: %s\n", sfxFiles[currentFileIndex].c_str());
    Serial.println("---------------------------");
}

// ---------------------------------------------------------------
// Setup
// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Wait for Web Serial to connect

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // SD — no CS arg, tinySpeak HAT uses default SPI CS pin
    if (!SD.begin()) {
        Serial.println("[ERROR] SD card mount failed. No sounds will play.");
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
        listSFXFiles();
    }

    // IMU
    Wire.begin(IMU_SDA, IMU_SCL);
    if (!lsm6ds.begin_I2C()) {
        Serial.println("[WARN] LSM6DS3TR-C not found. Shake detection disabled.");
        Serial.println("Sounds can still be triggered via button or serial commands.");
    } else {
        imuReady = true;
        Serial.println("IMU found. Shake detection active.");
        lsm6ds.setAccelRange(LSM6DS_ACCEL_RANGE_4_G); // Higher range needed for shakes
        lsm6ds.setAccelDataRate(LSM6DS_RATE_104_HZ);
    }

    // Audio library (manages I2S internally)
    audio.setPinout(I2S_SPKR_BCLK, I2S_SPKR_LRC, I2S_SPKR_DOUT);
    audio.setVolume(volume);

    printMenu();
}

// ---------------------------------------------------------------
// Loop
// ---------------------------------------------------------------

void loop() {
    audio.loop();

    // 1. Shake detection
    if (imuReady && (millis() - lastTriggerTime > TRIGGER_COOLDOWN)) {
        sensors_event_t a, g, temp;
        lsm6ds.getEvent(&a, &g, &temp);

        float x = a.acceleration.x;
        float y = a.acceleration.y;
        float z = a.acceleration.z;

        // Total acceleration magnitude in G, minus resting gravity (1G)
        float magnitude = sqrtf(x*x + y*y + z*z) / 9.81f;

        if (fabsf(magnitude - 1.0f) > SHAKE_THRESHOLD) {
            Serial.println("Shake detected!");
            playSoundEffect();
        }
    }

    // 2. Serial Commands
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() == 0) return;

        switch (input[0]) {
            case 's':
            case 'p': playSoundEffect(); break;
            case 'n': nextTrack();       break;
            case 'l': listSFXFiles();    break;
            case '?': printMenu();       break;
            case '+':
                volume = constrain(volume + 1, 0, 21);
                audio.setVolume(volume);
                Serial.printf("Volume: %d / 21\n", volume);
                break;
            case '-':
                volume = constrain(volume - 1, 0, 21);
                audio.setVolume(volume);
                Serial.printf("Volume: %d / 21\n", volume);
                break;
        }
    }

    // 3. Button handling (debounced) — play current sound
    int reading = digitalRead(PIN_BUTTON);
    if (reading != lastButtonState) {
        lastDebounceTime = millis();
    }
    if ((millis() - lastDebounceTime) > 50) {
        static int buttonState = HIGH;
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
                playSoundEffect();
            }
        }
    }
    lastButtonState = reading;
}

// ---------------------------------------------------------------
// Audio Callbacks
// ---------------------------------------------------------------

void audio_eof_mp3(const char *info) {
    Serial.print("Finished: "); Serial.println(info);
}

void audio_eof_wav(const char *info) {
    Serial.print("Finished: "); Serial.println(info);
}
