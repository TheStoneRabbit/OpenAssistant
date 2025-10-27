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
#include <SD.h>
#include <SPI.h>

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
const char* MODEL_NAME      = "gpt-4o-mini";
const char* TRANSCRIBE_MODEL = "gpt-4o-mini-transcribe";

// Voice recording configuration
const uint32_t VOICE_SAMPLE_RATE     = 16000;
const size_t   VOICE_CHUNK_SAMPLES   = 256;
const uint32_t VOICE_MAX_DURATION_MS = 6000;
const size_t   VOICE_MAX_SAMPLES     = (VOICE_MAX_DURATION_MS / 1000) * VOICE_SAMPLE_RATE;
const char*    VOICE_TEMP_PATH       = "/voice_tmp.raw";

// ---------- GLOBALS ----------

WiFiClientSecure httpsClient;
String inputBuffer = "";

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

// Voice recording state (SD-backed)
File voiceTempFile;
size_t voiceRecordedSamples = 0;
bool voiceFileReady = false;

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
  }
  audioFile.close();
  httpsClient.print(closing);

  String statusLine = httpsClient.readStringUntil('\n');
  int statusCode = 0;
  int spaceIdx = statusLine.indexOf(' ');
  if (spaceIdx > 0) {
    int spaceIdx2 = statusLine.indexOf(' ', spaceIdx + 1);
    if (spaceIdx2 > spaceIdx) {
      statusCode = statusLine.substring(spaceIdx + 1, spaceIdx2).toInt();
    }
  }

  while (httpsClient.connected()) {
    String headerLine = httpsClient.readStringUntil('\n');
    if (headerLine == "\r") break;
  }

  String response;
  while (httpsClient.available()) {
    response += (char)httpsClient.read();
  }
  httpsClient.stop();

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
  return requestTranscriptionFromSD(sampleCount);
}

// ------------------------------------------------------------
// Load config from SD card (/chat_config.txt)
// ------------------------------------------------------------
bool loadConfigFromSD() {
  if (!SD.begin()) {
    Serial.println("SD init FAILED");
    return false;
  }

  File f = SD.open("/chat_config.txt", "r");
  if (!f) {
    Serial.println("chat_config.txt MISSING");
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

void lcdHeader(const String& msg) {
  // Top bar (0..14 px tall)
  M5Cardputer.Display.fillRect(0, 0, 320, 14, BLUE);
  M5Cardputer.Display.setCursor(2, 2);
  M5Cardputer.Display.setTextColor(WHITE, BLUE);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.print(msg);
}

void lcdStatusLine(const String& msg) {
  // Status bar under header at y=16
  M5Cardputer.Display.fillRect(0, 16, 320, 12, BLACK);
  M5Cardputer.Display.setCursor(0, 16);
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.print(msg);
}

void lcdShowPromptEditing(const String& current) {
  // Draw the "You:" area at y=32
  int startY = 32;
  int h = 24;
  M5Cardputer.Display.fillRect(0, startY, 320, h, BLACK);

  M5Cardputer.Display.setCursor(0, startY);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.print("You: ");

  // Show up to ~120 chars, then "..."
  String clipped = current;
  if (clipped.length() > 120) {
    clipped = clipped.substring(0, 120) + "...";
  }

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.println(clipped);

  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.print("_");
}

// Convert the assistant's full reply string into wrapped lines
// and reset scrollOffset.
void prepareReplyLinesFromText(const String& replyText) {
  replyLines.clear();
  scrollOffset = 0;

  const int wrapWidth = 28; // ~28 chars wide for size=1 font
  int len = replyText.length();

  for (int i = 0; i < len; i += wrapWidth) {
    int endIdx = i + wrapWidth;
    if (endIdx > len) endIdx = len;
    replyLines.push_back(replyText.substring(i, endIdx));
  }
}

// Render the assistant reply window starting at scrollOffset
void lcdShowAssistantReplyWindow() {
  int startY = 64;
  int h = 240 - startY;

  // Clear area
  M5Cardputer.Display.fillRect(0, startY, 320, h, BLACK);

  // "AI:" label
  M5Cardputer.Display.setCursor(0, startY);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.println("AI:");

  // Body lines
  M5Cardputer.Display.setTextColor(WHITE, BLACK);

  const int maxLinesOnScreen = 10; // lines that can fit below "AI:"
  for (int i = 0; i < maxLinesOnScreen; i++) {
    int idx = scrollOffset + i;
    if (idx < 0 || idx >= (int)replyLines.size()) break;
    M5Cardputer.Display.println(replyLines[idx]);
  }

  // footer / hint
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.println("[; up / . down | GO send | ENTER exit]");
}

// ------------------------------------------------------------
// WiFi / HTTPS setup
// ------------------------------------------------------------

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());

  lcdStatusLine("WiFi: connecting...");
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  Serial.println("WiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  lcdStatusLine("WiFi OK: " + WiFi.localIP().toString());
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
  String body = buildRequestBody();

  lcdStatusLine("Querying OpenAI...");
  Serial.println("Connecting to OpenAI...");
  Serial.println("---- Request Body ----");
  Serial.println(body);
  Serial.println("----------------------");

  // Important: fresh connection each request
  httpsClient.stop();
  httpsClient.setInsecure(); // still insecure in dev, no root CA
  httpsClient.setTimeout(15000);

  if (!httpsClient.connect(OPENAI_HOST, OPENAI_PORT)) {
    Serial.println("HTTPS connect FAIL");
    lcdStatusLine("OpenAI connect FAIL");
    return "";
  }

  // Build HTTPS request by hand
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

  // Skip response headers
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") break;
  }

  // Read response body
  String response = "";
  while (httpsClient.available()) {
    response += (char)httpsClient.read();
  }

  httpsClient.stop();

  Serial.println("---- Raw Response ----");
  Serial.println(response);
  Serial.println("----------------------");

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
  }
}

// ------------------------------------------------------------
// Scroll / View Mode
// - ';' scrolls UP
// - '.' scrolls DOWN
// - GO button types lastAssistantReply over USB HID
// - ENTER exits back to prompt mode
// ------------------------------------------------------------
void viewAssistantReplyInteractive() {
  lcdShowAssistantReplyWindow();
  lcdStatusLine(";/.:scroll GO:type ENTER:exit");

  bool prevEnter = false;
  bool prevGo    = false;

  // for edge-detect on ';' and '.'
  std::vector<char> prevHeld;

  while (true) {
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();

    // -- EXIT on ENTER --
    if (ks.enter && !prevEnter) {
      return;
    }
    prevEnter = ks.enter;

    // -- GO button to type the last assistant reply --
    bool goNow = M5Cardputer.BtnA.isPressed();
    if (goNow && !prevGo) {
      // Send keystrokes over USB
      lcdStatusLine("Typing reply over HID...");
      typeReplyOverUSB(lastAssistantReply);
      lcdStatusLine("Done. ;/.:scroll ENTER:exit");
    }
    prevGo = goNow;

    // -- SCROLL with ';' (up) / '.' (down) --
    std::vector<char> currHeld = ks.word;

    for (char c_now : currHeld) {
      bool alreadyHeld = false;
      for (char c_prev : prevHeld) {
        if (c_prev == c_now) {
          alreadyHeld = true;
          break;
        }
      }

      if (!alreadyHeld) {
        // fresh press in scroll mode
        if (c_now == ';') {
          // scroll UP
          if (scrollOffset > 0) {
            scrollOffset--;
            lcdShowAssistantReplyWindow();
          }
        } else if (c_now == '.') {
          // scroll DOWN
          if (scrollOffset < (int)replyLines.size() - 1) {
            scrollOffset++;
            lcdShowAssistantReplyWindow();
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
  if (SD.exists(VOICE_TEMP_PATH)) {
    SD.remove(VOICE_TEMP_PATH);
  }

  auto removeLast = [&]() {
    if (inputBuffer.length() > 0) {
      inputBuffer.remove(inputBuffer.length() - 1);
    }
  };

  while (true) {
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();
    bool goNow = M5Cardputer.BtnA.isPressed();
    bool backspaceNow = ks.del;
    std::vector<char> currHeld = ks.word;

    // Voice recording handling when GO button is held
    if (goNow && !prevGoButton && !voiceRecording) {
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
      SD.remove(VOICE_TEMP_PATH);
      voiceTempFile = SD.open(VOICE_TEMP_PATH, FILE_WRITE);
      if (!voiceTempFile) {
        lcdStatusLine("Voice file open failed.");
      } else if (!voiceTempFile.seek(0)) {
        lcdStatusLine("Voice file prep failed.");
        voiceTempFile.close();
        voiceTempFile = File();
      } else if (!M5Cardputer.Mic.begin()) {
        lcdStatusLine("Mic init failed.");
      } else {
        voiceRecording = true;
      }
    }

    if (voiceRecording) {
      static int16_t chunk[VOICE_CHUNK_SAMPLES];
      if (M5Cardputer.Mic.record(chunk, VOICE_CHUNK_SAMPLES, VOICE_SAMPLE_RATE)) {
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
          delay(5);
        }
        M5Cardputer.Mic.end();
        if (voiceTempFile) {
          voiceTempFile.flush();
          voiceTempFile.close();
          voiceTempFile = File();
        }

        voiceFileReady = (!voiceWriteError && voiceRecordedSamples > 0 && SD.exists(VOICE_TEMP_PATH));
        if (voiceFileReady) {
          String transcript = transcribeVoiceFile(voiceRecordedSamples);
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
        if (SD.exists(VOICE_TEMP_PATH)) {
          SD.remove(VOICE_TEMP_PATH);
        }
        voiceTempFile = File();
        voiceFileReady = false;
        voiceRecordedSamples = 0;
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
          removeLast();
        }
        else if (code == '\t') {
          // tab => 4 spaces
          inputBuffer += "    ";
        }
        else if (code == '\n') {
          // newline in ks.word means Enter
          String finalPrompt = inputBuffer;
          finalPrompt.trim();
          return finalPrompt;
        }
        else if (code >= 0x20 && code <= 0x7E) {
          // printable ASCII
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
  M5Cardputer.Display.setTextSize(1);

  lcdClearAll();
  lcdHeader("Cardputer AI Assistant");
  lcdStatusLine("Booting...");

  // init USB HID keyboard
  USB.begin();
  HidKeyboard.begin();

  // load config from SD
  if (!loadConfigFromSD()) {
    lcdStatusLine("Config load FAIL");
    Serial.println("No valid config on SD!");
    delay(2000);
  }

  // connect WiFi + HTTPS
  connectWiFi();
  initHTTPS();

  // seed system message so model knows how to behave
  addMessageToHistory(
    "system",
    "You are a helpful assistant on a tiny handheld called the Cardputer. "
    "Keep answers short, clear, and friendly so they fit on the 320x240 screen."
  );

  lcdStatusLine("Ready. Type prompt.");
}

void loop() {
  // 1. Get the user's new prompt from keyboard
  String userMsg = readPromptFromKeyboard();
  if (userMsg.isEmpty()) {
    lcdStatusLine("Empty. Type again.");
    return;
  }

  // 2. Add the user message to the rolling conversation
  addMessageToHistory("user", userMsg);

  // Show frozen prompt while querying
  lcdShowPromptEditing(userMsg);
  lcdStatusLine("Asking model...");

  // 3. Send entire conversation to OpenAI
  String raw = callOpenAI();
  if (raw.isEmpty()) {
    lcdStatusLine("Request failed.");
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
}
