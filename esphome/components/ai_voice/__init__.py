"""
AI Voice — ESPHome external component.

Generic voice assistant: configurable STT + LLM + TTS endpoints.
Supports any OpenAI-compatible or Anthropic-compatible API.
No companion server needed. Hold center button to talk, release to process.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MICROPHONE
from esphome.components import microphone, speaker, media_player

DEPENDENCIES = ["network", "microphone"]
AUTO_LOAD = ["microphone", "audio"]

CONF_SPEAKER = "speaker"
CONF_MEDIA_PLAYER = "media_player"

# LLM config
CONF_LLM_ENDPOINT = "llm_endpoint"
CONF_LLM_API_KEY = "llm_api_key"
CONF_LLM_MODEL_DEFAULT = "llm_model_default"
CONF_LLM_MODEL_ALT = "llm_model_alt"
CONF_LLM_API_FORMAT = "llm_api_format"  # "anthropic" or "openai"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_MAX_TOKENS = "max_tokens"

# STT config
CONF_STT_ENDPOINT = "stt_endpoint"
CONF_STT_API_KEY = "stt_api_key"
CONF_STT_MODEL = "stt_model"

# TTS config
CONF_TTS_ENDPOINT = "tts_endpoint"
CONF_TTS_API_KEY = "tts_api_key"
CONF_TTS_MODEL = "tts_model"
CONF_TTS_VOICE = "tts_voice"

CONF_CONVERSATION_TURNS = "conversation_turns"

ai_voice_ns = cg.esphome_ns.namespace("ai_voice")
AiVoice = ai_voice_ns.class_("AiVoice", cg.Component)

# Actions
StartRecordingAction = ai_voice_ns.class_("StartRecordingAction", automation.Action)
StopRecordingAction = ai_voice_ns.class_("StopRecordingAction", automation.Action)
ToggleModelAction = ai_voice_ns.class_("ToggleModelAction", automation.Action)
AbortAction = ai_voice_ns.class_("AbortAction", automation.Action)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AiVoice),
    cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
    cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),

    # LLM (default: Anthropic Claude)
    cv.Optional(CONF_LLM_ENDPOINT, default="https://api.anthropic.com/v1/messages"): cv.string,
    cv.Required(CONF_LLM_API_KEY): cv.string,
    cv.Optional(CONF_LLM_MODEL_DEFAULT, default="claude-haiku-4-5-20251001"): cv.string,
    cv.Optional(CONF_LLM_MODEL_ALT, default="claude-sonnet-4-20250514"): cv.string,
    cv.Optional(CONF_LLM_API_FORMAT, default="anthropic"): cv.one_of("anthropic", "openai", lower=True),
    cv.Optional(CONF_SYSTEM_PROMPT,
                default="You are a concise voice assistant. Keep responses under 3 sentences."): cv.string,
    cv.Optional(CONF_MAX_TOKENS, default=256): cv.int_range(min=1, max=4096),

    # STT (default: DashScope Paraformer)
    cv.Optional(CONF_STT_ENDPOINT,
                default="https://dashscope-intl.aliyuncs.com/compatible-mode/v1/audio/transcriptions"): cv.string,
    cv.Required(CONF_STT_API_KEY): cv.string,
    cv.Optional(CONF_STT_MODEL, default="paraformer-v2"): cv.string,

    # TTS (default: DashScope CosyVoice)
    cv.Optional(CONF_TTS_ENDPOINT,
                default="https://dashscope-intl.aliyuncs.com/compatible-mode/v1/audio/speech"): cv.string,
    cv.Optional(CONF_TTS_API_KEY): cv.string,  # defaults to stt_api_key if not set
    cv.Optional(CONF_TTS_MODEL, default="cosyvoice-v2"): cv.string,
    cv.Optional(CONF_TTS_VOICE, default="longxiaochun_v2"): cv.string,

    cv.Optional(CONF_CONVERSATION_TURNS, default=5): cv.int_range(min=1, max=20),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    mp = await cg.get_variable(config[CONF_MEDIA_PLAYER])
    cg.add(var.set_media_player(mp))

    # LLM
    cg.add(var.set_llm_endpoint(config[CONF_LLM_ENDPOINT]))
    cg.add(var.set_llm_api_key(config[CONF_LLM_API_KEY]))
    cg.add(var.set_llm_model_default(config[CONF_LLM_MODEL_DEFAULT]))
    cg.add(var.set_llm_model_alt(config[CONF_LLM_MODEL_ALT]))
    cg.add(var.set_llm_api_format(config[CONF_LLM_API_FORMAT]))
    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))
    cg.add(var.set_max_tokens(config[CONF_MAX_TOKENS]))

    # STT
    cg.add(var.set_stt_endpoint(config[CONF_STT_ENDPOINT]))
    cg.add(var.set_stt_api_key(config[CONF_STT_API_KEY]))
    cg.add(var.set_stt_model(config[CONF_STT_MODEL]))

    # TTS
    cg.add(var.set_tts_endpoint(config[CONF_TTS_ENDPOINT]))
    tts_key = config.get(CONF_TTS_API_KEY, config[CONF_STT_API_KEY])
    cg.add(var.set_tts_api_key(tts_key))
    cg.add(var.set_tts_model(config[CONF_TTS_MODEL]))
    cg.add(var.set_tts_voice(config[CONF_TTS_VOICE]))

    cg.add(var.set_conversation_turns(config[CONF_CONVERSATION_TURNS]))

    cg.add_build_flag("-DUSE_AI_VOICE")


# --- Automation Actions ---

AI_VOICE_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(AiVoice),
})


@automation.register_action("ai_voice.start_recording", StartRecordingAction, AI_VOICE_ACTION_SCHEMA)
async def start_recording_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("ai_voice.stop_recording", StopRecordingAction, AI_VOICE_ACTION_SCHEMA)
async def stop_recording_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("ai_voice.toggle_model", ToggleModelAction, AI_VOICE_ACTION_SCHEMA)
async def toggle_model_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("ai_voice.abort", AbortAction, AI_VOICE_ACTION_SCHEMA)
async def abort_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
