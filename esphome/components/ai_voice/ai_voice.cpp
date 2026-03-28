#include "ai_voice.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "esphome/components/audio/audio.h"

#include <cstring>
#include <cJSON.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

namespace esphome {
namespace ai_voice {

static const char *const TAG = "ai_voice";

// HTTP event handler that accumulates response body into a buffer
struct ResponseData {
  char *buffer;
  size_t len;
  size_t capacity;
};

static esp_err_t text_event_handler(esp_http_client_event_t *evt) {
  ResponseData *rd = (ResponseData *) evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && rd) {
    if (rd->len + evt->data_len < rd->capacity) {
      memcpy(rd->buffer + rd->len, evt->data, evt->data_len);
      rd->len += evt->data_len;
    }
  }
  return ESP_OK;
}

// =============================================================================
// SETUP
// =============================================================================

void AiVoice::setup() {
  ESP_LOGI(TAG, "Setting up AI Voice...");

  this->capture_buffer_ = (uint8_t *) heap_caps_calloc(CAPTURE_BUFFER_SIZE, 1,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  this->tts_buffer_ = (uint8_t *) heap_caps_calloc(TTS_BUFFER_SIZE, 1,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->capture_buffer_ || !this->tts_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
    this->mark_failed();
    return;
  }

  // Mic callback: extract left-channel 16-bit mono from 32-bit stereo I2S
  this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (!this->capturing_) return;
    const size_t num_frames = data.size() / MIC_BYTES_PER_STEREO_PAIR;
    for (size_t i = 0; i < num_frames; i++) {
      if (this->capture_write_pos_ >= CAPTURE_BUFFER_SIZE) break;
      const size_t src = i * MIC_BYTES_PER_STEREO_PAIR;
      this->capture_buffer_[this->capture_write_pos_] = data[src + 2];
      this->capture_buffer_[this->capture_write_pos_ + 1] = data[src + 3];
      this->capture_write_pos_ += MIC_BYTES_PER_MONO_SAMPLE;
    }
  });

  // Pipeline task on Core 1
  this->pipeline_task_stack_ = (StackType_t *) heap_caps_malloc(
      32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->pipeline_task_stack_) {
    ESP_LOGE(TAG, "Failed to allocate pipeline task stack");
    this->mark_failed();
    return;
  }
  this->pipeline_task_handle_ = xTaskCreateStaticPinnedToCore(
      &AiVoice::pipeline_task_func_, "ai_voice", 32768, this, 5,
      this->pipeline_task_stack_, &this->pipeline_task_tcb_, 1);

  if (!this->providers_.empty()) {
    ESP_LOGI(TAG, "AI Voice ready (%d providers, default: %s)",
             this->providers_.size(), this->providers_[0].name.c_str());
  }
}

// =============================================================================
// LOOP
// =============================================================================

void AiVoice::loop() {
  if (!this->ready_ && network::is_connected()) {
    this->ready_ = true;
    this->set_phase_(PHASE_IDLE);
    ESP_LOGI(TAG, "Network connected, AI Voice ready");
  }

  // Pipeline task sets phase_changed_ flag, YAML polls via phase_changed()
}

// =============================================================================
// ACTIONS
// =============================================================================

void AiVoice::start_recording() {
  if (this->state_ != STATE_IDLE || !this->ready_) return;
  ESP_LOGI(TAG, "Recording...");
  this->capture_write_pos_ = 0;
  this->capturing_ = true;
  this->state_ = STATE_RECORDING;
  this->set_phase_(PHASE_RECORDING);
  this->microphone_->start();
}

void AiVoice::stop_recording() {
  if (this->state_ != STATE_RECORDING) return;
  ESP_LOGI(TAG, "Stopped (%u bytes)", this->capture_write_pos_);
  this->capturing_ = false;
  this->microphone_->stop();
  this->state_ = STATE_PROCESSING;
  this->set_phase_(PHASE_THINKING);
  this->abort_requested_ = false;
  this->pipeline_requested_ = true;
}

void AiVoice::abort() {
  this->abort_requested_ = true;
  this->capturing_ = false;
  if (this->state_ == STATE_SPEAKING) {
    this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_STOP).perform();
  }
  this->state_ = STATE_IDLE;
  this->set_phase_(PHASE_IDLE);
}

void AiVoice::cycle_provider() {
  if (this->providers_.empty()) return;
  this->current_provider_ = (this->current_provider_ + 1) % this->providers_.size();
  this->conversation_.clear();  // clear history on provider switch
  this->phase_changed_ = true;
  ESP_LOGI(TAG, "Provider: %s (%s)", this->providers_[this->current_provider_].name.c_str(),
           this->providers_[this->current_provider_].model.c_str());
}

void AiVoice::set_phase_(int phase) { this->voice_phase_ = phase; this->phase_changed_ = true; }

void AiVoice::trim_conversation_() {
  while ((int) this->conversation_.size() > this->max_turns_ * 2)
    this->conversation_.erase(this->conversation_.begin());
}

// =============================================================================
// PIPELINE TASK
// =============================================================================

void AiVoice::pipeline_task_func_(void *param) {
  AiVoice *self = (AiVoice *) param;
  while (true) {
    if (!self->pipeline_requested_) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    self->pipeline_requested_ = false;

    size_t pcm_len = self->capture_write_pos_;
    if (pcm_len < 3200) {
      ESP_LOGW(TAG, "Too short (%u bytes)", pcm_len);
      self->state_ = STATE_IDLE;
      self->set_phase_(PHASE_IDLE);
      continue;
    }

    ESP_LOGI(TAG, "Pipeline: %.1fs audio", (float) pcm_len / (SAMPLE_RATE * 2));

    // Step 1: STT
    self->set_phase_(PHASE_STT);
    std::string transcript = self->call_stt_(self->capture_buffer_, pcm_len);
    if (self->abort_requested_ || transcript.empty()) {
      if (self->last_error_.empty() || self->last_error_.find("STT") == std::string::npos) {
        self->last_error_ = "STT empty (pcm=" + std::to_string(pcm_len) + "b, maybe silence?)";
      }
      ESP_LOGW(TAG, "STT %s (pcm=%u) last_err=%s", self->abort_requested_ ? "aborted" : "failed/empty", pcm_len, self->last_error_.c_str());
      self->state_ = STATE_ERROR; self->set_phase_(PHASE_ERROR);
      vTaskDelay(pdMS_TO_TICKS(2000));
      self->state_ = STATE_IDLE; self->set_phase_(PHASE_IDLE);
      continue;
    }
    ESP_LOGI(TAG, "You said: %s", transcript.c_str());
    self->last_transcript_ = transcript;

    // Step 2: LLM
    self->set_phase_(PHASE_THINKING);
    std::string response = self->call_llm_(transcript);
    if (self->abort_requested_ || response.empty()) {
      self->last_error_ = "LLM " + std::string(self->abort_requested_ ? "aborted" : "empty response");
      ESP_LOGW(TAG, "%s", self->last_error_.c_str());
      self->state_ = STATE_ERROR; self->set_phase_(PHASE_ERROR);
      vTaskDelay(pdMS_TO_TICKS(2000));
      self->state_ = STATE_IDLE; self->set_phase_(PHASE_IDLE);
      continue;
    }
    ESP_LOGI(TAG, "AI: %.100s%s", response.c_str(), response.length() > 100 ? "..." : "");

    self->conversation_.push_back({"user", transcript});
    self->conversation_.push_back({"assistant", response});
    self->trim_conversation_();

    // Step 3: TTS (get audio URL)
    self->set_phase_(PHASE_SPEAKING);
    std::string audio_url = self->call_tts_(response);
    if (self->abort_requested_ || audio_url.empty()) {
      ESP_LOGW(TAG, "TTS %s", self->abort_requested_ ? "aborted" : "failed");
      self->state_ = STATE_ERROR; self->set_phase_(PHASE_ERROR);
      vTaskDelay(pdMS_TO_TICKS(2000));
      self->state_ = STATE_IDLE; self->set_phase_(PHASE_IDLE);
      continue;
    }

    // Download WAV and play directly through speaker
    self->state_ = STATE_SPEAKING;
    if (!self->download_and_play_(audio_url)) {
      ESP_LOGW(TAG, "Playback failed");
      self->last_error_ = "Playback failed";
    }
    self->state_ = STATE_IDLE;
    self->set_phase_(PHASE_IDLE);
    ESP_LOGI(TAG, "Pipeline complete");
  }
}

// =============================================================================
// WAV HEADER
// =============================================================================

void AiVoice::build_wav_header_(uint8_t *h, size_t pcm_size) {
  uint32_t file_size = pcm_size + 36;
  uint16_t ch = 1, bps = 16, fmt = 1;
  uint32_t sr = SAMPLE_RATE, br = SAMPLE_RATE * 2;
  uint16_t ba = 2;
  uint32_t fs = 16;
  memcpy(h, "RIFF", 4);     memcpy(h+4, &file_size, 4);
  memcpy(h+8, "WAVE", 4);   memcpy(h+12, "fmt ", 4);
  memcpy(h+16, &fs, 4);     memcpy(h+20, &fmt, 2);
  memcpy(h+22, &ch, 2);     memcpy(h+24, &sr, 4);
  memcpy(h+28, &br, 4);     memcpy(h+32, &ba, 2);
  memcpy(h+34, &bps, 2);    memcpy(h+36, "data", 4);
  memcpy(h+40, &pcm_size, 4);
}

// =============================================================================
// Base64 encoder (for STT audio upload)
// =============================================================================

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
  size_t olen = 4 * ((len + 2) / 3);
  char *out = (char *) heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out) return nullptr;
  size_t j = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t a = data[i];
    uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
    uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    out[j++] = b64_table[(triple >> 18) & 0x3F];
    out[j++] = b64_table[(triple >> 12) & 0x3F];
    out[j++] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
    out[j++] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
  }
  out[j] = '\0';
  *out_len = j;
  return out;
}

// =============================================================================
// STT: DashScope qwen3-asr via multimodal endpoint with base64 audio
// =============================================================================

std::string AiVoice::call_stt_(const uint8_t *pcm_data, size_t pcm_len) {
  ESP_LOGI(TAG, "STT: encoding %u bytes audio...", pcm_len);

  // Build WAV in memory
  uint8_t wav_header[44];
  build_wav_header_(wav_header, pcm_len);

  // Combine header + PCM
  size_t wav_len = 44 + pcm_len;
  uint8_t *wav = (uint8_t *) heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) return "";
  memcpy(wav, wav_header, 44);
  memcpy(wav + 44, pcm_data, pcm_len);

  // Base64 encode
  size_t b64_len = 0;
  char *b64 = base64_encode(wav, wav_len, &b64_len);
  heap_caps_free(wav);
  if (!b64) return "";

  ESP_LOGI(TAG, "STT: base64 encoded %u bytes, calling ASR...", b64_len);

  // Build DashScope multimodal request:
  // {"model":"qwen3-asr-flash","input":{"messages":[{"role":"user","content":[{"audio":"data:audio/wav;base64,XXX"}]}]}}
  // Build JSON manually to avoid huge string copies
  std::string json_prefix = "{\"model\":\"" + this->stt_model_ +
      "\",\"input\":{\"messages\":[{\"role\":\"user\",\"content\":[{\"audio\":\"data:audio/wav;base64,";
  std::string json_suffix = "\"}]}]}}";

  size_t body_len = json_prefix.size() + b64_len + json_suffix.size();
  char *body = (char *) heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!body) { heap_caps_free(b64); return ""; }

  memcpy(body, json_prefix.c_str(), json_prefix.size());
  memcpy(body + json_prefix.size(), b64, b64_len);
  memcpy(body + json_prefix.size() + b64_len, json_suffix.c_str(), json_suffix.size());
  body[body_len] = '\0';
  heap_caps_free(b64);

  // HTTP POST
  ResponseData rd;
  rd.capacity = HTTP_RESP_BUFFER_SIZE;
  rd.buffer = (char *) heap_caps_calloc(rd.capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rd.len = 0;
  if (!rd.buffer) { heap_caps_free(body); return ""; }

  esp_http_client_config_t cfg = {};
  cfg.url = this->stt_endpoint_.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 60000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 8192;
  cfg.buffer_size_tx = 8192;
  cfg.event_handler = text_event_handler;
  cfg.user_data = &rd;

  auto *client = esp_http_client_init(&cfg);
  std::string auth = "Bearer " + this->stt_api_key_;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, body_len);

  std::string result;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);

  if (err == ESP_OK && status == 200) {
    rd.buffer[rd.len] = '\0';
    ESP_LOGI(TAG, "STT response (%u bytes): %.200s", rd.len, rd.buffer);
    cJSON *root = cJSON_Parse(rd.buffer);
    if (root) {
      cJSON *output = cJSON_GetObjectItem(root, "output");
      if (output) {
        cJSON *choices = cJSON_GetObjectItem(output, "choices");
        if (choices && cJSON_IsArray(choices)) {
          cJSON *first = cJSON_GetArrayItem(choices, 0);
          if (first) {
            cJSON *msg = cJSON_GetObjectItem(first, "message");
            if (msg) {
              cJSON *content = cJSON_GetObjectItem(msg, "content");
              if (content && cJSON_IsArray(content)) {
                cJSON *first_content = cJSON_GetArrayItem(content, 0);
                if (first_content) {
                  cJSON *text = cJSON_GetObjectItem(first_content, "text");
                  if (text && cJSON_IsString(text)) result = text->valuestring;
                }
              }
            }
          }
        }
      }
      if (result.empty()) {
        this->last_error_ = "STT 200 but no text. resp=" + std::string(rd.buffer, std::min(rd.len, (size_t)150));
      }
      cJSON_Delete(root);
    } else {
      this->last_error_ = "STT JSON parse fail. raw=" + std::string(rd.buffer, std::min(rd.len, (size_t)150));
    }
  } else {
    ESP_LOGE(TAG, "STT failed: err=%s, status=%d, body_len=%u", esp_err_to_name(err), status, body_len);
    if (rd.len > 0) {
      rd.buffer[rd.len] = '\0';
      ESP_LOGE(TAG, "STT error: %.200s", rd.buffer);
      this->last_error_ = "STT http=" + std::to_string(status) + " " + std::string(rd.buffer, std::min(rd.len, (size_t)100));
    } else {
      this->last_error_ = "STT err=" + std::string(esp_err_to_name(err)) + " status=" + std::to_string(status) + " body=" + std::to_string(body_len);
    }
  }

  esp_http_client_cleanup(client);
  heap_caps_free(body);
  heap_caps_free(rd.buffer);
  return result;
}

// =============================================================================
// LLM (dispatches to anthropic or openai format)
// =============================================================================

std::string AiVoice::call_llm_(const std::string &user_text) {
  if (this->providers_.empty()) return "";
  const auto &prov = this->providers_[this->current_provider_];
  if (prov.format == "anthropic")
    return call_llm_anthropic_(user_text, prov);
  else
    return call_llm_openai_(user_text, prov);
}

std::string AiVoice::call_llm_anthropic_(const std::string &user_text, const LlmProvider &prov) {
  ESP_LOGI(TAG, "LLM (%s, anthropic): %s", prov.name.c_str(), prov.model.c_str());

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", prov.model.c_str());
  cJSON_AddNumberToObject(root, "max_tokens", this->max_tokens_);
  cJSON_AddStringToObject(root, "system", this->system_prompt_.c_str());

  // MiniMax requires temperature > 0
  if (prov.endpoint.find("minimax") != std::string::npos) {
    cJSON_AddNumberToObject(root, "temperature", 0.2);
  }

  cJSON *msgs = cJSON_CreateArray();
  for (const auto &t : this->conversation_) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", t.role.c_str());
    cJSON_AddStringToObject(m, "content", t.content.c_str());
    cJSON_AddItemToArray(msgs, m);
  }
  cJSON *um = cJSON_CreateObject();
  cJSON_AddStringToObject(um, "role", "user");
  cJSON_AddStringToObject(um, "content", user_text.c_str());
  cJSON_AddItemToArray(msgs, um);
  cJSON_AddItemToObject(root, "messages", msgs);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return "";

  ResponseData rd;
  rd.capacity = HTTP_RESP_BUFFER_SIZE;
  rd.buffer = (char *) heap_caps_calloc(rd.capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rd.len = 0;

  esp_http_client_config_t cfg = {};
  cfg.url = prov.endpoint.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 30000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 4096;
  cfg.event_handler = text_event_handler;
  cfg.user_data = &rd;

  auto *client = esp_http_client_init(&cfg);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "x-api-key", prov.api_key.c_str());
  esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
  esp_http_client_set_post_field(client, json, strlen(json));

  std::string result;
  if (esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200) {
    rd.buffer[rd.len] = '\0';
    cJSON *r = cJSON_Parse(rd.buffer);
    if (r) {
      cJSON *content = cJSON_GetObjectItem(r, "content");
      if (content && cJSON_IsArray(content)) {
        // Iterate blocks, find the "text" type (skip "thinking")
        int n = cJSON_GetArraySize(content);
        for (int i = 0; i < n; i++) {
          cJSON *block = cJSON_GetArrayItem(content, i);
          cJSON *btype = cJSON_GetObjectItem(block, "type");
          if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text)) { result = text->valuestring; break; }
          }
        }
      }
      cJSON_Delete(r);
    }
  } else {
    ESP_LOGE(TAG, "LLM failed (status %d)", esp_http_client_get_status_code(client));
  }

  esp_http_client_cleanup(client);
  free(json);
  heap_caps_free(rd.buffer);
  return result;
}

std::string AiVoice::call_llm_openai_(const std::string &user_text, const LlmProvider &prov) {
  ESP_LOGI(TAG, "LLM (%s, openai): %s", prov.name.c_str(), prov.model.c_str());

  bool is_kimi = prov.endpoint.find("kimi.com") != std::string::npos;

  // Build initial messages array
  cJSON *msgs = cJSON_CreateArray();
  cJSON *sm = cJSON_CreateObject();
  cJSON_AddStringToObject(sm, "role", "system");
  cJSON_AddStringToObject(sm, "content", this->system_prompt_.c_str());
  cJSON_AddItemToArray(msgs, sm);

  for (const auto &t : this->conversation_) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", t.role.c_str());
    cJSON_AddStringToObject(m, "content", t.content.c_str());
    cJSON_AddItemToArray(msgs, m);
  }
  cJSON *um = cJSON_CreateObject();
  cJSON_AddStringToObject(um, "role", "user");
  cJSON_AddStringToObject(um, "content", user_text.c_str());
  cJSON_AddItemToArray(msgs, um);

  // Tool-calling loop (max 3 rounds for web search)
  std::string result;
  for (int round = 0; round < 3; round++) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", prov.model.c_str());
    cJSON_AddNumberToObject(root, "max_tokens", this->max_tokens_);

    if (is_kimi) {
      cJSON *thinking = cJSON_CreateObject();
      cJSON_AddStringToObject(thinking, "type", "disabled");
      cJSON_AddItemToObject(root, "thinking", thinking);
    }

    // Add web search tool for Kimi
    if (this->web_search_enabled_ && is_kimi) {
      cJSON *tools = cJSON_CreateArray();
      cJSON *tool = cJSON_CreateObject();
      cJSON_AddStringToObject(tool, "type", "builtin_function");
      cJSON *func = cJSON_CreateObject();
      cJSON_AddStringToObject(func, "name", "$web_search");
      cJSON_AddItemToObject(tool, "function", func);
      cJSON_AddItemToArray(tools, tool);
      cJSON_AddItemToObject(root, "tools", tools);
    }

    // Deep copy messages array (cJSON_AddItemToObject transfers ownership)
    cJSON *msgs_copy = cJSON_Duplicate(msgs, true);
    cJSON_AddItemToObject(root, "messages", msgs_copy);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) break;

    ESP_LOGI(TAG, "LLM request round %d (%u bytes)", round, strlen(json));

    ResponseData rd;
    rd.capacity = HTTP_RESP_BUFFER_SIZE;
    rd.buffer = (char *) heap_caps_calloc(rd.capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rd.len = 0;
    if (!rd.buffer) { free(json); break; }

    esp_http_client_config_t cfg = {};
    cfg.url = prov.endpoint.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    cfg.event_handler = text_event_handler;
    cfg.user_data = &rd;

    auto *client = esp_http_client_init(&cfg);
    std::string auth = "Bearer " + prov.api_key;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    if (is_kimi) {
      esp_http_client_set_header(client, "User-Agent", "Kilo-Code/4.111.0");
      esp_http_client_set_header(client, "Referer", "https://kilocode.ai");
      esp_http_client_set_header(client, "Origin", "https://kilocode.ai");
      esp_http_client_set_header(client, "HTTP-Referer", "https://kilocode.ai");
      esp_http_client_set_header(client, "X-Title", "Kilo Code");
      esp_http_client_set_header(client, "X-KiloCode-Version", "4.111.0");
    }

    esp_http_client_set_post_field(client, json, strlen(json));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json);

    if (err != ESP_OK || status != 200) {
      ESP_LOGE(TAG, "LLM failed (status %d)", status);
      heap_caps_free(rd.buffer);
      break;
    }

    rd.buffer[rd.len] = '\0';
    cJSON *r = cJSON_Parse(rd.buffer);
    heap_caps_free(rd.buffer);
    if (!r) break;

    cJSON *choices = cJSON_GetObjectItem(r, "choices");
    cJSON *first = choices ? cJSON_GetArrayItem(choices, 0) : nullptr;
    if (!first) { cJSON_Delete(r); break; }

    cJSON *finish = cJSON_GetObjectItem(first, "finish_reason");
    std::string finish_reason = (finish && cJSON_IsString(finish)) ? finish->valuestring : "stop";

    cJSON *msg = cJSON_GetObjectItem(first, "message");
    if (!msg) { cJSON_Delete(r); break; }

    // Check for tool_calls (web search)
    cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (finish_reason == "tool_calls" && tool_calls && cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
      ESP_LOGI(TAG, "LLM requested web search, round %d", round);

      // Append assistant message with tool_calls to messages
      cJSON *asst_msg = cJSON_Duplicate(msg, true);
      cJSON_AddItemToArray(msgs, asst_msg);

      // For each tool call, append tool result
      int tc_count = cJSON_GetArraySize(tool_calls);
      for (int tc = 0; tc < tc_count; tc++) {
        cJSON *tc_item = cJSON_GetArrayItem(tool_calls, tc);
        cJSON *tc_id = cJSON_GetObjectItem(tc_item, "id");
        cJSON *tc_func = cJSON_GetObjectItem(tc_item, "function");
        cJSON *tc_args = tc_func ? cJSON_GetObjectItem(tc_func, "arguments") : nullptr;

        cJSON *tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        if (tc_id && cJSON_IsString(tc_id))
          cJSON_AddStringToObject(tool_msg, "tool_call_id", tc_id->valuestring);
        // Pass the search arguments back as the tool result content
        if (tc_args && cJSON_IsString(tc_args))
          cJSON_AddStringToObject(tool_msg, "content", tc_args->valuestring);
        else
          cJSON_AddStringToObject(tool_msg, "content", "");
        cJSON_AddItemToArray(msgs, tool_msg);
      }

      cJSON_Delete(r);
      continue;  // next round
    }

    // Normal response, extract content
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (content && cJSON_IsString(content)) result = content->valuestring;
    cJSON_Delete(r);
    break;  // done
  }

  cJSON_Delete(msgs);
  return result;
}

// =============================================================================
// TTS (OpenAI-compatible /audio/speech)
// =============================================================================

std::string AiVoice::call_tts_(const std::string &text) {
  ESP_LOGI(TAG, "TTS...");

  // DashScope TTS uses multimodal-generation endpoint, returns a URL to audio
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", this->tts_model_.c_str());
  cJSON *input = cJSON_CreateObject();
  cJSON_AddStringToObject(input, "text", text.c_str());
  cJSON_AddStringToObject(input, "voice", this->tts_voice_.c_str());
  cJSON_AddStringToObject(input, "language_type", "English");
  cJSON_AddItemToObject(root, "input", input);

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return "";

  // Step 1: POST to TTS endpoint, get audio URL back
  ResponseData rd;
  rd.capacity = HTTP_RESP_BUFFER_SIZE;
  rd.buffer = (char *) heap_caps_calloc(rd.capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rd.len = 0;
  if (!rd.buffer) { free(json); return ""; }

  esp_http_client_config_t cfg = {};
  cfg.url = this->tts_endpoint_.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 30000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 4096;
  cfg.event_handler = text_event_handler;
  cfg.user_data = &rd;

  auto *client = esp_http_client_init(&cfg);
  std::string auth = "Bearer " + this->tts_api_key_;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, json, strlen(json));

  std::string audio_url;
  if (esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200) {
    rd.buffer[rd.len] = '\0';
    cJSON *resp = cJSON_Parse(rd.buffer);
    if (resp) {
      cJSON *output = cJSON_GetObjectItem(resp, "output");
      if (output) {
        cJSON *audio = cJSON_GetObjectItem(output, "audio");
        if (audio) {
          cJSON *url = cJSON_GetObjectItem(audio, "url");
          if (url && cJSON_IsString(url)) audio_url = url->valuestring;
        }
      }
      cJSON_Delete(resp);
    }
  } else {
    ESP_LOGE(TAG, "TTS request failed (status %d)", esp_http_client_get_status_code(client));
  }

  esp_http_client_cleanup(client);
  free(json);
  heap_caps_free(rd.buffer);

  if (audio_url.empty()) {
    ESP_LOGE(TAG, "No audio URL in TTS response");
    return "";
  }

  // Force HTTPS (DashScope returns http:// but OSS supports both)
  if (audio_url.compare(0, 7, "http://") == 0) {
    audio_url = "https://" + audio_url.substr(7);
  }
  ESP_LOGI(TAG, "TTS audio URL: %.80s...", audio_url.c_str());
  return audio_url;
}

// =============================================================================
// Download WAV and play through speaker directly
// =============================================================================

bool AiVoice::download_and_play_(const std::string &url) {
  ESP_LOGI(TAG, "Downloading audio...");
  this->tts_data_size_ = 0;

  struct DlCtx { AiVoice *self; };
  DlCtx ctx = {this};

  auto handler = [](esp_http_client_event_t *evt) -> esp_err_t {
    DlCtx *c = (DlCtx *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
      size_t space = TTS_BUFFER_SIZE - c->self->tts_data_size_;
      size_t n = std::min((size_t) evt->data_len, space);
      if (n > 0) {
        memcpy(c->self->tts_buffer_ + c->self->tts_data_size_, evt->data, n);
        c->self->tts_data_size_ += n;
      }
    }
    return ESP_OK;
  };

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 30000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 8192;
  cfg.event_handler = handler;
  cfg.user_data = &ctx;

  auto *client = esp_http_client_init(&cfg);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200 || this->tts_data_size_ <= 44) {
    ESP_LOGE(TAG, "Download failed: err=%s status=%d size=%u", esp_err_to_name(err), status, this->tts_data_size_);
    this->last_error_ = "DL fail s=" + std::to_string(status) + " sz=" + std::to_string(this->tts_data_size_);
    return false;
  }

  // Skip 44-byte WAV header, keep raw PCM (24kHz 16-bit mono)
  size_t pcm_size = this->tts_data_size_ - 44;
  uint8_t *pcm = this->tts_buffer_ + 44;

  ESP_LOGI(TAG, "Playing %u bytes PCM (24kHz 16-bit mono)...", pcm_size);

  // Play through speaker using set_audio_stream_info (the key!)
  this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, 24000));
  this->speaker_->start();

  size_t written = 0;
  uint32_t timeout_ms = 60000;  // 60s max playback
  uint32_t start = millis();
  while (written < pcm_size && (millis() - start) < timeout_ms) {
    if (this->abort_requested_) break;
    size_t chunk = std::min((size_t) 512, pcm_size - written);
    size_t w = this->speaker_->play(pcm + written, chunk);
    if (w > 0) {
      written += w;
    } else {
      // Speaker buffer full, wait for it to drain a bit
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  ESP_LOGI(TAG, "Fed %u/%u bytes to speaker, draining...", written, pcm_size);

  // Wait for speaker to finish playing all buffered data
  uint32_t drain_start = millis();
  while (this->speaker_->has_buffered_data() && (millis() - drain_start) < 30000) {
    if (this->abort_requested_) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  this->speaker_->finish();
  ESP_LOGI(TAG, "Playback done");
  return true;
}

}  // namespace ai_voice
}  // namespace esphome
