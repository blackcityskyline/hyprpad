#pragma once

#include "config.hpp"

#include <memory>
#include <string>

struct lua_State;

namespace hyprpad {

struct LuaConfigResult {
    std::shared_ptr<const Config> config;
    std::string                   error;
};

// Parses the table at `index`. Never throws: a broken config leaves the
// previous bindings running.
LuaConfigResult parseLuaConfig(lua_State* L, int index, uint64_t generation);

} // namespace hyprpad
