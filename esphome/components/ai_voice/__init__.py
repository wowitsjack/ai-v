"""
AI Voice — ESPHome external component.

Generic voice assistant: configurable STT + LLM + TTS endpoints.
Multi-provider support with button cycling.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_NAME
from esphome.components import microphone, speaker, media_player

DEPENDENCIES = ["network", "microphone"]
AUTO_LOAD = ["microphone", "audio"]

CONF_SPEAKER = "speaker"
CONF_MEDIA_PLAYER = "media_player"
CONF_PROVIDERS = "providers"
CONF_ENDPOINT = "endpoint"
CONF_API_KEY = "api_key"
CONF_MODEL = "model"
CONF_FORMAT = "format"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_MAX_TOKENS = "max_tokens"
CONF_STT_ENDPOINT = "stt_endpoint"
CONF_STT_API_KEY = "stt_api_key"
CONF_STT_MODEL = "stt_model"
CONF_TTS_ENDPOINT = "tts_endpoint"
CONF_TTS_API_KEY = "tts_api_key"
CONF_TTS_MODEL = "tts_model"
CONF_TTS_VOICE = "tts_voice"
CONF_CONVERSATION_TURNS = "conversation_turns"
CONF_WEB_SEARCH = "web_search"

ai_voice_ns = cg.esphome_ns.namespace("ai_voice")
AiVoice = ai_voice_ns.class_("AiVoice", cg.Component)
LlmProvider = ai_voice_ns.struct("LlmProvider")

StartRecordingAction = ai_voice_ns.class_("StartRecordingAction", automation.Action)
StopRecordingAction = ai_voice_ns.class_("StopRecordingAction", automation.Action)
CycleProviderAction = ai_voice_ns.class_("CycleProviderAction", automation.Action)
AbortAction = ai_voice_ns.class_("AbortAction", automation.Action)

PROVIDER_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME): cv.string,
    cv.Required(CONF_ENDPOINT): cv.string,
    cv.Required(CONF_API_KEY): cv.string,
    cv.Required(CONF_MODEL): cv.string,
    cv.Optional(CONF_FORMAT, default="openai"): cv.one_of("anthropic", "openai", lower=True),
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AiVoice),
    cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
    cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),

    cv.Required(CONF_PROVIDERS): cv.ensure_list(PROVIDER_SCHEMA),

    cv.Optional(CONF_SYSTEM_PROMPT,
                default="You are a concise voice assistant. Keep responses under 3 sentences."): cv.string,
    cv.Optional(CONF_MAX_TOKENS, default=256): cv.int_range(min=1, max=4096),

    cv.Required(CONF_STT_ENDPOINT): cv.string,
    cv.Required(CONF_STT_API_KEY): cv.string,
    cv.Optional(CONF_STT_MODEL, default="qwen3-asr-flash-2026-02-10"): cv.string,

    cv.Required(CONF_TTS_ENDPOINT): cv.string,
    cv.Optional(CONF_TTS_API_KEY): cv.string,
    cv.Optional(CONF_TTS_MODEL, default="qwen3-tts-flash"): cv.string,
    cv.Optional(CONF_TTS_VOICE, default="Jennifer"): cv.string,

    cv.Optional(CONF_CONVERSATION_TURNS, default=5): cv.int_range(min=1, max=20),
    cv.Optional(CONF_WEB_SEARCH, default=True): cv.boolean,
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

    # Providers
    for prov in config[CONF_PROVIDERS]:
        prov_struct = cg.StructInitializer(
            LlmProvider,
            ("name", prov[CONF_NAME]),
            ("endpoint", prov[CONF_ENDPOINT]),
            ("api_key", prov[CONF_API_KEY]),
            ("model", prov[CONF_MODEL]),
            ("format", prov[CONF_FORMAT]),
        )
        cg.add(var.add_provider(prov_struct))

    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))
    cg.add(var.set_max_tokens(config[CONF_MAX_TOKENS]))

    cg.add(var.set_stt_endpoint(config[CONF_STT_ENDPOINT]))
    cg.add(var.set_stt_api_key(config[CONF_STT_API_KEY]))
    cg.add(var.set_stt_model(config[CONF_STT_MODEL]))

    cg.add(var.set_tts_endpoint(config[CONF_TTS_ENDPOINT]))
    tts_key = config.get(CONF_TTS_API_KEY, config[CONF_STT_API_KEY])
    cg.add(var.set_tts_api_key(tts_key))
    cg.add(var.set_tts_model(config[CONF_TTS_MODEL]))
    cg.add(var.set_tts_voice(config[CONF_TTS_VOICE]))

    cg.add(var.set_conversation_turns(config[CONF_CONVERSATION_TURNS]))
    cg.add(var.set_web_search_enabled(config[CONF_WEB_SEARCH]))

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

@automation.register_action("ai_voice.cycle_provider", CycleProviderAction, AI_VOICE_ACTION_SCHEMA)
async def cycle_provider_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)

@automation.register_action("ai_voice.abort", AbortAction, AI_VOICE_ACTION_SCHEMA)
async def abort_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
