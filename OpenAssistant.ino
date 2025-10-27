/*
  Cardputer AI Assistant (Context + Scroll + SD Config + GO->type over USB HID)

  Features:
  - Loads WiFi + API Key from /chat_config.txt on SD card
      Format example:
        WIFI_SSID = YourNetwork
        WIFI_PASS = YourPassword
        OPENAI_KEY = sk-...
  - Multi-turn context memory via chatHistory
  - Backspace works (0x7F / 0x2A and ks.del flag)
  - Long replies are scrollable:
        ';' scrolls UP
        '.' scrolls DOWN
        ENTER exits scroll mode
        GO button "types" (HID keyboard) the whole assistant reply into host
  - Sends full chat context to OpenAI Responses API each turn

  NOTE:
    This version uses USB HID keyboard output.
    Your board/SDK must support TinyUSB HID keyboard classes on ESP32-S3.
    If compilation fails around USBHIDKeyboard/USB.begin(), you'll need the
    ESP32-S3 HID keyboard support libraries (TinyUSB-based). On some setups
    you may need to enable USB CDC+HID in "Tools -> USB Mode".
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <ctype.h>
#include <algorithm>
#include <SD.h>
#include <SPI.h>
#include "esp32-hal-rgb-led.h"

// ---- USB HID keyboard (TinyUSB style on ESP32-S3) ----
#include "USB.h"
#include "USBHIDKeyboard.h"

USBHIDKeyboard HidKeyboard;  // we'll use this to "type" last reply

// ---------- GLOBAL CONFIG (loaded from SD) ----------

String WIFI_SSID = "";
String WIFI_PASS = "";
String OPENAI_API_KEY = "";

const char* OPENAI_HOST     = "api.openai.com";
const int   OPENAI_PORT     = 443;
const char* OPENAI_ENDPOINT = "/v1/responses";
const char* OPENAI_TRANSCRIBE_ENDPOINT = "/v1/audio/transcriptions";
const char* MODEL_NAME       = "gpt-4o-mini";
const char* TRANSCRIBE_MODEL = "gpt-4o-mini-transcribe";
const char* SYSTEM_PROMPT_TEXT =
  "You are a helpful assistant on a tiny handheld called the Cardputer. "
  "Keep answers short, clear, and friendly so they fit on the 320x240 screen.";

// Voice recording configuration
const uint32_t VOICE_SAMPLE_RATE     = 16000;
const size_t   VOICE_CHUNK_SAMPLES   = 256;
const uint32_t VOICE_MAX_DURATION_MS = 6000;
const size_t   VOICE_MAX_SAMPLES     = (VOICE_MAX_DURATION_MS / 1000) * VOICE_SAMPLE_RATE;
const char*    VOICE_TEMP_DIR        = "/oa_tmp";
const char*    VOICE_TEMP_PATH       = "/oa_tmp/voice.raw";
const char*    TRANSCRIPT_DIR        = "/transcripts";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

const int SCREEN_WIDTH       = 240;
const int SCREEN_HEIGHT      = 135;
const int HEADER_TEXT_SIZE   = 2;
const int STATUS_TEXT_SIZE   = 2;
const int CONTENT_TEXT_SIZE  = 2;
const int PROMPT_TEXT_SIZE   = CONTENT_TEXT_SIZE;
const int REPLY_TEXT_SIZE    = CONTENT_TEXT_SIZE;
const int HEADER_HEIGHT      = HEADER_TEXT_SIZE * 12 + 8;
const int STATUS_HEIGHT      = STATUS_TEXT_SIZE * 12 + 6;
const int CONTENT_MARGIN_X   = 8;
const int CONTENT_MARGIN_Y   = 4;
const int CONTENT_LINE_EXTRA = 6;
const int PROMPT_AREA_Y      = HEADER_HEIGHT + STATUS_HEIGHT + CONTENT_MARGIN_Y;
const int PROMPT_AREA_HEIGHT = SCREEN_HEIGHT > PROMPT_AREA_Y ? SCREEN_HEIGHT - PROMPT_AREA_Y : 0;
const int REPLY_AREA_Y       = PROMPT_AREA_Y;

const uint32_t INACTIVITY_TIMEOUT_MS      = 60000;
const uint32_t BATTERY_UPDATE_INTERVAL_MS = 5000;

// ---------- GLOBALS ----------

WiFiClientSecure httpsClient;
String inputBuffer = "";
String lastUserPrompt = "";

// Conversation memory
struct ChatMessage {
  String role;    // "system", "user", "assistant"
  String content;
};
std::vector<ChatMessage> chatHistory;

// Scrollable reply buffer
std::vector<String> replyLines;
int scrollOffset = 0;

// Keep the last assistant reply so GO button can "type" it out
String lastAssistantReply = "";
String headerTitle = "Cardputer AI";
String statusBaseLine = "";

// Voice recording state (SD-backed)
File voiceTempFile;
size_t voiceRecordedSamples = 0;
bool voiceFileReady = false;
bool voiceTranscribing = false;
bool displaySleeping = false;
unsigned long lastActivityMs = 0;
unsigned long lastBatteryUpdateMs = 0;
bool sdMounted = false;
unsigned long lastStatusScrollUpdateMs = 0;
int statusScrollOffset = 0;

// LED state
int8_t boardLedPin = -1;
bool ledAvailable = false;
bool ledBlinkState = false;
unsigned long lastLedToggleMs = 0;
int ledBusyDepth = 0;

// ------------------------------------------------------------
// Utility: trim whitespace from both ends of String
// ------------------------------------------------------------
String trimBoth(const String& s) {
  String out = s;
  while (out.length() > 0 && isspace(out[0])) {
    out.remove(0, 1);
  }
  while (out.length() > 0 && isspace(out[out.length() - 1])) {
    out.remove(out.length() - 1);
  }
  return out;
}

String normalizeQuotes(const String& text) {
  String t = text;
  t.replace("’", "'");  // right single quote
  t.replace("‘", "'");  // left single quote
  t.replace("“", "\""); // left double
  t.replace("”", "\""); // right double
  return t;
}

// ------------------------------------------------------------
// Voice helper utilities
// ------------------------------------------------------------
static inline void writeLE16(uint8_t* dst, uint16_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
}

static inline void writeLE32(uint8_t* dst, uint32_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
  dst[2] = (value >> 16) & 0xFF;
  dst[3] = (value >> 24) & 0xFF;
}

static void fillWavHeader(uint8_t* header, size_t sampleCount, uint32_t sampleRate) {
  size_t pcmBytes = sampleCount * sizeof(int16_t);
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  writeLE32(header + 4, pcmBytes + 36);
  header[8]  = 'W'; header[9]  = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  writeLE32(header + 16, 16);   // Subchunk1Size
  writeLE16(header + 20, 1);    // PCM
  writeLE16(header + 22, 1);    // Mono
  writeLE32(header + 24, sampleRate);
  uint32_t byteRate = sampleRate * 2; // mono, 16-bit
  writeLE32(header + 28, byteRate);
  writeLE16(header + 32, 2);    // block align
  writeLE16(header + 34, 16);   // bits per sample
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  writeLE32(header + 40, pcmBytes);
}

bool appendSamplesToVoiceFile(const int16_t* samples, size_t sampleCount) {
  if (!voiceTempFile) {
    return false;
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(samples);
  size_t bytesRemaining = sampleCount * sizeof(int16_t);

  while (bytesRemaining > 0) {
    size_t chunkBytes = bytesRemaining;
    if (chunkBytes > 512) {
      chunkBytes = 512;
    }
    size_t written = voiceTempFile.write(ptr, chunkBytes);
    if (written == 0) {
      voiceTempFile.flush();  // attempt to clear buffers then retry once
      written = voiceTempFile.write(ptr, chunkBytes);
      if (written == 0) {
        return false;
      }
    }
    ptr += written;
    bytesRemaining -= written;
  }

  voiceTempFile.flush();
  return true;
}

String generateTranscriptPath() {
  unsigned long stamp = millis();
  char name[64];
  snprintf(name, sizeof(name), "%s/context_%lu.txt", TRANSCRIPT_DIR, (unsigned long)stamp);
  return String(name);
}

bool ensureDirectory(const char* path) {
  if (SD.exists(path)) {
    return true;
  }
  return SD.mkdir(path);
}

bool mountSD() {
  if (sdMounted) return true;
  if (!SD.begin()) {
    Serial.println("SD mount failed.");
    return false;
  }
  sdMounted = true;
  return true;
}

void unmountSD() {
  if (!sdMounted) return;
  if (voiceTempFile) {
    voiceTempFile.flush();
    voiceTempFile.close();
    voiceTempFile = File();
  }
  SD.end();
  sdMounted = false;
}

bool saveContextTranscript(String& outPath) {
  if (!mountSD()) {
    Serial.println("Failed to mount SD for transcript.");
    return false;
  }
  if (!ensureDirectory(TRANSCRIPT_DIR)) {
    Serial.println("Failed to ensure transcript dir.");
    unmountSD();
    return false;
  }

  String filePath = generateTranscriptPath();
  File f = SD.open(filePath, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open transcript file.");
    unmountSD();
    return false;
  }

  unsigned long stamp = millis();
  f.print("# Cardputer transcript @ ");
  f.println(stamp);
  f.println("# Format: role: content");
  f.println();

  for (size_t i = 0; i < chatHistory.size(); ++i) {
    f.print(chatHistory[i].role);
    f.println(":");
    f.println(chatHistory[i].content);
    f.println();
  }
  f.close();
  unmountSD();

  outPath = filePath;
  Serial.print("Transcript saved to: ");
  Serial.println(filePath);
  return true;
}

void resetConversationState() {
  chatHistory.clear();
  addMessageToHistory("system", SYSTEM_PROMPT_TEXT);
  lastAssistantReply = "";
  replyLines.clear();
  scrollOffset = 0;
}

String requestTranscriptionFromSD(size_t sampleCount) {
  File audioFile = SD.open(VOICE_TEMP_PATH, FILE_READ);
  if (!audioFile) {
    Serial.println("Failed to open temp voice file for read.");
    return "";
  }

  const size_t wavHeaderSize = 44;
  size_t pcmBytes = sampleCount * sizeof(int16_t);
  if (audioFile.size() > 0) {
    pcmBytes = audioFile.size();
  }

  String boundary = "----CardputerVoiceBoundary";
  String partModel = "--" + boundary + "\r\n";
  partModel += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  partModel += TRANSCRIBE_MODEL;
  partModel += "\r\n";

  String partFormat = "--" + boundary + "\r\n";
  partFormat += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n";
  partFormat += "text\r\n";

  String partAudioHeader = "--" + boundary + "\r\n";
  partAudioHeader += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
  partAudioHeader += "Content-Type: audio/wav\r\n\r\n";

  String closing = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = partModel.length()
                       + partFormat.length()
                       + partAudioHeader.length()
                       + wavHeaderSize
                       + pcmBytes
                       + closing.length();

  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(30000);

  if (!httpsClient.connect(OPENAI_HOST, OPENAI_PORT)) {
    Serial.println("Transcription HTTPS connect FAIL");
    return "";
  }

  String req;
  req += "POST ";
  req += OPENAI_TRANSCRIBE_ENDPOINT;
  req += " HTTP/1.1\r\n";
  req += "Host: ";
  req += OPENAI_HOST;
  req += "\r\n";
  req += "Authorization: Bearer ";
  req += OPENAI_API_KEY;
  req += "\r\n";
  req += "Content-Type: multipart/form-data; boundary=";
  req += boundary;
  req += "\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: ";
  req += String((unsigned long)contentLength);
  req += "\r\n\r\n";

  httpsClient.print(req);
  httpsClient.print(partModel);
  httpsClient.print(partFormat);
  httpsClient.print(partAudioHeader);
  uint8_t header[44];
  size_t sampleCountForHeader = pcmBytes / sizeof(int16_t);
  fillWavHeader(header, sampleCountForHeader, VOICE_SAMPLE_RATE);
  httpsClient.write(header, sizeof(header));
  uint8_t ioBuf[512];
  while (audioFile.available()) {
    size_t rd = audioFile.read(ioBuf, sizeof(ioBuf));
    if (rd == 0) break;
    httpsClient.write(ioBuf, rd);
    maybeUpdateLed();
  }
  audioFile.close();
  httpsClient.print(closing);

  String statusLine = "";
  String headerAccum = "";
  bool headersFinished = false;
  unsigned long lastRead = millis();
  String response;

  while (httpsClient.connected() || httpsClient.available()) {
    while (httpsClient.available()) {
      char c = httpsClient.read();
      lastRead = millis();
      if (!headersFinished) {
        headerAccum += c;
        if (c == '\n') {
          if (statusLine.length() == 0) {
            statusLine = headerAccum;
          }
          if (headerAccum == "\r\n") {
            headersFinished = true;
          }
          headerAccum = "";
        }
      } else {
        response += c;
      }
    }

    if (headersFinished && !httpsClient.connected() && !httpsClient.available()) {
      break;
    }

    maybeUpdateLed();
    if (millis() - lastRead > 30000) {
      Serial.println("Transcription read timeout");
      break;
    }
    delay(10);
  }
  httpsClient.stop();

  int statusCode = 0;
  int spaceIdx = statusLine.indexOf(' ');
  if (spaceIdx > 0) {
    int spaceIdx2 = statusLine.indexOf(' ', spaceIdx + 1);
    if (spaceIdx2 > spaceIdx) {
      statusCode = statusLine.substring(spaceIdx + 1, spaceIdx2).toInt();
    }
  }

  response.trim();
  Serial.println("---- Transcription Response ----");
  Serial.println(response);
  Serial.println("--------------------------------");
  if (statusCode != 200) {
    Serial.print("Transcription HTTP error: ");
    Serial.println(statusCode);
    return "";
  }
  return response;
}

String transcribeVoiceFile(size_t sampleCount) {
  if (!sdMounted) {
    Serial.println("SD not mounted for transcription.");
    return "";
  }
  return requestTranscriptionFromSD(sampleCount);
}

// ------------------------------------------------------------
// Load config from SD card (/chat_config.txt)
// ------------------------------------------------------------
bool loadConfigFromSD() {
  if (!mountSD()) {
    Serial.println("SD init FAILED");
    return false;
  }

  File f = SD.open("/chat_config.txt", "r");
  if (!f) {
    Serial.println("chat_config.txt MISSING");
    unmountSD();
    return false;
  }

  Serial.println("Reading chat_config.txt...");
  while (f.available()) {
    String line = f.readStringUntil('\n');

    // strip comments (# to end of line)
    int hashPos = line.indexOf('#');
    if (hashPos >= 0) {
      line = line.substring(0, hashPos);
    }

    line = trimBoth(line);
    if (line.length() == 0) continue;

    int eqPos = line.indexOf('=');
    if (eqPos < 0) continue;

    String key = trimBoth(line.substring(0, eqPos));
    String value = trimBoth(line.substring(eqPos + 1));

    if (key == "WIFI_SSID") {
      WIFI_SSID = value;
      Serial.print("SSID: ");
      Serial.println(WIFI_SSID);
    } else if (key == "WIFI_PASS") {
      WIFI_PASS = value;
      Serial.println("PASS: (hidden)");
    } else if (key == "OPENAI_KEY") {
      OPENAI_API_KEY = value;
      Serial.println("KEY: (loaded)");
    }
  }

  f.close();
  unmountSD();

  if (WIFI_SSID.isEmpty() || WIFI_PASS.isEmpty() || OPENAI_API_KEY.isEmpty()) {
    Serial.println("Config missing one or more fields!");
    return false;
  }

  Serial.println("Config loaded OK.");
  return true;
}

// ------------------------------------------------------------
// Chat / Display Helpers
// ------------------------------------------------------------

void addMessageToHistory(const String& role, const String& text) {
  ChatMessage msg{ role, text };
  chatHistory.push_back(msg);

  // Limit memory: keep only last 8 messages
  if (chatHistory.size() > 8) {
    chatHistory.erase(
      chatHistory.begin(),
      chatHistory.begin() + (chatHistory.size() - 8)
    );
  }
}

void lcdClearAll() {
  M5Cardputer.Display.fillScreen(BLACK);
}

int getBatteryPercent() {
  int level = M5Cardputer.Power.getBatteryLevel();
  if (level >= 0 && level <= 100) {
    return level;
  }

  float voltage = M5Cardputer.Power.getBatteryVoltage();  // volts
  if (voltage <= 0.1f) {
    return -1;
  }

  const float minV = 3.3f;
  const float maxV = 4.2f;
  float pct = (voltage - minV) * 100.0f / (maxV - minV);
  return (int)constrain((int)(pct + 0.5f), 0, 100);
}

String batteryText() {
  int level = getBatteryPercent();
  if (level >= 0 && level <= 100) {
    return String(level) + "%";
  }
  return "--%";
}

void updateHeaderBattery() {
  if (displaySleeping) return;
  M5Cardputer.Display.setTextSize(HEADER_TEXT_SIZE);
  String batt = batteryText();
  int padding = 4;
  int width = M5Cardputer.Display.textWidth(batt.c_str());
  int x = SCREEN_WIDTH - width - padding;
  if (x < 0) x = 0;
  M5Cardputer.Display.fillRect(x - 2, 0, width + padding + 2, HEADER_HEIGHT, BLUE);
  M5Cardputer.Display.setCursor(x, 6);
  M5Cardputer.Display.setTextColor(WHITE, BLUE);
  M5Cardputer.Display.print(batt);
}

void renderStatusLineInner(bool force);
void renderStatusLine(bool force);

void maybeUpdateBatteryIndicator() {
  renderStatusLineInner(false);
}

void markActivity() {
  lastActivityMs = millis();
}

bool wakeDisplayIfNeeded() {
  if (!displaySleeping) return false;
  displaySleeping = false;
  M5Cardputer.Display.wakeup();
  M5Cardputer.Display.setBrightness(255);
  lcdClearAll();
  lcdHeader("Cardputer AI");
  renderStatusLineInner(true);
  if (ledAvailable && ledBusyDepth == 0) {
    setLedColor(0, 255, 0);
    ledBlinkState = true;
    lastLedToggleMs = millis();
  }
  return true;
}

void checkDisplaySleep() {
  if (displaySleeping) return;
  unsigned long now = millis();
  if (now - lastActivityMs >= INACTIVITY_TIMEOUT_MS) {
    displaySleeping = true;
    M5Cardputer.Display.sleep();
    if (ledAvailable) {
      setLedColor(0, 0, 0);
      ledBlinkState = false;
    }
  }
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!ledAvailable) return;
  rgbLedWrite(boardLedPin, r, g, b);
}

void initializeLed() {
  boardLedPin = M5.getPin(m5::pin_name_t::rgb_led);
  ledAvailable = (boardLedPin >= 0 && boardLedPin < 255);
  ledBlinkState = false;
  lastLedToggleMs = millis();
  ledBusyDepth = 0;
  if (ledAvailable) {
    setLedColor(0, 255, 0);
    ledBlinkState = true;
  }
}

void ledEnterBusy() {
  if (!ledAvailable) return;
  ledBusyDepth++;
  setLedColor(0, 0, 255);
  ledBlinkState = true;
  lastLedToggleMs = millis();
}

void ledExitBusy() {
  if (!ledAvailable) return;
  if (ledBusyDepth > 0) {
    ledBusyDepth--;
  }
  if (ledBusyDepth == 0) {
    if (displaySleeping) {
      setLedColor(0, 0, 0);
      ledBlinkState = false;
    } else {
      ledBlinkState = false;
      lastLedToggleMs = millis();
      setLedColor(0, 255, 0);
      ledBlinkState = true;
    }
  }
}

void maybeUpdateLed() {
  if (!ledAvailable) return;
  if (displaySleeping) {
    if (ledBlinkState) {
      setLedColor(0, 0, 0);
      ledBlinkState = false;
    }
    return;
  }
  unsigned long now = millis();
  if (ledBusyDepth > 0) {
    if (now - lastLedToggleMs >= 300) {
      ledBlinkState = !ledBlinkState;
      lastLedToggleMs = now;
      if (ledBlinkState) {
        setLedColor(0, 0, 255);
      } else {
        setLedColor(0, 0, 0);
      }
    }
  } else {
    if (!ledBlinkState) {
      setLedColor(0, 255, 0);
      ledBlinkState = true;
      lastLedToggleMs = now;
    }
  }
}

bool stringIsDigits(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (!isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

template <typename KeyState>
bool hasKeyboardActivity(const KeyState& ks) {
  return ks.enter || ks.del || !ks.word.empty();
}

int lineHeightForSize(int textSize) {
  return (8 * textSize) + CONTENT_LINE_EXTRA;
}

std::vector<String> wrapTextToLines(const String& text, int maxWidth, int textSize) {
  M5Cardputer.Display.setTextSize(textSize);
  std::vector<String> lines;
  String current = "";

  for (int i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(current);
      current = "";
      continue;
    }

    String candidate = current;
    candidate += c;
    int width = M5Cardputer.Display.textWidth(candidate.c_str());
    if (width > maxWidth && current.length() > 0) {
      lines.push_back(current);
      current = "";
      if (c == ' ') {
        continue;
      }
      candidate = String(c);
      width = M5Cardputer.Display.textWidth(candidate.c_str());
      if (width > maxWidth) {
        lines.push_back(candidate);
        continue;
      }
    }
    current += c;
  }

  if (current.length() > 0) {
    lines.push_back(current);
  }

  if (lines.empty()) {
    lines.push_back("");
  }

  return lines;
}

int visibleReplyLines() {
  int h = SCREEN_HEIGHT - REPLY_AREA_Y;
  int lineHeight = lineHeightForSize(REPLY_TEXT_SIZE);
  int available = h - lineHeight;
  if (available < 0) available = 0;
  int lines = available / lineHeight;
  if (available == 0) lines = 1;
  if (lines < 1) lines = 1;
  return lines;
}

String readSimpleTextLine(const String& statusMsg) {
  String buffer = "";
  std::vector<char> prevHeld;
  bool prevBackspace = false;

  markActivity();
  lcdShowPromptEditing(buffer);
  lcdStatusLine(statusMsg);

  while (true) {
    M5Cardputer.update();
    maybeUpdateLed();
    maybeUpdateBatteryIndicator();
    checkDisplaySleep();

    auto ks = M5Cardputer.Keyboard.keysState();
    bool goNow = M5Cardputer.BtnA.isPressed();
    bool inputActive = hasKeyboardActivity(ks) || goNow;
    if (displaySleeping) {
      if (!inputActive) {
        prevHeld = ks.word;
        prevBackspace = ks.del;
        delay(40);
        continue;
      }
      wakeDisplayIfNeeded();
      lcdShowPromptEditing(buffer);
      lcdStatusLine(statusMsg);
      markActivity();
    }

    bool backspaceNow = ks.del;
    if (backspaceNow && !prevBackspace) {
      if (buffer.length() > 0) {
        buffer.remove(buffer.length() - 1);
        markActivity();
      }
    }
    prevBackspace = backspaceNow;

    std::vector<char> currHeld = ks.word;
    for (char c_now : currHeld) {
      bool alreadyHeld = false;
      for (char c_prev : prevHeld) {
        if (c_prev == c_now) {
          alreadyHeld = true;
          break;
        }
      }
      if (alreadyHeld) continue;

      uint8_t code = static_cast<uint8_t>(c_now);
      if (code == 0x7F || code == 0x2A) {
        if (buffer.length() > 0) {
          buffer.remove(buffer.length() - 1);
          markActivity();
        }
      } else if (code == '\n') {
        String result = buffer;
        result.trim();
        return result;
      } else if (code >= 0x20 && code <= 0x7E) {
        buffer += static_cast<char>(code);
        markActivity();
      }
    }

    if (ks.enter) {
      String result = buffer;
      result.trim();
      markActivity();
      return result;
    }

    lcdShowPromptEditing(buffer);
    lcdStatusLine(statusMsg);

    prevHeld = currHeld;
    delay(30);
  }
}

bool connectToWiFi(const String& ssid, const String& pass, unsigned long timeoutMs = WIFI_CONNECT_TIMEOUT_MS) {
  if (ssid.isEmpty()) return false;

  ledEnterBusy();
  markActivity();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(120);

  if (pass.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    maybeUpdateLed();
    delay(250);
  }

  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok) {
    WiFi.setAutoReconnect(true);
  } else {
    WiFi.disconnect(true);
  }

  ledExitBusy();
  return ok;
}

bool writeConfigToSD() {
  if (!mountSD()) return false;
  SD.remove("/chat_config.txt");
  File f = SD.open("/chat_config.txt", FILE_WRITE);
  if (!f) {
    unmountSD();
    return false;
  }

  f.print("WIFI_SSID = ");
  f.println(WIFI_SSID);
  f.print("WIFI_PASS = ");
  f.println(WIFI_PASS);
  f.print("OPENAI_KEY = ");
  f.println(OPENAI_API_KEY);
  f.close();
  unmountSD();
  return true;
}

void lcdHeader(const String& msg) {
  headerTitle = msg;
  M5Cardputer.Display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, BLUE);
  M5Cardputer.Display.setTextColor(WHITE, BLUE);
  M5Cardputer.Display.setTextSize(HEADER_TEXT_SIZE);

  String title = headerTitle;
  int padding = 4;
  String batt = batteryText();
  int battWidth = M5Cardputer.Display.textWidth(batt.c_str());
  int availableWidth = SCREEN_WIDTH - battWidth - padding * 2;
  while (M5Cardputer.Display.textWidth(title.c_str()) > availableWidth && title.length() > 0) {
    title.remove(title.length() - 1);
  }
  M5Cardputer.Display.setCursor(padding, 6);
  M5Cardputer.Display.print(title);
  updateHeaderBattery();
}

void lcdStatusLine(const String& msg) {
  statusBaseLine = msg;
  statusScrollOffset = 0;
  lastStatusScrollUpdateMs = 0;
  renderStatusLineInner(true);
}

void lcdShowPromptEditing(const String& current) {
  int startY = PROMPT_AREA_Y;
  M5Cardputer.Display.fillRect(0, startY, SCREEN_WIDTH, PROMPT_AREA_HEIGHT, BLACK);

  M5Cardputer.Display.setTextSize(PROMPT_TEXT_SIZE);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, startY);
  M5Cardputer.Display.print("You:");

  int indent = CONTENT_MARGIN_X + M5Cardputer.Display.textWidth("You:") + (PROMPT_TEXT_SIZE * 6);
  int textWidth = SCREEN_WIDTH - indent - CONTENT_MARGIN_X;
  if (textWidth < 40) textWidth = 40;

  auto lines = wrapTextToLines(current, textWidth, PROMPT_TEXT_SIZE);
  int lineHeight = lineHeightForSize(PROMPT_TEXT_SIZE);
  int maxLines = PROMPT_AREA_HEIGHT / lineHeight;
  if (maxLines < 1) maxLines = 1;
  int cursorLine = std::min((int)lines.size() - 1, maxLines - 1);

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int i = 0; i < maxLines && i < (int)lines.size(); ++i) {
    int y = startY + i * lineHeight;
    if (y >= startY + PROMPT_AREA_HEIGHT) break;
    int x = indent;
    String textLine = lines[i];
    M5Cardputer.Display.setCursor(x, y);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.print(textLine);
    if (i == cursorLine) {
      int cursorX = x + M5Cardputer.Display.textWidth(textLine.c_str());
      int cursorMax = SCREEN_WIDTH - CONTENT_MARGIN_X - (PROMPT_TEXT_SIZE * 6);
      if (cursorX > cursorMax) cursorX = cursorMax;
      M5Cardputer.Display.setCursor(cursorX, y);
      M5Cardputer.Display.setTextColor(GREEN, BLACK);
      M5Cardputer.Display.print("_");
      M5Cardputer.Display.setTextColor(WHITE, BLACK);
    }
  }
}

// Convert the assistant's full reply string into wrapped lines
// and reset scrollOffset.
void prepareReplyLinesFromText(const String& replyText) {
  replyLines = wrapTextToLines(replyText, SCREEN_WIDTH - CONTENT_MARGIN_X * 2, CONTENT_TEXT_SIZE);
  if (replyLines.empty()) {
    replyLines.push_back("");
  }
  scrollOffset = 0;
}

// Render the assistant reply window starting at scrollOffset
void lcdShowAssistantReplyWindow() {
  int startY = REPLY_AREA_Y;
  int h = SCREEN_HEIGHT - startY;
  if (h < 0) h = 0;

  M5Cardputer.Display.fillRect(0, startY, SCREEN_WIDTH, h, BLACK);

  M5Cardputer.Display.setTextSize(REPLY_TEXT_SIZE);
  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, startY);
  M5Cardputer.Display.println("AI:");

  int lineHeight = lineHeightForSize(REPLY_TEXT_SIZE);
  int contentStartY = startY + lineHeight;
  int maxLinesOnScreen = visibleReplyLines();
  int maxOffset = (int)replyLines.size() - maxLinesOnScreen;
  if (maxOffset < 0) maxOffset = 0;
  if (scrollOffset > maxOffset) scrollOffset = maxOffset;

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int i = 0; i < maxLinesOnScreen; ++i) {
    int idx = scrollOffset + i;
    if (idx < 0 || idx >= (int)replyLines.size()) break;
    int y = contentStartY + i * lineHeight;
    if (y >= SCREEN_HEIGHT) break;
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, y);
    M5Cardputer.Display.print(replyLines[idx]);
  }
}

void lcdShowUserPromptView() {
  int startY = REPLY_AREA_Y;
  int h = SCREEN_HEIGHT - startY;
  if (h < 0) h = 0;

  M5Cardputer.Display.fillRect(0, startY, SCREEN_WIDTH, h, BLACK);

  M5Cardputer.Display.setTextSize(REPLY_TEXT_SIZE);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, startY);
  M5Cardputer.Display.println("You:");

  String promptText = lastUserPrompt;
  if (promptText.isEmpty()) {
    promptText = "(no prompt)";
  }
  auto lines = wrapTextToLines(promptText, SCREEN_WIDTH - CONTENT_MARGIN_X * 2, REPLY_TEXT_SIZE);
  int lineHeight = lineHeightForSize(REPLY_TEXT_SIZE);
  int contentStartY = startY + lineHeight;
  int maxLinesOnScreen = visibleReplyLines();

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int i = 0; i < maxLinesOnScreen; ++i) {
    if (i >= (int)lines.size()) break;
    int y = contentStartY + i * lineHeight;
    if (y >= SCREEN_HEIGHT) break;
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, y);
    M5Cardputer.Display.print(lines[i]);
  }
}

// ------------------------------------------------------------
// WiFi / HTTPS setup
// ------------------------------------------------------------

void connectWiFi() {
  if (WIFI_SSID.isEmpty()) {
    lcdStatusLine("WiFi config missing. Use /wifi");
    Serial.println("WiFi config missing.");
    return;
  }

  lcdStatusLine("WiFi: connecting...");
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  bool ok = connectToWiFi(WIFI_SSID, WIFI_PASS, WIFI_CONNECT_TIMEOUT_MS);

  if (ok) {
    Serial.println("WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    lcdStatusLine("WiFi OK: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi connection failed.");
    lcdStatusLine("WiFi failed. Use /wifi to set up");
  }
}

bool wifiInteractiveSetup() {
  lcdClearAll();
  lcdHeader("WiFi Setup");
  lcdStatusLine("Scanning networks...");
  markActivity();

  ledEnterBusy();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(120);
  int networkCount = WiFi.scanNetworks();
  ledExitBusy();

  if (networkCount <= 0) {
    lcdStatusLine("No networks found.");
    Serial.println("WiFi scan returned no networks.");
    delay(1200);
    WiFi.scanDelete();
    return false;
  }

  lcdClearAll();
  lcdHeader("WiFi Networks");
  int maxDisplay = networkCount < 6 ? networkCount : 6;
  int listY = PROMPT_AREA_Y;
  int lineHeight = lineHeightForSize(CONTENT_TEXT_SIZE);
  M5Cardputer.Display.setTextSize(CONTENT_TEXT_SIZE);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int i = 0; i < maxDisplay; ++i) {
    int y = listY + i * lineHeight;
    if (y >= SCREEN_HEIGHT) break;
    String line = String(i) + ": " + WiFi.SSID(i);
    if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
      line += " *";
    }
    const int maxChars = 16;
    if (line.length() > maxChars) {
      line = line.substring(0, maxChars - 3) + "...";
    }
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, y);
    M5Cardputer.Display.print(line);
  }
  if (networkCount > maxDisplay) {
    int y = listY + maxDisplay * lineHeight;
    if (y < SCREEN_HEIGHT) {
      M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, y);
      M5Cardputer.Display.print("... (" + String(networkCount - maxDisplay) + " more)");
    }
  }

  String selection = readSimpleTextLine("Select network # or name (blank=cancel)");
  selection.trim();
  if (selection.length() == 0 || selection.equalsIgnoreCase("exit")) {
    lcdStatusLine("WiFi setup cancelled.");
    delay(900);
    WiFi.scanDelete();
    return false;
  }

  String chosenSsid = "";
  String chosenPass = "";
  bool requiresPass = true;
  int chosenIndex = -1;

  bool selectionIsNumber = stringIsDigits(selection);
  if (selectionIsNumber) {
    int idx = selection.toInt();
    if (idx >= 0 && idx < networkCount) {
      chosenIndex = idx;
      chosenSsid = WiFi.SSID(idx);
      wifi_auth_mode_t auth = (wifi_auth_mode_t)WiFi.encryptionType(idx);
      requiresPass = (auth != WIFI_AUTH_OPEN);
    } else {
      lcdStatusLine("Invalid selection.");
      delay(900);
      WiFi.scanDelete();
      return false;
    }
  }

  if (!selectionIsNumber) {
    chosenSsid = selection;
    requiresPass = true;
  }

  WiFi.scanDelete();

  if (chosenSsid.isEmpty()) {
    lcdStatusLine("Invalid selection.");
    delay(900);
    return false;
  }

  if (requiresPass) {
    bool haveExisting = (chosenSsid == WIFI_SSID) && !WIFI_PASS.isEmpty();
    while (true) {
      String prompt = haveExisting
        ? "Password (blank=keep current /cancel)"
        : "Enter WiFi password (/cancel)";
      String entry = readSimpleTextLine(prompt);
      if (entry.equalsIgnoreCase("/cancel")) {
        lcdStatusLine("WiFi setup cancelled.");
        delay(900);
        return false;
      }
      if (entry.length() == 0) {
        if (haveExisting) {
          chosenPass = WIFI_PASS;
          break;
        } else {
          lcdStatusLine("Password required.");
          delay(800);
          continue;
        }
      }
      chosenPass = entry;
      break;
    }
  } else {
    lcdStatusLine("Open network selected.");
    delay(500);
  }

  lcdStatusLine("Connecting to " + chosenSsid);
  bool ok = connectToWiFi(chosenSsid, chosenPass, WIFI_CONNECT_TIMEOUT_MS);
  if (!ok) {
    lcdStatusLine("WiFi connect failed.");
    Serial.println("Interactive WiFi connect failed.");
    delay(1200);
    return false;
  }

  WIFI_SSID = chosenSsid;
  WIFI_PASS = chosenPass;

  lcdStatusLine("WiFi connected!");
  Serial.println("Interactive WiFi setup succeeded.");
  delay(800);

  String saveAnswer = readSimpleTextLine("Save network to SD? (y/n)");
  saveAnswer.trim();
  saveAnswer.toLowerCase();
  if (saveAnswer.startsWith("y")) {
    bool saved = writeConfigToSD();
    if (saved) {
      lcdStatusLine("Config saved.");
      Serial.println("WiFi configuration saved to SD.");
    } else {
      lcdStatusLine("Failed to save config.");
      Serial.println("Failed to save WiFi config to SD.");
    }
    delay(900);
  }

  lcdStatusLine("WiFi ready.");
  delay(600);
  return true;
}

void initHTTPS() {
  httpsClient.setInsecure();    // no cert validation (dev mode)
  httpsClient.setTimeout(15000);
}

// ------------------------------------------------------------
// Build JSON body for OpenAI "Responses" API
// We include entire rolling chatHistory as `input` array.
// ------------------------------------------------------------
String buildRequestBody() {
  // We'll build using ArduinoJson to avoid bad string escaping.

  // Reserve doc. 8k is a decent working size for ESP32.
  StaticJsonDocument<8192> doc;

  doc["model"] = MODEL_NAME;

  JsonArray inputArr = doc.createNestedArray("input");
  for (size_t i = 0; i < chatHistory.size(); i++) {
    JsonObject m = inputArr.createNestedObject();
    m["role"]    = chatHistory[i].role;
    m["content"] = chatHistory[i].content;
  }

  // Serialize into a String
  String body;
  serializeJson(doc, body);
  return body;
}

// ------------------------------------------------------------
// Send POST to OpenAI over HTTPS and capture the raw JSON reply
// ------------------------------------------------------------
String callOpenAI() {
  ledEnterBusy();
  String body = buildRequestBody();

  lcdStatusLine("Querying OpenAI...");
  markActivity();
  Serial.println("Connecting to OpenAI...");
  Serial.println("---- Request Body ----");
  Serial.println(body);
  Serial.println("----------------------");

  String response = "";

  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(15000);

  if (httpsClient.connect(OPENAI_HOST, OPENAI_PORT)) {
    String req;
    req += "POST ";
    req += OPENAI_ENDPOINT;
    req += " HTTP/1.1\r\n";
    req += "Host: ";
    req += OPENAI_HOST;
    req += "\r\n";
    req += "Authorization: Bearer ";
    req += OPENAI_API_KEY;
    req += "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Connection: close\r\n";
    req += "Content-Length: ";
    req += String(body.length());
    req += "\r\n\r\n";
    req += body;

    httpsClient.print(req);

    String statusLine = "";
    String headerAccum = "";
    bool headersFinished = false;
    unsigned long lastRead = millis();

    while (httpsClient.connected() || httpsClient.available()) {
      while (httpsClient.available()) {
        char c = httpsClient.read();
        lastRead = millis();
        if (!headersFinished) {
          headerAccum += c;
          if (c == '\n') {
            if (statusLine.length() == 0) {
              statusLine = headerAccum;
            }
            if (headerAccum == "\r\n") {
              headersFinished = true;
            }
            headerAccum = "";
          }
        } else {
          response += c;
        }
      }

      if (headersFinished && !httpsClient.connected() && !httpsClient.available()) {
        break;
      }

      maybeUpdateLed();
      if (millis() - lastRead > 30000) {
        Serial.println("OpenAI read timeout");
        break;
      }
      delay(10);
    }

    httpsClient.stop();
    if (statusLine.length()) {
      Serial.print("HTTP status: ");
      Serial.print(statusLine);
    }
  } else {
    Serial.println("HTTPS connect FAIL");
    lcdStatusLine("OpenAI connect FAIL");
  }

  Serial.println("---- Raw Response ----");
  Serial.println(response);
  Serial.println("----------------------");
  markActivity();
  ledExitBusy();
  return response;
}

// ------------------------------------------------------------
// Parse assistant text from OpenAI Responses API JSON
//   We expect shape like:
//   {
//     "output":[
//       {
//         "content":[{"text":"..."}]
//       }
//     ]
//   }
// ------------------------------------------------------------
String parseAssistantReply(const String& rawJson) {
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, rawJson);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return "[JSON parse error]";
  }

  JsonArray outputArr = doc["output"].as<JsonArray>();
  if (!outputArr.size()) {
    return "[No output]";
  }

  JsonArray contentArr = outputArr[0]["content"].as<JsonArray>();
  if (!contentArr.size()) {
    return "[No content]";
  }

  String text = contentArr[0]["text"].as<String>();
  return text;
}

// ------------------------------------------------------------
// USB HID typing of assistant reply
// ------------------------------------------------------------
void typeReplyOverUSB(const String& text) {
  // Dump reply into the connected host as keystrokes.
  // Requires TinyUSB HID keyboard support.
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    HidKeyboard.print(c);  // print single char
    delay(5);              // mild pacing
    if ((i & 0x0F) == 0) {
      markActivity();
    }
  }
  markActivity();
}

// ------------------------------------------------------------
// Scroll / View Mode
// - ';' scrolls UP
// - '.' scrolls DOWN
// - GO button types lastAssistantReply over USB HID
// - ENTER exits back to prompt mode
// ------------------------------------------------------------
void viewAssistantReplyInteractive() {
  auto showAssistant = [&]() {
    lcdShowAssistantReplyWindow();
    lcdStatusLine(";/.:scroll ,/:toggle GO:type ENTER:exit");
  };
  auto showUser = [&]() {
    lcdShowUserPromptView();
    lcdStatusLine(",/:toggle GO:type AI ENTER:exit");
  };

  bool showingAssistant = true;
  showAssistant();
  markActivity();

  bool prevEnter = false;
  bool prevGo    = false;

  // for edge-detect on ';' and '.'
  std::vector<char> prevHeld;

  while (true) {
    M5Cardputer.update();
    maybeUpdateLed();
    maybeUpdateBatteryIndicator();
    checkDisplaySleep();
    auto ks = M5Cardputer.Keyboard.keysState();
    bool goNow = M5Cardputer.BtnA.isPressed();
    bool inputActive = hasKeyboardActivity(ks) || goNow;
    if (displaySleeping) {
      if (!inputActive) {
        prevHeld = ks.word;
        prevEnter = ks.enter;
        prevGo = goNow;
        delay(60);
        continue;
      }
      wakeDisplayIfNeeded();
      if (showingAssistant) {
        showAssistant();
      } else {
        showUser();
      }
      markActivity();
    }

    // -- EXIT on ENTER --
    if (ks.enter && !prevEnter) {
      markActivity();
      return;
    }
    prevEnter = ks.enter;

    // -- GO button to type the last assistant reply --
    if (goNow && !prevGo) {
      markActivity();
      // Send keystrokes over USB
      lcdStatusLine("Typing reply over HID...");
      typeReplyOverUSB(lastAssistantReply);
      if (showingAssistant) {
        showAssistant();
      } else {
        showUser();
      }
    }
    prevGo = goNow;

    // -- SCROLL with ';' (up) / '.' (down) --
    std::vector<char> currHeld = ks.word;
    int visibleLines = visibleReplyLines();
    int maxOffset = (int)replyLines.size() - visibleLines;
    if (maxOffset < 0) maxOffset = 0;

    for (char c_now : currHeld) {
      bool alreadyHeld = false;
      for (char c_prev : prevHeld) {
        if (c_prev == c_now) {
          alreadyHeld = true;
          break;
        }
      }

        if (!alreadyHeld) {
          if (c_now == ',' || c_now == '/') {
            showingAssistant = !showingAssistant;
            if (showingAssistant) {
              showAssistant();
            } else {
              showUser();
            }
            markActivity();
          } else if (showingAssistant) {
            if (c_now == ';') {
              if (scrollOffset > 0) {
                markActivity();
                scrollOffset--;
              lcdShowAssistantReplyWindow();
            }
          } else if (c_now == '.') {
            if (scrollOffset < maxOffset) {
              markActivity();
              scrollOffset++;
              lcdShowAssistantReplyWindow();
            }
          }
        }
      }
    }

    prevHeld = currHeld;
    delay(80); // slow-ish poll so you don't overshoot instantly
  }
}

// ------------------------------------------------------------
// Keyboard input (prompt typing)
// - supports delete/backspace using ks.del or code 0x7F / 0x2A
// - ENTER submits
// ------------------------------------------------------------
String readPromptFromKeyboard() {
  inputBuffer = "";
  lcdShowPromptEditing(inputBuffer);
  lcdStatusLine("Type. ENTER=send");
  markActivity();

  std::vector<char> prevHeld;
  bool prevBackspace = false;
  bool prevGoButton = false;

  size_t voiceSampleCount = 0;
  bool voiceRecording = false;
  bool voiceWriteError = false;
  unsigned long voiceStartMs = 0;

  voiceRecordedSamples = 0;
  voiceFileReady = false;
  if (voiceTempFile) {
    voiceTempFile.close();
    voiceTempFile = File();
  }
  if (mountSD()) {
    if (SD.exists(VOICE_TEMP_PATH)) {
      SD.remove(VOICE_TEMP_PATH);
    }
    unmountSD();
  }

  auto removeLast = [&]() {
    if (inputBuffer.length() > 0) {
      inputBuffer.remove(inputBuffer.length() - 1);
    }
  };

  while (true) {
    M5Cardputer.update();
    maybeUpdateLed();
    maybeUpdateBatteryIndicator();
    checkDisplaySleep();
    auto ks = M5Cardputer.Keyboard.keysState();
    bool goNow = M5Cardputer.BtnA.isPressed();
    bool backspaceNow = ks.del;
    std::vector<char> currHeld = ks.word;

    bool inputActive = hasKeyboardActivity(ks) || goNow;
    if (displaySleeping) {
      if (!inputActive) {
        prevHeld = currHeld;
        prevGoButton = goNow;
        prevBackspace = backspaceNow;
        delay(50);
        continue;
      }
      wakeDisplayIfNeeded();
      lcdShowPromptEditing(inputBuffer);
      renderStatusLineInner(true);
      markActivity();
    }

    // Voice recording handling when GO button is held
    if (goNow && !prevGoButton && !voiceRecording) {
      markActivity();
      if (voiceTranscribing) {
        lcdStatusLine("Voice busy... finishing prior clip");
        prevGoButton = goNow;
        prevBackspace = backspaceNow;
        prevHeld = currHeld;
        delay(30);
        continue;
      }
      voiceSampleCount = 0;
      voiceRecordedSamples = 0;
      voiceFileReady = false;
      voiceWriteError = false;
      voiceStartMs = millis();
      lcdStatusLine("Voice: recording... release GO");
      Serial.println("Voice recording started");
      if (M5Cardputer.Speaker.isEnabled()) {
        M5Cardputer.Speaker.end();
      }
      if (voiceTempFile) {
        voiceTempFile.close();
        voiceTempFile = File();
      }
      if (!mountSD()) {
        lcdStatusLine("SD mount fail.");
      } else if (!ensureDirectory(VOICE_TEMP_DIR)) {
        lcdStatusLine("Voice dir fail.");
        unmountSD();
      } else {
        SD.remove(VOICE_TEMP_PATH);
        voiceTempFile = SD.open(VOICE_TEMP_PATH, FILE_WRITE);
        if (!voiceTempFile) {
          lcdStatusLine("Voice file open failed.");
          unmountSD();
        } else if (!voiceTempFile.seek(0)) {
          lcdStatusLine("Voice file prep failed.");
          voiceTempFile.close();
          voiceTempFile = File();
          unmountSD();
        } else if (!M5Cardputer.Mic.begin()) {
          lcdStatusLine("Mic init failed.");
          voiceTempFile.close();
          voiceTempFile = File();
          unmountSD();
        } else {
          voiceRecording = true;
          ledEnterBusy();
        }
      }
    }

    if (voiceRecording) {
      static int16_t chunk[VOICE_CHUNK_SAMPLES];
      if (M5Cardputer.Mic.record(chunk, VOICE_CHUNK_SAMPLES, VOICE_SAMPLE_RATE)) {
        markActivity();
        size_t remaining = (voiceSampleCount < VOICE_MAX_SAMPLES)
                         ? (VOICE_MAX_SAMPLES - voiceSampleCount)
                         : 0;
        if (remaining > 0 && voiceTempFile) {
          size_t toCopy = remaining < VOICE_CHUNK_SAMPLES ? remaining : VOICE_CHUNK_SAMPLES;
          if (!appendSamplesToVoiceFile(chunk, toCopy)) {
            voiceWriteError = true;
            lcdStatusLine("Voice write failed.");
          } else {
            voiceSampleCount += toCopy;
            voiceRecordedSamples += toCopy;
          }
        }
        float seconds = voiceSampleCount / (float)VOICE_SAMPLE_RATE;
        lcdStatusLine("Voice REC " + String(seconds, 1) + "s (release GO)");
      }

      bool maxSamplesReached = voiceSampleCount >= VOICE_MAX_SAMPLES;
      bool timeExceeded = (millis() - voiceStartMs) >= VOICE_MAX_DURATION_MS || maxSamplesReached || voiceWriteError;
      if (!goNow || timeExceeded) {
        markActivity();
        if (voiceWriteError) {
          lcdStatusLine("Voice write failed.");
        } else if (voiceSampleCount == 0) {
          lcdStatusLine("Voice too short. Hold GO longer.");
        } else if (timeExceeded) {
          float limitSeconds = voiceSampleCount / (float)VOICE_SAMPLE_RATE;
          lcdStatusLine("Voice maxed (" + String(limitSeconds, 1) + "s).");
        } else {
          lcdStatusLine("Voice: transcribing...");
        }

        while (M5Cardputer.Mic.isRecording()) {
          maybeUpdateLed();
          delay(5);
        }
        M5Cardputer.Mic.end();
        if (voiceTempFile) {
          voiceTempFile.flush();
          voiceTempFile.close();
          voiceTempFile = File();
        }

        bool voiceFilePresent = sdMounted && SD.exists(VOICE_TEMP_PATH);
        voiceFileReady = (!voiceWriteError && voiceRecordedSamples > 0 && voiceFilePresent);
        if (voiceFileReady) {
          voiceTranscribing = true;
          String transcript = transcribeVoiceFile(voiceRecordedSamples);
          voiceTranscribing = false;
          markActivity();
          if (transcript.length() > 0) {
            if (inputBuffer.length() > 0 && inputBuffer.charAt(inputBuffer.length() - 1) != ' ') {
              inputBuffer += ' ';
            }
            inputBuffer += transcript;
            lcdShowPromptEditing(inputBuffer);
            lcdStatusLine("Voice added. ENTER=send");
          } else {
            lcdStatusLine("Voice failed. Try GO again.");
          }
        }

        voiceRecording = false;
        if (sdMounted && SD.exists(VOICE_TEMP_PATH)) {
          SD.remove(VOICE_TEMP_PATH);
        }
        voiceTempFile = File();
        voiceFileReady = false;
        voiceRecordedSamples = 0;
        voiceTranscribing = false;
        unmountSD();
        ledExitBusy();
      }

      prevGoButton = goNow;
      prevBackspace = backspaceNow;
      prevHeld = currHeld;
      delay(10);
      continue;
    }

    prevGoButton = goNow;

    // 1. dedicated delete/backspace flag if present
    //    you said ks.del works on your firmware
    if (backspaceNow && !prevBackspace) {
      markActivity();
      removeLast();
    }
    prevBackspace = backspaceNow;

    // 2. handle new keypresses in ks.word
    for (char c_now : currHeld) {
      bool alreadyHeld = false;
      for (char c_prev : prevHeld) {
        if (c_prev == c_now) {
          alreadyHeld = true;
          break;
        }
      }

      if (!alreadyHeld) {
        uint8_t code = (uint8_t)c_now;

        if (code == 0x7F || code == 0x2A) {
          // backspace/delete from keycode
          markActivity();
          removeLast();
        }
        else if (code == '\t') {
          // tab => 4 spaces
          markActivity();
          inputBuffer += "    ";
        }
        else if (code == '\n') {
          // newline in ks.word means Enter
          markActivity();
          String finalPrompt = inputBuffer;
          finalPrompt.trim();
          return finalPrompt;
        }
        else if (code >= 0x20 && code <= 0x7E) {
          // printable ASCII
          markActivity();
          inputBuffer += (char)code;
        }
        else {
          // debug unknown stuff like function-key combos
          Serial.print("Unmapped key code: 0x");
          Serial.println(code, HEX);
        }
      }
    }

    // 3. ENTER via ks.enter flag
    if (ks.enter) {
      markActivity();
      String finalPrompt = inputBuffer;
      finalPrompt.trim();
      return finalPrompt;
    }

    // 4. redraw prompt view
    lcdShowPromptEditing(inputBuffer);

    // 5. update edge-detect state
    prevHeld = currHeld;
    delay(30);
  }
}

// ------------------------------------------------------------
// setup() / loop()
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  // init Cardputer hardware
  M5Cardputer.begin();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextWrap(false);
  M5Cardputer.Display.setTextSize(CONTENT_TEXT_SIZE);
  initializeLed();

  lcdClearAll();
  lcdHeader("Cardputer AI");
  lcdStatusLine("Booting...");
  displaySleeping = false;
  lastActivityMs = millis();
  lastBatteryUpdateMs = millis();

  // init USB HID keyboard
  USB.begin();
  HidKeyboard.begin();

  // load config from SD
  if (!loadConfigFromSD()) {
    lcdStatusLine("Config load FAIL");
    Serial.println("No valid config on SD!");
    delay(2000);
  }
  if (mountSD()) {
    ensureDirectory(VOICE_TEMP_DIR);
    ensureDirectory(TRANSCRIPT_DIR);
    unmountSD();
  }

  // connect WiFi + HTTPS
  connectWiFi();
  initHTTPS();

  // seed system message so model knows how to behave
  resetConversationState();

  lcdStatusLine("Ready. Type prompt.");
  markActivity();
}

void loop() {
  // 1. Get the user's new prompt from keyboard
  String userMsg = readPromptFromKeyboard();
  if (userMsg.isEmpty()) {
    lcdStatusLine("Empty. Type again.");
    markActivity();
    return;
  }
  if (userMsg.equalsIgnoreCase("/context")) {
    String savedPath;
    bool saved = saveContextTranscript(savedPath);
    if (saved) {
      resetConversationState();
      lcdShowPromptEditing("");
      String basename = savedPath;
      int slash = basename.lastIndexOf('/');
      if (slash >= 0 && slash < (int)basename.length() - 1) {
        basename = basename.substring(slash + 1);
      }
      lcdStatusLine("Context saved: " + basename);
    } else {
      lcdStatusLine("Context save failed.");
    }
    markActivity();
    delay(900);
    return;
  }
  if (userMsg.equalsIgnoreCase("/wifi")) {
    bool result = wifiInteractiveSetup();
    markActivity();
    delay(600);
    return;
  }

  // 2. Add the user message to the rolling conversation
  addMessageToHistory("user", userMsg);
  lastUserPrompt = userMsg;

  // Show frozen prompt while querying
  lcdShowPromptEditing(userMsg);
  lcdStatusLine("Asking model...");
  markActivity();

  // 3. Send entire conversation to OpenAI
  String raw = callOpenAI();
  if (raw.isEmpty()) {
    lcdStatusLine("Request failed.");
    markActivity();
    return;
  }

  // 4. Parse assistant reply string
  String reply = parseAssistantReply(raw);
  reply = normalizeQuotes(reply);
  // 5. Save reply to chat history + remember it for GO typing
  addMessageToHistory("assistant", reply);
  lastAssistantReply = reply;

  // 6. Prepare wrapped lines for scrolling UI
  prepareReplyLinesFromText(reply);

  // 7. Show reply, enter interactive scroll/GO mode
  lcdShowAssistantReplyWindow();
  viewAssistantReplyInteractive();

  // 8. Ready for next turn
  lcdStatusLine("Done. ENTER new prompt.");
  markActivity();
}
String marqueeTextForStatus() {
  return statusBaseLine;
}

void renderStatusLineInner(bool force) {
  if (displaySleeping) return;
  unsigned long now = millis();
  if (!force && (now - lastBatteryUpdateMs) < 100) {
    return;
  }

  String text = marqueeTextForStatus();
  M5Cardputer.Display.setTextSize(STATUS_TEXT_SIZE);
  int textWidth = M5Cardputer.Display.textWidth(text.c_str());
  int availableWidth = SCREEN_WIDTH - CONTENT_MARGIN_X * 2;
  if (textWidth <= availableWidth) {
    statusScrollOffset = 0;
  } else {
    const int scrollSpeed = 12;
    if (!force && (now - lastStatusScrollUpdateMs) < 120) {
      updateHeaderBattery();
      return;
    }
    lastStatusScrollUpdateMs = now;
    statusScrollOffset += scrollSpeed;
    int maxOffset = textWidth + CONTENT_MARGIN_X;
    if (statusScrollOffset > maxOffset) {
      statusScrollOffset = 0;
    }
  }

  M5Cardputer.Display.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, STATUS_HEIGHT, BLACK);
  M5Cardputer.Display.setCursor(CONTENT_MARGIN_X - statusScrollOffset, HEADER_HEIGHT + CONTENT_MARGIN_Y);
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.print(text);

  if (textWidth > availableWidth) {
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X - statusScrollOffset + textWidth + 16, HEADER_HEIGHT + CONTENT_MARGIN_Y);
    M5Cardputer.Display.print(text);
  }

  lastBatteryUpdateMs = now;
  updateHeaderBattery();
}

void renderStatusLine(bool force) {
  renderStatusLineInner(force);
}
