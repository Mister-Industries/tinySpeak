/*
 * Project: tinySpeak - tinyAI_Text
 * Author: Geoff McIntyre (w/ help from Claude)
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * A text-based AI assistant.
 * 1. User types a query in the Serial Monitor.
 * 2. ESP32 sends the query to OpenAI (ChatGPT).
 * 3. ESP32 reads the response aloud using Text-to-Speech.
 *
 * Requirements:
 * - Libraries: ArduinoJson, ESP32-audioI2S by schreibfaul1, HTTPClient by Adrian McEwen
 * - Hardware: tinyCore ESP32-S3 + tinySpeak HAT (SD card required)
 * - Note: TTS audio is saved to /tts.mp3 on SD before playback to avoid RAM exhaustion.
 *
 * Controls:
 * [Type Message] + Enter to chat
 * [s]  Stop Speaking
 * [c]  Configure WiFi / OpenAI Key
 * [?]  Show Menu
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "Audio.h"
#include "SD.h"
#include "SPI.h"

// --- PINS ---
#define I2S_SPKR_BCLK  9
#define I2S_SPKR_LRC   10
#define I2S_SPKR_DOUT  8

// --- GLOBALS ---
Audio audio;
Preferences prefs;

String cfg_ssid     = "";
String cfg_password = "";
String cfg_api_key  = "";

// ---------------------------------------------------------------
// Serial Helpers
// ---------------------------------------------------------------

// Blocks until the user types a line and presses Enter.
// Echoes characters so the user can see what they're typing.
// Pass mask=true to echo '*' instead (for passwords/keys).
String serialPrompt(const char* label, bool mask = false) {
    Serial.print(label);
    String result = "";
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (result.length() > 0) {
                    Serial.println();
                    return result;
                }
                // ignore lone CR/LF before any input
            } else if (c == 127 || c == '\b') {
                // Backspace
                if (result.length() > 0) {
                    result.remove(result.length() - 1);
                    Serial.print("\b \b");
                }
            } else {
                result += c;
                Serial.print(mask ? '*' : c);
            }
        }
        audio.loop(); // keep audio alive while waiting
    }
}

// ---------------------------------------------------------------
// Config
// ---------------------------------------------------------------

void saveConfig() {
    prefs.begin("tinyai", false);
    prefs.putString("ssid",     cfg_ssid);
    prefs.putString("password", cfg_password);
    prefs.putString("api_key",  cfg_api_key);
    prefs.end();
}

void loadConfig() {
    prefs.begin("tinyai", true);
    cfg_ssid     = prefs.getString("ssid",     "");
    cfg_password = prefs.getString("password", "");
    cfg_api_key  = prefs.getString("api_key",  "");
    prefs.end();
}

bool configIsComplete() {
    return cfg_ssid.length() > 0 &&
           cfg_password.length() > 0 &&
           cfg_api_key.length() > 0;
}

// Interactive config wizard. Shows current values so the user
// can press Enter to keep them unchanged.
void runConfigWizard() {
    Serial.println("\n=== tinyAI Configuration ===");
    Serial.println("Press Enter to keep the current value.\n");

    // SSID
    String prompt_ssid = "WiFi SSID";
    if (cfg_ssid.length() > 0) {
        prompt_ssid += " [" + cfg_ssid + "]";
    }
    prompt_ssid += ": ";
    String new_ssid = serialPrompt(prompt_ssid.c_str());
    if (new_ssid.length() > 0) cfg_ssid = new_ssid;

    // Password — shown masked, but we hint if one is already saved
    String prompt_pw = "WiFi Password";
    if (cfg_password.length() > 0) {
        prompt_pw += " [current: ****]";
    }
    prompt_pw += ": ";
    String new_pw = serialPrompt(prompt_pw.c_str(), true);
    if (new_pw.length() > 0) cfg_password = new_pw;

    // OpenAI Key — always masked, show first 8 chars as hint if saved
    String prompt_key = "OpenAI API Key";
    if (cfg_api_key.length() > 0) {
        prompt_key += " [current: " + cfg_api_key.substring(0, 8) + "...]";
    }
    prompt_key += ": ";
    String new_key = serialPrompt(prompt_key.c_str(), true);
    if (new_key.length() > 0) cfg_api_key = new_key;

    if (!configIsComplete()) {
        Serial.println("\n[ERROR] All three fields are required. Config not saved.");
        return;
    }

    saveConfig();
    Serial.println("\n[OK] Config saved to flash.");
}

// ---------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------

bool connectWiFi() {
    Serial.printf("Connecting to \"%s\"", cfg_ssid.c_str());
    WiFi.disconnect(true);
    WiFi.begin(cfg_ssid.c_str(), cfg_password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            Serial.println("\n[ERROR] WiFi connection timed out.");
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected.");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    return true;
}

// ---------------------------------------------------------------
// Menu
// ---------------------------------------------------------------

void printMenu() {
    Serial.println("\n--- tinyAI_Text Menu ---");
    Serial.println("Type a message and press Enter to chat.");
    Serial.println("[s]  Stop Speaking");
    Serial.println("[c]  Configure WiFi / OpenAI Key");
    Serial.println("[?]  Show Menu");
    Serial.println("------------------------");
}

// ---------------------------------------------------------------
// ChatGPT
// ---------------------------------------------------------------

String askChatGPT(String prompt) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    if (!https.begin(client, "https://api.openai.com/v1/chat/completions")) {
        Serial.println("Failed to connect to OpenAI");
        return "";
    }

    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", String("Bearer ") + cfg_api_key);

    DynamicJsonDocument doc(2048);
    doc["model"] = "gpt-4o-mini";
    JsonArray messages = doc.createNestedArray("messages");
    JsonObject msg = messages.createNestedObject();
    msg["role"] = "user";
    msg["content"] = prompt + " (Keep answer short, under 30 words, no special chars)";

    String body;
    serializeJson(doc, body);

    Serial.println("Sending to ChatGPT...");
    int code = https.POST(body);
    String responseText = "";

    if (code == 200) {
        String payload = https.getString();
        DynamicJsonDocument resDoc(4096);
        deserializeJson(resDoc, payload);
        responseText = resDoc["choices"][0]["message"]["content"].as<String>();
    } else {
        Serial.printf("HTTP Error: %d\n", code);
        Serial.println(https.getString());
    }

    https.end();
    return responseText;
}

// ---------------------------------------------------------------
// Audio
// ---------------------------------------------------------------

// Sends text to OpenAI TTS, streams the MP3 response directly to SD,
// then plays from SD. This avoids buffering audio in RAM, which causes
// the ESP32 to cut out mid-sentence on longer responses.
void speakText(String text) {
    if (text.length() == 0) return;

    Serial.println("\nAI Says:");
    Serial.println(text);
    Serial.println("Fetching TTS audio...");

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    https.begin(client, "https://api.openai.com/v1/audio/speech");
    https.addHeader("Authorization", String("Bearer ") + cfg_api_key);
    https.addHeader("Content-Type", "application/json");
    https.setTimeout(15000); // TTS can take a moment for longer replies

    // tts-1 is faster; swap for tts-1-hd for higher quality
    String body = "{\"model\":\"tts-1\",\"voice\":\"alloy\",\"input\":\"" + text + "\"}";

    int code = https.POST(body);

    if (code == 200) {
        // Stream response body straight to SD — never lives in RAM
        SD.remove("/tts.mp3");
        File f = SD.open("/tts.mp3", FILE_WRITE);
        if (f) {
            https.writeToStream(&f);
            f.close();
            Serial.println("Audio saved to SD, playing...");
            audio.connecttoFS(SD, "/tts.mp3");
        } else {
            Serial.println("[ERROR] Could not open /tts.mp3 for writing.");
        }
    } else {
        Serial.printf("[ERROR] TTS request failed: %d\n", code);
        Serial.println(https.getString());
    }

    https.end();
}

void stopSpeaking() {
    if (audio.isRunning()) {
        audio.stopSong();
        Serial.println("Stopped Speaking");
    }
}

// ---------------------------------------------------------------
// Setup
// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(2000); // Wait for Web Serial to connect

    // SD card must be mounted before audio library starts,
    // since playback reads from it. SD.begin() uses default SPI pins.
    if (!SD.begin()) {
        Serial.println("[ERROR] SD card mount failed. TTS will not work.");
        Serial.println("Check that an SD card is inserted in the tinySpeak HAT.");
    } else {
        Serial.println("SD card mounted.");
    }

    audio.setPinout(I2S_SPKR_BCLK, I2S_SPKR_LRC, I2S_SPKR_DOUT);
    audio.setVolume(21);

    // Load saved credentials from flash
    loadConfig();

    // If no config exists yet, run the wizard before anything else
    if (!configIsComplete()) {
        Serial.println("\n[SETUP] No configuration found. Let's get you set up!");
        runConfigWizard();
    }

    // Attempt WiFi connection
    if (!connectWiFi()) {
        Serial.println("Run [c] to update your WiFi credentials.");
    }

    printMenu();
}

// ---------------------------------------------------------------
// Loop
// ---------------------------------------------------------------

void loop() {
    audio.loop();

    // 1. Handle Serial Input
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input.length() == 0) return;

        // Single-character commands
        if (input.length() == 1) {
            if (input[0] == '?') { printMenu(); return; }
            if (input[0] == 's') { stopSpeaking(); return; }
            if (input[0] == 'c') {
                stopSpeaking();
                runConfigWizard();
                // Reconnect with potentially new credentials
                connectWiFi();
                printMenu();
                return;
            }
        }

        // Chat query
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[ERROR] Not connected to WiFi. Run [c] to reconfigure.");
            return;
        }

        if (audio.isRunning()) audio.stopSong();

        String reply = askChatGPT(input);
        if (reply.length() > 0) {
            speakText(reply);
        }
    }
}

// ---------------------------------------------------------------
// Audio Callbacks
// ---------------------------------------------------------------

void audio_eof_mp3(const char *info) {
    Serial.print("Finished: "); Serial.println(info);
}