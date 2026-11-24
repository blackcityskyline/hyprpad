# hyprpad

Hyprland plugin: touchpad edge gestures with Lua configuration. A C++ port of
[edgepad](../edgepad)'s recognizer, running inside the compositor.

## Layout

- `src/core/` — recognizer. Pure logic, no Hyprland symbols; unit tested.
- `src/config/` — config structs, binding matching, Lua parser.
- `src/input/` — evdev discovery and the input thread.
- `src/actions/` — argv action runner with per-key coalescing.
- `src/main.cpp` — plugin init, Lua functions, dispatchers, event-bus hooks.
- `tests/` — CTest binaries, no compositor needed.
- `examples/`, `docs/`, `packaging/` — user-facing configuration and udev rule.

## Build and check

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`cmake --build build --target reload` unloads and reloads the plugin into the
running compositor.

## Rules

- `src/core/` must not include Hyprland headers. It is the part that can be
  tested without a session; keep it that way.
- Never grab the touchpad and never create a uinput device. The plugin observes
  evdev read-only and lets Hyprland own the pointer. Pointer side effects are
  suppressed through cancellable event-bus events only.
- Axis events are never cancelled: two-finger scroll must keep working.
- Actions are argv arrays executed without a shell.
- Config errors must leave the previous configuration running; `setup` returns
  `false, message` rather than throwing across the Lua boundary.
- Hyprland threads: parse Lua on the main thread, hand the config to the input
  thread, and let the input thread touch the engine.
- The recognizer behaviour is intentionally identical to edgepad's. Change it
  here and in edgepad, or not at all.

## Style

- C++23, 4-space indent, `m_` members, `camelCase` functions, `PascalCase` types.
- Prefer existing helpers in `src/core/types.hpp` over new ones.
- No new dependencies: stdlib, Hyprland, and Lua only.
