#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprpad {

using Micros = int64_t;

inline constexpr int64_t DEFAULT_TAP_MIN_DURATION_MS       = 40;
inline constexpr int64_t DEFAULT_TAP_MAX_DURATION_MS       = 180;
inline constexpr int64_t DEFAULT_DOUBLE_TAP_TIMEOUT_MS     = 300;
inline constexpr float   DEFAULT_DOUBLE_TAP_MAX_DISTANCE   = 0.04F;
inline constexpr float   DEFAULT_SWIPE_MIN_DISTANCE        = 0.02F;
inline constexpr float   DEFAULT_EDGE_WIDTH                = 0.10F;
inline constexpr float   DEFAULT_SLIDER_STEP               = 0.04F;

struct AxisRange {
    int32_t min = 0;
    int32_t max = 0;

    float   normalize(int32_t value) const;
    float   normalizeDelta(int32_t start, int32_t end) const;
};

struct Capabilities {
    int32_t   slotMin = 0;
    int32_t   slotMax = 0;
    AxisRange x;
    AxisRange y;
};

struct EdgeWidths {
    float left   = DEFAULT_EDGE_WIDTH;
    float right  = DEFAULT_EDGE_WIDTH;
    float top    = DEFAULT_EDGE_WIDTH;
    float bottom = DEFAULT_EDGE_WIDTH;

    static EdgeWidths all(float width);
};

// A recognizer event, not an evdev event. The engine only cares about the
// multitouch protocol-B subset needed to tell an edge gesture from a pointer
// contact.
enum class EventType : uint8_t {
    Slot,
    TrackingId,
    X,
    Y,
    SynDropped,
};

struct Event {
    EventType type  = EventType::Slot;
    int32_t   value = 0;
};

enum class Zone : uint8_t {
    Left,
    Right,
    Top,
    Bottom,
    Pad,
};

enum class Direction : uint8_t {
    Up,
    Down,
    Left,
    Right,
    Tap,
    DoubleTap,
};

enum class SliderAxis : uint8_t {
    Horizontal,
    Vertical,
};

enum class SliderDirection : uint8_t {
    Up,
    Down,
    Left,
    Right,
};

inline constexpr uint8_t ZONE_LEFT   = 1U << 0;
inline constexpr uint8_t ZONE_RIGHT  = 1U << 1;
inline constexpr uint8_t ZONE_TOP    = 1U << 2;
inline constexpr uint8_t ZONE_BOTTOM = 1U << 3;
inline constexpr uint8_t ZONE_PAD    = 1U << 4;

constexpr uint8_t zoneBit(Zone zone) {
    switch (zone) {
        case Zone::Left: return ZONE_LEFT;
        case Zone::Right: return ZONE_RIGHT;
        case Zone::Top: return ZONE_TOP;
        case Zone::Bottom: return ZONE_BOTTOM;
        case Zone::Pad: return ZONE_PAD;
    }
    return 0;
}

struct Gesture {
    Zone      zone      = Zone::Pad;
    Direction direction = Direction::Tap;
    uint8_t   fingers   = 1;
    int32_t   slot      = 0;
    int32_t   trackingId = 0;
};

struct SliderSpec {
    Zone       zone = Zone::Left;
    SliderAxis axis = SliderAxis::Vertical;
    float      step = DEFAULT_SLIDER_STEP;
};

struct SliderStep {
    Zone            zone       = Zone::Left;
    SliderDirection direction  = SliderDirection::Up;
    int32_t         slot       = 0;
    int32_t         trackingId = 0;
};

struct ResyncContact {
    int32_t slot       = 0;
    int32_t trackingId = 0;
    int32_t x          = 0;
    int32_t y          = 0;
};

struct EngineOptions {
    Micros  tapMinDuration       = DEFAULT_TAP_MIN_DURATION_MS * 1000;
    Micros  tapMaxDuration       = DEFAULT_TAP_MAX_DURATION_MS * 1000;
    float   swipeMinDistance     = DEFAULT_SWIPE_MIN_DISTANCE;
    Micros  doubleTapTimeout     = DEFAULT_DOUBLE_TAP_TIMEOUT_MS * 1000;
    float   doubleTapMaxDistance = DEFAULT_DOUBLE_TAP_MAX_DISTANCE;
    uint8_t doubleTapZones       = 0;
    uint8_t singleTapZones       = 0;
    // Bit N is set when a whole-pad binding wants an N-finger tap. Zero keeps
    // ordinary clicks out of the tap machinery.
    uint8_t padTapFingers        = 0;
};

struct FrameOutput {
    std::vector<Event>      passthrough;
    std::vector<Gesture>    gestures;
    std::vector<SliderStep> sliderSteps;
    // Set when this frame released a contact that looked like an edge tap.
    // The host uses it to swallow the click libinput's tap-to-click produces.
    bool                    tapLike = false;
};

const char* zoneName(Zone zone);
const char* directionName(Direction direction);
const char* sliderDirectionName(SliderDirection direction);

} // namespace hyprpad
