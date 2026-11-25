#pragma once

#include "../core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprpad {

struct GestureBinding {
    Zone                     zone      = Zone::Left;
    Direction                direction = Direction::Up;
    std::optional<uint8_t>   fingers;
    std::vector<std::string> action;
    // edgepad's `action = { log = true }` diagnostic: record the gesture in
    // status instead of running a command.
    bool                     logGesture = false;
};

struct SliderBinding {
    Zone                     zone;
    SliderAxis               axis = SliderAxis::Vertical;
    float                    step = DEFAULT_SLIDER_STEP;
    std::vector<std::string> negative; // up / left
    std::vector<std::string> positive; // down / right
};

struct Config {
    std::string                    device = "auto";
    EdgeWidths                     edges;
    EngineOptions                  options;
    std::vector<GestureBinding>    gestures;
    std::vector<SliderBinding>     sliders;
    bool                           swallowPointer   = true;
    // Optional low-pass on touchpad pointer motion for cheap pads whose
    // coordinates jitter at light pressure. Off by default: it is a comfort
    // setting, not part of the recognizer.
    bool                           smoothPointer    = false;
    float                          smoothStrength   = 0.6F;
    bool                           blockWhileTyping = false;
    uint64_t                       typingTimeoutMs  = 500;
    uint64_t                       generation       = 0;

    // Validates the whole config and derives the recognizer zone/finger sets
    // from the bindings. On failure returns a human readable reason.
    std::optional<std::string> finalize();

    const GestureBinding*      matchGesture(const Gesture& gesture) const;
    const SliderBinding*       findSlider(Zone zone) const;
    const std::vector<std::string>* actionForSlider(const SliderBinding& slider, SliderDirection direction) const;
};

std::optional<Zone>      parseZone(const std::string& raw);
std::optional<Direction> parseDirection(const std::string& raw);
const char*              sliderAxisName(SliderAxis axis);

} // namespace hyprpad
