#include "../src/config/lua_config.hpp"
#include "check.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <string>

using namespace hyprpad;

namespace {

bool loadTable(lua_State* L, const char* code) {
    if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
        return false;
    }
    return lua_istable(L, -1);
}

void testValidConfig() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* code = R"(
        return {
            device = "auto",
            edge_width = 0.08,
            left_edge_width = 0.12,
            block_while_typing = { enabled = true, timeout_ms = 450 },
            gestures = {
                { zone = "top", direction = "left", action = { "notify-send", "top-left" } },
                { zone = "left", direction = "tap", fingers = 2, action = { "true" } },
                { zone = "pad", direction = "double-tap", fingers = 3, action = { "playerctl", "play-pause" } },
            },
            sliders = {
                { zone = "left", step = 0.05, up = { "a" }, down = { "b" } },
            },
        }
    )";

    CHECK(loadTable(L, code));
    const auto result = parseLuaConfig(L, -1, 7);
    CHECK(result.config != nullptr);
    CHECK(result.error.empty());
    CHECK(result.config->generation == 7);
    CHECK(result.config->device == "auto");
    CHECK(result.config->edges.left == 0.12F);
    CHECK(result.config->edges.right == 0.08F);
    CHECK(result.config->blockWhileTyping);
    CHECK(result.config->typingTimeoutMs == 450);
    CHECK(result.config->gestures.size() == 3);
    CHECK(result.config->sliders.size() == 1);
    CHECK(result.config->options.doubleTapZones & ZONE_PAD);
    CHECK(result.config->options.singleTapZones & ZONE_LEFT);
    CHECK(result.config->options.padTapFingers == (1U << 3));
    CHECK(result.config->sliders[0].axis == SliderAxis::Vertical);

    const Gesture padTap{Zone::Pad, Direction::DoubleTap, 3, 0, 1};
    const auto* binding = result.config->matchGesture(padTap);
    CHECK(binding != nullptr);
    CHECK(binding->action.size() == 2);

    lua_close(L);
}

void testLogAction() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* code = R"(
        return { gestures = {
            { zone = "pad", direction = "tap", fingers = 3, action = { log = true } },
        } }
    )";
    CHECK(loadTable(L, code));
    const auto result = parseLuaConfig(L, -1, 3);
    CHECK(result.config != nullptr);
    CHECK(result.config->gestures.size() == 1);
    CHECK(result.config->gestures[0].logGesture);
    CHECK(result.config->gestures[0].action.empty());

    const Gesture gesture{Zone::Pad, Direction::Tap, 3, 0, 1};
    const auto*   binding = result.config->matchGesture(gesture);
    CHECK(binding != nullptr);
    CHECK(binding->logGesture);
    lua_pop(L, 1);

    const char* both = R"(
        return { gestures = {
            { zone = "left", direction = "tap", action = { "true", log = true } },
        } }
    )";
    CHECK(loadTable(L, both));
    const auto rejected = parseLuaConfig(L, -1, 4);
    CHECK(rejected.config == nullptr);
    CHECK(!rejected.error.empty());

    lua_close(L);
}

void testLuaErrors() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    const char* invalidFingers = R"(
        return { gestures = { { zone = "left", direction = "up", fingers = 9, action = { "true" } } } }
    )";
    CHECK(loadTable(L, invalidFingers));
    auto result = parseLuaConfig(L, -1, 1);
    CHECK(result.config == nullptr);
    CHECK(!result.error.empty());
    lua_pop(L, 1);

    const char* conflict = R"(
        return {
            gestures = { { zone = "left", direction = "up", action = { "true" } } },
            sliders = { { zone = "left", up = { "a" }, down = { "b" } } },
        }
    )";
    CHECK(loadTable(L, conflict));
    result = parseLuaConfig(L, -1, 2);
    CHECK(result.config == nullptr);
    CHECK(result.error.find("slider") != std::string::npos);
    lua_pop(L, 1);

    const char* invalidTimeout = R"(
        return { block_while_typing = { enabled = true, timeout_ms = -5 } }
    )";
    CHECK(loadTable(L, invalidTimeout));
    result = parseLuaConfig(L, -1, 3);
    CHECK(result.config == nullptr);
    CHECK(!result.error.empty());

    lua_close(L);
}

} // namespace

int main() {
    testValidConfig();
    testLogAction();
    testLuaErrors();
    std::puts("hyprpad lua config checks passed");
    return 0;
}
