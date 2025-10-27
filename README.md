# Cardputer OpenAssistant

This sketch turns an M5Stack Cardputer (ESP32-S3) into a handheld ChatGPT client that can accept typed prompts, speak-to-type via the onboard microphone, and replay replies over USB HID.

## Highlights
- Connects to Wi-Fi using credentials loaded from `/chat_config.txt` on the SD card.
- Sends rolling conversation context to OpenAI’s `responses` API (`gpt-4o-mini` by default).
- Displays replies in a scrollable UI and can “type” the last answer into a connected host via USB keyboard emulation.
- Optional voice input: hold the `GO` button while editing to record audio, transcribe it with OpenAI (`gpt-4o-mini-transcribe`), and append the text into your prompt.
- `/context` command saves the current conversation to `transcripts/` on the SD card and starts a fresh session.

## Hardware & Software Requirements
- M5Stack Cardputer (ESP32-S3) with TinyUSB HID support enabled.
- MicroSD card formatted as FAT32.
- Arduino IDE (or PlatformIO) with the M5Stack board support package installed.
- OpenAI API key with access to `gpt-4o-mini` and `gpt-4o-mini-transcribe`.

## SD Card Configuration
Create `/chat_config.txt` on the SD card with:

```
WIFI_SSID = YourNetwork
WIFI_PASS = YourPassword
OPENAI_KEY = sk-...
```

Lines starting with `#` are treated as comments. The file is read during boot; missing entries will stop initialization.

## Building & Uploading
1. Open `OpenAssistant.ino` in the Arduino IDE.
2. Select the M5Stack ESP32-S3 board variant that matches your Cardputer.
3. Enable USB CDC+HID (Tools → USB Mode) so TinyUSB keyboard output works.
4. Compile and upload the sketch as usual.

## Usage
1. After boot the Cardputer connects to Wi-Fi, then shows the prompt editor (`You:`).
2. Type normally using the built-in keyboard; `ENTER` submits the prompt.
3. While editing, **hold the `GO` button** to capture audio. Release to stop:
   - Recording status shows elapsed seconds.
   - Audio is streamed to `/oa_tmp/voice.raw` on the SD card; if the card fills or a write fails you’ll see `Voice write failed`.
   - When recording stops, the sketch uploads the audio to OpenAI, receives the transcript, and appends it to the prompt. You can edit further before submitting.
4. Type `/context` (and press `ENTER`) any time you want to archive the current conversation:
   - The whole chat history, including the system prompt, is written to `transcripts/context_<timestamp>.txt`.
   - A fresh conversation context is started automatically.
5. After the assistant replies:
   - `;` scrolls up, `.` scrolls down.
   - `GO` types the full reply into the host computer via USB HID.
   - `ENTER` exits view mode and returns to prompt editing.

## Troubleshooting
- **Wi-Fi stuck on “connecting”**: double-check SSID/password and that the SD card was detected.
- **Voice write failed**: ensure the SD card is present, writable, and not full. Reformat or replace if errors persist.
- **Missing transcripts**: confirm the SD card has a `transcripts/` folder (the sketch creates it automatically) and that you used the `/context` command to save.
- **Transcription errors**: confirm API access to `gpt-4o-mini-transcribe`; watch the serial monitor for OpenAI HTTP status codes.
- **USB typing not working**: verify TinyUSB keyboard support is enabled for the selected board profile.

## Customization Tips
- Adjust `VOICE_MAX_DURATION_MS` or `VOICE_CHUNK_SAMPLES` in the `.ino` file to tune max recording length and streaming granularity.
- Swap `MODEL_NAME` / `TRANSCRIBE_MODEL` for different OpenAI models if your account supports them.
- Increase the conversation memory window by changing the `chatHistory` pruning logic in `addMessageToHistory`.

## License
This project follows the license of the original sources you pulled in (M5Stack libraries under MIT). Adapt and redistribute responsibly.
