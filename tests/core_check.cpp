#include "../src/config/config.hpp"
#include "../src/core/engine.hpp"
#include "check.hpp"

#include <vector>

using namespace hyprpad;

namespace {

constexpr Micros MS = 1000;

Capabilities caps() {
    return Capabilities{0, 4, AxisRange{0, 1000}, AxisRange{0, 1000}};
}

void down(std::vector<Event>& frame, int slot, int id, int x, int y) {
    frame.push_back({EventType::Slot, slot});
    frame.push_back({EventType::TrackingId, id});
    frame.push_back({EventType::X, x});
    frame.push_back({EventType::Y, y});
}

void move(std::vector<Event>& frame, int slot, int x, int y) {
    frame.push_back({EventType::Slot, slot});
    frame.push_back({EventType::X, x});
    frame.push_back({EventType::Y, y});
}

void up(std::vector<Event>& frame, int slot, int id) {
    frame.push_back({EventType::Slot, slot});
    frame.push_back({EventType::TrackingId, -1});
    (void)id;
}

EngineOptions tapOptions() {
    EngineOptions options;
    options.singleTapZones = ZONE_LEFT;
    return options;
}

void testEdgeSwipe() {
    Engine engine(caps(), EdgeWidths::all(0.10F));
    std::vector<Event> frame;
    down(frame, 0, 1, 50, 700);
    auto output = engine.processFrameAt(frame, 1'000 * MS);
    CHECK(output.gestures.empty());
    CHECK(engine.hasClaimedContacts());

    frame.clear();
    move(frame, 0, 50, 300);
    engine.processFrameAt(frame, 1'020 * MS);

    frame.clear();
    up(frame, 0, 1);
    output = engine.processFrameAt(frame, 1'040 * MS);
    CHECK(output.gestures.size() == 1);
    CHECK(output.gestures[0].zone == Zone::Left);
    CHECK(output.gestures[0].direction == Direction::Up);
    CHECK(output.gestures[0].fingers == 1);
    CHECK(!engine.hasClaimedContacts());
}

void testCenterIsPassthrough() {
    Engine engine(caps(), EdgeWidths::all(0.10F));
    std::vector<Event> frame;
    down(frame, 0, 1, 500, 500);
    auto output = engine.processFrameAt(frame, 1'000 * MS);
    CHECK(output.gestures.empty());
    CHECK(!engine.hasClaimedContacts());
    CHECK(!output.passthrough.empty());

    frame.clear();
    move(frame, 0, 600, 400);
    engine.processFrameAt(frame, 1'010 * MS);
    frame.clear();
    up(frame, 0, 1);
    output = engine.processFrameAt(frame, 1'020 * MS);
    CHECK(output.gestures.empty());
}

void testEdgeTap() {
    Engine engine(caps(), EdgeWidths::all(0.10F), {}, tapOptions());
    std::vector<Event> frame;
    down(frame, 0, 7, 30, 500);
    engine.processFrameAt(frame, 1'000 * MS);
    frame.clear();
    up(frame, 0, 7);
    const auto output = engine.processFrameAt(frame, 1'060 * MS);
    CHECK(output.gestures.size() == 1);
    CHECK(output.gestures[0].direction == Direction::Tap);
    CHECK(output.tapLike);
}

void testDoubleTap() {
    EngineOptions options = tapOptions();
    options.doubleTapZones = ZONE_LEFT;
    Engine engine(caps(), EdgeWidths::all(0.10F), {}, options);

    std::vector<Event> frame;
    down(frame, 0, 1, 30, 500);
    engine.processFrameAt(frame, 1'000 * MS);
    frame.clear();
    up(frame, 0, 1);
    auto output = engine.processFrameAt(frame, 1'050 * MS);
    CHECK(output.gestures.empty());
    CHECK(engine.nextDeadline().has_value());

    frame.clear();
    down(frame, 0, 2, 32, 502);
    engine.processFrameAt(frame, 1'150 * MS);
    frame.clear();
    up(frame, 0, 2);
    output = engine.processFrameAt(frame, 1'200 * MS);
    CHECK(output.gestures.size() == 1);
    CHECK(output.gestures[0].direction == Direction::DoubleTap);
}

void testSlider() {
    std::vector<SliderSpec> sliders{{Zone::Left, SliderAxis::Vertical, 0.05F}};
    Engine engine(caps(), EdgeWidths::all(0.10F), sliders, EngineOptions{});
    std::vector<Event> frame;
    down(frame, 0, 1, 30, 500);
    engine.processFrameAt(frame, 1'000 * MS);

    frame.clear();
    move(frame, 0, 30, 390);
    auto output = engine.processFrameAt(frame, 1'010 * MS);
    CHECK(output.sliderSteps.size() == 2);
    CHECK(output.sliderSteps[0].direction == SliderDirection::Up);
    CHECK(output.gestures.empty());

    frame.clear();
    up(frame, 0, 1);
    output = engine.processFrameAt(frame, 1'100 * MS);
    CHECK(output.gestures.empty());
}

void testFingerCount() {
    Engine engine(caps(), EdgeWidths::all(0.10F));
    std::vector<Event> frame;
    down(frame, 0, 1, 30, 500);
    down(frame, 1, 2, 500, 500);
    engine.processFrameAt(frame, 1'000 * MS);

    frame.clear();
    move(frame, 0, 30, 200);
    engine.processFrameAt(frame, 1'020 * MS);

    frame.clear();
    up(frame, 1, 2);
    engine.processFrameAt(frame, 1'030 * MS);
    frame.clear();
    up(frame, 0, 1);
    const auto output = engine.processFrameAt(frame, 1'040 * MS);
    CHECK(output.gestures.size() == 1);
    CHECK(output.gestures[0].fingers == 2);
}

void testPadTap() {
    EngineOptions options;
    options.padTapFingers = 1U << 3;
    Engine engine(caps(), EdgeWidths::all(0.10F), {}, options);

    std::vector<Event> frame;
    down(frame, 0, 10, 500, 500);
    down(frame, 1, 11, 510, 500);
    down(frame, 2, 12, 520, 500);
    engine.processFrameAt(frame, 1'000 * MS);

    frame.clear();
    up(frame, 1, 11);
    engine.processFrameAt(frame, 1'040 * MS);
    frame.clear();
    up(frame, 2, 12);
    engine.processFrameAt(frame, 1'045 * MS);
    frame.clear();
    up(frame, 0, 10);
    const auto output = engine.processFrameAt(frame, 1'050 * MS);

    CHECK(output.gestures.size() == 1);
    CHECK(output.gestures[0].zone == Zone::Pad);
    CHECK(output.gestures[0].fingers == 3);
}

void testBindingPrecedenceAndValidation() {
    Config config;
    config.gestures.push_back({Zone::Left, Direction::Up, std::nullopt, {"echo", "any"}});
    config.gestures.push_back({Zone::Left, Direction::Up, 3, {"echo", "three"}});
    CHECK(!config.finalize().has_value());

    Gesture gesture{Zone::Left, Direction::Up, 3, 0, 1};
    const auto* binding = config.matchGesture(gesture);
    CHECK(binding);
    CHECK(binding->action[1] == "three");

    gesture.fingers = 2;
    binding         = config.matchGesture(gesture);
    CHECK(binding);
    CHECK(binding->action[1] == "any");

    Config conflicting;
    conflicting.gestures.push_back({Zone::Left, Direction::Up, std::nullopt, {"true"}});
    conflicting.sliders.push_back({Zone::Left, SliderAxis::Vertical, 0.04F, {"true"}, {"true"}});
    CHECK(conflicting.finalize().has_value());
}

} // namespace

int main() {
    testEdgeSwipe();
    testCenterIsPassthrough();
    testEdgeTap();
    testDoubleTap();
    testSlider();
    testFingerCount();
    testPadTap();
    testBindingPrecedenceAndValidation();
    std::puts("hyprpad core checks passed");
    return 0;
}
