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
#include <queue>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <ctype.h>
#include <algorithm>
#include <SD.h>
#include <SPI.h>
#include "esp32-hal-rgb-led.h"
#include "esp_heap_caps.h"

// ---- USB HID keyboard (TinyUSB style on ESP32-S3) ----
#include "USB.h"
#include "USBHIDKeyboard.h"

USBHIDKeyboard HidKeyboard;  // we'll use this to "type" last reply

// ---------- GLOBAL CONFIG (loaded from SD) ----------

String WIFI_SSID = "";
String WIFI_PASS = "";
String OPENAI_API_KEY = "";

const char* OPENAI_HOST = "api.openai.com";
const int OPENAI_PORT = 443;
const char* OPENAI_ENDPOINT = "/v1/responses";
const char* OPENAI_TRANSCRIBE_ENDPOINT = "/v1/audio/transcriptions";
const char* OPENAI_SPEECH_ENDPOINT = "/v1/audio/speech";
const char* MODEL_NAME = "gpt-5-mini";
const char* TRANSCRIBE_MODEL = "gpt-4o-mini-transcribe";
const char* TTS_MODEL_NAME = "gpt-4o-mini-tts";
const char* DEFAULT_TTS_VOICE = "verse";
const char* SYSTEM_PROMPT_TEXT =
  "You are a helpful assistant on a tiny handheld called the Cardputer. "
  "Keep answers short, clear, and friendly so they fit on the 320x240 screen.";

// Voice recording configuration
const uint32_t VOICE_SAMPLE_RATE = 16000;
const size_t VOICE_CHUNK_SAMPLES = 256;
const uint32_t VOICE_MAX_DURATION_MS = 6000;
const size_t VOICE_MAX_SAMPLES = (VOICE_MAX_DURATION_MS / 1000) * VOICE_SAMPLE_RATE;
const char* VOICE_TEMP_DIR = "/oa_tmp";
const char* VOICE_TEMP_PATH = "/oa_tmp/voice.raw";
const char* TRANSCRIPT_DIR = "/transcripts";
const char* TTS_CACHE_DIR = "/tts_cache";
const char* TTS_CACHE_PATH = "/tts_cache/last.pcm";
const char* RESPONSE_TEMP_PATH = "/oa_tmp/response.json";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

const int SCREEN_WIDTH = 240;
const int SCREEN_HEIGHT = 135;
const int HEADER_TEXT_SIZE = 2;
const int STATUS_TEXT_SIZE = 2;
const int CONTENT_TEXT_SIZE = 2;
const int PROMPT_TEXT_SIZE = CONTENT_TEXT_SIZE;
const int REPLY_TEXT_SIZE = CONTENT_TEXT_SIZE;
const int HEADER_HEIGHT = HEADER_TEXT_SIZE * 12 + 8;
const int STATUS_HEIGHT = STATUS_TEXT_SIZE * 12 + 6;
const int CONTENT_MARGIN_X = 8;
const int CONTENT_MARGIN_Y = 4;
const int CONTENT_LINE_EXTRA = 6;
const int PROMPT_AREA_Y = HEADER_HEIGHT + STATUS_HEIGHT + CONTENT_MARGIN_Y;
const int PROMPT_AREA_HEIGHT = SCREEN_HEIGHT > PROMPT_AREA_Y ? SCREEN_HEIGHT - PROMPT_AREA_Y : 0;
const int REPLY_AREA_Y = PROMPT_AREA_Y;

const uint32_t INACTIVITY_TIMEOUT_MS = 60000;
const uint32_t BATTERY_UPDATE_INTERVAL_MS = 60000;
const uint32_t TTS_DOWNLOAD_TIMEOUT_MS = 30000;
const uint32_t TTS_DEFAULT_SAMPLE_RATE = 24000;

// ---------- GLOBALS ----------

WiFiClientSecure httpsClient;
String inputBuffer = "";
String lastUserPrompt = "";

// Conversation memory
struct ChatMessage {
  String role;  // "system", "user", "assistant"
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
String lastRenderedStatusText = "";
int lastRenderedStatusOffset = -9999;
String lastRenderedBatteryText = "";

// Voice recording state (SD-backed)
File voiceTempFile;
size_t voiceRecordedSamples = 0;
bool voiceFileReady = false;
bool voiceTranscribing = false;
bool displaySleeping = false;
unsigned long lastActivityMs = 0;
unsigned long lastBatteryDrawMs = 0;
bool sdMounted = false;
unsigned long lastStatusScrollUpdateMs = 0;
int statusScrollOffset = 0;

// LED state
int8_t boardLedPin = -1;
bool ledAvailable = false;
bool ledBlinkState = false;
unsigned long lastLedToggleMs = 0;
int ledBusyDepth = 0;

// TTS cache state
String ttsVoice = DEFAULT_TTS_VOICE;
String ttsCachedText = "";
String ttsCachedPath = "";
bool ttsAudioReady = false;
bool ttsBusy = false;
bool isMuted = false;
uint8_t ttsVolume = 255;
const int TTS_VOLUME_STEP = 16;
const uint8_t UI_IDLE_DELAY_MS = 45;
volatile bool ttsCancelRequested = false;
TaskHandle_t ttsTaskHandle = nullptr;
volatile bool ttsPrefetchPending = false;
volatile bool ttsPauseRequested = false;
volatile bool ttsPaused = false;
size_t ttsResumeOffset = 0;
String ttsResumePath = "";
volatile bool ttsPlaybackActive = false;
#if defined(ARDUINO_ARCH_ESP32)
volatile bool ttsStatusDirty = false;
String ttsPendingStatus = "";
portMUX_TYPE ttsStatusMux = portMUX_INITIALIZER_UNLOCKED;
#endif

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
  t.replace("’", "'");   // right single quote
  t.replace("‘", "'");   // left single quote
  t.replace("“", "\"");  // left double
  t.replace("”", "\"");  // right double
  return t;
}

static void appendSanitizedChar(String& out, char c, bool& lastWasSpace) {
  out += c;
  lastWasSpace = false;
}

String sanitizeForTts(const String& input) {
  String sanitized;
  sanitized.reserve(input.length());
  bool lastWasSpace = true;  // treat start as if a space was already written

  auto appendSpace = [&]() {
    if (!lastWasSpace && sanitized.length() > 0) {
      sanitized += ' ';
      lastWasSpace = true;
    }
  };

  auto appendChars = [&](const char* chars) {
    while (*chars) {
      appendSanitizedChar(sanitized, *chars, lastWasSpace);
      ++chars;
    }
  };

  auto handleFolded = [&](uint32_t codepoint) -> bool {
    switch (codepoint) {
      // Basic Latin letters with accents
      case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
      case 0x0100: case 0x0102: case 0x0104:
        appendSanitizedChar(sanitized, 'A', lastWasSpace);
        return true;
      case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
      case 0x0101: case 0x0103: case 0x0105:
        appendSanitizedChar(sanitized, 'a', lastWasSpace);
        return true;
      case 0x00C7: case 0x0106: case 0x0108: case 0x010A: case 0x010C:
        appendSanitizedChar(sanitized, 'C', lastWasSpace);
        return true;
      case 0x00E7: case 0x0107: case 0x0109: case 0x010B: case 0x010D:
        appendSanitizedChar(sanitized, 'c', lastWasSpace);
        return true;
      case 0x00D0: case 0x010E: case 0x0110:
        appendSanitizedChar(sanitized, 'D', lastWasSpace);
        return true;
      case 0x00F0: case 0x010F: case 0x0111:
        appendSanitizedChar(sanitized, 'd', lastWasSpace);
        return true;
      case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
      case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011A:
        appendSanitizedChar(sanitized, 'E', lastWasSpace);
        return true;
      case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
      case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011B:
        appendSanitizedChar(sanitized, 'e', lastWasSpace);
        return true;
      case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
      case 0x0128: case 0x012A: case 0x012C: case 0x012E: case 0x0130:
        appendSanitizedChar(sanitized, 'I', lastWasSpace);
        return true;
      case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
      case 0x0129: case 0x012B: case 0x012D: case 0x012F: case 0x0131:
        appendSanitizedChar(sanitized, 'i', lastWasSpace);
        return true;
      case 0x0141:
        appendSanitizedChar(sanitized, 'L', lastWasSpace);
        return true;
      case 0x0142:
        appendSanitizedChar(sanitized, 'l', lastWasSpace);
        return true;
      case 0x00D1: case 0x0143: case 0x0145: case 0x0147:
        appendSanitizedChar(sanitized, 'N', lastWasSpace);
        return true;
      case 0x00F1: case 0x0144: case 0x0146: case 0x0148: case 0x0149:
        appendSanitizedChar(sanitized, 'n', lastWasSpace);
        return true;
      case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
      case 0x014C: case 0x014E: case 0x0150:
        appendSanitizedChar(sanitized, 'O', lastWasSpace);
        return true;
      case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
      case 0x014D: case 0x014F: case 0x0151:
        appendSanitizedChar(sanitized, 'o', lastWasSpace);
        return true;
      case 0x0152:
        appendChars("OE");
        return true;
      case 0x0153:
        appendChars("oe");
        return true;
      case 0x0154: case 0x0156: case 0x0158:
        appendSanitizedChar(sanitized, 'R', lastWasSpace);
        return true;
      case 0x0155: case 0x0157: case 0x0159:
        appendSanitizedChar(sanitized, 'r', lastWasSpace);
        return true;
      case 0x015A: case 0x015C: case 0x015E: case 0x0160:
        appendSanitizedChar(sanitized, 'S', lastWasSpace);
        return true;
      case 0x015B: case 0x015D: case 0x015F: case 0x0161:
        appendSanitizedChar(sanitized, 's', lastWasSpace);
        return true;
      case 0x0162: case 0x0164:
        appendSanitizedChar(sanitized, 'T', lastWasSpace);
        return true;
      case 0x0163: case 0x0165:
        appendSanitizedChar(sanitized, 't', lastWasSpace);
        return true;
      case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
      case 0x0168: case 0x016A: case 0x016C: case 0x016E: case 0x0170: case 0x0172:
        appendSanitizedChar(sanitized, 'U', lastWasSpace);
        return true;
      case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
      case 0x0169: case 0x016B: case 0x016D: case 0x016F: case 0x0171: case 0x0173:
        appendSanitizedChar(sanitized, 'u', lastWasSpace);
        return true;
      case 0x00DD: case 0x0178: case 0x0176:
        appendSanitizedChar(sanitized, 'Y', lastWasSpace);
        return true;
      case 0x00FD: case 0x00FF: case 0x0177:
        appendSanitizedChar(sanitized, 'y', lastWasSpace);
        return true;
      case 0x0179: case 0x017B: case 0x017D:
        appendSanitizedChar(sanitized, 'Z', lastWasSpace);
        return true;
      case 0x017A: case 0x017C: case 0x017E:
        appendSanitizedChar(sanitized, 'z', lastWasSpace);
        return true;
      case 0x00DF:
        appendChars("ss");
        return true;
      case 0x011E:
        appendSanitizedChar(sanitized, 'G', lastWasSpace);
        return true;
      case 0x011F:
        appendSanitizedChar(sanitized, 'g', lastWasSpace);
        return true;
      case 0x0132:
        appendChars("IJ");
        return true;
      case 0x0133:
        appendChars("ij");
        return true;
      case 0x0134:
        appendSanitizedChar(sanitized, 'J', lastWasSpace);
        return true;
      case 0x0135:
        appendSanitizedChar(sanitized, 'j', lastWasSpace);
        return true;
      case 0x0136:
        appendSanitizedChar(sanitized, 'K', lastWasSpace);
        return true;
      case 0x0137: case 0x0138:
        appendSanitizedChar(sanitized, 'k', lastWasSpace);
        return true;
      case 0x0139: case 0x013B: case 0x013D:
        appendSanitizedChar(sanitized, 'L', lastWasSpace);
        return true;
      case 0x013A: case 0x013C: case 0x013E:
        appendSanitizedChar(sanitized, 'l', lastWasSpace);
        return true;
      default:
        return false;
    }
  };

  size_t len = input.length();
  size_t i = 0;
  while (i < len) {
    uint8_t b = static_cast<uint8_t>(input[i]);
    if (b < 0x80) {
      char c = static_cast<char>(b);
      if (c == '\r' || c == '\n' || c == '\t') {
        appendSpace();
      } else if (c == ' ') {
        appendSpace();
      } else if (c >= 0x20 && c <= 0x7E) {
        appendSanitizedChar(sanitized, c, lastWasSpace);
      } else {
        appendSpace();
      }
      ++i;
      continue;
    }

    uint32_t codepoint = 0;
    size_t extraBytes = 0;
    if ((b & 0xE0) == 0xC0 && (i + 1) < len) {
      codepoint = ((uint32_t)(b & 0x1F) << 6) |
                  (uint32_t)(static_cast<uint8_t>(input[i + 1]) & 0x3F);
      extraBytes = 1;
    } else if ((b & 0xF0) == 0xE0 && (i + 2) < len) {
      codepoint = ((uint32_t)(b & 0x0F) << 12) |
                  ((uint32_t)(static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
                  (uint32_t)(static_cast<uint8_t>(input[i + 2]) & 0x3F);
      extraBytes = 2;
    } else if ((b & 0xF8) == 0xF0 && (i + 3) < len) {
      codepoint = ((uint32_t)(b & 0x07) << 18) |
                  ((uint32_t)(static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
                  ((uint32_t)(static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
                  (uint32_t)(static_cast<uint8_t>(input[i + 3]) & 0x3F);
      extraBytes = 3;
    } else {
      appendSpace();
      ++i;
      continue;
    }

    i += extraBytes + 1;

    if (handleFolded(codepoint)) {
      continue;
    }

    if (codepoint == 0x00A0) {  // non-breaking space
      appendSpace();
      continue;
    }
    if (codepoint == 0x2013 || codepoint == 0x2014) {  // en/em dash
      appendSanitizedChar(sanitized, '-', lastWasSpace);
      continue;
    }
    if (codepoint == 0x2018 || codepoint == 0x2019 || codepoint == 0x02BC) {
      appendSanitizedChar(sanitized, '\'', lastWasSpace);
      continue;
    }
    if (codepoint == 0x201C || codepoint == 0x201D) {
      appendSanitizedChar(sanitized, '"', lastWasSpace);
      continue;
    }
    if (codepoint == 0x2026) {  // ellipsis
      appendChars("...");
      continue;
    }

    appendSpace();
  }

  sanitized.trim();
  if (sanitized.length() == 0) {
    return String("response unavailable");
  }
  return sanitized;
}

String sanitizeForHidTyping(const String& input) {
  String sanitized = sanitizeForTts(input);
  sanitized.replace('\r', ' ');
  return sanitized;
}

struct TtsJobContext {
  String sanitizedText;
  bool useCache;
  String cachedPath;
  bool playAfterDownload;
  size_t startOffset;
};

void ttsWorkerTask(void* param);
bool startTtsTask(const String& sanitizedText, bool useCache, bool playAfterDownload, const String& cachedPath, size_t startOffset = 0);
void startTtsPrefetch(const String& reply);
void postTtsStatus(const String& msg);
void pumpTtsStatus();
void requestTtsPause();
void resumeTtsIfPaused();
void speakLastAssistantReply();

// ------------------------------------------------------------
// Voice helper utilities
// ------------------------------------------------------------

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
    lcdStatusLine("Transcribe: file open failed.");
    return "";
  }

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

  const size_t wavHeaderSize = 44;
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
    lcdStatusLine("Transcribe: HTTPS connect FAIL");
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
  uint8_t header[wavHeaderSize];
  size_t sampleCountForHeader = pcmBytes / sizeof(int16_t);

  // Fill WAV header
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  writeLE32(header + 4, pcmBytes + 36); // File size - 8
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  writeLE32(header + 16, 16); // Subchunk1Size
  writeLE16(header + 20, 1);  // AudioFormat (PCM)
  writeLE16(header + 22, 1);  // NumChannels (Mono)
  writeLE32(header + 24, VOICE_SAMPLE_RATE); // SampleRate
  uint32_t byteRate = VOICE_SAMPLE_RATE * 2; // ByteRate (SampleRate * NumChannels * BitsPerSample/8)
  writeLE32(header + 28, byteRate);
  writeLE16(header + 32, 2);  // BlockAlign (NumChannels * BitsPerSample/8)
  writeLE16(header + 34, 16); // BitsPerSample
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  writeLE32(header + 40, pcmBytes); // Subchunk2Size (data size)

  httpsClient.write(header, wavHeaderSize);
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
      lcdStatusLine("Transcribe: read timeout");
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

  if (statusCode != 200) {
    lcdStatusLine("Transcribe HTTP error: " + String(statusCode));
    return "";
  }
  return response;
}

String transcribeVoiceFile(size_t sampleCount) {
  if (!sdMounted) {
    lcdStatusLine("Transcribe: SD not mounted.");
    return "";
  }
  return requestTranscriptionFromSD(sampleCount);
}

// ------------------------------------------------------------
// Text-to-Speech helpers (GPT voice via OpenAI API)
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

static bool readLineFromClient(WiFiClientSecure& client, String& line, unsigned long timeoutMs) {
  line = "";
  unsigned long start = millis();
  while (true) {
    while (client.available()) {
      char c = client.read();
      if (c == '\r') {
        start = millis();
        continue;
      }
      if (c == '\n') {
        return true;
      }
      line += c;
      start = millis();
    }
    if (millis() - start > timeoutMs) {
      return !line.isEmpty();
    }
    if (!client.connected()) {
      return !line.isEmpty();
    }
    delay(5);
  }
}

static bool readFixedBodyToFile(WiFiClientSecure& client, File& file, long contentLength) {
  const size_t BUF_SIZE = 1024;
  uint8_t buffer[BUF_SIZE];
  unsigned long lastData = millis();
  bool lengthKnown = contentLength >= 0;

  while (client.connected() || client.available()) {
    if (lengthKnown && contentLength <= 0) {
      return true;
    }
    if (!client.available()) {
      if (millis() - lastData > TTS_DOWNLOAD_TIMEOUT_MS) {
        return false;
      }
      delay(5);
      continue;
    }

    size_t toRead = lengthKnown
      ? (size_t)std::min<long>(contentLength, (long)BUF_SIZE)
      : BUF_SIZE;

    int readLen = client.read(buffer, toRead);
    if (readLen <= 0) {
      break;
    }
    file.write(buffer, readLen);
    markActivity();
    lastData = millis();
    if (lengthKnown) {
      contentLength -= readLen;
    }
  }
  return !lengthKnown || contentLength <= 0;
}

static bool readChunkedBodyToFile(WiFiClientSecure& client, File& file) {
  const size_t BUF_SIZE = 1024;
  uint8_t buffer[BUF_SIZE];

  while (true) {
    String sizeLine;
    if (!readLineFromClient(client, sizeLine, TTS_DOWNLOAD_TIMEOUT_MS)) {
      return false;
    }
    sizeLine.trim();
    if (sizeLine.length() == 0) {
      continue;
    }

    long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
    if (chunkSize <= 0) {
      // consume trailer (possibly empty line)
      String trailer;
      readLineFromClient(client, trailer, TTS_DOWNLOAD_TIMEOUT_MS);
      return true;
    }

    long remaining = chunkSize;
    unsigned long lastData = millis();
    while (remaining > 0) {
      if (!client.available()) {
        if (millis() - lastData > TTS_DOWNLOAD_TIMEOUT_MS) {
          return false;
        }
        if (!client.connected()) {
          return false;
        }
        delay(5);
        continue;
      }
      size_t toRead = (remaining < (long)BUF_SIZE) ? remaining : BUF_SIZE;
      int len = client.read(buffer, toRead);
      if (len <= 0) {
        return false;
      }
      file.write(buffer, len);
      markActivity();
      remaining -= len;
      lastData = millis();
    }

    // Discard CRLF after chunk
    for (int i = 0; i < 2; ++i) {
      unsigned long start = millis();
      while (!client.available()) {
        if (millis() - start > TTS_DOWNLOAD_TIMEOUT_MS) {
          return false;
        }
        if (!client.connected()) {
          return false;
        }
        delay(2);
      }
      client.read();
    }
  }
}

bool downloadTtsAudio(const String& text, String& outPath) {
  if (OPENAI_API_KEY.isEmpty()) {
    postTtsStatus("Voice key missing");
    return false;
  }
  if (text.isEmpty()) {
    postTtsStatus("No voice text");
    return false;
  }

  String sanitizedText = sanitizeForTts(text);
  if (sanitizedText != text) {
    Serial.println("TTS: sanitized input to remove unsupported characters.");
    Serial.println("  original: " + text);
    Serial.println("  sanitized: " + sanitizedText);
  }

  if (!mountSD()) {
    postTtsStatus("Voice storage error");
    return false;
  }
  if (!ensureDirectory(TTS_CACHE_DIR)) {
    postTtsStatus("Voice storage error");
    unmountSD();
    return false;
  }

  if (SD.exists(TTS_CACHE_PATH)) {
    SD.remove(TTS_CACHE_PATH);
  }
  File audioFile = SD.open(TTS_CACHE_PATH, FILE_WRITE);
  if (!audioFile) {
    postTtsStatus("Voice storage error");
    unmountSD();
    return false;
  }

  StaticJsonDocument<1024> doc;
  doc["model"] = TTS_MODEL_NAME;
  doc["voice"] = ttsVoice;
  doc["input"] = sanitizedText;
  doc["response_format"] = "pcm";

  String body;
  serializeJson(doc, body);

  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(15000);

  if (!httpsClient.connect(OPENAI_HOST, OPENAI_PORT)) {
    postTtsStatus("Voice connect fail");
    audioFile.close();
    unmountSD();
    return false;
  }

  String req;
  req.reserve(body.length() + 256);
  req += "POST ";
  req += OPENAI_SPEECH_ENDPOINT;
  req += " HTTP/1.1\r\n";
  req += "Host: ";
  req += OPENAI_HOST;
  req += "\r\n";
  req += "Authorization: Bearer ";
  req += OPENAI_API_KEY;
  req += "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Accept: audio/pcm\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: ";
  req += String(body.length());
  req += "\r\n\r\n";
  req += body;

  httpsClient.print(req);

  bool headersFinished = false;
  bool chunked = false;
  long contentLength = -1;
  String statusLine = "";
  String headerLine = "";
  unsigned long lastRead = millis();

  while (!headersFinished && (httpsClient.connected() || httpsClient.available())) {
    while (httpsClient.available() && !headersFinished) {
      char c = httpsClient.read();
      lastRead = millis();
      headerLine += c;
      if (c == '\n') {
        if (statusLine.length() == 0) {
          statusLine = headerLine;
        }
        String trimmed = headerLine;
        trimmed.trim();
        String lowered = trimmed;
        lowered.toLowerCase();
        if (lowered.startsWith("transfer-encoding:")) {
          if (lowered.indexOf("chunked") >= 0) {
            chunked = true;
            Serial.println("TTS: Chunked transfer encoding detected.");
          }
        } else if (lowered.startsWith("content-length:")) {
          contentLength = trimmed.substring(trimmed.indexOf(':') + 1).toInt();
          Serial.print("TTS: Content-Length: ");
          Serial.println(contentLength);
        }
        if (headerLine == "\r\n") {
          headersFinished = true;
        }
        headerLine = "";
      }
    }
    if (!headersFinished) {
      if (millis() - lastRead > TTS_DOWNLOAD_TIMEOUT_MS) {
        postTtsStatus("Voice timeout");
        httpsClient.stop();
        audioFile.close();
        unmountSD();
        return false;
      }
      delay(2);
    }
  }

  int statusCode = 0;
  int spaceIdx = statusLine.indexOf(' ');
  if (spaceIdx > 0) {
    int spaceIdx2 = statusLine.indexOf(' ', spaceIdx + 1);
    if (spaceIdx2 > spaceIdx) {
      statusCode = statusLine.substring(spaceIdx + 1, spaceIdx2).toInt();
    }
  }
  if (statusCode != 200) {
    postTtsStatus("Voice HTTP " + String(statusCode));
    String errorPreview;
    unsigned long errStart = millis();
    while (httpsClient.available() && errorPreview.length() < 256) {
      int c = httpsClient.read();
      if (c < 0) {
        if (millis() - errStart > TTS_DOWNLOAD_TIMEOUT_MS) {
          break;
        }
        continue;
      }
      errorPreview += static_cast<char>(c);
    }
    if (errorPreview.length() > 0) {
      Serial.println("TTS error preview: " + errorPreview);
      String display = errorPreview;
      display.replace('\r', ' ');
      display.replace('\n', ' ');
      display.trim();
      if (display.length() > 60) {
        display = display.substring(0, 57) + "...";
      }
      Serial.println("TTS HTTP detail: " + display);
    }
    httpsClient.stop();
    audioFile.close();
    if (SD.exists(TTS_CACHE_PATH)) {
      SD.remove(TTS_CACHE_PATH);
    }
    unmountSD();
    return false;
  }

  bool ok = chunked
          ? readChunkedBodyToFile(httpsClient, audioFile)
          : readFixedBodyToFile(httpsClient, audioFile, contentLength);

  httpsClient.stop();
  audioFile.flush();
  audioFile.close();

  if (!ok) {
    postTtsStatus("Voice read error");
    if (SD.exists(TTS_CACHE_PATH)) {
      SD.remove(TTS_CACHE_PATH);
    }
    unmountSD();
    return false;
  }

  outPath = String(TTS_CACHE_PATH);
  unmountSD();
  return true;
}

void adjustTtsVolume(int delta) {
  int newVol = (int)ttsVolume + delta;
  if (newVol < 0) newVol = 0;
  if (newVol > 255) newVol = 255;
  ttsVolume = (uint8_t)newVol;
  if (M5Cardputer.Speaker.isEnabled()) {
    M5Cardputer.Speaker.setVolume(ttsVolume);
  }
  postTtsStatus("Volume " + String(ttsVolume));
}

bool playTtsFromSD(const String& path, size_t startOffset, size_t& outResumeOffset, bool& outPaused) {
  outResumeOffset = startOffset;
  outPaused = false;
  if (!mountSD()) {
    postTtsStatus("Voice storage error");
    return false;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    postTtsStatus("Voice file error");
    unmountSD();
    return false;
  }
  size_t fileSize = f.size();
  if (fileSize == 0) {
    postTtsStatus("Voice file empty");
    f.close();
    unmountSD();
    return false;
  }

  if (startOffset > 0 && startOffset < fileSize) {
    if (!f.seek(startOffset)) {
      postTtsStatus("Voice seek error");
      f.close();
      unmountSD();
      return false;
    }
  } else if (startOffset >= fileSize) {
    postTtsStatus("Voice done");
    f.close();
    unmountSD();
    return true;
  }
  outResumeOffset = f.position();

  const size_t BUFFER_COUNT = 3;
  const size_t CHUNK_BYTES_OPTIONS[] = { 16384, 12288, 8192, 4096 };

  struct Buffer {
    int16_t* data = nullptr;
    bool usingHeapCaps = false;
    bool inUse = false;
  };
  Buffer buffers[BUFFER_COUNT];
  size_t chunkBytes = 0;

  auto freeBuffers = [&]() {
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
      if (buffers[i].data) {
        if (buffers[i].usingHeapCaps) {
          heap_caps_free(buffers[i].data);
        } else {
          free(buffers[i].data);
        }
        buffers[i].data = nullptr;
        buffers[i].usingHeapCaps = false;
        buffers[i].inUse = false;
      }
    }
  };

  for (size_t option : CHUNK_BYTES_OPTIONS) {
    bool allocated = true;
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
      int16_t* ptr = (int16_t*)heap_caps_malloc(option, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      bool usedHeap = true;
      if (!ptr) {
        ptr = (int16_t*)heap_caps_malloc(option, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
      if (!ptr) {
        ptr = (int16_t*)malloc(option);
        usedHeap = false;
      }
      if (!ptr) {
        allocated = false;
        freeBuffers();
        break;
      }
      buffers[i].data = ptr;
      buffers[i].usingHeapCaps = usedHeap;
      buffers[i].inUse = false;
    }
    if (allocated) {
      chunkBytes = option;
      break;
    }
  }

  if (chunkBytes == 0) {
    postTtsStatus("Voice memory error");
    f.close();
    unmountSD();
    return false;
  }

  std::queue<size_t> inFlightOrder;
  size_t recordedInFlight = 0;
  bool firstChunk = true;
  bool fileDone = false;
  bool cancelled = false;
  bool pausePending = false;
  bool paused = false;
  if (!M5Cardputer.Speaker.isEnabled()) {
    M5Cardputer.Speaker.begin();
  }
  M5Cardputer.Speaker.setVolume(ttsVolume);
  M5Cardputer.Speaker.stop();

  auto findFreeBuffer = [&]() -> int {
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
      if (!buffers[i].inUse) {
        return (int)i;
      }
    }
    return -1;
  };

  const int playbackChannel = 0;

  while (true) {
    if (!pausePending && ttsPauseRequested) {
      ttsPauseRequested = false;
      pausePending = true;
    }
    if (ttsCancelRequested) {
      cancelled = true;
      M5Cardputer.Speaker.stop();
      break;
    }

    size_t playingNow = M5Cardputer.Speaker.isPlaying(playbackChannel);
    while (recordedInFlight > playingNow && !inFlightOrder.empty()) {
      size_t freedIndex = inFlightOrder.front();
      inFlightOrder.pop();
      buffers[freedIndex].inUse = false;
      recordedInFlight--;
    }

    if (pausePending) {
      if (playingNow == 0) {
        M5Cardputer.Speaker.stop();
        paused = true;
        break;
      }
      delay(6);
      continue;
    }

    if (!fileDone && recordedInFlight < BUFFER_COUNT) {
      int bufferIndex = findFreeBuffer();
      if (bufferIndex >= 0) {
        size_t bytesRead = f.read((uint8_t*)buffers[bufferIndex].data, chunkBytes);
        if (bytesRead == 0) {
          fileDone = true;
        } else {
          if (bytesRead & 1) {
            bytesRead -= 1;
          }
          size_t samples = bytesRead / sizeof(int16_t);
          if (samples == 0) {
            fileDone = true;
          } else {
            if (ttsCancelRequested) {
              fileDone = true;
              break;
            }
            bool queued = M5Cardputer.Speaker.playRaw(
              buffers[bufferIndex].data,
              samples,
              TTS_DEFAULT_SAMPLE_RATE,
              false,
              1,
              playbackChannel,
              firstChunk
            );
            if (!queued) {
              postTtsStatus("Voice queue error");
              M5Cardputer.Speaker.stop();
              freeBuffers();
              f.close();
              unmountSD();
              return false;
            }
            firstChunk = false;
            buffers[bufferIndex].inUse = true;
            inFlightOrder.push(bufferIndex);
            recordedInFlight++;
            outResumeOffset = f.position();
            continue;
          }
        }
      }
      if (ttsCancelRequested) {
        cancelled = true;
        M5Cardputer.Speaker.stop();
        break;
      }
    }

    playingNow = M5Cardputer.Speaker.isPlaying(playbackChannel);
    if (fileDone && recordedInFlight == 0 && playingNow == 0) {
      break;
    }

    if (ttsCancelRequested) {
      cancelled = true;
      M5Cardputer.Speaker.stop();
      break;
    }

    delay(6);
  }

  size_t finalOffset = f.position();
  f.close();
  unmountSD();
  M5Cardputer.Speaker.stop();

  freeBuffers();
  outResumeOffset = finalOffset;
  outPaused = paused;
  if (paused) {
    return true;
  }
  return !cancelled;
}

void ttsWorkerTask(void* param) {
  std::unique_ptr<TtsJobContext> ctx(static_cast<TtsJobContext*>(param));
  String audioPath = ctx->cachedPath;
  ttsCancelRequested = false;

  ledEnterBusy();
  markActivity();

  bool shouldPlay = ctx->playAfterDownload;
  if (ctx->useCache) {
    postTtsStatus(shouldPlay ? "Voice playing" : "Voice ready");
  } else {
    postTtsStatus(shouldPlay ? "Voice loading" : "Caching voice");
  }

  bool readyToPlay = ctx->useCache;
  if (!ctx->useCache) {
    String downloadedPath;
    if (!downloadTtsAudio(ctx->sanitizedText, downloadedPath) || ttsCancelRequested) {
      readyToPlay = false;
      if (!ttsCancelRequested) {
        postTtsStatus("Voice error");
      }
    } else {
      ttsCachedText = ctx->sanitizedText;
      ttsCachedPath = downloadedPath;
      ttsAudioReady = true;
      audioPath = downloadedPath;
      readyToPlay = true;
    }
  }

  if (readyToPlay && !ttsCancelRequested) {
    if (shouldPlay) {
      size_t resumeOffset = ctx->startOffset;
      bool pausedPlayback = false;
      bool success = playTtsFromSD(audioPath, ctx->startOffset, resumeOffset, pausedPlayback);
      if (pausedPlayback) {
        ttsPaused = true;
        ttsResumeOffset = resumeOffset;
        ttsResumePath = audioPath;
        postTtsStatus("Voice paused");
        ttsPlaybackActive = false;
      } else {
        ttsPaused = false;
        ttsResumeOffset = 0;
        ttsResumePath = "";
        if (!success && !ttsCancelRequested) {
          postTtsStatus("Voice error");
        } else if (!ttsCancelRequested) {
          postTtsStatus("Voice done");
        }
      }
    } else {
      ttsPaused = false;
      ttsResumeOffset = 0;
      ttsResumePath = "";
      ttsPlaybackActive = false;
      postTtsStatus("Voice ready");
    }
  }

  if (ttsCancelRequested) {
    postTtsStatus("Voice cancelled");
    ttsPaused = false;
    ttsResumeOffset = 0;
    ttsResumePath = "";
  }

  ledExitBusy();
  ttsBusy = false;
  ttsCancelRequested = false;
  ttsPauseRequested = false;
  ttsPlaybackActive = false;
  ttsTaskHandle = nullptr;
  bool launchPending = ttsPrefetchPending;
  ctx.reset();
  if (launchPending) {
    ttsPrefetchPending = false;
    startTtsPrefetch(lastAssistantReply);
  }
  vTaskDelete(nullptr);
}

bool startTtsTask(const String& sanitizedText, bool useCache, bool playAfterDownload, const String& cachedPath, size_t startOffset) {
  if (ttsBusy) {
    return false;
  }

  if (!playAfterDownload && useCache) {
    // Already cached; nothing to do.
    ttsAudioReady = true;
    postTtsStatus("Voice ready");
    return true;
  }

  auto ctx = new TtsJobContext{
    sanitizedText,
    useCache,
    cachedPath,
    playAfterDownload,
    startOffset
  };

  ttsCancelRequested = false;
  ttsBusy = true;
  ttsPaused = false;
  ttsPauseRequested = false;
  if (!playAfterDownload) {
    ttsResumeOffset = 0;
    ttsResumePath = "";
  } else if (startOffset == 0) {
    ttsResumeOffset = 0;
    ttsResumePath = "";
  } else {
    ttsResumeOffset = startOffset;
    ttsResumePath = cachedPath;
  }
  ttsPlaybackActive = playAfterDownload;
  markActivity();

#if defined(ARDUINO_ARCH_ESP32)
  BaseType_t created = xTaskCreatePinnedToCore(
    ttsWorkerTask,
    "ttsWorker",
    8192,
    ctx,
    1,
    &ttsTaskHandle,
    1
  );
#else
  BaseType_t created = xTaskCreate(
    ttsWorkerTask,
    "ttsWorker",
    8192,
    ctx,
    1,
    &ttsTaskHandle
  );
#endif

  if (created != pdPASS) {
    ttsBusy = false;
    ttsTaskHandle = nullptr;
    delete ctx;
    postTtsStatus("Voice error");
    return false;
  }

  if (playAfterDownload) {
    if (startOffset > 0) {
      postTtsStatus("Voice resuming");
    } else {
      postTtsStatus(useCache ? "Voice playing" : "Voice loading");
    }
  } else {
    postTtsStatus("Caching voice");
  }

  return true;
}

void startTtsPrefetch(const String& reply) {
  if (reply.isEmpty()) return;
  if (isMuted) return;
  ttsPauseRequested = false;
  ttsPaused = false;
  if (ttsBusy) {
    ttsPrefetchPending = true;
    return;
  }

  String sanitized = sanitizeForTts(reply);
  bool useCache = ttsAudioReady && !ttsCachedPath.isEmpty() && sanitized == ttsCachedText;

  ttsPrefetchPending = false;
  startTtsTask(sanitized, useCache, false, ttsCachedPath, 0);
}

void postTtsStatus(const String& msg) {
#if defined(ARDUINO_ARCH_ESP32)
  if (ttsTaskHandle != nullptr && xTaskGetCurrentTaskHandle() == ttsTaskHandle) {
    portENTER_CRITICAL(&ttsStatusMux);
    ttsPendingStatus = msg;
    ttsStatusDirty = true;
    portEXIT_CRITICAL(&ttsStatusMux);
    return;
  }
#endif
  lcdStatusLine(msg);
}

void pumpTtsStatus() {
#if defined(ARDUINO_ARCH_ESP32)
  if (!ttsStatusDirty) return;
  String msg;
  portENTER_CRITICAL(&ttsStatusMux);
  if (ttsStatusDirty) {
    msg = ttsPendingStatus;
    ttsStatusDirty = false;
  }
  portEXIT_CRITICAL(&ttsStatusMux);
  if (msg.length() > 0) {
    lcdStatusLine(msg);
  }
#endif
}

void requestTtsPause() {
  if (!ttsBusy || !ttsPlaybackActive || ttsPauseRequested) {
    return;
  }
  if (!M5Cardputer.Speaker.isPlaying()) {
    return;
  }
  ttsPauseRequested = true;
  postTtsStatus("Voice pausing");
}

void resumeTtsIfPaused() {
  if (!ttsPaused) {
    return;
  }
  if (ttsBusy) {
    return;
  }
  if (!ttsAudioReady || ttsCachedPath.isEmpty()) {
    ttsPaused = false;
    ttsResumeOffset = 0;
    ttsResumePath = "";
    postTtsStatus("Voice not ready");
    return;
  }
  speakLastAssistantReply();
}

void clearTtsCache() {
  ttsAudioReady = false;
  ttsCachedText = "";
  ttsCachedPath = "";
  ttsPaused = false;
  ttsPauseRequested = false;
  ttsResumeOffset = 0;
  ttsResumePath = "";
  ttsPlaybackActive = false;
  if (mountSD()) {
    if (SD.exists(TTS_CACHE_PATH)) {
      SD.remove(TTS_CACHE_PATH);
    }
    unmountSD();
  }
}

void speakLastAssistantReply() {
  if (isMuted) {
    lcdStatusLine("Muted.");
    return;
  }
  if (ttsBusy) {
    postTtsStatus("Voice busy");
    return;
  }
  if (lastAssistantReply.isEmpty()) {
    postTtsStatus("No voice text");
    return;
  }

  String sanitizedReply = sanitizeForTts(lastAssistantReply);
  Serial.println("TTS request text: " + sanitizedReply);
  bool useCache = ttsAudioReady && !ttsCachedPath.isEmpty() && sanitizedReply == ttsCachedText;

  size_t startOffset = 0;
  if (ttsPaused && useCache && ttsResumeOffset > 0 && !ttsResumePath.isEmpty() && ttsResumePath == ttsCachedPath) {
    startOffset = ttsResumeOffset;
  }

  if (!startTtsTask(sanitizedReply, useCache, true, ttsCachedPath, startOffset)) {
    // startTtsTask already emitted status.
    return;
  }
}

// ------------------------------------------------------------
// Load config from SD card (/chat_config.txt)
// ------------------------------------------------------------
bool loadConfigFromSD() {
  if (!mountSD()) {
    lcdStatusLine("SD init FAILED");
    return false;
  }

  File f = SD.open("/chat_config.txt", "r");
  if (!f) {
    lcdStatusLine("chat_config.txt MISSING");
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
      lcdStatusLine("SSID: " + WIFI_SSID);
    } else if (key == "WIFI_PASS") {
      WIFI_PASS = value;
      lcdStatusLine("PASS: (hidden)");
    } else if (key == "OPENAI_KEY") {
      OPENAI_API_KEY = value;
      lcdStatusLine("KEY: (loaded)");
    } else if (key == "TTS_VOICE") {
      ttsVoice = value;
      lcdStatusLine("TTS voice: " + ttsVoice);
    }
  }

  f.close();
  unmountSD();

  if (WIFI_SSID.isEmpty() || WIFI_PASS.isEmpty() || OPENAI_API_KEY.isEmpty()) {
    lcdStatusLine("Config missing one or more fields!");
    return false;
  }

  lcdStatusLine("Config loaded OK.");
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
      chatHistory.begin() + (chatHistory.size() - 8));
  }
}

void lcdClearAll() {
  M5Cardputer.Display.fillScreen(BLACK);
  lastRenderedStatusText = "";
  lastRenderedStatusOffset = -9999;
  lastRenderedBatteryText = "";
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

void updateHeaderBattery(bool force) {
  if (displaySleeping) return;
  unsigned long now = millis();
  if (!force && (now - lastBatteryDrawMs) < BATTERY_UPDATE_INTERVAL_MS) {
    return;
  }
  String batt = batteryText();
  if (!lastRenderedBatteryText.isEmpty() && batt == lastRenderedBatteryText) {
    if (!force) {
      lastBatteryDrawMs = now;
      return;
    }
  }
  M5Cardputer.Display.setTextSize(HEADER_TEXT_SIZE);
  int padding = 4;
  int width = M5Cardputer.Display.textWidth(batt.c_str());
  int x = SCREEN_WIDTH - width - padding;
  if (x < 0) x = 0;
  M5Cardputer.Display.fillRect(x - 2, 0, width + padding + 2, HEADER_HEIGHT, BLUE);
  M5Cardputer.Display.setCursor(x, 6);
  M5Cardputer.Display.setTextColor(WHITE, BLUE);
  M5Cardputer.Display.print(batt);
  lastRenderedBatteryText = batt;
  lastBatteryDrawMs = now;
}

void renderStatusLineInner(bool force);
void renderStatusLine(bool force);
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void clearTtsCache();
bool downloadTtsAudio(const String& text, String& outPath);
bool playTtsFromSD(const String& path, size_t startOffset, size_t& outResumeOffset, bool& outPaused);

void maybeUpdateBatteryIndicator() {
  renderStatusLineInner(false);
}

void markActivity() {
  lastActivityMs = millis();
}

void enterDisplaySleep() {
  if (displaySleeping) {
    return;
  }
  displaySleeping = true;
  M5Cardputer.Display.sleep();
  if (ledAvailable) {
    setLedColor(0, 0, 0);
    ledBlinkState = false;
  }
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
    enterDisplaySleep();
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

template<typename KeyState>
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
    pumpTtsStatus();
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
    delay(UI_IDLE_DELAY_MS);
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
  if (!ttsVoice.isEmpty()) {
    f.print("TTS_VOICE = ");
    f.println(ttsVoice);
  }
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
  lastRenderedBatteryText = "";
  updateHeaderBattery(true);
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

  int totalLines = lines.size();
  int firstLine = 0;
  if (totalLines > maxLines) {
    firstLine = totalLines - maxLines;
  }
  int cursorLineIndex = totalLines > 0 ? totalLines - 1 : 0;
  int cursorRow = cursorLineIndex - firstLine;
  if (cursorRow < 0) cursorRow = 0;
  if (cursorRow >= maxLines) cursorRow = maxLines - 1;

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int row = 0; row < maxLines; ++row) {
    int lineIndex = firstLine + row;
    if (lineIndex >= totalLines) break;
    int y = startY + row * lineHeight;
    if (y >= startY + PROMPT_AREA_HEIGHT) break;
    int x = indent;
    String textLine = lines[lineIndex];
    M5Cardputer.Display.setCursor(x, y);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.print(textLine);
    if (row == cursorRow) {
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
  M5Cardputer.Display.setTextSize(REPLY_TEXT_SIZE);
  int labelWidth = M5Cardputer.Display.textWidth("AI:");
  int indent = CONTENT_MARGIN_X + labelWidth + (REPLY_TEXT_SIZE * 6);
  int availableWidth = SCREEN_WIDTH - indent - CONTENT_MARGIN_X;
  if (availableWidth < 40) availableWidth = 40;

  replyLines = wrapTextToLines(replyText, availableWidth, REPLY_TEXT_SIZE);
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
  M5Cardputer.Display.print("AI:");

  int indent = CONTENT_MARGIN_X + M5Cardputer.Display.textWidth("AI:") + (REPLY_TEXT_SIZE * 6);
  int lineHeight = lineHeightForSize(REPLY_TEXT_SIZE);
  int maxLinesOnScreen = visibleReplyLines();
  int totalVisible = maxLinesOnScreen + 1;
  int maxOffset = (int)replyLines.size() - totalVisible;
  if (maxOffset < 0) maxOffset = 0;
  if (scrollOffset > maxOffset) scrollOffset = maxOffset;

  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  if (!replyLines.empty()) {
    int firstIdx = scrollOffset;
    if (firstIdx >= 0 && firstIdx < (int)replyLines.size()) {
      M5Cardputer.Display.setCursor(indent, startY);
      M5Cardputer.Display.print(replyLines[firstIdx]);
    }
  }

  for (int i = 0; i < maxLinesOnScreen; ++i) {
    int idx = scrollOffset + 1 + i;
    if (idx < 0 || idx >= (int)replyLines.size()) break;
    int y = startY + (i + 1) * lineHeight;
    if (y >= SCREEN_HEIGHT) break;
    M5Cardputer.Display.setCursor(indent, y);
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
    return;
  }

  lcdStatusLine("WiFi: connecting...");

  bool ok = connectToWiFi(WIFI_SSID, WIFI_PASS, WIFI_CONNECT_TIMEOUT_MS);

  if (ok) {
    lcdStatusLine("WiFi OK: " + WiFi.localIP().toString());
  } else {
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
    delay(1200);
    WiFi.scanDelete();
    return false;
  }

  lcdClearAll();
  lcdHeader("WiFi Networks");
  int listStartY = HEADER_HEIGHT + CONTENT_MARGIN_Y;
  if (listStartY >= PROMPT_AREA_Y) {
    listStartY = HEADER_HEIGHT;
  }
  int lineHeight = lineHeightForSize(CONTENT_TEXT_SIZE);
  int listAreaHeight = PROMPT_AREA_Y - listStartY;
  if (listAreaHeight < lineHeight) {
    listAreaHeight = lineHeight;
  }
  int maxDisplay = listAreaHeight / lineHeight;
  if (maxDisplay < 1) {
    maxDisplay = 1;
  }
  if (maxDisplay > networkCount) {
    maxDisplay = networkCount;
  }
  M5Cardputer.Display.fillRect(0, listStartY, SCREEN_WIDTH, listAreaHeight, BLACK);
  M5Cardputer.Display.setTextSize(CONTENT_TEXT_SIZE);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  for (int i = 0; i < maxDisplay; ++i) {
    int y = listStartY + i * lineHeight;
    if (y + lineHeight > PROMPT_AREA_Y) break;
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
  int moreStartY = listStartY + maxDisplay * lineHeight;
  if (networkCount > maxDisplay && moreStartY + lineHeight <= PROMPT_AREA_Y) {
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X, moreStartY);
    M5Cardputer.Display.print("... (" + String(networkCount - maxDisplay) + " more)");
  }

  String selection = readSimpleTextLine("Type network name (blank=cancel)");
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
    delay(1200);
    return false;
  }

  WIFI_SSID = chosenSsid;
  WIFI_PASS = chosenPass;

  lcdStatusLine("WiFi connected!");
  delay(800);

  String saveAnswer = readSimpleTextLine("Save network to SD? (y/n)");
  saveAnswer.trim();
  saveAnswer.toLowerCase();
  if (saveAnswer.startsWith("y")) {
    bool saved = writeConfigToSD();
    if (saved) {
      lcdStatusLine("Config saved.");
    } else {
      lcdStatusLine("Failed to save config.");
    }
    delay(900);
  }

  lcdStatusLine("WiFi ready.");
  delay(600);
  return true;
}

void initHTTPS() {
  httpsClient.setInsecure();  // no cert validation (dev mode)
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
    m["role"] = chatHistory[i].role;
    m["content"] = chatHistory[i].content;
  }

  // Serialize into a String
  String body;
  serializeJson(doc, body);
  return body;
}

// ------------------------------------------------------------
// Send POST to OpenAI and stream the JSON reply to SD storage
// ------------------------------------------------------------
bool callOpenAI(String& outResponsePath) {
  ledEnterBusy();
  lcdStatusLine("Querying OpenAI...");
  markActivity();

  outResponsePath = "";
  String body = buildRequestBody();

  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(15000);

  bool success = false;
  bool sdReady = false;
  File responseFile;
  String statusLine = "";

  do {
    if (!httpsClient.connect(OPENAI_HOST, OPENAI_PORT)) {
      lcdStatusLine("OpenAI connect FAIL");
      break;
    }

    if (!mountSD()) {
      lcdStatusLine("SD mount fail");
      break;
    }
    sdReady = true;
    if (!ensureDirectory(VOICE_TEMP_DIR)) {
      lcdStatusLine("Resp dir fail");
      break;
    }
    if (SD.exists(RESPONSE_TEMP_PATH)) {
      SD.remove(RESPONSE_TEMP_PATH);
    }
    responseFile = SD.open(RESPONSE_TEMP_PATH, FILE_WRITE);
    if (!responseFile) {
      lcdStatusLine("Resp file fail");
      break;
    }

    httpsClient.print(String("POST ") + OPENAI_ENDPOINT + " HTTP/1.1\r\n");
    httpsClient.print(String("Host: ") + OPENAI_HOST + "\r\n");
    httpsClient.print(String("Authorization: Bearer ") + OPENAI_API_KEY + "\r\n");
    httpsClient.print("Content-Type: application/json\r\n");
    httpsClient.print("Connection: close\r\n");
    httpsClient.print(String("Content-Length: ") + String(body.length()) + "\r\n\r\n");
    httpsClient.print(body);

    String headerAccum = "";
    bool headersFinished = false;
    bool streamOk = true;
    unsigned long lastRead = millis();

    while ((httpsClient.connected() || httpsClient.available()) && streamOk) {
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
          responseFile.write(static_cast<uint8_t>(c));
        }
      }

      if (headersFinished && !httpsClient.connected() && !httpsClient.available()) {
        break;
      }

      maybeUpdateLed();
      if (millis() - lastRead > 30000) {
        lcdStatusLine("OpenAI read timeout");
        streamOk = false;
        break;
      }
      delay(10);
    }

    if (!headersFinished) {
      streamOk = false;
      lcdStatusLine("OpenAI headers fail");
    }

    if (!streamOk) {
      break;
    }

    responseFile.flush();
    responseFile.close();
    responseFile = File();
    outResponsePath = String(RESPONSE_TEMP_PATH);
    success = true;
  } while (false);

  httpsClient.stop();
  if (responseFile) {
    responseFile.flush();
    responseFile.close();
    responseFile = File();
  }
  if (!success && sdReady) {
    if (SD.exists(RESPONSE_TEMP_PATH)) {
      SD.remove(RESPONSE_TEMP_PATH);
    }
  }
  if (sdReady) {
    unmountSD();
  }
  if (statusLine.length()) {
    lcdStatusLine("HTTP status: " + statusLine);
  }

  markActivity();
  ledExitBusy();
  return success;
}

void collectResponseText(JsonVariantConst node, String& out) {
  if (node.isNull()) return;

  auto appendText = [&](const String& piece) {
    if (piece.length() == 0) return;
    if (out.length() > 0) out += "\n";
    out += piece;
  };

  if (node.is<const char*>()) {
    appendText(String(node.as<const char*>()));
    return;
  }
  if (node.is<String>()) {
    appendText(node.as<String>());
    return;
  }

  if (node.is<JsonArrayConst>()) {
    for (JsonVariantConst item : node.as<JsonArrayConst>()) {
      collectResponseText(item, out);
    }
    return;
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();

    if (obj.containsKey("text")) {
      collectResponseText(obj["text"], out);
    }
    if (obj.containsKey("value")) {
      collectResponseText(obj["value"], out);
    }
    if (obj.containsKey("content")) {
      collectResponseText(obj["content"], out);
    }
    if (obj.containsKey("output_text")) {
      collectResponseText(obj["output_text"], out);
    }
    if (obj.containsKey("delta")) {
      collectResponseText(obj["delta"], out);
    }

    for (JsonPairConst kv : obj) {
      const char* key = kv.key().c_str();
      if (!key) continue;
      if (strcmp(key, "type") == 0 || strcmp(key, "role") == 0 || strcmp(key, "id") == 0 || strcmp(key, "index") == 0) {
        continue;
      }
      if (strcmp(key, "text") == 0 || strcmp(key, "value") == 0 || strcmp(key, "content") == 0 || strcmp(key, "output_text") == 0 || strcmp(key, "delta") == 0) {
        continue;
      }
      collectResponseText(kv.value(), out);
    }
    return;
  }
}

// ------------------------------------------------------------
// Parse assistant text from an OpenAI response JSON file.
// Supports both legacy `output[].content[].text` layout and
// the newer `output_text` helper plus nested structures.
// ------------------------------------------------------------
String parseAssistantReplyFromFile(const String& path) {
  if (!mountSD()) {
    lcdStatusLine("Resp read fail");
    return "[SD error]";
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    lcdStatusLine("Resp open fail");
    unmountSD();
    return "[SD error]";
  }

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (SD.exists(path)) {
    SD.remove(path);
  }
  unmountSD();

  if (err) {
    lcdStatusLine("JSON parse error: " + String(err.c_str()));
    return "[JSON parse error]";
  }

  String text = "";

  JsonVariantConst outputTextVar = doc["output_text"];
  if (!outputTextVar.isNull()) {
    collectResponseText(outputTextVar, text);
  }

  if (text.length() == 0) {
    JsonArrayConst outputArr = doc["output"].as<JsonArrayConst>();
    for (JsonVariantConst item : outputArr) {
      JsonVariantConst content = item["content"];
      collectResponseText(content, text);
    }
  }

  if (text.length() == 0) {
    return "[No content]";
  }

  return text;
}

String extractFirstCodeBlock(const String& text) {
  int firstFence = text.indexOf("```");
  if (firstFence < 0) {
    return "";
  }

  int contentStart = firstFence + 3;
  int newlineAfterFence = text.indexOf('\n', contentStart);
  if (newlineAfterFence < 0) {
    return "";
  }

  int secondFence = text.indexOf("```", newlineAfterFence + 1);
  if (secondFence < 0) {
    return "";
  }

  int codeStart = newlineAfterFence + 1;
  if (codeStart >= secondFence) {
    return "";
  }

  String code = text.substring(codeStart, secondFence);
  while (code.endsWith("\r") || code.endsWith("\n")) {
    code.remove(code.length() - 1);
  }
  return code;
}

// ------------------------------------------------------------
// USB HID typing of assistant reply
// ------------------------------------------------------------
void typeReplyOverUSB(const String& text) {
  // Dump reply into the connected host as keystrokes.
  // Requires TinyUSB HID keyboard support.
  String sanitized = sanitizeForHidTyping(text);
  for (size_t i = 0; i < sanitized.length(); i++) {
    char c = sanitized[i];
    if (c == '\n') {
      HidKeyboard.write('\n');
    } else {
      HidKeyboard.write(c);
    }
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
  auto showAssistant = [&](bool updateStatus = true) {
    lcdShowAssistantReplyWindow();
    if (updateStatus) {
      lcdStatusLine(";/ up . down ,/ toggle v voice p pause -/ vol=+/ c cancel s sleep GO type ENTER exit");
    }
  };
  auto showUser = [&](bool updateStatus = true) {
    lcdShowUserPromptView();
    if (updateStatus) {
      lcdStatusLine(",/ toggle v voice p pause -/ vol=+/ c cancel s sleep GO type AI ENTER exit");
    }
  };

  bool showingAssistant = true;
  showAssistant();
  markActivity();

  bool prevEnter = false;
  bool prevGo = false;
  bool manualSleepHold = false;  // prevents immediate wake when 's' triggers sleep

  // for edge-detect on ';' and '.'
  std::vector<char> prevHeld;

  while (true) {
    M5Cardputer.update();
    maybeUpdateLed();
    maybeUpdateBatteryIndicator();
    checkDisplaySleep();
    pumpTtsStatus();
    auto ks = M5Cardputer.Keyboard.keysState();
    bool goNow = M5Cardputer.BtnA.isPressed();
    bool inputActive = hasKeyboardActivity(ks) || goNow;
    if (displaySleeping) {
      if (!inputActive) {
        manualSleepHold = false;
        prevHeld = ks.word;
        prevEnter = ks.enter;
        prevGo = goNow;
        delay(UI_IDLE_DELAY_MS);
        continue;
      }
      if (manualSleepHold) {
        // user is still holding 's'; stay asleep until released
        prevHeld = ks.word;
        prevEnter = ks.enter;
        prevGo = goNow;
        delay(UI_IDLE_DELAY_MS);
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
      if (ttsBusy && !ttsPlaybackActive) {
        ttsCancelRequested = true;
        ttsPauseRequested = false;
        ttsPrefetchPending = false;
        postTtsStatus("Voice cancelled");
      }
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
        } else if (c_now == 'v' || c_now == 'V') {
          speakLastAssistantReply();
          if (showingAssistant) {
            showAssistant(false);
          } else {
            showUser(false);
          }
        } else if (c_now == 'c' || c_now == 'C') {
          markActivity();
          if (ttsBusy) {
            ttsCancelRequested = true;
            ttsPauseRequested = false;
            ttsPaused = false;
            ttsResumeOffset = 0;
            ttsResumePath = "";
            M5Cardputer.Speaker.stop();
            postTtsStatus("Voice stopping");
          } else {
            ttsPaused = false;
            ttsPauseRequested = false;
            ttsResumeOffset = 0;
            ttsResumePath = "";
            postTtsStatus("Voice idle");
          }
        } else if (c_now == '-' || c_now == '_') {
          markActivity();
          adjustTtsVolume(-TTS_VOLUME_STEP);
        } else if (c_now == '=' || c_now == '+') {
          markActivity();
          adjustTtsVolume(TTS_VOLUME_STEP);
        } else if (c_now == 'p' || c_now == 'P') {
          markActivity();
          if (ttsBusy && ttsPlaybackActive) {
            requestTtsPause();
          } else if (ttsPaused) {
            resumeTtsIfPaused();
          } else if (ttsBusy) {
            postTtsStatus("Voice loading");
          } else {
            postTtsStatus("Voice idle");
          }
        } else if (c_now == 'x' || c_now == 'X') {
          markActivity();
          String code = extractFirstCodeBlock(lastAssistantReply);
          if (code.length() == 0) {
            lcdStatusLine("No code block.");
          } else {
            lcdStatusLine("Typing code...");
            typeReplyOverUSB(code);
            if (showingAssistant) {
              showAssistant(false);
            } else {
              showUser(false);
            }
          }
        } else if (c_now == 's' || c_now == 'S') {
          manualSleepHold = true;
          enterDisplaySleep();
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
    delay(UI_IDLE_DELAY_MS);
  }
}

// ------------------------------------------------------------
// Keyboard input (prompt typing)
// - supports delete/backspace using ks.del or code 0x7F / 0x2A
// - ENTER submits
// ------------------------------------------------------------
String readPromptFromKeyboard() {
  clearTtsCache();
  inputBuffer = "";
  lcdShowPromptEditing(inputBuffer);
  lcdStatusLine("Type. ENTER=send");
  markActivity();
  String renderedPrompt = inputBuffer;
  bool forcePromptRedraw = false;

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
    pumpTtsStatus();
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
        delay(UI_IDLE_DELAY_MS);
        continue;
      }
      wakeDisplayIfNeeded();
      forcePromptRedraw = true;
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
        delay(UI_IDLE_DELAY_MS);
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
          pumpTtsStatus();
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
            forcePromptRedraw = true;
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
        } else if (code == '\t') {
          // tab => 4 spaces
          markActivity();
          inputBuffer += "    ";
        } else if (code == '\n') {
          // newline in ks.word means Enter
          markActivity();
          String finalPrompt = inputBuffer;
          finalPrompt.trim();
          return finalPrompt;
        } else if (code >= 0x20 && code <= 0x7E) {
          // printable ASCII
          markActivity();
          inputBuffer += (char)code;
        } else {
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

    // 4. redraw prompt view only when needed
    if (forcePromptRedraw || inputBuffer != renderedPrompt) {
      lcdShowPromptEditing(inputBuffer);
      renderedPrompt = inputBuffer;
      forcePromptRedraw = false;
    }

    // 5. update edge-detect state
    prevHeld = currHeld;
    delay(UI_IDLE_DELAY_MS);
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
  lastBatteryDrawMs = millis();

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

  // 3. Send entire conversation to OpenAI (response saved to SD)
  String responsePath;
  if (!callOpenAI(responsePath)) {
    lcdStatusLine("Request failed.");
    markActivity();
    return;
  }

  // 4. Parse assistant reply string
  String reply = parseAssistantReplyFromFile(responsePath);
  reply = normalizeQuotes(reply);
  // 5. Save reply to chat history + remember it for GO typing
  addMessageToHistory("assistant", reply);
  lastAssistantReply = reply;
  ttsAudioReady = false;
  ttsCachedText = "";
  ttsCachedPath = "";
  if (ttsBusy) {
    ttsCancelRequested = true;
  }

  // 6. Prepare wrapped lines for scrolling UI
  prepareReplyLinesFromText(reply);

  // 6b. Kick off background TTS download so audio is ready when requested.
  startTtsPrefetch(reply);

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

  String text = marqueeTextForStatus();
  M5Cardputer.Display.setTextSize(STATUS_TEXT_SIZE);
  int textWidth = M5Cardputer.Display.textWidth(text.c_str());
  int availableWidth = SCREEN_WIDTH - CONTENT_MARGIN_X * 2;

  int newOffset = statusScrollOffset;
  if (textWidth <= availableWidth) {
    newOffset = 0;
  } else {
    const int scrollSpeed = 12;
    if (force || text != lastRenderedStatusText) {
      newOffset = 0;
      lastStatusScrollUpdateMs = now;
    } else if ((now - lastStatusScrollUpdateMs) >= 120) {
      lastStatusScrollUpdateMs = now;
      newOffset += scrollSpeed;
      int maxOffset = textWidth + CONTENT_MARGIN_X;
      if (newOffset > maxOffset) {
        newOffset = 0;
      }
    }
  }

  bool needsRedraw = force || text != lastRenderedStatusText || newOffset != lastRenderedStatusOffset;
  if (!needsRedraw) {
    updateHeaderBattery(force);
    return;
  }

  statusScrollOffset = newOffset;

  M5Cardputer.Display.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, STATUS_HEIGHT, BLACK);
  M5Cardputer.Display.setCursor(CONTENT_MARGIN_X - statusScrollOffset, HEADER_HEIGHT + CONTENT_MARGIN_Y);
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.print(text);

  if (textWidth > availableWidth) {
    M5Cardputer.Display.setCursor(CONTENT_MARGIN_X - statusScrollOffset + textWidth + 16, HEADER_HEIGHT + CONTENT_MARGIN_Y);
    M5Cardputer.Display.print(text);
  }

  lastRenderedStatusText = text;
  lastRenderedStatusOffset = statusScrollOffset;
  updateHeaderBattery(force);
}

void renderStatusLine(bool force) {
  renderStatusLineInner(force);
}
