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
