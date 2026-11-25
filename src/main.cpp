#include "config/lua_config.hpp"
#include "core/smoothing.hpp"
#include "input/daemon.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/devices/IPointer.hpp>
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
CHyprSignalListener               g_swipeBeginListener;
CHyprSignalListener               g_swipeUpdateListener;
CHyprSignalListener               g_swipeEndListener;
// Compositor-thread only: true when the begin event of the swipe in flight was
// cancelled because hyprpad owns those contacts. Its updates and end must be
// cancelled too, or Hyprland's swipe state machine sees a partial gesture.
bool                              g_swipeCancelled = false;
std::atomic<uint64_t>             g_generation{0};

// Optional touchpad motion smoothing. Hyprland's mouse.move bus event only lets
// a listener cancel a move, not change its delta, so this needs a function hook
// on CInputManager::onMouseMoved. Off unless smooth_pointer is set.
struct PointerSmoothing {
    bool           enabled  = false;
    float          strength = 0.0F;
    MotionSmoother delta;
    MotionSmoother unaccel;

    void configure(bool on, float value) {
        enabled  = on;
        strength = value;
        delta.setStrength(value);
        unaccel.setStrength(value);
    }
};

// Last config rejection, surfaced in status: hyprctl eval returns "ok" even
// when the plugin's lua function returns false, so this is the only way a
// user sees why their config did nothing.
std::string       g_configError;

PointerSmoothing g_smoothing;
HANDLE            g_handle               = nullptr;
CFunctionHook*    g_mouseMoveHook        = nullptr;
void*             g_onMouseMovedOriginal = nullptr;

using PONMOUSEMOVED = void (*)(void*, IPointer::SMotionEvent);

void hkOnMouseMoved(void* self, IPointer::SMotionEvent event) {
    if (g_smoothing.enabled && event.device && event.device->m_isTouchpad) {
        float        x   = 0.0F;
        float        y   = 0.0F;
        g_smoothing.delta.apply(static_cast<float>(event.delta.x), static_cast<float>(event.delta.y), event.timeMs, x, y);
        event.delta = Vector2D(x, y);
        g_smoothing.unaccel.apply(static_cast<float>(event.unaccel.x), static_cast<float>(event.unaccel.y), event.timeMs, x, y);
        event.unaccel = Vector2D(x, y);
    }
    reinterpret_cast<PONMOUSEMOVED>(g_onMouseMovedOriginal)(self, event);
}

constexpr const char* PLUGIN_NAME    = "hyprpad";
constexpr const char* PLUGIN_VERSION = "0.1.6";

void logError(const std::string& message) {
    LOG(Hyprutils::CLI::LOG_ERR, "[hyprpad] {}", message);
}

void logInfo(const std::string& message) {
    LOG(Hyprutils::CLI::LOG_DEBUG, "[hyprpad] {}", message);
}

std::string statusText() {
    std::string text = g_daemon ? g_daemon->status() : std::string("state: plugin not running\n");
    text += "smoothing: ";
    if (!g_smoothing.enabled)
        text += "off\n";
    else {
        text += std::to_string(g_smoothing.strength);
        text += g_mouseMoveHook ? "\n" : " (hook unavailable)\n";
    }
    if (!g_configError.empty())
        text += "config error: " + g_configError + "\n";
    return text;
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
        g_configError = result.error;
        logError("config rejected: " + result.error);
        lua_pushboolean(L, 0);
        lua_pushlstring(L, result.error.data(), result.error.size());
        return 2;
    }

    const size_t gestures = result.config->gestures.size();
    const size_t sliders  = result.config->sliders.size();
    const bool   smooth   = result.config->smoothPointer;
    const float  strength = result.config->smoothStrength;
    g_configError.clear();
    g_daemon->applyConfig(std::move(result.config));
    g_smoothing.configure(smooth, strength);
    logInfo("config applied: " + std::to_string(gestures) + " gestures, " + std::to_string(sliders) + " sliders, smoothing " +
            (smooth ? "on" : "off"));
    lua_pushboolean(L, 1);
    return 1;
}

int luaStatus(lua_State* L) {
    const std::string status = statusText();
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
    g_handle  = handle;
    g_daemon  = std::make_unique<Daemon>();
    g_daemon->start();

    auto& events = Event::bus()->m_events;

    // Optional: smooth touchpad motion before Hyprland applies it. Failing to
    // find the function only disables the filter; gestures keep working.
    for (const auto& match : HyprlandAPI::findFunctionsByName(handle, "onMouseMoved")) {
        if (match.demangled.find("CInputManager::onMouseMoved(") == std::string::npos)
            continue;
        g_mouseMoveHook = HyprlandAPI::createFunctionHook(handle, match.address, reinterpret_cast<void*>(&hkOnMouseMoved));
        if (g_mouseMoveHook && g_mouseMoveHook->hook()) {
            g_onMouseMovedOriginal = g_mouseMoveHook->m_original;
            logInfo("pointer smoothing hook installed");
        } else {
            logError("pointer smoothing unavailable: could not hook onMouseMoved");
            g_mouseMoveHook = nullptr;
        }
        break;
    }

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

    // edgepad grabbed the pad, so a swipe that started on an edge never reached
    // the compositor. A plugin cannot unsee libinput events, so cancel the
    // built-in touchpad gesture instead: otherwise the same three fingers run a
    // hyprpad binding and a compositor workspace swipe at once.
    g_swipeBeginListener = events.gesture.swipe.begin.listen([](IPointer::SSwipeBeginEvent, Event::SCallbackInfo& info) {
        g_swipeCancelled = g_daemon && g_daemon->contactsClaimed();
        if (g_swipeCancelled)
            info.cancelled = true;
    });

    g_swipeUpdateListener = events.gesture.swipe.update.listen([](IPointer::SSwipeUpdateEvent, Event::SCallbackInfo& info) {
        if (g_swipeCancelled)
            info.cancelled = true;
    });

    g_swipeEndListener = events.gesture.swipe.end.listen([](IPointer::SSwipeEndEvent, Event::SCallbackInfo& info) {
        if (g_swipeCancelled)
            info.cancelled = true;
        g_swipeCancelled = false;
    });

    HyprlandAPI::addDispatcherV2(handle, "hyprpad:status", [](std::string) -> SDispatchResult {
        if (!g_daemon)
            return {.success = false, .error = "hyprpad is not running"};
        logInfo(statusText());
        return {.success = true};
    });

    HyprlandAPI::addDispatcherV2(handle, "hyprpad:devices", [](std::string) -> SDispatchResult {
        logInfo(deviceReport());
        return {.success = true};
    });

    // Live tuning for smooth_strength, so finding the right damping does not
    // need a config edit and reload per attempt. Not persisted; the config
    // value is what survives a restart.
    HyprlandAPI::registerHyprCtlCommand(
        handle, IPC::Socket1::SCommand{.name    = "hyprpad-smooth",
                                       .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                       .handler = [](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse {
                                           const std::string arg = request.command.substr(std::string("hyprpad-smooth").size());
                                           const auto        value = arg.find_first_not_of(" \t");
                                           if (value == std::string::npos)
                                               return IPC::Socket1::SResponse{"usage: hyprctl hyprpad-smooth <0..1|off>\n"};
                                           const std::string setting = arg.substr(value);
                                           if (setting == "off") {
                                               g_smoothing.configure(false, 0.0F);
                                               return IPC::Socket1::SResponse{"smoothing off\n"};
                                           }
                                           try {
                                               const float strength = std::stof(setting);
                                               g_smoothing.configure(true, strength);
                                               return IPC::Socket1::SResponse{"smooth_strength = " + std::to_string(strength) + "\n"};
                                           } catch (...) {
                                               return IPC::Socket1::SResponse{"usage: hyprctl hyprpad-smooth <0..1|off>\n"};
                                           }
                                       }});

    HyprlandAPI::registerHyprCtlCommand(
        handle, IPC::Socket1::SCommand{.name    = "hyprpadstatus",
                                       .match   = IPC::Socket1::COMMAND_MATCH_EXACT,
                                       .handler = [](const IPC::Socket1::SRequest&) -> IPC::Socket1::SResponse {
                                           return IPC::Socket1::SResponse{statusText()};
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
    if (g_mouseMoveHook) {
        HyprlandAPI::removeFunctionHook(g_handle, g_mouseMoveHook);
        g_mouseMoveHook        = nullptr;
        g_onMouseMovedOriginal = nullptr;
    }
    g_mouseMoveListener.reset();
    g_mouseButtonListener.reset();
    g_keyboardListener.reset();
    g_swipeBeginListener.reset();
    g_swipeUpdateListener.reset();
    g_swipeEndListener.reset();
    g_swipeCancelled = false;
    if (g_daemon) {
        g_daemon->stop();
        g_daemon.reset();
    }
}
