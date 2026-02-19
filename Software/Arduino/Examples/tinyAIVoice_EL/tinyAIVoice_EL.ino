/*
 * Project: tinySpeak - tinyAI_Voice_EL
 * Author: Geoff McIntyre
 * Revision Date: 2/18/26
 * License: GNU General Public License v3.0
 *
 * Description:
 * Premium Voice Assistant Pipeline using ElevenLabs TTS.
 * 1. Hold Button (RX) -> Records audio to SD Card (/rec.wav).
 * 2. Release Button   -> Kicks off pipeline:
 *    a. Audio sent to OpenAI Whisper (Speech-to-Text).
 *    b. Transcript sent to ChatGPT (GPT-4o-mini).
 *    c. Reply streamed to SD as MP3 via ElevenLabs TTS, then played from SD.
 *
 * Requirements:
 * - Libraries: ArduinoJson, ESP32-audioI2S by schreibfaul1, HTTPClient by Adrian McEwen
 * - Hardware: tinyCore ESP32-S3 + tinySpeak HAT (SD card required)
 * - Note: TTS audio is saved to /tts.mp3 on SD before playback to avoid RAM exhaustion.
 *
 * Controls:
 * [Hold RX Button]    Start Recording
 * [Release RX Button] Stop Recording & Process
 * [r]  Toggle Recording via Serial
 * [s]  Stop Speaking
 * [c]  Configure WiFi / API Keys / Voice ID
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
Preferences prefs;

String cfg_ssid       = "";
String cfg_password   = "";
String cfg_openai_key = "";
String cfg_eleven_key = "";
String cfg_voice_id   = "";

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
                // Ignore lone CR/LF before any input
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
        audio.loop(); // Keep audio alive while waiting
    }
}

// ---------------------------------------------------------------
// Config
// ---------------------------------------------------------------

void saveConfig() {
    prefs.begin("tinyai", false);
    prefs.putString("ssid",       cfg_ssid);
    prefs.putString("password",   cfg_password);
    prefs.putString("openai_key", cfg_openai_key);
    prefs.putString("eleven_key", cfg_eleven_key);
    prefs.putString("voice_id",   cfg_voice_id);
    prefs.end();
}

void loadConfig() {
    prefs.begin("tinyai", true);
    cfg_ssid       = prefs.getString("ssid",       "");
    cfg_password   = prefs.getString("password",   "");
    cfg_openai_key = prefs.getString("openai_key", "");
    cfg_eleven_key = prefs.getString("eleven_key", "");
    // Default voice ID is 'George' — a safe starting point
    cfg_voice_id   = prefs.getString("voice_id",   "JBFqnCBsd6RMkjVDRZzb");
    prefs.end();
}

bool configIsComplete() {
    return cfg_ssid.length() > 0 &&
           cfg_password.length() > 0 &&
           cfg_openai_key.length() > 0 &&
           cfg_eleven_key.length() > 0 &&
           cfg_voice_id.length() > 0;
}

// Interactive config wizard. Shows current values so the user
// can press Enter to keep them unchanged.
void runConfigWizard() {
    Serial.println("\n=== tinyAI Configuration ===");
    Serial.println("Press Enter to keep the current value.\n");

    // SSID
    String prompt_ssid = "WiFi SSID";
    if (cfg_ssid.length() > 0) prompt_ssid += " [" + cfg_ssid + "]";
    prompt_ssid += ": ";
    String new_ssid = serialPrompt(prompt_ssid.c_str());
    if (new_ssid.length() > 0) cfg_ssid = new_ssid;

    // Password
    String prompt_pw = "WiFi Password";
    if (cfg_password.length() > 0) prompt_pw += " [current: ****]";
    prompt_pw += ": ";
    String new_pw = serialPrompt(prompt_pw.c_str(), true);
    if (new_pw.length() > 0) cfg_password = new_pw;

    // OpenAI Key
    String prompt_oai = "OpenAI API Key";
    if (cfg_openai_key.length() > 0) prompt_oai += " [current: " + cfg_openai_key.substring(0, 8) + "...]";
    prompt_oai += ": ";
    String new_oai = serialPrompt(prompt_oai.c_str(), true);
    if (new_oai.length() > 0) cfg_openai_key = new_oai;

    // ElevenLabs Key
    String prompt_el = "ElevenLabs API Key";
    if (cfg_eleven_key.length() > 0) prompt_el += " [current: " + cfg_eleven_key.substring(0, 8) + "...]";
    prompt_el += ": ";
    String new_el = serialPrompt(prompt_el.c_str(), true);
    if (new_el.length() > 0) cfg_eleven_key = new_el;

    // Voice ID — shown in full since it's not sensitive
    String prompt_vid = "ElevenLabs Voice ID";
    if (cfg_voice_id.length() > 0) prompt_vid += " [" + cfg_voice_id + "]";
    prompt_vid += ": ";
    String new_vid = serialPrompt(prompt_vid.c_str());
    if (new_vid.length() > 0) cfg_voice_id = new_vid;

    if (!configIsComplete()) {
        Serial.println("\n[ERROR] All fields are required. Config not saved.");
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
    Serial.println("\n--- tinyAI_Voice_EL Menu ---");
    Serial.println("[Hold RX] Start Recording");
    Serial.println("[Release RX] Stop & Process");
    Serial.println("[r]  Toggle Recording via Serial");
    Serial.println("[s]  Stop Speaking");
    Serial.println("[c]  Configure WiFi / API Keys / Voice ID");
    Serial.println("[?]  Show Menu");
    Serial.println("----------------------------");
}

// ---------------------------------------------------------------
// Microphone
// ---------------------------------------------------------------

void setupMic() {
    // New driver: create a channel on I2S_NUM_1 in master/RX role
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

    SD.remove("/rec.wav");
    wavFile = SD.open("/rec.wav", FILE_WRITE);
    if (!wavFile) {
        Serial.println("[ERROR] Could not open /rec.wav for writing.");
        return;
    }

    // Write blank placeholder header — finalized in stopRecording()
    uint8_t blank[44] = {0};
    wavFile.write(blank, 44);

    isRecording = true;
    Serial.println("Recording... Press [r] or release button to stop.");
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
    Serial.println("Recording stopped. Processing...");

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ERROR] Not connected to WiFi. Run [c] to reconfigure.");
        return;
    }

    // Step 1: Whisper transcription
    String transcription = transcribeAudio();
    if (transcription.length() < 2) {
        Serial.println("[ERROR] Transcription empty or too short, skipping.");
        return;
    }

    // Step 2: ChatGPT reply
    String reply = getChatResponse(transcription);
    if (reply.length() == 0) return;

    // Step 3: ElevenLabs TTS -> SD -> Play
    playElevenLabs(reply);
}

// ---------------------------------------------------------------
// Pipeline: Whisper (Speech-to-Text)
// ---------------------------------------------------------------

String transcribeAudio() {
    Serial.println("Transcribing with Whisper...");

    WiFiClientSecure client;
    client.setInsecure();

    String boundary = "------------------------7d33a816d302b653";

    if (!client.connect("api.openai.com", 443)) {
        Serial.println("[ERROR] Connection to OpenAI failed.");
        return "";
    }

    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"rec.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n--" + boundary + "--\r\n";

    File f = SD.open("/rec.wav");
    if (!f) { Serial.println("[ERROR] Could not open /rec.wav"); return ""; }

    size_t totalLen = head.length() + f.size() + tail.length();

    client.println("POST /v1/audio/transcriptions HTTP/1.1");
    client.println("Host: api.openai.com");
    client.println("Authorization: Bearer " + cfg_openai_key);
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.print("Content-Length: "); client.println(totalLen);
    client.println();
    client.print(head);

    // Stream WAV file to server in 512-byte chunks — avoids loading file into RAM
    uint8_t buf[512];
    while (f.available()) {
        size_t r = f.read(buf, sizeof(buf));
        client.write(buf, r);
    }
    f.close();
    client.print(tail);

    // Read response
    String res = "";
    unsigned long timeout = millis();
    while (millis() - timeout < 10000) {
        if (client.available()) {
            char c = client.read();
            res += c;
            if (c == '}') break;
        }
    }

    int jsonStart = res.indexOf('{');
    if (jsonStart >= 0) res = res.substring(jsonStart);

    DynamicJsonDocument d(1024);
    deserializeJson(d, res);
    String text = d["text"].as<String>();
    Serial.print("User Said: "); Serial.println(text);
    return text;
}

// ---------------------------------------------------------------
// Pipeline: ChatGPT (Text Intelligence)
// ---------------------------------------------------------------

String getChatResponse(String text) {
    Serial.println("Asking ChatGPT...");

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    if (!https.begin(client, "https://api.openai.com/v1/chat/completions")) return "";

    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", String("Bearer ") + cfg_openai_key);

    DynamicJsonDocument doc(2048);
    doc["model"] = "gpt-4o-mini";
    JsonArray messages = doc.createNestedArray("messages");
    JsonObject msg = messages.createNestedObject();
    msg["role"]    = "user";
    msg["content"] = text + " (Be concise, under 30 words)";

    String body;
    serializeJson(doc, body);

    int code = https.POST(body);
    String reply = "";

    if (code == 200) {
        String payload = https.getString();
        DynamicJsonDocument resDoc(4096);
        deserializeJson(resDoc, payload);
        reply = resDoc["choices"][0]["message"]["content"].as<String>();
        Serial.print("AI Reply: "); Serial.println(reply);
    } else {
        Serial.printf("[ERROR] ChatGPT request failed: %d\n", code);
        Serial.println(https.getString());
    }

    https.end();
    return reply;
}

// ---------------------------------------------------------------
// Pipeline: ElevenLabs TTS -> SD -> Play
// ---------------------------------------------------------------

// Streams the ElevenLabs MP3 response directly to SD before playing.
// This avoids buffering audio in RAM, which causes the ESP32 to
// cut out mid-sentence on longer responses.
void playElevenLabs(String text) {
    if (text.length() == 0) return;
    if (!sdReady) {
        Serial.println("[ERROR] SD card not available. Cannot save or play TTS.");
        return;
    }

    Serial.println("Fetching ElevenLabs TTS audio...");

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    // Turbo v2.5 for lowest latency; mp3_44100_128 for good quality at reasonable size
    String url = "https://api.elevenlabs.io/v1/text-to-speech/" + cfg_voice_id + "?output_format=mp3_44100_128";

    https.begin(client, url);
    https.addHeader("xi-api-key",   cfg_eleven_key);
    https.addHeader("Content-Type", "application/json");
    https.setTimeout(15000); // ElevenLabs can take a moment for longer responses

    DynamicJsonDocument doc(2048);
    doc["text"]     = text;
    doc["model_id"] = "eleven_turbo_v2_5"; // Fast model; swap for eleven_multilingual_v2 for higher quality
    JsonObject settings = doc.createNestedObject("voice_settings");
    settings["stability"]        = 0.5;
    settings["similarity_boost"] = 0.7;

    String requestBody;
    serializeJson(doc, requestBody);

    int code = https.POST(requestBody);

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
        Serial.printf("[ERROR] ElevenLabs request failed: %d\n", code);
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

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // SD card must be mounted first — both recording and TTS playback depend on it.
    // No CS pin argument: the tinySpeak HAT uses the default SPI CS pin.
    if (!SD.begin()) {
        Serial.println("[ERROR] SD card mount failed. Recording and TTS will not work.");
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

    audio.setPinout(I2S_SPKR_BCLK, I2S_SPKR_LRC, I2S_SPKR_DOUT);
    audio.setVolume(21);

    setupMic();

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

    // Drain mic samples to SD every tick while recording
    if (isRecording) {
        captureAudio();
    }

    // Button handling (debounced): hold to record, release to process
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
            // Released (HIGH) -> stop and process
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
        if (input[0] == 's') { stopSpeaking(); return; }

        if (input[0] == 'c') {
            if (isRecording) stopRecording();
            stopSpeaking();
            runConfigWizard();
            connectWiFi();
            printMenu();
            return;
        }

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

void audio_eof_mp3(const char *info) {
    Serial.print("Finished: "); Serial.println(info);
}
