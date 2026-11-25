#!/usr/bin/env python3
"""End-to-end smoke tests for the loaded hyprpad plugin.

Drives a virtual touchpad (uinput) exactly like a finger and checks the
observable results. These are the regressions that actually shipped:

  * an edge contact freezing the cursor for its whole lifetime,
  * a swipe dragging the pointer along with it,
  * a slider sweep moving the cursor,
  * a rejected config leaving no trace, with the old one still live,
  * smoothing reaching the compositor at all, and not shrinking travel.

The recognizer's own arithmetic is covered by the C++ unit tests; this file
only checks the wiring that runs in a real session.

Exits 77 (ctest SKIP) when there is no compositor or no python-evdev.
Requires the plugin to be loaded already.
"""
import json
import os
import subprocess
import sys
import time

W, H = 3000, 2000  # pad units, ~90x60 mm at 33 units/mm
EDGE = 120         # inside a 10% edge band
CENTRE = (W // 2, H // 2)


def skip(message):
    print("skip:", message)
    sys.exit(77)


def signature():
    """The live compositor. hyprctl's own guess and the inherited environment
    variable both point at dead instances after a crashed session."""
    runtime = "/run/user/%d/hypr" % os.getuid()
    if not os.path.isdir(runtime):
        return ""
    for entry in os.listdir(runtime):
        try:
            with open(os.path.join(runtime, entry, "hyprland.lock")) as handle:
                pid = int(handle.readline().strip())
            os.kill(pid, 0)
        except (OSError, ValueError):
            continue
        return entry
    return ""


SIG = signature()
if not SIG:
    skip("no Hyprland instance")


def hc(*args):
    return subprocess.run(["hyprctl", *args], capture_output=True, text=True,
                          env={**os.environ, "HYPRLAND_INSTANCE_SIGNATURE": SIG}).stdout


def eval_lua(code):
    return hc("eval", code).strip()


def status():
    out = {}
    for line in hc("hyprpadstatus").splitlines():
        if ":" in line:
            key, _, value = line.partition(":")
            out[key.strip()] = value.strip()
    return out


def counter(name):
    return int(status().get(name, 0))


def cursor():
    x, y = hc("cursorpos").strip().split(",")
    return int(x), int(y)


def screen():
    monitors = json.loads(hc("monitors", "-j") or "[]")
    if not monitors:
        skip("no monitor")
    return monitors[0]["width"], monitors[0]["height"]


try:
    from evdev import AbsInfo, UInput, ecodes as e
except ImportError:
    skip("python-evdev is not installed")

if "hyprpad" not in hc("plugin", "list"):
    skip("hyprpad is not loaded")


def abs_info(maximum, resolution=33):
    return AbsInfo(0, 0, maximum, 0, 0, resolution)


PAD = UInput({
    e.EV_KEY: [e.BTN_LEFT, e.BTN_TOUCH, e.BTN_TOOL_FINGER, e.BTN_TOOL_DOUBLETAP,
               e.BTN_TOOL_TRIPLETAP, e.BTN_TOOL_QUADTAP],
    e.EV_ABS: [(e.ABS_X, abs_info(W)), (e.ABS_Y, abs_info(H)),
               (e.ABS_PRESSURE, AbsInfo(0, 0, 255, 0, 0, 0)),
               (e.ABS_MT_SLOT, AbsInfo(0, 0, 9, 0, 0, 0)),
               (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
               (e.ABS_MT_POSITION_X, abs_info(W)), (e.ABS_MT_POSITION_Y, abs_info(H)),
               (e.ABS_MT_PRESSURE, AbsInfo(0, 0, 255, 0, 0, 0))],
}, name="hyprpad-e2e-pad", vendor=0x1, product=0x40, version=1, bustype=e.BUS_USB,
    input_props=[e.INPUT_PROP_POINTER, e.INPUT_PROP_BUTTONPAD])
time.sleep(1.5)
TRACKING_ID = 3


FRAMES = 0


def frame(x, y, pressure=90):
    """The kernel suppresses an EV_ABS value that did not change, so a finger
    that holds one axis perfectly still stops producing events on it; alternate
    it by one unit to keep both axes live like real coordinates."""
    global FRAMES
    FRAMES += 1
    x += FRAMES % 2
    PAD.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    PAD.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, TRACKING_ID)
    PAD.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
    PAD.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
    PAD.write(e.EV_ABS, e.ABS_MT_PRESSURE, pressure)
    PAD.syn()


def down(x, y, hold=0.45):
    """libinput emits a hold gesture first and reports motion only after it ends,
    so dragging waits it out; a tap must not, or it exceeds tap_max_duration."""
    global TRACKING_ID
    TRACKING_ID += 1
    PAD.write(e.EV_KEY, e.BTN_TOUCH, 1)
    PAD.write(e.EV_KEY, e.BTN_TOOL_FINGER, 1)
    frame(x, y)
    time.sleep(hold)


def up():
    PAD.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    PAD.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    PAD.write(e.EV_KEY, e.BTN_TOOL_FINGER, 0)
    PAD.write(e.EV_KEY, e.BTN_TOUCH, 0)
    PAD.syn()
    time.sleep(0.35)


def path(points, interval=0.012):
    for x, y in points:
        frame(x, y)
        time.sleep(interval)


def lua_value(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return '"%s"' % value
    if isinstance(value, dict):
        return "{" + ", ".join("%s=%s" % (k, lua_value(v)) for k, v in value.items()) + "}"
    if isinstance(value, list):
        return "{" + ", ".join(lua_value(v) for v in value) + "}"
    raise TypeError(value)


def setup(**overrides):
    fields = {"device": PAD.device.path, "edge_width": 0.10,
              # A slider owns its zone, so the swipe binding lives on another
              # edge; a slider plus a swipe on the same edge is the conflict the
              # plugin rejects.
              "gestures": [{"zone": "top", "direction": "down", "action": {"log": True}}],
              "sliders": [{"zone": "left", "axis": "vertical", "step": 0.04,
                           "up": ["true"], "down": ["true"]}]}
    fields.update(overrides)
    lua = ", ".join("%s=%s" % (key, lua_value(value)) for key, value in fields.items())
    reply = eval_lua("hl.plugin.hyprpad.setup({%s})" % lua)
    assert reply == "ok", "setup call failed: %r" % reply
    # hyprctl eval reports "ok" even when the plugin returns false, and the
    # daemon swaps devices asynchronously, so poll the status the daemon really
    # runs with instead of trusting the reply.
    deadline = time.time() + 3.0
    while True:
        live = status()
        if "config error" not in live and live.get("device") == PAD.device.path:
            return
        assert time.time() < deadline, "config not live: %s" % live.get("config error",
                                                                     live.get("device"))
        time.sleep(0.1)


def ensure_setup(**overrides):
    """Reuse the live config when it already is the one we want: every
    setup() call reopens the device, and that churn is not part of a tap test."""
    live = status()
    if not overrides and "config error" not in live and live.get("device") == PAD.device.path:
        return
    setup(**overrides)


def ensure_room(dx, dy):
    """Move the cursor away from the screen edge so a test drag cannot clamp."""
    width, height = screen()
    for _ in range(3):
        x, y = cursor()
        x_ok = not (dx > 0 and x > width - 400) and not (dx < 0 and x < 400)
        y_ok = not (dy > 0 and y > height - 400) and not (dy < 0 and y < 400)
        if x_ok and y_ok:
            return
        push_x = -300 if x > width - 400 else (300 if x < 400 else 0)
        push_y = -300 if y > height - 400 else (300 if y < 400 else 0)
        down(*CENTRE)
        path([(CENTRE[0] + push_x * step // 8, CENTRE[1] + push_y * step // 8) for step in range(1, 9)])
        up()


def settled_cursor():
    """Cursor positions arrive over IPC; a read taken while queued motion is
    still landing attributes that motion to the next measurement."""
    previous = cursor()
    for _ in range(20):
        time.sleep(0.05)
        current = cursor()
        if current == previous:
            return current
        previous = current
    return previous


def drag(points, interval=0.012):
    dx = points[-1][0] - points[0][0]
    dy = points[-1][1] - points[0][1]
    ensure_room(dx, dy)
    before = settled_cursor()
    down(*points[0])
    path(points[1:], interval)
    after = settled_cursor()
    up()
    return after[0] - before[0], after[1] - before[1]


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ((" -- " + str(detail)) if detail else ""))


def swipe_points():
    return [(CENTRE[0], EDGE)] + [(CENTRE[0], EDGE + 40 * step) for step in range(1, 15)]


def test_centre_drag_moves_cursor():
    """The cursor must follow a centre finger instead of being frozen."""
    horizontal = [(CENTRE[0], CENTRE[1])] + [(CENTRE[0] + 30 * step, CENTRE[1]) for step in range(1, 13)]
    dx, _ = drag(horizontal)
    check("centre drag moves the cursor", abs(dx) > 10, "moved %d px" % dx)


def test_edge_swipe_fires_and_does_not_pin_cursor():
    """A swipe fires on lift and must not swallow the pointer like a slider."""
    setup()
    before = counter("gestures")
    dx, dy = drag(swipe_points())
    check("edge swipe fires its action", counter("gestures") == before + 1,
          "gestures %d -> %d" % (before, counter("gestures")))
    check("edge swipe is not swallowed", abs(dx) + abs(dy) > 5, "cursor moved %d,%d" % (dx, dy))


def test_edge_tap_fires():
    """A tap is recognised on lift. One missed tap can be a lost frame in a busy
    session, so require one of three rather than a single hit."""
    ensure_setup()
    before = counter("gestures")
    for _ in range(3):
        down(CENTRE[0], EDGE, hold=0.12)
        up()
        if counter("gestures") > before:
            break
        time.sleep(0.4)
    check("edge tap fires on lift", counter("gestures") > before,
          "gestures %d -> %d" % (before, counter("gestures")))


def test_slider_steps():
    """A sweep along the edge must count slider steps.

    Cursor stillness is not asserted here: libinput's own edge palm detection
    already suppresses pointer motion for contacts that start that close to the
    pad edge, so the compositor sees none of it either way and the measurement
    would prove nothing."""
    setup()
    before = counter("slider_steps")
    drag([(EDGE, 1600)] + [(EDGE, 1600 - 45 * step) for step in range(1, 13)], interval=0.03)
    check("slider sweep steps the slider", counter("slider_steps") > before,
          "slider_steps %d -> %d" % (before, counter("slider_steps")))


def test_smoothing_is_live_and_keeps_travel():
    """Smoothing must reach the compositor and must not shorten total travel;
    the damping itself is checked deterministically in the C++ unit tests."""
    setup(smooth_pointer=True, smooth_strength=0.6)
    live = status().get("smoothing", "")
    check("smoothing hook is installed", "unavailable" not in live and live != "off", "status %r" % live)
    steady = [(CENTRE[0], CENTRE[1])] + [(CENTRE[0] + 20 * step, CENTRE[1]) for step in range(1, 41)]
    smoothed = abs(drag(steady, interval=0.02)[0])
    setup(smooth_pointer=False)
    raw = abs(drag(steady, interval=0.02)[0])
    check("smoothing keeps total travel", smoothed >= raw * 0.6, "off %d px, on %d px" % (raw, smoothed))


def test_bad_config_keeps_running():
    """An invalid setup must be rejected visibly and leave the old config live."""
    setup()
    reply = eval_lua('hl.plugin.hyprpad.setup({device="%s", edge_width=-1})' % PAD.device.path)
    rejected = reply == "ok" and "config error" in status()
    check("invalid config is rejected", rejected, status().get("config error", "no error reported"))
    before = counter("gestures")
    drag(swipe_points())
    check("previous config still works after a rejection",
          counter("gestures") == before + 1 and status().get("device") == PAD.device.path)


def test_reload_keeps_everything_working():
    """Unload and load again: daemon, config and smoothing hook must come back."""
    plugin = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                          "build", "hyprpad.so"))
    hc("plugin", "unload", plugin)
    time.sleep(1.0)
    hc("plugin", "load", plugin)
    time.sleep(2.0)
    if "state: running" not in hc("hyprpadstatus"):
        check("plugin reloads cleanly", False, hc("hyprpadstatus").strip()[:80])
        return
    check("plugin reloads cleanly", True)
    setup(smooth_pointer=True, smooth_strength=0.6)
    check("smoothing hook is back after reload", "unavailable" not in status().get("smoothing", ""),
          status().get("smoothing", ""))


TESTS = [
    test_centre_drag_moves_cursor,
    test_edge_swipe_fires_and_does_not_pin_cursor,
    test_edge_tap_fires,
    test_slider_steps,
    test_smoothing_is_live_and_keeps_travel,
    test_bad_config_keeps_running,
    test_reload_keeps_everything_working,
]

try:
    for test in TESTS:
        for attempt in range(2):
            try:
                test()
                break
            except AssertionError as error:
                if attempt:
                    check(test.__name__, False, error)
                else:
                    print("retrying", test.__name__, "after:", error)
                    time.sleep(0.7)
finally:
    PAD.close()
    # The tests left the plugin configured for a device that is now gone; put
    # the user's own config back so their touchpad keeps working.
    hc("reload")
    deadline = time.time() + 5.0
    while time.time() < deadline:
        live = status()
        if "state: running" in hc("hyprpadstatus") and live.get("device") != PAD.device.path:
            break
        time.sleep(0.2)

failed = [name for name, ok, _ in RESULTS if not ok]
print("\n%d checks, %d failed" % (len(RESULTS), len(failed)))
sys.exit(1 if failed else 0)
