#include "../src/config/config.hpp"
#include "../src/core/engine.hpp"
#include "../src/core/smoothing.hpp"
#include "check.hpp"

#include <linux/input.h>

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
    // Owning a gesture contact must not freeze the cursor; only a slider does.
    CHECK(!engine.hasSliderContacts());

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
    CHECK(engine.hasSliderContacts());
    CHECK(engine.sliderActivityAt() == 1'010 * MS);

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

void testResyncPassthrough() {
    Engine engine(caps(), EdgeWidths::all(0.10F), {}, tapOptions());
    auto   output = engine.restorePassthroughContacts({ResyncContact{0, 42, 30, 500}});
    CHECK(!output.passthrough.empty());
    CHECK(!engine.hasClaimedContacts());

    std::vector<Event> frame;
    up(frame, 0, 42);
    output = engine.processFrameAt(frame, 1'000 * MS);
    CHECK(output.gestures.empty());
    CHECK(!engine.hasClaimedContacts());
}

void testButtonpadRestoreBlocksEdge() {
    Engine engine(caps(), EdgeWidths::all(0.10F));
    engine.setButtonpad(true);

    // A physical click is down, so an edge contact that appears during the
    // click is passthrough, not a gesture.
    engine.restorePressedPhysicalButtons({BTN_LEFT});
    std::vector<Event> frame;
    down(frame, 0, 1, 50, 500);
    engine.processFrameAt(frame, 1'000 * MS);
    CHECK(!engine.hasClaimedContacts());

    frame.clear();
    up(frame, 0, 1);
    engine.processFrameAt(frame, 1'100 * MS);
    engine.restorePressedPhysicalButtons({});

    frame.clear();
    down(frame, 0, 2, 50, 500);
    engine.processFrameAt(frame, 1'200 * MS);
    CHECK(engine.hasClaimedContacts());
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

void testMotionSmoothing() {
    // Steady motion must come out 1:1: smoothing delays travel, it must not
    // shrink it, or the cursor would feel like it lost sensitivity.
    MotionSmoother stroke;
    stroke.setStrength(0.6F);
    float travel = 0.0F;
    float outX    = 0.0F;
    float outY    = 0.0F;
    for (int i = 0; i < 200; ++i) {
        stroke.apply(10.0F, 0.0F, 1000 + static_cast<uint32_t>(i) * 8, outX, outY);
        travel += outX;
    }
    CHECK(std::abs(travel - 2000.0F) < 60.0F);

    // Frame-to-frame jitter with a still finger is damped.
    MotionSmoother jitter;
    jitter.setStrength(0.6F);
    float maxStep = 0.0F;
    float drift   = 0.0F;
    for (int i = 0; i < 100; ++i) {
        const float in = (i % 2) ? 1.0F : -1.0F;
        jitter.apply(in, 0.0F, 5000 + static_cast<uint32_t>(i) * 8, outX, outY);
        maxStep = std::max(maxStep, std::abs(outX));
        drift += outX;
    }
    CHECK(maxStep < 0.6F);
    CHECK(std::abs(drift) < 2.0F);

    // A single-frame spike (the pad losing and re-acquiring its contact) must
    // not drag the cursor with it.
    MotionSmoother spike;
    spike.setStrength(0.6F);
    float maxSpike = 0.0F;
    for (int i = 0; i < 40; ++i) {
        const float in = (i == 20) ? 10.0F : 1.0F;
        spike.apply(in, 0.0F, 9000 + static_cast<uint32_t>(i) * 8, outX, outY);
        if (i > 5)
            maxSpike = std::max(maxSpike, std::abs(outX));
    }
    CHECK(maxSpike < 2.5F);

    // A pause longer than the gap starts a fresh touch with no leftover motion.
    MotionSmoother restart;
    restart.setStrength(0.6F);
    restart.apply(20.0F, 0.0F, 7000, outX, outY);
    restart.apply(0.0F, 0.0F, 7400, outX, outY);
    CHECK(outX == 0.0F);
    CHECK(outY == 0.0F);
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
    testResyncPassthrough();
    testButtonpadRestoreBlocksEdge();
    testBindingPrecedenceAndValidation();
    testMotionSmoothing();
    std::puts("hyprpad core checks passed");
    return 0;
}
