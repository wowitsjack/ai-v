#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/media_player/media_player.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string>
#include <vector>

namespace esphome {
namespace ai_voice {

// Voice phase IDs for LED control
static constexpr int PHASE_IDLE = 1;
static constexpr int PHASE_STT = 2;
static constexpr int PHASE_RECORDING = 3;
static constexpr int PHASE_THINKING = 4;   // LLM
static constexpr int PHASE_SPEAKING = 5;
static constexpr int PHASE_NOT_READY = 10;
static constexpr int PHASE_ERROR = 11;

// Audio constants
static constexpr int SAMPLE_RATE = 16000;
static constexpr int MIC_BYTES_PER_STEREO_PAIR = 8;
static constexpr int MIC_BYTES_PER_MONO_SAMPLE = 2;
static constexpr size_t CAPTURE_BUFFER_SIZE = 320000;   // 10s @ 16kHz 16-bit mono
static constexpr size_t TTS_BUFFER_SIZE = 3670016;       // 3.5MB ~75s of 24kHz 16-bit PCM
static constexpr size_t HTTP_RESP_BUFFER_SIZE = 65536;

struct ConversationTurn {
  std::string role;
  std::string content;
};

class AiVoice : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Config setters
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_media_player(media_player::MediaPlayer *mp) { this->media_player_ = mp; }

  void set_llm_endpoint(const std::string &v) { this->llm_endpoint_ = v; }
  void set_llm_api_key(const std::string &v) { this->llm_api_key_ = v; }
  void set_llm_model_default(const std::string &v) { this->llm_model_default_ = v; }
  void set_llm_model_alt(const std::string &v) { this->llm_model_alt_ = v; }
  void set_llm_api_format(const std::string &v) { this->llm_api_format_ = v; }
  void set_system_prompt(const std::string &v) { this->system_prompt_ = v; }
  void set_max_tokens(int v) { this->max_tokens_ = v; }

  void set_stt_endpoint(const std::string &v) { this->stt_endpoint_ = v; }
  void set_stt_api_key(const std::string &v) { this->stt_api_key_ = v; }
  void set_stt_model(const std::string &v) { this->stt_model_ = v; }

  void set_tts_endpoint(const std::string &v) { this->tts_endpoint_ = v; }
  void set_tts_api_key(const std::string &v) { this->tts_api_key_ = v; }
  void set_tts_model(const std::string &v) { this->tts_model_ = v; }
  void set_tts_voice(const std::string &v) { this->tts_voice_ = v; }

  void set_conversation_turns(int v) { this->max_turns_ = v; }

  // Actions
  void start_recording();
  void stop_recording();
  void toggle_model();
  void abort();

  // State queries for YAML
  int get_voice_phase() const { return this->voice_phase_; }
  bool phase_changed() { bool c = this->phase_changed_; this->phase_changed_ = false; return c; }
  bool is_using_alt_model() const { return this->use_alt_model_; }
  const std::string &get_last_error() const { return this->last_error_; }
  const std::string &get_last_transcript() const { return this->last_transcript_; }

 protected:
  // Config
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  media_player::MediaPlayer *media_player_{nullptr};

  std::string llm_endpoint_;
  std::string llm_api_key_;
  std::string llm_model_default_;
  std::string llm_model_alt_;
  std::string llm_api_format_;  // "anthropic" or "openai"
  std::string system_prompt_;
  int max_tokens_{256};

  std::string stt_endpoint_;
  std::string stt_api_key_;
  std::string stt_model_;

  std::string tts_endpoint_;
  std::string tts_api_key_;
  std::string tts_model_;
  std::string tts_voice_;

  int max_turns_{5};

  // State
  enum State : uint8_t { STATE_IDLE = 0, STATE_RECORDING, STATE_PROCESSING, STATE_SPEAKING, STATE_ERROR };
  volatile State state_{STATE_IDLE};
  volatile int voice_phase_{PHASE_NOT_READY};
  bool use_alt_model_{false};
  bool ready_{false};
  volatile bool phase_changed_{false};
  std::string last_error_;
  std::string last_transcript_;

  // Audio capture buffer (PSRAM)
  uint8_t *capture_buffer_{nullptr};
  volatile size_t capture_write_pos_{0};
  volatile bool capturing_{false};

  // TTS receive buffer (PSRAM) for download+play
  uint8_t *tts_buffer_{nullptr};
  size_t tts_data_size_{0};

  // Pipeline task
  volatile bool pipeline_requested_{false};
  volatile bool abort_requested_{false};
  TaskHandle_t pipeline_task_handle_{nullptr};
  StackType_t *pipeline_task_stack_{nullptr};
  StaticTask_t pipeline_task_tcb_;
  static void pipeline_task_func_(void *param);

  // Conversation history
  std::vector<ConversationTurn> conversation_;

  // Pipeline steps
  std::string call_stt_(const uint8_t *pcm_data, size_t pcm_len);
  std::string call_llm_(const std::string &user_text);
  std::string call_llm_anthropic_(const std::string &user_text, const std::string &model);
  std::string call_llm_openai_(const std::string &user_text, const std::string &model);
  std::string call_tts_(const std::string &text);  // returns audio URL
  bool download_and_play_(const std::string &url);  // download WAV and play via speaker

  // Helpers
  void build_wav_header_(uint8_t *header, size_t pcm_data_size);
  void set_phase_(int phase);
  void trim_conversation_();
};

// =============================================================================
// Automation Actions
// =============================================================================

template<typename... Ts> class StartRecordingAction : public Action<Ts...> {
 public:
  explicit StartRecordingAction(AiVoice *p) : parent_(p) {}
  void play(const Ts &...x) override { this->parent_->start_recording(); }
 protected:
  AiVoice *parent_;
};

template<typename... Ts> class StopRecordingAction : public Action<Ts...> {
 public:
  explicit StopRecordingAction(AiVoice *p) : parent_(p) {}
  void play(const Ts &...x) override { this->parent_->stop_recording(); }
 protected:
  AiVoice *parent_;
};

template<typename... Ts> class ToggleModelAction : public Action<Ts...> {
 public:
  explicit ToggleModelAction(AiVoice *p) : parent_(p) {}
  void play(const Ts &...x) override { this->parent_->toggle_model(); }
 protected:
  AiVoice *parent_;
};

template<typename... Ts> class AbortAction : public Action<Ts...> {
 public:
  explicit AbortAction(AiVoice *p) : parent_(p) {}
  void play(const Ts &...x) override { this->parent_->abort(); }
 protected:
  AiVoice *parent_;
};

}  // namespace ai_voice
}  // namespace esphome
