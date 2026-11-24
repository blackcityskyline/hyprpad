-- hyprpad example configuration.
-- Safe actions: these only show notifications. Replace them with your own
-- volume, brightness, media, and workspace commands.

local ok, err = hl.plugin.hyprpad.setup({
    device = "auto",
    edge_width = 0.10,
    -- Optional per-edge overrides; omitted edges use edge_width.
    -- left_edge_width = 0.08,
    -- right_edge_width = 0.12,
    -- top_edge_width = 0.10,
    -- bottom_edge_width = 0.10,
    tap_min_duration_ms = 40,
    tap_max_duration_ms = 180,
    double_tap_timeout_ms = 300,
    double_tap_max_distance = 0.04,
    swipe_min_distance = 0.02,

    -- Swallow pointer motion and libinput tap clicks while an edge contact is
    -- owned by hyprpad. Two-finger scroll and pinch still reach applications.
    swallow_pointer = true,

    -- Suppress gestures and slider steps for a short window after a keystroke.
    block_while_typing = { enabled = false, timeout_ms = 500 },

    sliders = {
        { zone = "left", step = 0.04,
          up   = { "notify-send", "hyprpad", "volume-up" },
          down = { "notify-send", "hyprpad", "volume-down" } },

        { zone = "right", step = 0.04,
          up   = { "notify-send", "hyprpad", "brightness-up" },
          down = { "notify-send", "hyprpad", "brightness-down" } },
    },

    gestures = {
        { zone = "left", direction = "tap",
          action = { "notify-send", "hyprpad", "volume-mute" } },

        { zone = "right", direction = "tap",
          action = { "notify-send", "hyprpad", "brightness-toggle" } },

        { zone = "top", direction = "left",
          action = { "notify-send", "hyprpad", "media-previous" } },
        { zone = "top", direction = "right",
          action = { "notify-send", "hyprpad", "media-next" } },
        { zone = "top", direction = "tap",
          action = { "notify-send", "hyprpad", "media-play-pause" } },
        { zone = "top", direction = "double-tap",
          action = { "notify-send", "hyprpad", "media-stop" } },

        { zone = "bottom", direction = "left",
          action = { "notify-send", "hyprpad", "workspace-previous" } },
        { zone = "bottom", direction = "right",
          action = { "notify-send", "hyprpad", "workspace-next" } },

        -- Finger-count binding: fingers = 2..4 matches only when that many
        -- fingers were on the pad while the edge finger was down. A binding
        -- without fingers accepts any count; an exact count wins.
        -- { zone = "bottom", direction = "up", fingers = 3,
        --   action = { "notify-send", "hyprpad", "three-finger-up" } },
    },
})

if not ok then
    hl.notification.create({ text = "hyprpad: " .. tostring(err), timeout = 5000 })
end
