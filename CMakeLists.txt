cmake_minimum_required(VERSION 3.19)
project(hyprpad VERSION "0.1.0" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WLR_USE_UNSTABLE)
# -fno-gnu-unique: without it static guards are STB_GNU_UNIQUE, glibc marks the
# .so NODELETE and a same-path reload silently keeps the old image.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC -fno-gnu-unique)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
pkg_search_module(LUA REQUIRED lua lua5.4 lua-5.4 lua54 luajit)
find_package(Threads REQUIRED)

add_library(hyprpad SHARED
    src/main.cpp
    src/core/engine.cpp
    src/config/config.cpp
    src/config/lua_config.cpp
    src/input/daemon.cpp
    src/actions/actions.cpp
)
set_target_properties(hyprpad PROPERTIES PREFIX "")

target_include_directories(hyprpad PRIVATE ${HYPR_DEPS_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS})
target_link_directories(hyprpad PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS})
target_link_libraries(hyprpad PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} Threads::Threads)

install(TARGETS hyprpad LIBRARY DESTINATION lib)

enable_testing()
add_executable(hyprpad_core_check
    tests/core_check.cpp
    src/core/engine.cpp
    src/config/config.cpp
)
target_include_directories(hyprpad_core_check PRIVATE src)
add_test(NAME core COMMAND hyprpad_core_check)

add_executable(hyprpad_lua_config_check
    tests/lua_config_check.cpp
    src/core/engine.cpp
    src/config/config.cpp
    src/config/lua_config.cpp
)
target_include_directories(hyprpad_lua_config_check PRIVATE src ${LUA_INCLUDE_DIRS})
target_link_directories(hyprpad_lua_config_check PRIVATE ${LUA_LIBRARY_DIRS})
target_link_libraries(hyprpad_lua_config_check PRIVATE ${LUA_LIBRARIES})
add_test(NAME lua_config COMMAND hyprpad_lua_config_check)

add_custom_target(reload
    COMMAND bash -c "hyprctl plugin unload $<TARGET_FILE:hyprpad> || true"
    COMMAND hyprctl plugin load $<TARGET_FILE:hyprpad>
    DEPENDS hyprpad
    USES_TERMINAL
    COMMENT "Reloading hyprpad into the running Hyprland"
)
