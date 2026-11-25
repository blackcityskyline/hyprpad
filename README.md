# hyprpad

Touchpad edge gestures for Hyprland, configured in Lua.

hyprpad is a Hyprland plugin (a C++ shared library) rather than a daemon. It
watches the touchpad's evdev node read-only, recognizes swipes, taps and
double-taps that start in a screen-edge band, and runs your commands. It is a
port of [edgepad](../edgepad)'s recognizer to run inside the compositor.

Because it runs inside Hyprland, it can hide its own side effects: while an edge
contact is owned by hyprpad, pointer motion is cancelled, and the pointer press
that libinput turns tap-to-click into is swallowed for a moment after an edge
tap. Two-finger scroll and pinch are never touched.

## Features

- Edge swipes, taps and double taps on `left`, `right`, `top`, `bottom`.
- Multi-finger bindings (`fingers = 1..4`) with exact-count precedence.
- Multi-finger tap groups, so a three-finger double tap is one gesture.
- Whole-pad taps (`zone = "pad"`) that let pointer input through.
- Sliders along edges that repeat actions while the finger moves.
- Block-while-typing guard using Hyprland keyboard events.
- Actions are argv arrays, run without a shell, on a worker thread.
- An idle touchpad costs no wakeups: the input thread blocks in `poll()`.
- `action = { log = true }` records gestures for diagnostics instead of running
  a command.

## Requirements

- Hyprland 0.57 or newer, built with its Lua configuration (`hl` global).
- Read access to the touchpad's `/dev/input/eventN` node, granted by
  [`packaging/70-hyprpad.rules`](packaging/70-hyprpad.rules) or membership in
  the `input` group.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/hyprpad.so`. Dependencies come from `pkg-config hyprland` and
the Lua package Hyprland links (5.4 or 5.5).

## Install

With hyprpm:

```bash
hyprpm add /path/to/hyprpad
hyprpm enable hyprpad
```

Or load it from your Lua config:

```lua
hl.plugin.load("/path/to/hyprpad.so")
```

Hyprland registers the path during the config pass and opens the library after
it, then reloads the config. Put `setup()` in the module that reload requires,
and skip it when the plugin API is not there yet:

```lua
hl.plugin.load("/path/to/hyprpad.so")
if hl.plugin.hyprpad == nil then
    return
end
hl.plugin.hyprpad.setup({ --[[ ... ]] })
```

See [`examples/hyprpad.lua`](examples/hyprpad.lua) and
[`docs/configuration.md`](docs/configuration.md).

## Reload

```bash
hyprctl plugin unload /path/to/hyprpad.so
hyprctl plugin load /path/to/hyprpad.so
```

`cmake --build build --target reload` does both for the local build. A
`hyprctl reload` alone re-applies `setup()` but keeps the loaded library, so a
rebuilt `.so` needs the unload/load pair.

## Check

```bash
hyprctl hyprpadstatus    # device, counters, last error
hyprctl hyprpaddevices   # touchpad candidates
```

Unit checks need no compositor:

```bash
ctest --test-dir build --output-on-failure
```

## Differences from edgepad

| edgepad | hyprpad |
| --- | --- |
| Standalone daemon | Hyprland plugin |
| TOML | Lua (`hl.plugin.hyprpad.setup`) |
| Grabs the touchpad and republishes it through `uinput` | Reads the touchpad passively; Hyprland keeps the real device |
| Filters pointer events in the proxy | Cancels Hyprland event-bus events for owned contacts |
| Watches keyboard evdev nodes for typing | Uses Hyprland's keyboard key event |
| `edgepad devices`, `status`, `doctor` | `hl.plugin.hyprpad.devices()`, `hyprctl hyprpadstatus`, `hyprctl hyprpaddevices` |

Only edge-owned contacts are affected. Everything else on the pad behaves as it
did before the plugin was loaded. While a claimed contact is down, the built-in
touchpad swipe events are cancelled too, so the same fingers cannot drive both a
hyprpad binding and a compositor gesture.

## License

MIT or Apache-2.0, at your option. See [LICENSE-MIT](LICENSE-MIT) and
[LICENSE-APACHE](LICENSE-APACHE).
