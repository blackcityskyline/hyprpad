#include "config.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace hyprpad {

std::optional<Zone> parseZone(const std::string& raw) {
    if (raw == "left")
        return Zone::Left;
    if (raw == "right")
        return Zone::Right;
    if (raw == "top")
        return Zone::Top;
    if (raw == "bottom")
        return Zone::Bottom;
    if (raw == "pad")
        return Zone::Pad;
    return std::nullopt;
}

std::optional<Direction> parseDirection(const std::string& raw) {
    if (raw == "up")
        return Direction::Up;
    if (raw == "down")
        return Direction::Down;
    if (raw == "left")
        return Direction::Left;
    if (raw == "right")
        return Direction::Right;
    if (raw == "tap")
        return Direction::Tap;
    if (raw == "double-tap")
        return Direction::DoubleTap;
    return std::nullopt;
}

const char* sliderAxisName(SliderAxis axis) {
    return axis == SliderAxis::Horizontal ? "horizontal" : "vertical";
}

namespace {

std::string gestureLabel(size_t index) {
    return "gestures[" + std::to_string(index) + "]";
}

std::optional<SliderAxis> sliderAxisForZone(Zone zone) {
    if (zone == Zone::Left || zone == Zone::Right)
        return SliderAxis::Vertical;
    if (zone == Zone::Top || zone == Zone::Bottom)
        return SliderAxis::Horizontal;
    return std::nullopt;
}

std::string bindingKey(const GestureBinding& binding) {
    return std::string(zoneName(binding.zone)) + "/" + directionName(binding.direction) + "/" +
           (binding.fingers ? std::to_string(*binding.fingers) : "any");
}

} // namespace

std::optional<std::string> Config::finalize() {
    if (!(edges.left > 0.0F && edges.left < 0.5F))
        return "edge widths must be > 0 and < 0.5";
    if (!(edges.right > 0.0F && edges.right < 0.5F))
        return "edge widths must be > 0 and < 0.5";
    if (!(edges.top > 0.0F && edges.top < 0.5F))
        return "edge widths must be > 0 and < 0.5";
    if (!(edges.bottom > 0.0F && edges.bottom < 0.5F))
        return "edge widths must be > 0 and < 0.5";

    if (options.tapMinDuration < 0 || options.tapMinDuration >= options.tapMaxDuration)
        return "tap_min_duration_ms must be < tap_max_duration_ms";
    if (options.doubleTapTimeout <= 0 || options.doubleTapTimeout > 10'000'000)
        return "double_tap_timeout_ms must be > 0 and <= 10000";
    if (!(options.swipeMinDistance > 0.0F && options.swipeMinDistance <= 1.0F))
        return "swipe_min_distance must be > 0 and <= 1";
    if (!(options.doubleTapMaxDistance > 0.0F && options.doubleTapMaxDistance <= 1.0F))
        return "double_tap_max_distance must be > 0 and <= 1";

    if (!device.empty() && device != "auto" && device.rfind("/dev/input/", 0) != 0)
        return "device must be \"auto\" or a /dev/input/eventN path";

    uint8_t doubleTapZones = 0;
    uint8_t singleTapZones = 0;
    uint8_t padTapFingers   = 0;
    std::set<std::string> keys;
    std::set<Zone>        sliderZones;

    for (size_t index = 0; index < gestures.size(); ++index) {
        const auto& gesture = gestures[index];
        const auto  label   = gestureLabel(index);
        if (gesture.action.empty())
            return label + ".action must be a non-empty argv array";

        if (gesture.fingers && (*gesture.fingers < 1 || *gesture.fingers > 4))
            return label + ".fingers must be between 1 and 4";

        if (gesture.zone == Zone::Pad &&
            gesture.direction != Direction::Tap && gesture.direction != Direction::DoubleTap)
            return label + ": zone \"pad\" only accepts tap and double-tap";

        if (!keys.insert(bindingKey(gesture)).second)
            return "duplicate gesture binding for " + bindingKey(gesture);

        if (gesture.direction == Direction::DoubleTap)
            doubleTapZones |= zoneBit(gesture.zone);
        if (gesture.direction == Direction::Tap)
            singleTapZones |= zoneBit(gesture.zone);

        if (gesture.zone == Zone::Pad) {
            if (gesture.fingers)
                padTapFingers |= static_cast<uint8_t>(1U << *gesture.fingers);
            else
                padTapFingers |= 0b11110U;
        }
    }

    for (size_t index = 0; index < sliders.size(); ++index) {
        const auto& slider = sliders[index];
        const auto  label  = "sliders[" + std::to_string(index) + "]";
        const auto  axis   = sliderAxisForZone(slider.zone);
        if (!axis)
            return label + ".zone must be left, right, top, or bottom";
        if (slider.axis != *axis)
            return label + ": axis does not match zone";
        if (!(slider.step > 0.0F && slider.step <= 1.0F))
            return label + ".step must be > 0 and <= 1";
        if (!sliderZones.insert(slider.zone).second)
            return "duplicate slider for zone " + std::string(zoneName(slider.zone));
        if (slider.negative.empty() || slider.positive.empty())
            return label + " needs both slider directions";
    }

    for (size_t index = 0; index < gestures.size(); ++index) {
        const auto& gesture = gestures[index];
        if (gesture.direction == Direction::Tap || gesture.direction == Direction::DoubleTap)
            continue;
        if (sliderZones.count(gesture.zone))
            return gestureLabel(index) + " conflicts with the slider on zone " + zoneName(gesture.zone) +
                   "; slider zones only allow tap or double-tap gestures";
    }

    options.doubleTapZones = doubleTapZones;
    options.singleTapZones = singleTapZones;
    options.padTapFingers  = padTapFingers;
    return std::nullopt;
}

const GestureBinding* Config::matchGesture(const Gesture& gesture) const {
    const GestureBinding* wildcard = nullptr;
    for (const auto& binding : gestures) {
        if (binding.zone != gesture.zone || binding.direction != gesture.direction)
            continue;
        if (binding.fingers) {
            if (*binding.fingers == gesture.fingers)
                return &binding;
        } else if (!wildcard) {
            wildcard = &binding;
        }
    }
    return wildcard;
}

const SliderBinding* Config::findSlider(Zone zone) const {
    for (const auto& slider : sliders) {
        if (slider.zone == zone)
            return &slider;
    }
    return nullptr;
}

const std::vector<std::string>* Config::actionForSlider(const SliderBinding& slider, SliderDirection direction) const {
    switch (direction) {
        case SliderDirection::Up:
        case SliderDirection::Left: return &slider.negative;
        case SliderDirection::Down:
        case SliderDirection::Right: return &slider.positive;
    }
    return nullptr;
}

} // namespace hyprpad
