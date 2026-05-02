/*
 * ESP32 Voice Assistant — Direct Mic + Speaker (no external I2S modules)
 * ─────────────────────────────────────────────────────────────────────────────
 * Board: ESP32-WROOM-32 (30-pin dev board)
 *
 * Uses the ESP32 built-in ADC (for mic) and built-in DAC (for speaker).
 * NO external modules like INMP441 or MAX98357A needed.
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  MICROPHONE  (electret condenser mic, 2 pins)                │
 * │                                                              │
 * │   Mic (+) ──┬── 10kΩ ── 3.3V                                │
 * │             └── 100nF ── GPIO 34  (D34, ADC1_CH6)           │
 * │   Mic (–) ──── GND                                          │
 * │                                                              │
 * │  The resistor biases the mic. The capacitor removes DC.      │
 * └──────────────────────────────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  SPEAKER  (small 8Ω or 4Ω speaker)                          │
 * │                                                              │
 * │   GPIO 25 (D25, DAC1) ── 100Ω ── Speaker (+)                │
 * │                                   Speaker (–) ── GND        │
 * │                                                              │
 * │  The 100Ω resistor protects the pin. For louder output       │
 * │  add a small transistor amp or LM386 module between.         │
 * └──────────────────────────────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  BUTTON                                                      │
 * │   BOOT button (bottom-right, built-in) = GPIO 0              │
 * │   Press once to start recording, recording stops after 3s    │
 * └──────────────────────────────────────────────────────────────┘
 *
 * Arduino IDE:
 *   Board  : ESP32 Dev Module
 *   Library: ArduinoJson by Benoit Blanchon  (install via Library Manager)
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ── Vercel backend ────────────────────────────────────────────────────────────
const char* SERVER = "https://esp32chatbot.vercel.app";

// ── Pins ──────────────────────────────────────────────────────────────────────
#define MIC_PIN      34          // GPIO 34 = ADC1_CH6 (input-only, perfect for mic)
#define SPK_PIN      25          // GPIO 25 = DAC1
#define BUTTON_PIN    0          // BOOT button (active LOW)
#define LED_PIN       2          // Built-in LED

// ── Recording ─────────────────────────────────────────────────────────────────
#define SAMPLE_RATE   16000      // Hz — good for speech STT
#define RECORD_SECS   3
#define PCM_BYTES     (SAMPLE_RATE * 2 * RECORD_SECS)  // 16-bit mono = 96 000 B
#define WAV_SIZE      (PCM_BYTES + 44)

// ADC → I2S uses I2S_NUM_0 (only port that supports built-in ADC/DAC)
#define I2S_PORT      I2S_NUM_0

static uint8_t wavBuf[WAV_SIZE];   // 96 044 B — fits in ESP32 SRAM

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
  memcpy(h,      "RIFF",  4); memcpy(h +  4, &fileSize,  4);
  memcpy(h + 8,  "WAVE",  4); memcpy(h + 12, "fmt ",     4);
  memcpy(h + 16, &fmtSz,  4); memcpy(h + 20, &fmt,       2);
  memcpy(h + 22, &ch,     2); memcpy(h + 24, &sr,        4);
  memcpy(h + 28, &byteRate,4); memcpy(h + 32, &blockAlign,2);
  memcpy(h + 34, &bps,    2);
  memcpy(h + 36, "data",  4); memcpy(h + 40, &dataSize,  4);
}

// ─────────────────────────────────────────────────────────────────────────────
// I2S ADC — microphone input via built-in ADC on GPIO 34
// The ESP32 I2S ADC mode samples the ADC through DMA — efficient and accurate.
// ─────────────────────────────────────────────────────────────────────────────
void micStart() {
  i2s_config_t cfg = {};
  cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  cfg.sample_rate       = SAMPLE_RATE;
  cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format    = I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count     = 4;
  cfg.dma_buf_len       = 256;
  cfg.use_apll          = false;

  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  // ADC1_CHANNEL_6 = GPIO 34
  // Set 11dB attenuation via Arduino API (0–3.6V input range)
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_6);
  i2s_adc_enable(I2S_PORT);
}

void micStop() {
  i2s_adc_disable(I2S_PORT);
  i2s_driver_uninstall(I2S_PORT);
}

// ─────────────────────────────────────────────────────────────────────────────
// I2S DAC — speaker output via built-in DAC on GPIO 25
// ─────────────────────────────────────────────────────────────────────────────
void spkStart(int sampleRate = 24000) {
  i2s_config_t cfg = {};
  cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate       = (uint32_t)sampleRate;
  cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format    = I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count     = 8;
  cfg.dma_buf_len       = 256;
  cfg.use_apll          = false;
  cfg.tx_desc_auto_clear = true;

  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, NULL);                        // NULL = use internal DAC
  i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);         // RIGHT = GPIO 25 = DAC1
  i2s_zero_dma_buffer(I2S_PORT);
}

void spkStop() {
  i2s_driver_uninstall(I2S_PORT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Record 3 seconds from mic → fill wavBuf as WAV
// ADC I2S returns 16-bit unsigned values (12-bit ADC, centered ~2048).
// We convert to signed 16-bit PCM by subtracting 2048 and scaling.
// ─────────────────────────────────────────────────────────────────────────────
void recordAudio() {
  Serial.println("[REC] Recording " + String(RECORD_SECS) + "s ...");
  digitalWrite(LED_PIN, HIGH);

  micStart();

  uint16_t dmaBuf[128];
  size_t   bytesRead;
  int      pcmOffset = 0;

  while (pcmOffset < PCM_BYTES) {
    int want = min((int)sizeof(dmaBuf), PCM_BYTES - pcmOffset);
    i2s_read(I2S_PORT, dmaBuf, want, &bytesRead, portMAX_DELAY);
    int n = bytesRead / 2;

    for (int i = 0; i < n && pcmOffset < PCM_BYTES; i++) {
      // ADC value: 12-bit unsigned (0–4095), center = 2048
      // Mask lower 12 bits (upper 4 bits can be channel number)
      int16_t sample = (int16_t)((dmaBuf[i] & 0x0FFF) - 2048) * 16;

      wavBuf[44 + pcmOffset]     = (uint8_t)(sample & 0xFF);
      wavBuf[44 + pcmOffset + 1] = (uint8_t)((sample >> 8) & 0xFF);
      pcmOffset += 2;
    }
  }

  micStop();
  buildWAVHeader();
  digitalWrite(LED_PIN, LOW);
  Serial.printf("[REC] Done — %d bytes captured\n", PCM_BYTES);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST WAV → /api/stt → transcribed text
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
  esc.replace("\\", "\\\\"); esc.replace("\"", "\\\"");

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
// POST text → /api/tts → stream signed 16-bit PCM (24 kHz) → DAC speaker
//
// The server sends signed int16 little-endian PCM.
// The ESP32 DAC needs unsigned 8-bit values written as uint16 in the upper byte.
// Conversion: dacOut = ((sample >> 8) + 128) << 8
// ─────────────────────────────────────────────────────────────────────────────
void speak(const String& text) {
  Serial.println("[TTS] Speaking...");

  String esc = text;
  esc.replace("\\", "\\\\"); esc.replace("\"", "\\\"");

  HTTPClient http;
  http.begin(String(SERVER) + "/api/tts");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(25000);

  int code = http.POST("{\"text\":\"" + esc + "\"}");
  if (code != 200) {
    Serial.printf("[TTS] HTTP %d\n", code);
    http.end(); return;
  }

  spkStart(24000);

  WiFiClient* stream = http.getStreamPtr();

  // Read signed int16 PCM from network, convert to DAC uint16 for I2S DAC mode
  const int CHUNK = 512;
  uint8_t  netBuf[CHUNK];   // raw bytes from network (pairs of int16 LE)
  uint16_t dacBuf[CHUNK/2]; // converted samples for DAC
  size_t   written;

  while (http.connected() || stream->available()) {
    int avail = stream->available();
    if (avail == 0) { delay(1); continue; }

    int n = stream->readBytes(netBuf, min(avail, CHUNK));

    // Process pairs of bytes → int16 → DAC uint16
    int samples = n / 2;
    for (int i = 0; i < samples; i++) {
      int16_t s = (int16_t)(netBuf[i*2] | (netBuf[i*2+1] << 8));
      // Shift signed 16-bit to unsigned 8-bit, put in upper byte for DAC
      dacBuf[i] = (uint16_t)((uint8_t)((s >> 8) + 128)) << 8;
    }

    i2s_write(I2S_PORT, dacBuf, samples * 2, &written, portMAX_DELAY);
  }

  spkStop();
  http.end();
  Serial.println("[TTS] Done");
}

// ─────────────────────────────────────────────────────────────────────────────
// Log conversation to web page via /api/message
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

  Serial.println("\n=== ESP32 Voice Assistant ===");
  Serial.print("[WiFi] Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400); Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  digitalWrite(LED_PIN, LOW);
  Serial.println("\n[WiFi] " + WiFi.localIP().toString());
  Serial.println("[READY] Press BOOT button to speak.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  if (digitalRead(BUTTON_PIN) != LOW) return;
  delay(50);
  if (digitalRead(BUTTON_PIN) != LOW) return;

  Serial.println("\n─── New turn ───");

  recordAudio();

  String userText = transcribe();
  if (!userText.length()) { Serial.println("[ERR] Nothing heard"); return; }

  String botReply = chat(userText);
  if (!botReply.length()) { Serial.println("[ERR] No reply"); return; }

  speak(botReply);
  logMessage(userText, botReply);

  while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  delay(200);
  Serial.println("[READY] Press BOOT button to speak.");
}
