#include "lua_config.hpp"

#include <cmath>
#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace hyprpad {

namespace {

class Parser {
  public:
    Parser(lua_State* L, int table) : m_lua(L), m_table(table) {
    }

    LuaConfigResult parse(uint64_t generation) {
        Config config;
        config.generation = generation;

        if (!readString("device", config.device, false))
            return fail();
        if (!readNumber("edge_width", config.edges.left, false) || !readNumber("edge_width", config.edges.right, false) ||
            !readNumber("edge_width", config.edges.top, false) || !readNumber("edge_width", config.edges.bottom, false))
            return fail();
        // the four calls above overwrite all edges; per-edge overrides follow
        if (!readNumber("left_edge_width", config.edges.left, false) ||
            !readNumber("right_edge_width", config.edges.right, false) ||
            !readNumber("top_edge_width", config.edges.top, false) ||
            !readNumber("bottom_edge_width", config.edges.bottom, false))
            return fail();

        double number = 0.0;
        if (!readNumber("tap_min_duration_ms", number, false))
            return fail();
        if (m_hadField)
            config.options.tapMinDuration = static_cast<Micros>(number * 1000.0);
        if (!readNumber("tap_max_duration_ms", number, false))
            return fail();
        if (m_hadField)
            config.options.tapMaxDuration = static_cast<Micros>(number * 1000.0);
        if (!readNumber("double_tap_timeout_ms", number, false))
            return fail();
        if (m_hadField)
            config.options.doubleTapTimeout = static_cast<Micros>(number * 1000.0);
        if (!readNumber("double_tap_max_distance", config.options.doubleTapMaxDistance, false))
            return fail();
        if (!readNumber("swipe_min_distance", config.options.swipeMinDistance, false))
            return fail();

        if (!readBool("swallow_pointer", config.swallowPointer, false))
            return fail();
        if (!readTyping(config))
            return fail();
        if (!readGestures(config))
            return fail();
        if (!readSliders(config))
            return fail();

        if (const auto error = config.finalize())
            return fail(*error);

        auto shared = std::make_shared<Config>(std::move(config));
        return LuaConfigResult{std::shared_ptr<const Config>(std::move(shared)), {}};
    }

  private:
    LuaConfigResult fail() {
        return LuaConfigResult{nullptr, m_error};
    }

    LuaConfigResult fail(const std::string& error) {
        return LuaConfigResult{nullptr, error};
    }

    void setError(const std::string& error) {
        if (m_error.empty())
            m_error = error;
    }

    bool readString(const char* key, std::string& out, bool required) {
        m_hadField = false;
        lua_getfield(m_lua, m_table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            if (required)
                setError(std::string(key) + " is required");
            return !required;
        }
        if (!lua_isstring(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be a string");
            return false;
        }
        out        = lua_tostring(m_lua, -1);
        m_hadField = true;
        lua_pop(m_lua, 1);
        return true;
    }

    bool readNumber(const char* key, double& out, bool required) {
        m_hadField = false;
        lua_getfield(m_lua, m_table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            if (required)
                setError(std::string(key) + " is required");
            return !required;
        }
        if (!lua_isnumber(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be a number");
            return false;
        }
        out        = lua_tonumber(m_lua, -1);
        m_hadField = true;
        lua_pop(m_lua, 1);
        return true;
    }

    bool readNumber(const char* key, float& out, bool required) {
        double value = 0.0;
        if (!readNumber(key, value, required))
            return false;
        if (m_hadField)
            out = static_cast<float>(value);
        return true;
    }

    bool readInteger(const char* key, std::optional<uint8_t>& out, bool required) {
        double value = 0.0;
        if (!readNumber(key, value, required))
            return false;
        if (!m_hadField)
            return true;
        if (std::floor(value) != value || value < 1.0 || value > 4.0) {
            setError(std::string(key) + " must be an integer between 1 and 4");
            return false;
        }
        out = static_cast<uint8_t>(value);
        return true;
    }

    bool readBool(const char* key, bool& out, bool required) {
        m_hadField = false;
        lua_getfield(m_lua, m_table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            if (required)
                setError(std::string(key) + " is required");
            return !required;
        }
        if (!lua_isboolean(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be a boolean");
            return false;
        }
        out        = lua_toboolean(m_lua, -1) != 0;
        m_hadField = true;
        lua_pop(m_lua, 1);
        return true;
    }

    bool readTyping(Config& config) {
        lua_getfield(m_lua, m_table, "block_while_typing");
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
        } else if (lua_isboolean(m_lua, -1)) {
            config.blockWhileTyping = lua_toboolean(m_lua, -1) != 0;
            lua_pop(m_lua, 1);
        } else if (lua_istable(m_lua, -1)) {
            const int table = lua_gettop(m_lua);
            bool      enabled = true;
            double    timeout = static_cast<double>(config.typingTimeoutMs);
            if (!readBoolAt(table, "enabled", enabled) || !readNumberAt(table, "timeout_ms", timeout))
                return false;
            config.blockWhileTyping = enabled;
            config.typingTimeoutMs  = static_cast<uint64_t>(timeout);
            lua_pop(m_lua, 1);
        } else {
            lua_pop(m_lua, 1);
            setError("block_while_typing must be a boolean or a table");
            return false;
        }

        double timeout = static_cast<double>(config.typingTimeoutMs);
        if (!readNumber("block_while_typing_timeout_ms", timeout, false))
            return false;
        if (m_hadField) {
            if (timeout <= 0.0 || timeout > 10'000.0) {
                setError("block_while_typing_timeout_ms must be > 0 and <= 10000");
                return false;
            }
            config.typingTimeoutMs = static_cast<uint64_t>(timeout);
        }
        if (config.typingTimeoutMs == 0) {
            setError("block_while_typing timeout must be greater than zero");
            return false;
        }
        return true;
    }

    bool readBoolAt(int table, const char* key, bool& out) {
        lua_getfield(m_lua, table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            return true;
        }
        if (!lua_isboolean(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string("block_while_typing.") + key + " must be a boolean");
            return false;
        }
        out = lua_toboolean(m_lua, -1) != 0;
        lua_pop(m_lua, 1);
        return true;
    }

    bool readNumberAt(int table, const char* key, double& out) {
        lua_getfield(m_lua, table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            return true;
        }
        if (!lua_isnumber(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string("block_while_typing.") + key + " must be a number");
            return false;
        }
        out = lua_tonumber(m_lua, -1);
        lua_pop(m_lua, 1);
        return true;
    }

    bool readAction(int table, const std::string& label, std::vector<std::string>& action) {
        lua_getfield(m_lua, table, "action");
        if (!lua_istable(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(label + ".action must be an argv array");
            return false;
        }
        const size_t length = lua_rawlen(m_lua, -1);
        if (length == 0) {
            lua_pop(m_lua, 1);
            setError(label + ".action must not be empty");
            return false;
        }
        for (size_t index = 1; index <= length; ++index) {
            lua_rawgeti(m_lua, -1, static_cast<lua_Integer>(index));
            if (!lua_isstring(m_lua, -1)) {
                lua_pop(m_lua, 2);
                setError(label + ".action[" + std::to_string(index) + "] must be a string");
                return false;
            }
            action.emplace_back(lua_tostring(m_lua, -1));
            lua_pop(m_lua, 1);
        }
        lua_pop(m_lua, 1);
        return true;
    }

    bool readGestures(Config& config) {
        lua_getfield(m_lua, m_table, "gestures");
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            return true;
        }
        if (!lua_istable(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError("gestures must be an array");
            return false;
        }

        const size_t length = lua_rawlen(m_lua, -1);
        for (size_t index = 1; index <= length; ++index) {
            lua_rawgeti(m_lua, -1, static_cast<lua_Integer>(index));
            if (!lua_istable(m_lua, -1)) {
                lua_pop(m_lua, 2);
                setError("gestures[" + std::to_string(index) + "] must be a table");
                return false;
            }
            const int table = lua_gettop(m_lua);
            const auto label = "gestures[" + std::to_string(index) + "]";

            GestureBinding binding;
            std::string    zone;
            std::string    direction;
            if (!readStringAt(table, "zone", zone, true) || !readStringAt(table, "direction", direction, true)) {
                lua_pop(m_lua, 2);
                return false;
            }
            const auto parsedZone = parseZone(zone);
            if (!parsedZone) {
                lua_pop(m_lua, 2);
                setError(label + ".zone must be left, right, top, bottom, or pad");
                return false;
            }
            const auto parsedDirection = parseDirection(direction);
            if (!parsedDirection) {
                lua_pop(m_lua, 2);
                setError(label + ".direction must be up, down, left, right, tap, or double-tap");
                return false;
            }
            binding.zone      = *parsedZone;
            binding.direction = *parsedDirection;

            if (!readIntegerAt(table, "fingers", binding.fingers)) {
                lua_pop(m_lua, 2);
                return false;
            }
            if (!readAction(table, label, binding.action)) {
                lua_pop(m_lua, 2);
                return false;
            }

            config.gestures.push_back(std::move(binding));
            lua_pop(m_lua, 1);
        }
        lua_pop(m_lua, 1);
        return true;
    }

    bool readSliders(Config& config) {
        lua_getfield(m_lua, m_table, "sliders");
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            return true;
        }
        if (!lua_istable(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError("sliders must be an array");
            return false;
        }

        const size_t length = lua_rawlen(m_lua, -1);
        for (size_t index = 1; index <= length; ++index) {
            lua_rawgeti(m_lua, -1, static_cast<lua_Integer>(index));
            if (!lua_istable(m_lua, -1)) {
                lua_pop(m_lua, 2);
                setError("sliders[" + std::to_string(index) + "] must be a table");
                return false;
            }
            const int  table = lua_gettop(m_lua);
            const auto label = "sliders[" + std::to_string(index) + "]";

            SliderBinding binding;
            std::string   zone;
            double        step = binding.step;
            if (!readStringAt(table, "zone", zone, true) || !readNumberAt(table, "step", step)) {
                lua_pop(m_lua, 2);
                return false;
            }
            binding.step = static_cast<float>(step);
            const auto parsedZone = parseZone(zone);
            if (!parsedZone || *parsedZone == Zone::Pad) {
                lua_pop(m_lua, 2);
                setError(label + ".zone must be left, right, top, or bottom");
                return false;
            }
            binding.zone = *parsedZone;
            binding.axis = (binding.zone == Zone::Left || binding.zone == Zone::Right) ? SliderAxis::Vertical
                                                                                       : SliderAxis::Horizontal;

            const bool vertical = binding.axis == SliderAxis::Vertical;
            if (vertical) {
                if (!readActionAt(table, "up", binding.negative) || !readActionAt(table, "down", binding.positive)) {
                    lua_pop(m_lua, 2);
                    return false;
                }
                if (!rejectField(table, "left", label) || !rejectField(table, "right", label)) {
                    lua_pop(m_lua, 2);
                    return false;
                }
            } else {
                if (!readActionAt(table, "left", binding.negative) || !readActionAt(table, "right", binding.positive)) {
                    lua_pop(m_lua, 2);
                    return false;
                }
                if (!rejectField(table, "up", label) || !rejectField(table, "down", label)) {
                    lua_pop(m_lua, 2);
                    return false;
                }
            }

            config.sliders.push_back(std::move(binding));
            lua_pop(m_lua, 1);
        }
        lua_pop(m_lua, 1);
        return true;
    }

    bool readStringAt(int table, const char* key, std::string& out, bool required) {
        lua_getfield(m_lua, table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            if (required)
                setError(std::string(key) + " is required");
            return !required;
        }
        if (!lua_isstring(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be a string");
            return false;
        }
        out = lua_tostring(m_lua, -1);
        lua_pop(m_lua, 1);
        return true;
    }

    bool readIntegerAt(int table, const char* key, std::optional<uint8_t>& out) {
        lua_getfield(m_lua, table, key);
        if (lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            return true;
        }
        if (!lua_isnumber(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be an integer between 1 and 4");
            return false;
        }
        const double value = lua_tonumber(m_lua, -1);
        lua_pop(m_lua, 1);
        if (std::floor(value) != value || value < 1.0 || value > 4.0) {
            setError(std::string(key) + " must be an integer between 1 and 4");
            return false;
        }
        out = static_cast<uint8_t>(value);
        return true;
    }

    bool readActionAt(int table, const char* key, std::vector<std::string>& action) {
        lua_getfield(m_lua, table, key);
        if (!lua_istable(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must be an argv array");
            return false;
        }
        const size_t length = lua_rawlen(m_lua, -1);
        if (length == 0) {
            lua_pop(m_lua, 1);
            setError(std::string(key) + " must not be empty");
            return false;
        }
        for (size_t index = 1; index <= length; ++index) {
            lua_rawgeti(m_lua, -1, static_cast<lua_Integer>(index));
            if (!lua_isstring(m_lua, -1)) {
                lua_pop(m_lua, 2);
                setError(std::string(key) + "[" + std::to_string(index) + "] must be a string");
                return false;
            }
            action.emplace_back(lua_tostring(m_lua, -1));
            lua_pop(m_lua, 1);
        }
        lua_pop(m_lua, 1);
        return true;
    }

    bool rejectField(int table, const char* key, const std::string& label) {
        lua_getfield(m_lua, table, key);
        if (!lua_isnil(m_lua, -1)) {
            lua_pop(m_lua, 1);
            setError(label + ": " + key + " does not apply to this zone's axis");
            return false;
        }
        lua_pop(m_lua, 1);
        return true;
    }

    lua_State* m_lua   = nullptr;
    int        m_table = 0;
    bool       m_hadField = false;
    std::string m_error;
};

} // namespace

LuaConfigResult parseLuaConfig(lua_State* L, int index, uint64_t generation) {
    if (!L)
        return {nullptr, "no Lua state"};
    const int absoluteIndex = lua_absindex(L, index);
    if (!lua_istable(L, absoluteIndex))
        return {nullptr, "setup() expects a table"};

    Parser parser(L, absoluteIndex);
    return parser.parse(generation);
}

} // namespace hyprpad
