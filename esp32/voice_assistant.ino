/*
 * ESP32-S3 Voice Assistant
 * INMP441 (I2S Mic) + MAX98357A (I2S Amplifier)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ┌─────────────────────────────────────────┐
 * │  INMP441 Microphone                     │
 * │  Pin    →  ESP32-S3                     │
 * │  VDD    →  3.3V                         │
 * │  GND    →  GND                          │
 * │  SCK    →  GPIO 35 (D35)               │
 * │  WS     →  GPIO 32 (D32)               │
 * │  SD     →  GPIO 33 (D33)               │
 * │  L/R    →  GND  (left channel)         │
 * └─────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────┐
 * │  MAX98357A Speaker Amplifier            │
 * │  Pin    →  ESP32-S3                     │
 * │  VIN    →  5V (Vin)                     │
 * │  GND    →  GND                          │
 * │  BCLK   →  GPIO 26 (D26)               │
 * │  LRC    →  GPIO 27 (D27)               │
 * │  DIN    →  GPIO 25 (D25)               │
 * │  SD     →  GND  (always enabled)       │
 * └─────────────────────────────────────────┘
 *
 * BOOT button (GPIO 0) = push to talk
 * Built-in LED (GPIO 2) = status indicator
 *
 * Arduino IDE:
 *   Board   : ESP32S3 Dev Module
 *   Library : ArduinoJson by Benoit Blanchon
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ── Server ────────────────────────────────────────────────────────────────────
const char* SERVER = "https://esp32chatbot.vercel.app";

// ── INMP441 — I2S Port 0 (microphone input) ───────────────────────────────────
#define MIC_PORT      I2S_NUM_0
#define MIC_SCK_PIN   35    // SCK  on INMP441
#define MIC_WS_PIN    32    // WS   on INMP441
#define MIC_SD_PIN    33    // SD   on INMP441

// ── MAX98357A — I2S Port 1 (speaker output) ───────────────────────────────────
#define SPK_PORT      I2S_NUM_1
#define SPK_BCLK_PIN  26    // BCLK on MAX98357A
#define SPK_LRC_PIN   27    // LRC  on MAX98357A
#define SPK_DIN_PIN   25    // DIN  on MAX98357A

// ── Button & LED ──────────────────────────────────────────────────────────────
#define BUTTON_PIN     0
#define LED_PIN        2

// ── Recording config ──────────────────────────────────────────────────────────
#define SAMPLE_RATE    16000
#define RECORD_SECS    3
#define PCM_BYTES      (SAMPLE_RATE * 2 * RECORD_SECS)   // 96 000 bytes
#define WAV_SIZE       (PCM_BYTES + 44)

static uint8_t wavBuf[WAV_SIZE];

// ─────────────────────────────────────────────────────────────────────────────
// WAV header
// ─────────────────────────────────────────────────────────────────────────────
void buildWAVHeader() {
  int32_t dataSize  = PCM_BYTES;
  int32_t fileSize  = dataSize + 36;
  int32_t sr        = SAMPLE_RATE;
  int32_t byteRate  = sr * 2;
  int16_t blockAlign = 2, bps = 16, ch = 1, fmt = 1, fmtSz = 16;
  uint8_t* h = wavBuf;
  memcpy(h,       "RIFF", 4); memcpy(h +  4, &fileSize,   4);
  memcpy(h +  8,  "WAVE", 4); memcpy(h + 12, "fmt ",      4);
  memcpy(h + 16, &fmtSz,  4); memcpy(h + 20, &fmt,        2);
  memcpy(h + 22, &ch,     2); memcpy(h + 24, &sr,         4);
  memcpy(h + 28, &byteRate,4); memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 34, &bps,    2);
  memcpy(h + 36, "data",  4); memcpy(h + 40, &dataSize,   4);
}

// ─────────────────────────────────────────────────────────────────────────────
// INMP441 — I2S input
// INMP441 sends 24-bit audio left-justified in a 32-bit I2S frame.
// ─────────────────────────────────────────────────────────────────────────────
void micStart() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441 needs 32-bit frame
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;  // L/R tied to GND = left
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = MIC_SCK_PIN;
  pins.ws_io_num    = MIC_WS_PIN;
  pins.data_in_num  = MIC_SD_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
}

void micStop() { i2s_driver_uninstall(MIC_PORT); }

// ─────────────────────────────────────────────────────────────────────────────
// MAX98357A — I2S output
// Server sends 24 kHz 16-bit signed PCM → play directly via I2S
// ─────────────────────────────────────────────────────────────────────────────
void spkStart(int rate = 24000) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = (uint32_t)rate;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;   // silence on underrun (no pop)

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = SPK_BCLK_PIN;
  pins.ws_io_num    = SPK_LRC_PIN;
  pins.data_out_num = SPK_DIN_PIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
}

void spkStop() { i2s_driver_uninstall(SPK_PORT); }

// ─────────────────────────────────────────────────────────────────────────────
// Record from INMP441 → wavBuf (WAV format)
// INMP441 gives 24-bit data left-justified in 32-bit word.
// Shift right 11 bits → good 16-bit signed level.
// ─────────────────────────────────────────────────────────────────────────────
void recordAudio() {
  Serial.println("[REC] Recording " + String(RECORD_SECS) + "s...");
  digitalWrite(LED_PIN, HIGH);

  micStart();

  int32_t dmaBuf[64];
  size_t  bytesRead;
  int     offset = 0;

  while (offset < PCM_BYTES) {
    i2s_read(MIC_PORT, dmaBuf, sizeof(dmaBuf), &bytesRead, portMAX_DELAY);
    int samples = bytesRead / 4;

    for (int i = 0; i < samples && offset < PCM_BYTES; i++) {
      // 24-bit INMP441 data is left-justified in 32-bit → shift right 11 for 16-bit
      int16_t s = (int16_t)(dmaBuf[i] >> 11);
      wavBuf[44 + offset]     = (uint8_t)(s & 0xFF);
      wavBuf[44 + offset + 1] = (uint8_t)((s >> 8) & 0xFF);
      offset += 2;
    }
  }

  micStop();
  buildWAVHeader();
  digitalWrite(LED_PIN, LOW);
  Serial.printf("[REC] Done — %d bytes PCM\n", PCM_BYTES);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST WAV → /api/stt → text
// ─────────────────────────────────────────────────────────────────────────────
String transcribe() {
  Serial.println("[STT] Uploading...");
  digitalWrite(LED_PIN, HIGH);

  HTTPClient http;
  http.begin(String(SERVER) + "/api/stt");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(25000);

  int code = http.POST(wavBuf, WAV_SIZE);
  digitalWrite(LED_PIN, LOW);

  if (code != 200) {
    Serial.printf("[STT] HTTP %d\n", code);
    http.end(); return "";
  }

  StaticJsonDocument<512> doc;
  deserializeJson(doc, http.getString());
  http.end();

  String text = doc["text"] | "";
  text.trim();
  Serial.println("[STT] Heard: " + text);
  return text;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST text → /api/chat → GPT reply
// ─────────────────────────────────────────────────────────────────────────────
String chat(const String& msg) {
  Serial.println("[GPT] " + msg);

  String esc = msg;
  esc.replace("\\", "\\\\");
  esc.replace("\"", "\\\"");

  HTTPClient http;
  http.begin(String(SERVER) + "/api/chat");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(25000);

  int code = http.POST("{\"message\":\"" + esc + "\"}");
  if (code != 200) {
    Serial.printf("[GPT] HTTP %d\n", code);
    http.end(); return "";
  }

  StaticJsonDocument<1024> doc;
  deserializeJson(doc, http.getString());
  http.end();

  String reply = doc["reply"] | "";
  reply.trim();
  Serial.println("[GPT] Reply: " + reply);
  return reply;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST text → /api/tts → stream 24 kHz 16-bit PCM → MAX98357A
// Server returns raw signed int16 little-endian PCM — write directly to I2S
// ─────────────────────────────────────────────────────────────────────────────
void speak(const String& text) {
  Serial.println("[TTS] Speaking...");

  String esc = text;
  esc.replace("\\", "\\\\");
  esc.replace("\"", "\\\"");

  HTTPClient http;
  http.begin(String(SERVER) + "/api/tts");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(25000);

  int code = http.POST("{\"text\":\"" + esc + "\"}");
  if (code != 200) {
    Serial.printf("[TTS] HTTP %d\n", code);
    http.end(); return;
  }

  spkStart(24000);  // 24 kHz matches server PCM output

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  size_t  written;

  // Stream PCM directly into I2S DMA — no buffering needed
  while (http.connected() || stream->available()) {
    int avail = stream->available();
    if (avail == 0) { delay(1); continue; }
    int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
    if (n > 0) i2s_write(SPK_PORT, buf, n, &written, portMAX_DELAY);
  }

  spkStop();
  http.end();
  Serial.println("[TTS] Done");
}

// ─────────────────────────────────────────────────────────────────────────────
// POST conversation → /api/message (web page display)
// ─────────────────────────────────────────────────────────────────────────────
void logMessage(const String& user, const String& bot) {
  String u = user; u.replace("\"", "\\\"");
  String b = bot;  b.replace("\"", "\\\"");
  HTTPClient http;
  http.begin(String(SERVER) + "/api/message");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  http.POST("{\"user\":\"" + u + "\",\"bot\":\"" + b + "\"}");
  http.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  Serial.println("\n=== ESP32-S3 Voice Assistant ===");
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  digitalWrite(LED_PIN, LOW);
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  Serial.println("[READY] Press BOOT button to speak.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop — press BOOT button to run one full pipeline turn
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  if (digitalRead(BUTTON_PIN) != LOW) return;
  delay(50);
  if (digitalRead(BUTTON_PIN) != LOW) return;

  Serial.println("\n─── New turn ───");

  recordAudio();

  String userText = transcribe();
  if (!userText.length()) {
    Serial.println("[ERR] Nothing heard");
    return;
  }

  String botReply = chat(userText);
  if (!botReply.length()) {
    Serial.println("[ERR] No reply");
    return;
  }

  speak(botReply);
  logMessage(userText, botReply);

  while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  delay(200);
  Serial.println("[READY] Press BOOT button to speak.");
}
