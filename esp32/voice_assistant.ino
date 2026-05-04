/*
 * ESP32-S3 Voice Assistant — Always-On Mic + VAD
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ┌─────────────────────────────────────────┐
 * │  INMP441 Microphone                     │
 * │  VDD  →  3.3V                           │
 * │  GND  →  GND                            │
 * │  SCK  →  GPIO 35                        │
 * │  WS   →  GPIO 32                        │
 * │  SD   →  GPIO 33                        │
 * │  L/R  →  GND                            │
 * └─────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────┐
 * │  MAX98357A Speaker                      │
 * │  VIN  →  5V                             │
 * │  GND  →  GND                            │
 * │  BCLK →  GPIO 26                        │
 * │  LRC  →  GPIO 27                        │
 * │  DIN  →  GPIO 25                        │
 * │  SD   →  GND                            │
 * └─────────────────────────────────────────┘
 *
 * Board  : ESP32S3 Dev Module
 * Library: ArduinoJson by Benoit Blanchon
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* WIFI_SSID = "Crazzysaradar";
const char* WIFI_PASS = "crazzysardar";

// ── Server ────────────────────────────────────────────────────────────────────
const char* SERVER = "https://esp32chatbot.vercel.app";

// ── INMP441 — I2S port 0 ──────────────────────────────────────────────────────
#define MIC_PORT     I2S_NUM_0
#define MIC_SCK_PIN  35
#define MIC_WS_PIN   32
#define MIC_SD_PIN   33

// ── MAX98357A — I2S port 1 ────────────────────────────────────────────────────
#define SPK_PORT     I2S_NUM_1
#define SPK_BCLK_PIN 26
#define SPK_LRC_PIN  27
#define SPK_DIN_PIN  25

// ── LED ───────────────────────────────────────────────────────────────────────
#define LED_PIN      2

// ── VAD (Voice Activity Detection) config ─────────────────────────────────────
#define SAMPLE_RATE       16000
#define VAD_THRESHOLD     800      // amplitude to detect speech  — tune if needed
#define SILENCE_MS        900      // ms of silence → end of speech
#define MIN_SPEECH_MS     300      // ignore triggers shorter than this (noise)
#define MAX_RECORD_SECS   2        // hard cap — ESP32-WROOM DRAM limit (~300KB usable)
#define COOLDOWN_MS       600      // wait after TTS before listening again

// ── Buffer ─────────────────────────────────────────────────────────────────────
#define PCM_BYTES  (SAMPLE_RATE * 2 * MAX_RECORD_SECS)   // 192 000 B
#define WAV_SIZE   (PCM_BYTES + 44)

static uint8_t  wavBuf[WAV_SIZE];
static bool     isSpeaking = false;   // blocks mic while TTS is playing

// ─────────────────────────────────────────────────────────────────────────────
// WAV header
// ─────────────────────────────────────────────────────────────────────────────
void buildWAVHeader(int dataBytes) {
  int32_t fileSize  = dataBytes + 36;
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
  memcpy(h + 36, "data",  4); memcpy(h + 40, &dataBytes,  4);
}

// ─────────────────────────────────────────────────────────────────────────────
// I2S — INMP441 mic
// ─────────────────────────────────────────────────────────────────────────────
void micStart() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
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
// I2S — MAX98357A speaker
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
  cfg.tx_desc_auto_clear   = true;

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
// Read one DMA chunk — always writes into chunkPCM (128 bytes = 64 samples).
// Returns peak amplitude of the chunk.
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t chunkPCM[128];   // 64 samples × 2 bytes — reused every chunk

int readMicChunk() {
  int32_t raw[64];
  size_t  bytesRead = 0;
  i2s_read(MIC_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
  int samples = bytesRead / 4;
  int peak    = 0;

  for (int i = 0; i < samples; i++) {
    int16_t s   = (int16_t)(raw[i] >> 11);
    int     amp = abs((int)s);
    if (amp > peak) peak = amp;
    chunkPCM[i * 2]     = (uint8_t)(s & 0xFF);
    chunkPCM[i * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
  }
  return peak;
}

// ─────────────────────────────────────────────────────────────────────────────
// Always-on VAD — returns PCM bytes recorded into wavBuf+44, or 0 on noise
// ─────────────────────────────────────────────────────────────────────────────
int listenAndRecord() {
  int  pcmOffset    = 0;
  bool recording    = false;
  int  silenceChunks = 0;
  int  speechChunks  = 0;

  const int CHUNK_MS          = (64 * 1000) / SAMPLE_RATE;   // ≈ 4 ms
  const int SILENCE_CHUNKS    = SILENCE_MS  / CHUNK_MS;
  const int MIN_SPEECH_CHUNKS = MIN_SPEECH_MS / CHUNK_MS;
  const int CHUNK_BYTES       = 128;

  Serial.println("[VAD] Listening...");
  digitalWrite(LED_PIN, LOW);

  while (true) {
    if (isSpeaking) { delay(10); continue; }

    int peak = readMicChunk();   // always fills chunkPCM

    if (!recording) {
      if (peak > VAD_THRESHOLD) {
        recording     = true;
        silenceChunks = 0;
        speechChunks  = 1;
        pcmOffset     = 0;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[VAD] Voice — recording");
        // Save this first chunk
        memcpy(wavBuf + 44, chunkPCM, CHUNK_BYTES);
        pcmOffset += CHUNK_BYTES;
      }
    } else {
      // Copy chunk into buffer
      if (pcmOffset + CHUNK_BYTES <= PCM_BYTES) {
        memcpy(wavBuf + 44 + pcmOffset, chunkPCM, CHUNK_BYTES);
        pcmOffset += CHUNK_BYTES;
      }

      if (peak > VAD_THRESHOLD) {
        silenceChunks = 0;
        speechChunks++;
      } else {
        silenceChunks++;
      }

      if (silenceChunks >= SILENCE_CHUNKS) {
        if (speechChunks >= MIN_SPEECH_CHUNKS) {
          Serial.printf("[VAD] Done — %d bytes\n", pcmOffset);
          digitalWrite(LED_PIN, LOW);
          return pcmOffset;
        }
        // Too short — reset
        Serial.println("[VAD] Too short, ignoring");
        recording = false; pcmOffset = 0;
        speechChunks = 0;  silenceChunks = 0;
        digitalWrite(LED_PIN, LOW);
      }

      if (pcmOffset >= PCM_BYTES) {
        Serial.println("[VAD] Max length");
        digitalWrite(LED_PIN, LOW);
        return pcmOffset;
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST WAV → /api/stt
// ─────────────────────────────────────────────────────────────────────────────
String transcribe(int pcmBytes) {
  buildWAVHeader(pcmBytes);
  int wavBytes = pcmBytes + 44;

  Serial.println("[STT] Uploading...");
  HTTPClient http;
  http.begin(String(SERVER) + "/api/stt");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(25000);

  int code = http.POST(wavBuf, wavBytes);
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
// POST text → /api/chat
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
// POST text → /api/tts → stream PCM → MAX98357A
// ─────────────────────────────────────────────────────────────────────────────
void speak(const String& text) {
  Serial.println("[TTS] Speaking: " + text);
  isSpeaking = true;

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
    http.end();
    isSpeaking = false;
    return;
  }

  spkStart(24000);

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  size_t  written;

  while (http.connected() || stream->available()) {
    int avail = stream->available();
    if (avail == 0) { delay(1); continue; }
    int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
    if (n > 0) i2s_write(SPK_PORT, buf, n, &written, portMAX_DELAY);
  }

  spkStop();
  http.end();

  delay(COOLDOWN_MS);   // short pause so mic doesn't pick up speaker echo
  isSpeaking = false;
  Serial.println("[TTS] Done");
}

// ─────────────────────────────────────────────────────────────────────────────
// Log to web page
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
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("\n=== Zulu Bot — ESP32-S3 ===");

  // Connect WiFi
  Serial.print("[WiFi] Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400); Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  digitalWrite(LED_PIN, LOW);
  Serial.println("\n[WiFi] " + WiFi.localIP().toString());

  // Start mic — stays open the whole time
  micStart();

  // Greet on startup
  speak("Hi, I am Zulu bot. How can I help you?");

  Serial.println("[READY] Always listening...");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop — always listening, full pipeline on every detected utterance
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Listen until speech ends
  int pcmBytes = listenAndRecord();
  if (pcmBytes == 0) return;

  // Pause mic during API calls (uninstall/reinstall so I2S_NUM_0 is free)
  micStop();

  // 2. STT
  String userText = transcribe(pcmBytes);
  if (!userText.length()) {
    micStart();
    return;
  }

  // 3. GPT
  String botReply = chat(userText);
  if (!botReply.length()) {
    micStart();
    return;
  }

  // 4. TTS → speaker
  speak(botReply);

  // 5. Log to web
  logMessage(userText, botReply);

  // Restart mic for next utterance
  micStart();
  Serial.println("[READY] Listening...");
}
