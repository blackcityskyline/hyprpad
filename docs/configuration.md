# Configuration

hyprpad is a Hyprland plugin, so its configuration lives in your Hyprland Lua
config. Call `hl.plugin.hyprpad.setup(table)` once. Re-calling it replaces the
running configuration.

```lua
local ok, err = hl.plugin.hyprpad.setup({
    edge_width = 0.10,
    gestures = {
        { zone = "top", direction = "right", action = { "notify-send", "hyprpad", "top-right" } },
    },
})
if not ok then
    hl.notification.create({ text = "hyprpad: " .. tostring(err), timeout = 5000 })
end
```

Put the call in a `require("plugins/hyprpad")` file next to your other plugins.
See the [example config](../examples/hyprpad.lua).

`setup` returns `true` on success and `false, message` on a rejected
configuration. A rejected configuration leaves the previous bindings running.

## Gestures

A gesture starts in an edge zone and runs its action when the finger lifts.
Zones are `left`, `right`, `top`, and `bottom`. Directions are `up`, `down`,
`left`, `right`, `tap`, and `double-tap`.

```lua
gestures = {
    { zone = "top", direction = "right",
      action = { "notify-send", "hyprpad", "top-right" } },
}
```

Each zone/direction pair can have one binding. Edges without bindings remain
ordinary touchpad area: hyprpad never touches those contacts.

### Multi-finger gestures

Add `fingers` to a directional binding to pick it by how many fingers took part
in the gesture:

```lua
{ zone = "left", direction = "up", fingers = 3,
  action = { "notify-send", "hyprpad", "three-finger-up" } },
```

Gestures fire as before: a contact that starts inside an edge zone runs its
action when it lifts, whatever else is on the touchpad. `fingers` only selects
which binding answers.

The edge zone only decides that a gesture started. The count is how many fingers
were on the touchpad while the gesture finger stayed down, wherever they
touched: three fingertips do not fit inside the edge band, so the other fingers
are expected to rest elsewhere. A gesture finger keeps the highest count it saw,
so fingers lifting one after another still report the whole group.

Fingers that already rested on the pad count too. Lift them before starting a
multi-finger gesture, or rely on [block-while-typing](#block-while-typing) after
typing.

A binding without `fingers` accepts any count, and an explicit count wins when
both match:

```lua
gestures = {
    { zone = "left", direction = "up", action = { "pamixer", "-i", "3" } },
    { zone = "left", direction = "up", fingers = 3, action = { "playerctl", "play-pause" } },
}
```

`fingers` accepts `1` to `4`; omitting it accepts any count, and `fingers = 1`
matches only a single finger. Slider zones keep allowing only taps.

### Multi-finger taps

Taps work by group: contacts that overlap are one tap, and the first contact of
the group emits it with the whole group's finger count. Three fingers tapped
twice therefore produce one `double-tap` gesture with `fingers = 3`:

```lua
{ zone = "top", direction = "double-tap", fingers = 3,
  action = { "playerctl", "play-pause" } },
```

For a double tap to complete, the second group must have the same finger count
as the first: a one-finger tap after a three-finger tap is just a single tap of
each. Directional swipes keep firing once per finger that started in the edge
band; only taps are grouped.

### Whole-pad taps

A tap binding with `zone = "pad"` works anywhere on the touchpad instead of on
an edge:

```lua
{ zone = "pad", direction = "double-tap", fingers = 3,
  action = { "playerctl", "play-pause" } },
```

`pad` accepts only `tap` and `double-tap`. These contacts are never held back:
the pointer still receives them, so an ordinary click, scroll or drag behaves as
usual, and the tap action runs in addition to whatever the desktop does. Only
the finger counts named by a binding are watched, so a config that binds
`fingers = 3` leaves one-finger clicks alone.

A finger that starts inside an edge band still belongs to that edge, so edge
bindings win over `pad` there.

## Sliders

Sliders repeat actions while your finger moves. Left and right edges use
`up`/`down`; top and bottom edges use `left`/`right`.

```lua
sliders = {
    { zone = "left", step = 0.04,
      up   = { "pamixer", "-i", "3" },
      down = { "pamixer", "-d", "3" } },
}
```

`step` is the distance between actions as a fraction of the corresponding
touchpad axis. The default, `0.04`, means one step per 4% of travel. Smaller
values produce more steps for the same movement.

An edge can have one slider alongside tap and double-tap bindings. It cannot
have both a slider and a directional swipe binding. Once a slider emits a step,
lifting the finger won't also trigger a tap.

Bursts of slider steps are coalesced per zone and direction: a fast swipe runs
the newest step's action, not one process per step.

## Actions

An action is an array containing a program and its arguments. Commands are not
passed through a shell; if you need pipes or redirection, invoke a shell
explicitly:

```lua
{ zone = "bottom", direction = "tap",
  action = { "sh", "-c", "date >> /tmp/hyprpad-actions.log" } },
```

The program must be on Hyprland's `PATH`. Actions run on a helper thread, so a
slow command cannot stall gesture recognition.

## Device and edge widths

```lua
device = "auto",
edge_width = 0.10,
left_edge_width = 0.08,
right_edge_width = 0.12,
```

`auto` selects the touchpad when exactly one readable candidate is found, and
skips synthetic devices such as edgepad's virtual touchpad. If several real
touchpads remain, list them with `hl.plugin.hyprpad.devices()` or
`hyprctl hyprpaddevices`, then set an explicit path such as
`device = "/dev/input/event7"`.

`edge_width` defaults to `0.10`: 10% of the relevant touchpad dimension for each
edge. `left_edge_width`, `right_edge_width`, `top_edge_width`, and
`bottom_edge_width` override individual edges. Only edges with bindings reserve
space for gestures.

hyprpad reads the touchpad's evdev node read-only. It never grabs the device and
never creates a synthetic one, so libinput keeps full control of the pointer.
Grant read access with the shipped udev rule
(`packaging/70-hyprpad.rules`).

## Tap and swipe sensitivity

| Setting | Default | Meaning |
| --- | --- | --- |
| `tap_min_duration_ms` | `40` | Minimum tap duration. Set to `0` to allow very short taps. |
| `tap_max_duration_ms` | `180` | A tap must end before this duration; longer contacts are holds. |
| `swipe_min_distance` | `0.02` | Per-axis travel needed to become a swipe: 2% of the axis. |
| `double_tap_timeout_ms` | `300` | Maximum time from the first tap's release to the second tap's start. |
| `double_tap_max_distance` | `0.04` | Maximum Euclidean distance between the first tap's release and the second tap's start, in normalized coordinates. Ignored for groups of two or more fingers. |

A contact stops qualifying as a tap once it reaches the swipe distance, even if
it returns to its starting point.

Double taps must land on the same edge within both limits. If an edge has both
`tap` and `double-tap` bindings, the single-tap action waits for the double-tap
window to expire, unless another interaction rules out the second tap first.
Edges without a double-tap binding run taps immediately on release.

## Pointer side effects

hyprpad owns edge contacts, and by default it hides their side effects from
applications:

- while an edge-owned contact exists, pointer motion events are cancelled, so
  the cursor does not drift while you slide along an edge;
- shortly after an edge tap, the next pointer press is cancelled, which
  swallows libinput's own tap-to-click so an edge tap does not also click;
- axis (scroll) events are never cancelled: two-finger scroll still works.

```lua
swallow_pointer = false, -- pass motion and clicks through
```

Turn it off if you want edge gestures to behave like plain touchpad input for
the applications under them.

## Block while typing

`block_while_typing` suppresses gestures and slider steps for a short window
after a real keystroke, so stray palm or thumb contacts near the keyboard don't
trigger actions. It only suppresses hyprpad's own actions; the pointer and the
desktop's own disable-while-typing are unaffected.

```lua
block_while_typing = { enabled = true, timeout_ms = 500 },
```

It is also accepted as a boolean (`block_while_typing = true`) plus
`block_while_typing_timeout_ms = 500`, matching edgepad's TOML spelling.
Keyboard events come from Hyprland itself, so no keyboard device access is
needed.

## Inspecting state

```lua
print(hl.plugin.hyprpad.status())    -- device, counters, last error
print(hl.plugin.hyprpad.version())
local devices = hl.plugin.hyprpad.devices()
```

From a terminal:

```bash
hyprctl hyprpadstatus    # state, device, counters, last error
hyprctl hyprpaddevices   # touchpad candidates and readability
```

## Apply changes

Re-run `setup()` — for example by reloading your Hyprland config. hyprpad
validates the whole table first and keeps the previous configuration when it
rejects the new one. Recognition-only changes apply at the next idle frame; a
changed `device` closes and reopens the touchpad.
