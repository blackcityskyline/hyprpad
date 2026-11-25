# hyprpad

Touchpad edge gestures for Hyprland, configured in Lua.

hyprpad is a Hyprland plugin (a C++ shared library) rather than a daemon. It
watches the touchpad's evdev node read-only, recognizes swipes, taps and
double-taps that start in a screen-edge band, and runs your commands. It is a
port of [edgepad](../edgepad)'s recognizer to run inside the compositor.

Because it runs inside Hyprland, it can hide its own side effects: while an edge
contact is driving a slider, pointer motion is cancelled, and the pointer press
that libinput turns tap-to-click into is swallowed for a moment after an edge
tap. Swipes and taps fire on lift, so they leave the cursor alone. Two-finger
scroll and pinch are never touched.

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

## Configuration

The complete configuration, every option and every binding kind. It is the same
file as [`examples/hyprpad.lua`](examples/hyprpad.lua); delete what you do not
want, the bindings you leave out stay ordinary touchpad area.

```lua
-- Hyprland runs its config pass before it opens the plugin library, so the API
-- is nil on the first pass and the reload right after provides it.
pcall(function()
    hl.plugin.load("/path/to/hyprpad.so")
end)

if hl.plugin.hyprpad == nil then
    return
end

-- Small builders keep the bindings Lua instead of a wall of table literals.
-- The plugin only sees the resulting tables.
local function notify(text)
    return { "notify-send", "hyprpad", text }
end

local function shell(command)
    return { "sh", "-c", command }
end

-- An action is an argv array, run without a shell. A binding without `fingers`
-- accepts any count; 1..4 selects one and wins over the wildcard.
local gestures = {}
local function gesture(zone, direction, action, fingers)
    gestures[#gestures + 1] = {
        zone = zone, direction = direction, action = action, fingers = fingers,
    }
end

-- Whole pad, not an edge. These contacts are never held back: the pointer and
-- the desktop see them as usual and the action runs in addition.
gesture("pad", "tap", notify("pad tap"), 3)
gesture("pad", "double-tap", notify("pad double tap"), 3)

-- Top edge: swipes, taps, and one finger-count variant.
gesture("top", "left", notify("top left"))
gesture("top", "right", notify("top right"))
gesture("top", "up", notify("top up"))
gesture("top", "down", notify("top down"))
gesture("top", "tap", notify("top tap"))
gesture("top", "double-tap", notify("top double tap"))
gesture("top", "left", notify("top left, three fingers"), 3)

-- Bottom edge.
gesture("bottom", "left", notify("bottom left"))
gesture("bottom", "right", notify("bottom right"))
gesture("bottom", "up", notify("bottom up"))
gesture("bottom", "down", notify("bottom down"))
gesture("bottom", "tap", notify("bottom tap"))
gesture("bottom", "double-tap", notify("bottom double tap"))
gesture("bottom", "right", notify("bottom right, two fingers"), 2)

-- The left and right edges host sliders below, and a slider zone rejects
-- directional swipes at setup(). Taps are still allowed there.
gesture("left", "tap", notify("left tap"))
gesture("right", "tap", notify("right tap"))
-- gesture("left", "double-tap", notify("left double tap"))

-- Diagnostic binding: records the gesture in hyprpadstatus instead of running
-- anything. An argv action and log = true cannot be combined.
-- gesture("pad", "tap", { log = true }, 4)

-- Left/right edges take up/down, top/bottom take left/right. `step` is the
-- fraction of the axis between repeats; smaller means more steps.
local sliders = {
    { zone = "left", step = 0.04,
      up = shell("wpctl set-volume @DEFAULT_SINK@ 2%+"),
      down = shell("wpctl set-volume @DEFAULT_SINK@ 2%-") },

    { zone = "right", step = 0.04,
      up = shell("brightnessctl set 2%+"),
      down = shell("brightnessctl set 2%-") },

    -- Top and bottom sliders only work when that edge has no swipe binding.
    -- { zone = "top", step = 0.04,
    --   left = { "playerctl", "position", "5-" },
    --   right = { "playerctl", "position", "5+" } },
    -- { zone = "bottom", step = 0.04,
    --   left = { "playerctl", "previous" },
    --   right = { "playerctl", "next" } },
}

local ok, err = hl.plugin.hyprpad.setup({
    -- "auto" takes the first touchpad-looking device; name the node instead.
    device = "auto",
    -- device = "/dev/input/event12",

    -- Default edge band, then optional per-edge overrides. Fractions of the pad
    -- width and height, each between 0 and 0.5.
    edge_width = 0.10,
    -- left_edge_width = 0.04,
    -- right_edge_width = 0.06,
    -- top_edge_width = 0.10,
    -- bottom_edge_width = 0.08,

    -- Timing and distance; these values are the defaults.
    tap_min_duration_ms = 40, -- shorter holds are not taps
    tap_max_duration_ms = 180,
    double_tap_timeout_ms = 300,
    double_tap_max_distance = 0.04,
    swipe_min_distance = 0.02,

    -- Cancel pointer motion and libinput tap clicks for contacts hyprpad owns.
    -- Only edge slider contacts are swallowed; swipes and taps let the pointer
    -- through, and two-finger scroll and pinch keep working.
    swallow_pointer = true,

    -- Optional low-pass for pads whose cursor jitters at light pressure.
    -- Steady motion stays 1:1; only the frame-to-frame wobble is damped.
    smooth_pointer = false,
    smooth_strength = 0.6, -- 0 = off, 1 = maximum damping

    -- Suppress gestures and slider steps for a moment after a keystroke.
    block_while_typing = { enabled = true, timeout_ms = 500 },

    gestures = gestures,
    sliders = sliders,
})

if not ok then
    -- setup() rejects the whole table and keeps the previous config running.
    hl.notification.create({ text = "hyprpad: " .. tostring(err), timeout = 5000 })
end
```

The keys are a Lua table, so they can be built with locals, helpers and loops
(`gestures[#gestures + 1] = {...}`) rather than written out one literal at a
time. See [`docs/configuration.md`](docs/configuration.md) for what each option
does and how the recognizer picks a binding.

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

Unit checks need no compositor. The end-to-end check drives a virtual touchpad
into the running session and verifies gestures, slider swallowing, the smoothing
hook and config rejection; it skips when no compositor is reachable:

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R e2e --output-on-failure   # integration only
```

The e2e run needs `python-evdev` and leaves the plugin back on your own config.

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
