/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Tool implementations of the LAN MCP Agent.
//
// Porting note: the robot/reminder tool bodies are copied from the official
// cloud tools in hal/hal_mcp.cpp (parameter ranges and semantics kept
// identical so client prompts transfer 1:1); they were re-bound to the
// standalone registry instead of the vendored xiaozhi McpServer.
#include <sdkconfig.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <cJSON.h>

#include <memory>
#include <string>

#include <hal/hal.h>
#include "../platform/device_access.h"
#include "../service/lan_service.h"
#include "mcp_photo.h"
#include "../protocol/mcp_registry.h"
#include <stackchan/avatar/avatar.h>
#include <stackchan/stackchan.h>

using namespace stackchan;

namespace mcp_lan {

namespace {

// ---------------- argument helpers ----------------

bool get_int(const cJSON* args, const char* key, int* out, ToolOutput* err, bool required, int def, int lo, int hi)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (v == nullptr) {
        if (required) {
            err->is_error = true;
            err->text     = std::string("missing required argument: ") + key;
            return false;
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v)) {
        err->is_error = true;
        err->text     = std::string("argument must be a number: ") + key;
        return false;
    }
    int value = v->valueint;
    if (value < lo || value > hi) {
        err->is_error = true;
        err->text     = key + std::string(" must be between ") + std::to_string(lo) + " and " + std::to_string(hi);
        return false;
    }
    *out = value;
    return true;
}

bool get_double(const cJSON* args, const char* key, double* out, ToolOutput* err, bool required, double def, double lo, double hi)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (v == nullptr) {
        if (required) {
            err->is_error = true;
            err->text     = std::string("missing required argument: ") + key;
            return false;
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v)) {
        err->is_error = true;
        err->text     = std::string("argument must be a number: ") + key;
        return false;
    }
    double value = v->valuedouble;
    if (value < lo || value > hi) {
        err->is_error = true;
        err->text     = key + std::string(" must be between ") + std::to_string(lo) + " and " + std::to_string(hi);
        return false;
    }
    *out = value;
    return true;
}

bool get_bool(const cJSON* args, const char* key, bool* out, ToolOutput* err, bool required, bool def)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (v == nullptr) {
        if (required) {
            err->is_error = true;
            err->text     = std::string("missing required argument: ") + key;
            return false;
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsBool(v)) {
        err->is_error = true;
        err->text     = std::string("argument must be a boolean: ") + key;
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

bool get_string(const cJSON* args, const char* key, std::string* out, ToolOutput* err, bool required, const char* def, size_t max_len)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (v == nullptr) {
        if (required) {
            err->is_error = true;
            err->text     = std::string("missing required argument: ") + key;
            return false;
        }
        *out = def != nullptr ? def : "";
        return true;
    }
    if (!cJSON_IsString(v) || v->valuestring == nullptr) {
        err->is_error = true;
        err->text     = std::string("argument must be a string: ") + key;
        return false;
    }
    if (strlen(v->valuestring) > max_len) {
        err->is_error = true;
        err->text     = key + std::string(" is too long (max ") + std::to_string(max_len) + " bytes)";
        return false;
    }
    out->assign(v->valuestring);
    return true;
}

ToolOutput ok_text(const std::string& text)
{
    ToolOutput out;
    out.text = text;
    return out;
}

ToolOutput ok_json(cJSON* node)
{
    char* printed       = cJSON_PrintUnformatted(node);
    ToolOutput out;
    out.text            = printed != nullptr ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(node);
    return out;
}

ToolOutput ui_error()
{
    ToolOutput out;
    out.is_error = true;
    out.text = "UI unavailable";
    return out;
}

// ---------------- device tools ----------------

ToolOutput tool_get_device_status(const cJSON*)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", mcp_dev::wifi_connected());
    cJSON_AddStringToObject(wifi, "ssid", mcp_dev::wifi_ssid().c_str());
    cJSON_AddStringToObject(wifi, "ip", mcp_dev::wifi_ip().c_str());
    cJSON_AddNumberToObject(wifi, "rssi", mcp_dev::wifi_rssi());
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON* battery = cJSON_CreateObject();
    cJSON_AddNumberToObject(battery, "level", GetHAL().getBatteryLevel());
    cJSON_AddBoolToObject(battery, "charging", GetHAL().isBatteryCharging());
    cJSON_AddItemToObject(root, "battery", battery);
    cJSON* speaker = cJSON_CreateObject();
    cJSON_AddNumberToObject(speaker, "volume", GetHAL().getSpeakerVolume());
    cJSON_AddItemToObject(root, "speaker", speaker);
    cJSON* screen = cJSON_CreateObject();
    cJSON_AddNumberToObject(screen, "brightness", GetHAL().getBackLightBrightness());
    cJSON_AddItemToObject(root, "screen", screen);
    cJSON* memory = cJSON_CreateObject();
    cJSON_AddNumberToObject(memory, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(memory, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddItemToObject(root, "memory", memory);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    return ok_json(root);
}

ToolOutput tool_get_system_info(const cJSON*)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "mcp_server_version", "1.0.0");
    cJSON_AddStringToObject(root, "chip", "esp32s3");
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(root, "mac", GetHAL().getFactoryMacString(":").c_str());
    cJSON_AddStringToObject(root, "ip", mcp_dev::wifi_ip().c_str());
    cJSON_AddNumberToObject(root, "mcp_port", CONFIG_MCP_AGENT_PORT);
    return ok_json(root);
}

ToolOutput tool_set_volume(const cJSON* args)
{
    ToolOutput err;
    int volume = 0;
    if (!get_int(args, "volume", &volume, &err, true, 0, 0, 100)) {
        return err;
    }
    {
        std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
        GetHAL().setSpeakerVolume((uint8_t)volume);
    }
    return ok_text("ok");
}

ToolOutput tool_get_screen_info(const cJSON*)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "width", 320);
    cJSON_AddNumberToObject(root, "height", 240);
    cJSON_AddBoolToObject(root, "monochrome", false);
    return ok_json(root);
}

ToolOutput tool_set_brightness(const cJSON* args)
{
    ToolOutput err;
    int brightness = 0;
    if (!get_int(args, "brightness", &brightness, &err, true, 0, 0, 100)) {
        return err;
    }
    GetHAL().setBackLightBrightness((uint8_t)brightness);
    return ok_text("ok");
}

// ---------------- display tools ----------------

bool map_emotion(const std::string& name, avatar::Emotion* out)
{
    if (name == "neutral") *out = avatar::Emotion::Neutral;
    else if (name == "happy") *out = avatar::Emotion::Happy;
    else if (name == "laughing") *out = avatar::Emotion::Happy;
    else if (name == "angry") *out = avatar::Emotion::Angry;
    else if (name == "sad") *out = avatar::Emotion::Sad;
    else if (name == "crying") *out = avatar::Emotion::Sad;
    else if (name == "sleepy") *out = avatar::Emotion::Sleepy;
    else if (name == "doubtful") *out = avatar::Emotion::Doubt;
    else return false;
    return true;
}

ToolOutput tool_set_emotion(LanService& service, const cJSON* args)
{
    ToolOutput err;
    std::string emotion;
    if (!get_string(args, "emotion", &emotion, &err, true, nullptr, 16)) {
        return err;
    }
    avatar::Emotion mapped;
    if (!map_emotion(emotion, &mapped)) {
        err.is_error = true;
        err.text = "unknown emotion: " + emotion +
                   " (supported: neutral, happy, laughing, angry, sad, crying, sleepy, doubtful)";
        return err;
    }

    auto out = std::make_shared<ToolOutput>();
    const bool executed = service.ui_execute([out, mapped] {
        if (!GetStackChan().hasAvatar()) {
            out->is_error = true;
            out->text = "no avatar attached (MCP Agent app not running)";
            return;
        }
        GetStackChan().avatar().setEmotion(mapped);
    });
    if (!executed) {
        return ui_error();
    }
    return *out;
}

ToolOutput tool_show_message(LanService& service, const cJSON* args)
{
    ToolOutput err;
    std::string text;
    int duration_ms = 0;
    if (!get_string(args, "text", &text, &err, true, nullptr, 200)) {
        return err;
    }
    if (!get_int(args, "duration_ms", &duration_ms, &err, false, 10000, 0, 600000)) {
        return err;
    }
    if (!service.show_message(text, (uint32_t)duration_ms)) {
        return ui_error();
    }
    return ok_text("ok");
}

ToolOutput tool_clear_message(LanService& service, const cJSON*)
{
    return service.clear_message() ? ok_text("ok") : ui_error();
}

// ---------------- robot tools (ported from hal_mcp.cpp, same semantics) ----

ToolOutput tool_get_head_angles(LanService& service, const cJSON*)
{
    auto out = std::make_shared<ToolOutput>();
    const bool executed = service.ui_execute([out] {
        auto& motion = GetStackChan().motion();
        const int current_yaw = motion.yawServo().getCurrentAngle() / 10;
        const int current_pitch = motion.pitchServo().getCurrentAngle() / 10;
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"yaw\": %d, \"pitch\": %d}", current_yaw, current_pitch);
        *out = ok_text(buf);
    });
    return executed ? *out : ui_error();
}

ToolOutput tool_set_head_angles(LanService& service, const cJSON* args)
{
    ToolOutput err;
    int yaw = 0, pitch = 0, speed = 0;
    // -9999 means "leave unchanged" (official semantics).
    if (!get_int(args, "yaw", &yaw, &err, false, -9999, -9999, 128)) return err;
    if (!get_int(args, "pitch", &pitch, &err, false, -9999, -9999, 90)) return err;
    if (!get_int(args, "speed", &speed, &err, false, 150, 100, 1000)) return err;

    auto out = std::make_shared<ToolOutput>();
    const bool executed = service.ui_execute([out, yaw, pitch, speed] {
        auto& motion = GetStackChan().motion();
        // Position writes are ignored while torque is released. Enable it
        // for the move; the servo stack auto-releases after rest.
        motion.setTorqueEnabled(true);
        if (pitch != -9999) {
            motion.pitchServo().moveWithSpeed(pitch * 10, speed);
        }
        if (yaw != -9999) {
            motion.yawServo().moveWithSpeed(yaw * 10, speed);
        }
        *out = ok_text("ok");
    });
    return executed ? *out : ui_error();
}

ToolOutput tool_look_at(LanService& service, const cJSON* args)
{
    ToolOutput err;
    double x = 0, y = 0;
    if (!get_double(args, "x", &x, &err, true, 0, -1.0, 1.0)) return err;
    if (!get_double(args, "y", &y, &err, true, 0, -1.0, 1.0)) return err;

    const bool executed = service.ui_execute([x, y] {
        auto& motion = GetStackChan().motion();
        motion.setTorqueEnabled(true);
        motion.lookAtNormalized((float)x, (float)y);
    });
    return executed ? ok_text("ok") : ui_error();
}

ToolOutput tool_go_home(LanService& service, const cJSON*)
{
    const bool executed = service.ui_execute([] {
        auto& motion = GetStackChan().motion();
        motion.setTorqueEnabled(true);
        motion.goHome(500);
    });
    return executed ? ok_text("ok") : ui_error();
}

ToolOutput tool_set_led_color(LanService& service, const cJSON* args)
{
    ToolOutput err;
    int r = 0, g = 0, b = 0;
    if (!get_int(args, "red", &r, &err, true, 0, 0, 168)) return err;
    if (!get_int(args, "green", &g, &err, true, 0, 0, 168)) return err;
    if (!get_int(args, "blue", &b, &err, true, 0, 0, 168)) return err;

    const bool executed = service.ui_execute([r, g, b] {
        GetStackChan().leftNeonLight().setColor(r, g, b);
        GetStackChan().rightNeonLight().setColor(r, g, b);
    });
    return executed ? ok_text("ok") : ui_error();
}

// ---------------- reminder tools (ported from hal_mcp.cpp) ----------------

ToolOutput tool_create_reminder(LanService& service, const cJSON* args)
{
    ToolOutput err;
    int duration_seconds = 0;
    std::string message;
    bool repeat = false;
    if (!get_int(args, "duration_seconds", &duration_seconds, &err, true, 0, 1, 86400)) return err;
    if (!get_string(args, "message", &message, &err, false, "Time's up!", 120)) return err;
    if (!get_bool(args, "repeat", &repeat, &err, false, false)) return err;

    const int id = service.create_reminder((uint32_t)duration_seconds * 1000, message, repeat);
    if (id < 0) {
        return ui_error();
    }
    return ok_text(std::to_string(id));
}

ToolOutput tool_get_reminders(LanService& service, const cJSON*)
{
    cJSON* array = cJSON_CreateArray();
    for (const auto& r : service.get_reminders()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r.id);
        cJSON_AddNumberToObject(item, "duration_ms", (double)r.durationMs);
        cJSON_AddStringToObject(item, "message", r.message.c_str());
        cJSON_AddBoolToObject(item, "repeat", r.repeat);
        cJSON_AddItemToArray(array, item);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "reminders", array);
    return ok_json(root);
}

ToolOutput tool_stop_reminder(LanService& service, const cJSON* args)
{
    ToolOutput err;
    int id = 0;
    if (!get_int(args, "id", &id, &err, true, 0, -1000000, 1000000)) return err;
    if (!service.stop_reminder(id)) {
        return ui_error();
    }
    return ok_text("ok");
}

// ---------------- camera tool ----------------

ToolOutput tool_take_photo(LanService& service, const cJSON* args)
{
    ToolOutput err;
    int quality = 0;
    bool embed  = false;
    if (!get_int(args, "quality", &quality, &err, false, 20, 1, 100)) return err;
    if (!get_bool(args, "embed", &embed, &err, false, false)) return err;

    std::string cap_err;
    if (!mcp_photo::capture(quality, &cap_err)) {
        err.is_error = true;
        err.text     = cap_err;
        return err;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "url", (service.base_url() + "/photo.jpg").c_str());
    cJSON_AddNumberToObject(root, "width", mcp_photo::width());
    cJSON_AddNumberToObject(root, "height", mcp_photo::height());
    {
        mcp_photo::PhotoSnapshot snapshot;
        if (mcp_photo::get(&snapshot)) {
            cJSON_AddNumberToObject(root, "size_bytes", (double)snapshot.bytes->size());
        }
    }
    cJSON_AddNumberToObject(root, "quality", mcp_photo::quality());
    ToolOutput out = ok_json(root);
    if (embed) {
        std::string b64_err;
        out.image_b64 = mcp_photo::base64(&b64_err);
        if (!b64_err.empty()) {
            out.is_error  = true;
            out.text      = b64_err;
            out.image_b64.clear();
        }
    }
    return out;
}

// ---------------- audio stream discovery tools ----------------

std::string ws_base()
{
    const std::string ip = mcp_dev::wifi_ip();
    if (ip.empty() || ip == "0.0.0.0") {
        return {};
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "ws://%s:%d", ip.c_str(), CONFIG_MCP_AGENT_PORT);
    return buf;
}

ToolOutput tool_mic_stream(const cJSON*)
{
    const std::string base = ws_base();
    if (base.empty()) {
        ToolOutput err;
        err.is_error = true;
        err.text     = "device is not connected to a network";
        return err;
    }
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"url\": \"%s/mic\", \"codec\": \"opus\", \"sample_rate\": 16000, \"channels\": 1, \"frame_ms\": 60}",
             base.c_str());
    return ok_text(buf);
}

ToolOutput tool_spk_stream(const cJSON*)
{
    const std::string base = ws_base();
    if (base.empty()) {
        ToolOutput err;
        err.is_error = true;
        err.text     = "device is not connected to a network";
        return err;
    }
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"url\": \"%s/speaker\", \"codec\": \"opus\", \"sample_rate\": 16000, \"channels\": 1, \"frame_ms\": 60}",
             base.c_str());
    return ok_text(buf);
}

// ---------------- owner tool ----------------

ToolOutput tool_reboot(LanService& service, const cJSON*)
{
    // The JSON-RPC layer reboots after the HTTP reply went out.
    service.request_reboot();
    return ok_text("rebooting");
}

}  // namespace

void register_all_tools(LanService& service)
{
    auto& reg = service.registry();

    reg.add({"self.get_device_status",
             "Get device status: wifi, battery, speaker volume, screen brightness, memory, uptime.",
             R"({})",
             tool_get_device_status});
    reg.add({"self.get_system_info",
             "Get system info: firmware version, MAC, IP, MCP port.",
             R"({})",
             tool_get_system_info});
    reg.add({"self.audio_speaker.set_volume",
             "Set speaker volume (0-100).",
             R"({"volume":{"type":"integer","minimum":0,"maximum":100}})",
             tool_set_volume});
    reg.add({"self.screen.get_info",
             "Get screen info.",
             R"({})",
             tool_get_screen_info});
    reg.add({"self.screen.set_brightness",
             "Set screen brightness (0-100).",
             R"({"brightness":{"type":"integer","minimum":0,"maximum":100}})",
             tool_set_brightness});

    reg.add({"self.display.set_emotion",
             "Set the robot's facial expression. One of: neutral, happy, laughing, angry, sad, crying, sleepy, doubtful.",
             R"({"emotion":{"type":"string","enum":["neutral","happy","laughing","angry","sad","crying","sleepy","doubtful"]}})",
             [&service](const cJSON* args) { return tool_set_emotion(service, args); }});
    reg.add({"self.display.show_message",
             "Show a speech bubble with text on the robot's screen. duration_ms=0 keeps it until cleared.",
             R"({"text":{"type":"string","maxLength":200},"duration_ms":{"type":"integer","minimum":0,"maximum":600000,"default":10000}})",
             [&service](const cJSON* args) { return tool_show_message(service, args); }});
    reg.add({"self.display.clear_message",
             "Clear the speech bubble.",
             R"({})",
             [&service](const cJSON* args) { return tool_clear_message(service, args); }});

    reg.add({"self.robot.get_head_angles",
             "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
             R"({})",
             [&service](const cJSON* args) { return tool_get_head_angles(service, args); }});
    reg.add({"self.robot.set_head_angles",
             "Adjust head position. GUIDELINES: 1. For natural interaction, stay within +/- 45 degrees. "
             "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
             "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
             "Speed(100-1000, 150 is natural). Omit yaw/pitch to leave it unchanged.",
             R"({"yaw":{"type":"integer","minimum":-128,"maximum":128},"pitch":{"type":"integer","minimum":0,"maximum":90},"speed":{"type":"integer","minimum":100,"maximum":1000,"default":150}})",
             [&service](const cJSON* args) { return tool_set_head_angles(service, args); }});
    reg.add({"self.robot.look_at",
             "Point the head at a normalized position. x/y in [-1,1] spans the full motion range.",
             R"({"x":{"type":"number","minimum":-1,"maximum":1},"y":{"type":"number","minimum":-1,"maximum":1}})",
             [&service](const cJSON* args) { return tool_look_at(service, args); }});
    reg.add({"self.robot.go_home",
             "Move the head back to the neutral position.",
             R"({})",
             [&service](const cJSON* args) { return tool_go_home(service, args); }});
    reg.add({"self.robot.set_led_color",
             "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
             "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
             R"({"red":{"type":"integer","minimum":0,"maximum":168},"green":{"type":"integer","minimum":0,"maximum":168},"blue":{"type":"integer","minimum":0,"maximum":168}})",
             [&service](const cJSON* args) { return tool_set_led_color(service, args); }});

    reg.add({"self.robot.create_reminder",
             "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat to true to repeat the reminder.",
             R"({"duration_seconds":{"type":"integer","minimum":1,"maximum":86400},"message":{"type":"string","default":"Time's up!"},"repeat":{"type":"boolean","default":false}})",
             [&service](const cJSON* args) { return tool_create_reminder(service, args); }});
    reg.add({"self.robot.get_reminders",
             "Get list of active reminders.",
             R"({})",
             [&service](const cJSON* args) { return tool_get_reminders(service, args); }});
    reg.add({"self.robot.stop_reminder",
             "Stop a reminder by ID.",
             R"({"id":{"type":"integer"}})",
             [&service](const cJSON* args) { return tool_stop_reminder(service, args); }});

    reg.add({"self.camera.take_photo",
             "Capture a photo (320x240) and return its URL served by the device. Takes 0.5-1 s. "
             "Set embed=true to also inline the JPEG as base64 (larger response).",
             R"({"quality":{"type":"integer","minimum":1,"maximum":100,"default":20},"embed":{"type":"boolean","default":false}})",
             [&service](const cJSON* args) { return tool_take_photo(service, args); }});

    reg.add({"self.microphone.stream",
             "Start listening: returns a WebSocket URL streaming Opus audio (16 kHz mono 60 ms frames). "
             "Connect, send {\"type\":\"start\"}, receive binary Opus packets, send {\"type\":\"stop\"} to end. One client at a time. No echo cancellation.",
             R"({})",
             tool_mic_stream});
    reg.add({"self.speaker.stream",
             "Returns a WebSocket URL for playing audio on the robot: send {\"type\":\"start\"}, then binary Opus packets "
             "(16 kHz mono 60 ms, paced ~16.7 frames/s), then {\"type\":\"stop\"}. One client at a time.",
             R"({})",
             tool_spk_stream});

#if CONFIG_MCP_AGENT_ENABLE_OWNER_TOOLS
    reg.add({"self.reboot",
             "OWNER ONLY. Reboot the device (1 s after the reply).",
             R"({})",
             [&service](const cJSON* args) { return tool_reboot(service, args); }});
#endif
}

}  // namespace mcp_lan
