# AI-V

**Voice assistant firmware for the Home Assistant Voice Preview Edition, without Home Assistant.**

AI-V turns the HA Voice PE into a standalone, cloud-powered voice assistant. Hold the button, speak, release, hear the reply. No Home Assistant, no companion server, no subscription. Just an ESP32-S3 talking directly to cloud APIs.

## What it does

Press the center button and talk. The device records your voice, sends it to a speech-to-text service, forwards the transcript to an LLM, converts the response to speech, and plays it back through the built-in speaker. The whole round trip takes a few seconds.

The LLM, STT, and TTS providers are all configurable. Swap endpoints and API keys in the YAML to use whatever services you want.

## Default stack

| Component | Provider | Cost |
|-----------|----------|------|
| **STT** | DashScope Qwen3-ASR (Singapore) | ~$0.50/mo |
| **LLM** | Kimi K2.5 via Kilo Code gateway, with web search | Free (with API key) |
| **TTS** | DashScope Qwen3-TTS-Flash, Jennifer voice | ~$1-3/mo |

Total: roughly $2-4/month at personal usage levels. No servers to maintain.

### Features

- **Hold-to-talk**: Hold center button for 2+ seconds, speak, release to process
- **Web search**: Kimi automatically searches the web when your question needs current info
- **Conversation memory**: Remembers last 5 exchanges (configurable), resets on reboot
- **Configurable providers**: Swap STT, LLM, TTS by changing URLs and keys in YAML
- **Dual LLM API formats**: Supports both Anthropic and OpenAI-compatible endpoints
- **Volume control**: Rotary dial with smooth teal-to-cyan LED gradient
- **Mute switch**: Hardware mute with red pulse on mute, green pulse on unmute
- **75 seconds of TTS**: 3.5MB audio buffer in PSRAM
- **Web dashboard**: ESPHome web server with hardware test buttons and status sensors
- **OTA updates**: Flash over WiFi after initial USB flash

## Hardware

Built for the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/), which is an ESP32-S3 board with:

- ESP32-S3, 16MB flash, 8MB PSRAM
- XMOS XU316 DSP (acoustic echo cancellation, noise suppression)
- TLV320AIC3204 audio DAC
- I2S microphone input (16kHz, 32-bit stereo, XMOS-processed)
- I2S speaker output (48kHz, 32-bit stereo)
- 12x WS2812 RGB LED ring
- Rotary encoder (volume control)
- Center button (push-to-talk)
- Hardware mute switch
- 3.5mm audio jack

### Pin Map

| Function | GPIO |
|----------|------|
| I2C SDA | 5 |
| I2C SCL | 6 |
| I2S Speaker LRCLK | 7 |
| I2S Speaker BCLK | 8 |
| I2S Speaker DOUT | 10 |
| I2S Mic LRCLK | 14 |
| I2S Mic BCLK | 13 |
| I2S Mic DIN | 15 |
| LED Ring (WS2812 x12) | 21 |
| LED Power | 45 |
| Speaker Amp Enable | 47 |
| Center Button | 0 |
| Mute Switch | 3 |
| XMOS Reset | 4 |
| Rotary Encoder A | 16 |
| Rotary Encoder B | 18 |
| Jack Detect | 17 |

## How it works

### Voice Pipeline

```
[Hold Button 2+ seconds]
    --> Mic (via XMOS DSP) --> Record PCM to PSRAM
[Release Button]
    --> Base64 encode audio
    --> POST to Qwen3-ASR (DashScope multimodal endpoint)
    --> Receive transcript
    --> POST transcript + conversation history to LLM
        --> If LLM needs current info: automatic web search (up to 3 rounds)
    --> Receive response text
    --> POST text to Qwen3-TTS
    --> Receive audio URL
    --> Download WAV to PSRAM (up to 3.5MB / 75 seconds)
    --> Play through speaker (AIC3204 DAC)
```

### LED Feedback

| State | LED Effect |
|-------|-----------|
| Idle | Off |
| Recording (button held) | Gentle pulsing blue |
| Processing (STT + LLM + TTS) | Spinning rainbow |
| Speaking | Spinning rainbow |
| Mute toggled ON | Red pulse (3s) |
| Mute toggled OFF | Green pulse (3s) |
| Volume change | Smooth teal-to-cyan gradient arc |
| Error | Pulsing red |
| WiFi connecting | Warm white solid |
| WiFi connected, initializing | Blue twinkle |
| Short press (< 2s) | Ignored, LEDs clear |

### Conversation Memory

The device keeps the last N turns (configurable, default 5) in PSRAM. Each exchange appends your message and the assistant's reply to the history, so the LLM has context across a conversation. The history resets on reboot.

## Architecture

The firmware is built on ESPHome with a custom C++ external component (`ai_voice`). ESPHome handles all the hardware drivers (XMOS, AIC3204, I2S, LED ring, buttons, rotary encoder, WiFi, OTA). The custom component handles the voice pipeline.

```
ai-v.yaml                          Main ESPHome configuration
esphome/components/ai_voice/
    __init__.py                     ESPHome component registration + config schema
    ai_voice.h                      Class definition, state machine, buffers
    ai_voice.cpp                    Pipeline implementation: STT, LLM, TTS, audio playback
```

The pipeline runs on a dedicated FreeRTOS task pinned to Core 1, so it never blocks the ESPHome main loop. Audio capture uses a PSRAM ring buffer fed by a microphone data callback. TTS audio is downloaded to a 3.5MB PSRAM buffer (supports ~75 seconds of speech) and played back through the speaker with proper audio stream info.

### LLM API Formats

The component supports two LLM API formats:

- **`anthropic`**: Anthropic Messages API (`/v1/messages`), with `x-api-key` header and `system` field
- **`openai`**: OpenAI Chat Completions API (`/v1/chat/completions`), with `Bearer` auth and system message in the messages array

Set `llm_api_format` in the YAML to switch between them. When the endpoint contains `kimi.com`, the component automatically adds the required Kilo Code gateway headers and disables thinking mode for fast responses.

## Setup

### Prerequisites

- [ESPHome](https://esphome.io/) 2026.3.0 or later
- A DashScope account on [Alibaba Cloud International](https://www.alibabacloud.com/) (Singapore region, no ID required, just email + phone)
- An LLM API key (Kimi, Anthropic, OpenAI, or any compatible provider)

### 1. Clone this repo

```bash
git clone https://github.com/wowitsjack/ai-v.git
cd ai-v
```

### 2. Create secrets.yaml

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml` with your credentials:

```yaml
wifi_ssid: "YourNetwork"
wifi_password: "YourPassword"
kimi_api_key: "sk-kimi-..."
dashscope_api_key: "sk-..."
```

### 3. Compile and flash

```bash
pip install esphome
esphome compile ai-v.yaml
esphome upload ai-v.yaml --device /dev/ttyACM0
```

First flash must be over USB. Subsequent updates can be done over WiFi (OTA).

### 4. Use it

- Wait for the LED ring to stop twinkling (WiFi connected)
- Hold the center button, speak, release
- Rainbow spin while processing
- Hear the response through the speaker
- Turn the dial to adjust volume

## Configuration

All service endpoints are configurable in `ai-v.yaml` under the `ai_voice:` section:

```yaml
ai_voice:
  # LLM
  llm_endpoint: "https://api.kimi.com/coding/v1/chat/completions"
  llm_api_key: !secret kimi_api_key
  llm_model_default: "kimi-for-coding"
  llm_api_format: "openai"
  system_prompt: "You are a concise voice assistant. Keep responses under 3 sentences."
  max_tokens: 256
  web_search: true  # Kimi searches the web automatically when needed

  # STT
  stt_endpoint: "https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
  stt_api_key: !secret dashscope_api_key
  stt_model: "qwen3-asr-flash-2026-02-10"

  # TTS
  tts_endpoint: "https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
  tts_api_key: !secret dashscope_api_key
  tts_model: "qwen3-tts-flash"
  tts_voice: "Jennifer"
```

### Switching to Claude

```yaml
  llm_endpoint: "https://api.anthropic.com/v1/messages"
  llm_api_key: !secret anthropic_api_key
  llm_model_default: "claude-haiku-4-5-20251001"
  llm_api_format: "anthropic"
```

### Switching to OpenAI

```yaml
  llm_endpoint: "https://api.openai.com/v1/chat/completions"
  llm_api_key: !secret openai_api_key
  llm_model_default: "gpt-4o-mini"
  llm_api_format: "openai"
```

### Available TTS Voices

Jennifer (cinematic American English, default), Cherry, Ethan, Ryan, Neil, Serena, Chelsie, Momo, Vivian, Bella, Aiden, Moon, Kai, and many more. See [DashScope TTS docs](https://www.alibabacloud.com/help/en/model-studio/qwen-tts) for the full list.

## Web Interface

The device runs a web server on port 80. Visit `http://<device-ip>/` for the ESPHome dashboard with:

- Hardware test buttons (LEDs, Speaker, Mic, XMOS, Network, Full Pipeline)
- Status sensors (AI state, last transcript, last error, WiFi info)
- Mute switch
- LED ring control

## Remote Testing

Test hardware without touching the device:

```bash
# Find the device
ping ai-v-XXXX.local

# Test speaker
curl -X POST http://192.168.1.108/button/test_speaker/press -H "Content-Length: 0"

# Test LEDs
curl -X POST http://192.168.1.108/button/test_leds/press -H "Content-Length: 0"

# Run full pipeline (records 3s of audio)
curl -X POST http://192.168.1.108/button/test_full_pipeline/press -H "Content-Length: 0"

# Check status
curl -s http://192.168.1.108/events --max-time 3 | grep "AI Status"
```

## Technical Details

### Memory Layout (8MB PSRAM)

| Allocation | Size | Purpose |
|-----------|------|---------|
| Code + rodata | ~2-3 MB | ESP-IDF SPIRAM code mapping |
| Mic capture buffer | 320 KB | 10 seconds @ 16kHz 16-bit mono |
| TTS audio buffer | 3.5 MB | ~75 seconds @ 24kHz 16-bit mono (downloaded WAV) |
| HTTP response buffers | 128 KB | STT + LLM JSON responses |
| Pipeline task stack | 32 KB | FreeRTOS task on Core 1 |
| Audio pipeline | ~256 KB | Mixer, resampler, I2S DMA |
| TLS sessions | ~80 KB | mbedTLS from PSRAM |
| Misc | ~256 KB | Conversation history, cJSON, etc |

### Audio Pipeline

```
Mic -> XMOS (AEC + NS + AGC) -> I2S Input (16kHz 32-bit stereo)
    -> Callback extracts left channel 16-bit mono -> PSRAM ring buffer

TTS WAV (24kHz 16-bit mono) -> set_audio_stream_info() -> speaker.play()
    -> Resampler (24kHz -> 48kHz) -> Mixer -> I2S Output -> AIC3204 DAC -> Speaker
```

### Dependencies

- ESPHome 2026.3.0+
- [Voice Kit component](https://github.com/esphome/home-assistant-voice-pe) (XMOS driver, pulled automatically)
- ESP-IDF 5.x (managed by ESPHome)

## Credits

Built by studying and borrowing patterns from:

- [esphome/home-assistant-voice-pe](https://github.com/esphome/home-assistant-voice-pe), the official HA Voice PE firmware
- [croll83/nabuvoice](https://github.com/croll83/nabuvoice), the first custom Voice PE firmware
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), the most popular ESP32 voice assistant
- [AvantMaker/ESP32_AI_Connect](https://github.com/AvantMaker/ESP32_AI_Connect), Claude API client for ESP32

## License

MIT
