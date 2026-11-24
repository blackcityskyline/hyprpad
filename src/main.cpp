#include "config/lua_config.hpp"
#include "input/daemon.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <atomic>
#include <memory>
#include <string>

using namespace hyprpad;

namespace {

std::unique_ptr<Daemon>           g_daemon;
CHyprSignalListener               g_mouseMoveListener;
CHyprSignalListener               g_mouseButtonListener;
CHyprSignalListener               g_keyboardListener;
std::atomic<uint64_t>             g_generation{0};

constexpr const char* PLUGIN_NAME    = "hyprpad";
constexpr const char* PLUGIN_VERSION = "0.1.0";

void logError(const std::string& message) {
    LOG(Hyprutils::CLI::LOG_ERR, "[hyprpad] {}", message);
}

void logInfo(const std::string& message) {
    LOG(Hyprutils::CLI::LOG_DEBUG, "[hyprpad] {}", message);
}

int luaSetup(lua_State* L) {
    if (!g_daemon) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "hyprpad daemon is not running");
        return 2;
    }

    const uint64_t generation = g_generation.fetch_add(1) + 1;
    auto           result     = parseLuaConfig(L, 1, generation);
    if (!result.config) {
        logError("config rejected: " + result.error);
        lua_pushboolean(L, 0);
        lua_pushlstring(L, result.error.data(), result.error.size());
        return 2;
    }

    const size_t gestures = result.config->gestures.size();
    const size_t sliders  = result.config->sliders.size();
    g_daemon->applyConfig(std::move(result.config));
    logInfo("config applied: " + std::to_string(gestures) + " gestures, " + std::to_string(sliders) + " sliders");
    lua_pushboolean(L, 1);
    return 1;
}

int luaStatus(lua_State* L) {
    const std::string status = g_daemon ? g_daemon->status() : "state: plugin not running\n";
    lua_pushlstring(L, status.data(), status.size());
    return 1;
}

int luaDevices(lua_State* L) {
    const auto candidates = discoverDevices();
    lua_createtable(L, static_cast<int>(candidates.size()), 0);
    int index = 1;
    for (const auto& candidate : candidates) {
        lua_createtable(L, 0, 7);
        lua_pushlstring(L, candidate.path.data(), candidate.path.size());
        lua_setfield(L, -2, "path");
        lua_pushlstring(L, candidate.name.data(), candidate.name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, candidate.readable);
        lua_setfield(L, -2, "readable");
        lua_pushboolean(L, candidate.touchpad);
        lua_setfield(L, -2, "touchpad");
        lua_pushboolean(L, candidate.buttonpad);
        lua_setfield(L, -2, "buttonpad");
        lua_pushboolean(L, candidate.virtualDev);
        lua_setfield(L, -2, "virtual");
        lua_pushlstring(L, candidate.error.data(), candidate.error.size());
        lua_setfield(L, -2, "error");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

std::string deviceReport() {
    std::string text = "hyprpad touchpad candidates:";
    bool        any  = false;
    for (const auto& candidate : discoverDevices()) {
        if (candidate.readable && !candidate.touchpad)
            continue;
        any = true;
        text += "\n  " + candidate.path + " (" + candidate.name + ", " +
                (candidate.touchpad ? (candidate.virtualDev ? "virtual touchpad" : "touchpad") : "unreadable") + ")" +
                (candidate.error.empty() ? "" : ": " + candidate.error);
    }
    if (!any)
        text += "\n  none";
    return text;
}

int luaVersion(lua_State* L) {
    lua_pushstring(L, PLUGIN_VERSION);
    return 1;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_daemon = std::make_unique<Daemon>();
    g_daemon->start();

    auto& events = Event::bus()->m_events;

    g_mouseMoveListener = events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        if (g_daemon && g_daemon->motionShouldBeSwallowed())
            info.cancelled = true;
    });

    g_mouseButtonListener = events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (g_daemon && event.state == WL_POINTER_BUTTON_STATE_PRESSED && g_daemon->consumeClickSuppression())
            info.cancelled = true;
    });

    g_keyboardListener = events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo&) {
        if (g_daemon && event.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            g_daemon->noteKeyPressed();
    });

    HyprlandAPI::addDispatcherV2(handle, "hyprpad:status", [](std::string) -> SDispatchResult {
        if (!g_daemon)
            return {.success = false, .error = "hyprpad is not running"};
        logInfo("status\n" + g_daemon->status());
        return {.success = true};
    });

    HyprlandAPI::addDispatcherV2(handle, "hyprpad:devices", [](std::string) -> SDispatchResult {
        logInfo(deviceReport());
        return {.success = true};
    });

    HyprlandAPI::registerHyprCtlCommand(
        handle, IPC::Socket1::SCommand{.name    = "hyprpadstatus",
                                       .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                       .handler = [](const IPC::Socket1::SRequest&) -> IPC::Socket1::SResponse {
                                           return IPC::Socket1::SResponse{g_daemon ? g_daemon->status() + "\n" : "not running\n"};
                                       }});

    HyprlandAPI::registerHyprCtlCommand(
        handle, IPC::Socket1::SCommand{.name    = "hyprpaddevices",
                                       .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                       .handler = [](const IPC::Socket1::SRequest&) -> IPC::Socket1::SResponse {
                                           return IPC::Socket1::SResponse{deviceReport() + "\n"};
                                       }});

    if (Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA) {
        HyprlandAPI::addLuaFunction(handle, PLUGIN_NAME, "setup", &luaSetup);
        HyprlandAPI::addLuaFunction(handle, PLUGIN_NAME, "status", &luaStatus);
        HyprlandAPI::addLuaFunction(handle, PLUGIN_NAME, "devices", &luaDevices);
        HyprlandAPI::addLuaFunction(handle, PLUGIN_NAME, "version", &luaVersion);
    } else {
        logError("Hyprland is not using its Lua config; hyprpad configuration is unavailable");
    }

    return {
        .name        = PLUGIN_NAME,
        .description = "Touchpad edge gestures with Lua configuration",
        .author      = "hyprpad contributors",
        .version     = PLUGIN_VERSION,
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_mouseMoveListener.reset();
    g_mouseButtonListener.reset();
    g_keyboardListener.reset();
    if (g_daemon) {
        g_daemon->stop();
        g_daemon.reset();
    }
}
