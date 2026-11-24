#include "engine.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace hyprpad {

namespace {

bool tapDurationValid(std::optional<Micros> startedAt, std::optional<Micros> releasedAt, Micros minDuration, Micros maxDuration) {
    if (!startedAt || !releasedAt)
        return true;
    const Micros duration = *releasedAt - *startedAt;
    return duration >= minDuration && duration < maxDuration;
}

std::optional<Direction> directionForDisplacement(float dx, float dy, float swipeMinDistance) {
    if (std::abs(dx) < swipeMinDistance && std::abs(dy) < swipeMinDistance)
        return std::nullopt;

    if (std::abs(dx) >= std::abs(dy))
        return dx >= 0.0F ? Direction::Right : Direction::Left;
    return dy >= 0.0F ? Direction::Down : Direction::Up;
}

SliderDirection positiveSliderDirection(SliderAxis axis) {
    return axis == SliderAxis::Horizontal ? SliderDirection::Right : SliderDirection::Down;
}

SliderDirection negativeSliderDirection(SliderAxis axis) {
    return axis == SliderAxis::Horizontal ? SliderDirection::Left : SliderDirection::Up;
}

std::optional<float> sliderPosition(Capabilities caps, SliderAxis axis, const auto& slotState) {
    if (axis == SliderAxis::Horizontal)
        return slotState.currentX ? std::optional<float>(caps.x.normalize(*slotState.currentX)) : std::nullopt;
    return slotState.currentY ? std::optional<float>(caps.y.normalize(*slotState.currentY)) : std::nullopt;
}

void validateEngineOptions(const EngineOptions& options) {
    if (!(options.tapMinDuration < options.tapMaxDuration))
        throw SlotError("tap minimum duration must be below maximum duration");
    if (options.doubleTapTimeout <= 0)
        throw SlotError("double-tap timeout must be positive");
    if (!std::isfinite(options.doubleTapMaxDistance) || options.doubleTapMaxDistance <= 0.0F || options.doubleTapMaxDistance > 1.0F)
        throw SlotError("double-tap distance must be > 0 and <= 1");
}

void validateSliderSpecs(const std::vector<SliderSpec>& sliders) {
    for (const auto& slider : sliders) {
        if (!std::isfinite(slider.step) || slider.step <= 0.0F || slider.step > 1.0F)
            throw SlotError("invalid slider step");
    }
}

} // namespace

const char* zoneName(Zone zone) {
    switch (zone) {
        case Zone::Left: return "left";
        case Zone::Right: return "right";
        case Zone::Top: return "top";
        case Zone::Bottom: return "bottom";
        case Zone::Pad: return "pad";
    }
    return "?";
}

const char* directionName(Direction direction) {
    switch (direction) {
        case Direction::Up: return "up";
        case Direction::Down: return "down";
        case Direction::Left: return "left";
        case Direction::Right: return "right";
        case Direction::Tap: return "tap";
        case Direction::DoubleTap: return "double-tap";
    }
    return "?";
}

const char* sliderDirectionName(SliderDirection direction) {
    switch (direction) {
        case SliderDirection::Up: return "up";
        case SliderDirection::Down: return "down";
        case SliderDirection::Left: return "left";
        case SliderDirection::Right: return "right";
    }
    return "?";
}

float AxisRange::normalize(int32_t value) const {
    const float span = static_cast<float>(std::max(max - min, 1));
    return static_cast<float>(value - min) / span;
}

float AxisRange::normalizeDelta(int32_t start, int32_t end) const {
    const float span = static_cast<float>(std::max(max - min, 1));
    return static_cast<float>(end - start) / span;
}

EdgeWidths EdgeWidths::all(float width) {
    return EdgeWidths{width, width, width, width};
}

Engine::Engine(Capabilities caps, EdgeWidths edges) : Engine(caps, edges, {}, EngineOptions{}) {
}

Engine::Engine(Capabilities caps, EdgeWidths edges, std::vector<SliderSpec> sliders, EngineOptions options)
    : m_caps(caps), m_edges(edges), m_options(options), m_sliders(std::move(sliders)) {
    if (m_caps.slotMin > m_caps.slotMax)
        throw SlotError("invalid slot range");
    validateSliderSpecs(m_sliders);
    validateEngineOptions(m_options);
    m_currentSlot = m_caps.slotMin;
    m_slots.resize(static_cast<size_t>(m_caps.slotMax - m_caps.slotMin + 1));
}

void Engine::setButtonpad(bool buttonpad) {
    m_buttonpad = buttonpad;
    if (!buttonpad)
        m_pressedPhysicalButtons.clear();
}

std::vector<Event> Engine::updatePhysicalButton(uint16_t code, bool pressed) {
    if (!m_buttonpad)
        return {};

    if (!pressed) {
        m_pressedPhysicalButtons.erase(std::remove(m_pressedPhysicalButtons.begin(), m_pressedPhysicalButtons.end(), code),
                                       m_pressedPhysicalButtons.end());
        return {};
    }

    if (std::find(m_pressedPhysicalButtons.begin(), m_pressedPhysicalButtons.end(), code) == m_pressedPhysicalButtons.end())
        m_pressedPhysicalButtons.push_back(code);
    return promoteClaimedContacts();
}

void Engine::restorePressedPhysicalButtons(const std::vector<uint16_t>& codes) {
    m_pressedPhysicalButtons.clear();
    if (m_buttonpad)
        m_pressedPhysicalButtons = codes;
}

std::vector<Gesture> Engine::interruptTapSequence() {
    for (auto& state : m_slots) {
        if (state.active) {
            state.tapSequenceEligible = false;
            state.tapSequenceMatch    = false;
        }
    }
    FrameOutput output;
    flushPendingTap(output);
    return output.gestures;
}

FrameOutput Engine::processFrame(const std::vector<Event>& frame) {
    return processFrameWithTime(frame, std::nullopt);
}

FrameOutput Engine::processFrameAt(const std::vector<Event>& frame, Micros timestamp) {
    return processFrameWithTime(frame, timestamp);
}

FrameOutput Engine::advanceTime(Micros timestamp) {
    FrameOutput output;
    expirePendingTap(timestamp, output);
    return output;
}

bool Engine::hasClaimedContacts() const {
    return std::any_of(m_slots.begin(), m_slots.end(), [](const SlotState& state) {
        return state.active && state.ownership == Ownership::Claimed;
    });
}

bool Engine::hasActiveContacts() const {
    return std::any_of(m_slots.begin(), m_slots.end(), [](const SlotState& state) { return state.active; });
}

std::optional<Micros> Engine::nextDeadline() const {
    if (!m_pendingTap)
        return std::nullopt;
    if (const auto secondTapDeadline = matchingContactDeadline())
        return secondTapDeadline;
    return m_pendingTap->releasedAt + m_options.doubleTapTimeout;
}

FrameOutput Engine::restorePassthroughContacts(const std::vector<ResyncContact>& contacts) {
    resetForResync();
    FrameOutput output;
    for (const auto& contact : contacts) {
        ensureSlot(contact.slot);
        m_currentSlot = contact.slot;
        auto& state   = slotMut(contact.slot);
        state.active       = true;
        state.trackingId   = contact.trackingId;
        state.ownership    = Ownership::Passthrough;
        state.startX       = contact.x;
        state.startY       = contact.y;
        state.currentX     = contact.x;
        state.currentY     = contact.y;
        state.retainedX    = contact.x;
        state.retainedY    = contact.y;
        output.passthrough.push_back({EventType::Slot, contact.slot});
        output.passthrough.push_back({EventType::TrackingId, contact.trackingId});
        output.passthrough.push_back({EventType::X, contact.x});
        output.passthrough.push_back({EventType::Y, contact.y});
    }
    return output;
}

void Engine::SlotState::finishContact() {
    const auto keepX = retainedX;
    const auto keepY = retainedY;
    *this            = SlotState{};
    retainedX        = keepX;
    retainedY        = keepY;
}

void Engine::SlotState::observePosition(Capabilities caps, float swipeMinDistance) {
    if (contactPhase != ContactPhase::TapCandidate)
        return;

    const bool movedX = startX && currentX && std::abs(caps.x.normalizeDelta(*startX, *currentX)) >= swipeMinDistance;
    const bool movedY = startY && currentY && std::abs(caps.y.normalizeDelta(*startY, *currentY)) >= swipeMinDistance;
    if (movedX || movedY)
        contactPhase = ContactPhase::Motion;
}

FrameOutput Engine::processFrameWithTime(const std::vector<Event>& frame, std::optional<Micros> timestamp) {
    FrameOutput output;
    if (timestamp)
        expirePendingTap(*timestamp, output);

    for (const auto& event : frame) {
        switch (event.type) {
            case EventType::SynDropped: resetForResync(); break;

            case EventType::Slot:
                ensureSlot(event.value);
                m_currentSlot = event.value;
                routeEventForCurrentSlot(event, output);
                break;

            case EventType::TrackingId:
                if (event.value >= 0) {
                    const int32_t slotNumber = m_currentSlot;
                    if (slot(slotNumber).active)
                        throw SlotError("slot already active");

                    const bool anotherContactActive = std::any_of(m_slots.begin(), m_slots.end(), [](const SlotState& state) {
                        return state.active;
                    });
                    auto& state                 = slotMut(slotNumber);
                    state.active                = true;
                    state.trackingId            = event.value;
                    state.ownership             = Ownership::Unknown;
                    state.contactPhase          = ContactPhase::TapCandidate;
                    state.beganInFrame          = true;
                    state.startedAt             = timestamp;
                    state.tapSequenceEligible   = !anotherContactActive;
                    state.tapSequenceMatch      = false;
                    state.tapGroupMember        = anotherContactActive;
                    state.heldEvents.push_back(event);
                    recordPadFingerCount();
                } else {
                    releaseCurrentSlot(event, timestamp, output);
                }
                break;

            case EventType::X:
            case EventType::Y: {
                const bool isX = event.type == EventType::X;
                auto&      state = slotMut(m_currentSlot);
                (isX ? state.retainedX : state.retainedY) = event.value;
                (isX ? state.currentX : state.currentY)   = event.value;
                if (state.active && !(isX ? state.startX : state.startY))
                    (isX ? state.startX : state.startY) = event.value;
                state.observePosition(m_caps, m_options.swipeMinDistance);
                const bool sequenceInvalidated =
                    state.tapSequenceMatch && state.contactPhase != ContactPhase::TapCandidate;
                if (sequenceInvalidated)
                    flushPendingTap(output);
                routeEventForCurrentSlot(event, output);
                break;
            }
        }

        if (!slot(m_currentSlot).beganInFrame) {
            decideOwnershipIfReady(output);
            emitSliderStepsIfReady(output);
        }
    }

    finalizeContactsStartedInFrame(output);
    return output;
}

void Engine::finalizeContactsStartedInFrame(FrameOutput& output) {
    const int32_t previousSlot = m_currentSlot;
    for (size_t index = 0; index < m_slots.size(); ++index) {
        const int32_t slotNumber = m_caps.slotMin + static_cast<int32_t>(index);
        bool          finalize   = false;
        {
            auto& state = m_slots[index];
            if (state.active && state.beganInFrame) {
                state.currentX = state.currentX ? state.currentX : state.retainedX;
                state.currentY = state.currentY ? state.currentY : state.retainedY;
                state.startX   = state.startX ? state.startX : state.currentX;
                state.startY   = state.startY ? state.startY : state.currentY;
                state.beganInFrame = false;
                finalize           = true;
            }
        }
        if (finalize) {
            m_currentSlot = slotNumber;
            decideOwnershipIfReady(output);
            emitSliderStepsIfReady(output);
        }
    }
    m_currentSlot = previousSlot;
}

void Engine::releaseCurrentSlot(const Event& event, std::optional<Micros> timestamp, FrameOutput& output) {
    const int32_t slotNumber = m_currentSlot;
    switch (slot(slotNumber).ownership) {
        case Ownership::Claimed: {
            const auto maybeZone         = zoneForCurrentSlot();
            const bool releasesSliderZone = maybeZone && sliderForZone(*maybeZone).has_value();
            std::optional<ClassifiedContact> classified;
            if (auto zone = maybeZone) {
                const auto& state = slot(slotNumber);
                const auto  startX = state.startX;
                const auto  startY = state.startY;
                if (startX && startY) {
                    const int32_t currentX = state.currentX ? *state.currentX : *startX;
                    const int32_t currentY = state.currentY ? *state.currentY : *startY;
                    const float   dx       = m_caps.x.normalizeDelta(*startX, currentX);
                    const float   dy       = m_caps.y.normalizeDelta(*startY, currentY);
                    const auto    finalDirection = directionForDisplacement(dx, dy, m_options.swipeMinDistance);

                    std::optional<Direction> direction;
                    switch (state.contactPhase) {
                        case ContactPhase::TapCandidate:
                            direction = finalDirection ? finalDirection : std::nullopt;
                            if (!direction) {
                                if (!tapDurationValid(state.startedAt, timestamp, m_options.tapMinDuration, m_options.tapMaxDuration))
                                    break;
                                direction = Direction::Tap;
                            }
                            break;
                        case ContactPhase::Motion: direction = finalDirection; break;
                        case ContactPhase::ConsumedBySlider: break;
                    }

                    if (direction && state.trackingId) {
                        classified = ClassifiedContact{
                            .gesture = Gesture{*zone, *direction, static_cast<uint8_t>(std::max<int>(state.gestureFingers, 1)),
                                               slotNumber, *state.trackingId},
                            .sequenceMatch    = state.tapSequenceMatch,
                            .sequenceEligible = state.tapSequenceEligible,
                            .groupMember      = state.tapGroupMember,
                            .x                = state.currentX ? state.currentX : state.startX,
                            .y                = state.currentY ? state.currentY : state.startY,
                        };
                    }
                }
            }

            if (slot(slotNumber).active)
                slotMut(slotNumber).finishContact();

            if (classified) {
                if (classified->gesture.direction == Direction::Tap || classified->gesture.direction == Direction::DoubleTap)
                    output.tapLike = true;
                const bool allowedForSlider =
                    !releasesSliderZone ||
                    classified->gesture.direction == Direction::Tap || classified->gesture.direction == Direction::DoubleTap;
                if (allowedForSlider)
                    handleClassifiedGesture(*classified, timestamp, output);
                else
                    flushPendingTap(output);
            } else if (m_pendingTap) {
                flushPendingTap(output);
            }
            break;
        }

        case Ownership::Passthrough:
            classifyPadTap(slotNumber, timestamp, output);
            pushPassthroughEventForCurrentSlot(event, output);
            if (slot(slotNumber).active)
                slotMut(slotNumber).finishContact();
            break;

        case Ownership::Unknown: {
            auto& state = slotMut(slotNumber);
            state.heldEvents.push_back(event);
            if (state.active)
                state.finishContact();
            break;
        }
    }
}

void Engine::routeEventForCurrentSlot(const Event& event, FrameOutput& output) {
    auto& state = slotMut(m_currentSlot);
    switch (state.ownership) {
        case Ownership::Passthrough: pushPassthroughEventForCurrentSlot(event, output); break;
        case Ownership::Unknown: state.heldEvents.push_back(event); break;
        case Ownership::Claimed: break;
    }
}

void Engine::pushPassthroughEventForCurrentSlot(const Event& event, FrameOutput& output) {
    pushPassthroughEventForSlot(m_currentSlot, event, output);
}

void Engine::pushPassthroughEventForSlot(int32_t slotNumber, const Event& event, FrameOutput& output) {
    if (event.type == EventType::Slot || event.type == EventType::SynDropped) {
        output.passthrough.push_back(event);
        return;
    }

    // Filtering edge contacts also filters slot switches, so the output slot
    // can differ from the physical one. Emit the slot before the event.
    bool lastSlotIsCurrent = false;
    for (auto it = output.passthrough.rbegin(); it != output.passthrough.rend(); ++it) {
        if (it->type == EventType::Slot) {
            lastSlotIsCurrent = it->value == slotNumber;
            break;
        }
    }
    if (!lastSlotIsCurrent)
        output.passthrough.push_back({EventType::Slot, slotNumber});
    output.passthrough.push_back(event);
}

void Engine::decideOwnershipIfReady(FrameOutput& output) {
    const int32_t slotNumber = m_currentSlot;
    const auto    zone =
        m_buttonpad && !m_pressedPhysicalButtons.empty() ? std::optional<Zone>{} : zoneForCurrentSlot();

    if (!zone) {
        std::vector<Event> held;
        bool               becamePassthrough = false;
        {
            auto& state = slotMut(slotNumber);
            if (state.active && state.ownership == Ownership::Unknown && state.startX && state.startY) {
                state.ownership = Ownership::Passthrough;
                held            = std::move(state.heldEvents);
                state.heldEvents.clear();
                becamePassthrough = true;
            }
        }
        if (becamePassthrough && !pendingPadTap())
            flushPendingTap(output);
        for (const auto& event : held)
            pushPassthroughEventForSlot(slotNumber, event, output);
        return;
    }

    std::optional<float> sliderAnchor;
    {
        const auto& state = slot(slotNumber);
        if (state.active && state.ownership == Ownership::Unknown && state.startX && state.startY) {
            if (const auto spec = sliderForZone(*zone))
                sliderAnchor = sliderPosition(m_caps, spec->axis, state);
        }
    }

    const bool sequenceMatch = currentContactMatchesPendingTap(slotNumber, *zone);
    const bool groupLeader   = slot(slotNumber).tapSequenceEligible;
    if (m_pendingTap && !sequenceMatch && groupLeader)
        flushPendingTap(output);

    auto& state = slotMut(slotNumber);
    if (state.active && state.ownership == Ownership::Unknown && state.startX && state.startY) {
        state.ownership        = Ownership::Claimed;
        state.sliderAnchor     = sliderAnchor;
        state.tapSequenceMatch = sequenceMatch;
        state.heldEvents.clear();
    }
}

void Engine::recordPadFingerCount() {
    const uint8_t active = static_cast<uint8_t>(std::count_if(m_slots.begin(), m_slots.end(), [](const SlotState& state) {
        return state.active;
    }));
    if (active < 2)
        return;
    for (auto& state : m_slots) {
        if (state.active)
            state.gestureFingers = std::max(state.gestureFingers, active);
    }
}

void Engine::classifyPadTap(int32_t slotNumber, std::optional<Micros> timestamp, FrameOutput& output) {
    std::optional<Gesture> candidate;
    {
        const auto& state   = slot(slotNumber);
        const uint8_t fingers = std::max<uint8_t>(state.gestureFingers, 1);
        const bool watched  = fingers <= 4 && (m_options.padTapFingers & (1U << fingers)) != 0;
        const bool isTap    = state.contactPhase == ContactPhase::TapCandidate && state.startedAt &&
                           tapDurationValid(state.startedAt, timestamp, m_options.tapMinDuration, m_options.tapMaxDuration);
        if (isTap && watched && state.trackingId)
            candidate = Gesture{Zone::Pad, Direction::Tap, fingers, slotNumber, *state.trackingId};
    }

    if (!candidate) {
        if (pendingPadTap() && !slot(slotNumber).tapGroupMember)
            flushPendingTap(output);
        return;
    }

    const bool sequenceMatch = currentContactMatchesPendingTap(slotNumber, Zone::Pad);
    const auto& state        = slot(slotNumber);
    handleClassifiedGesture(ClassifiedContact{.gesture          = *candidate,
                                              .sequenceMatch    = sequenceMatch,
                                              .sequenceEligible = state.tapSequenceEligible,
                                              .groupMember      = state.tapGroupMember,
                                              .x                = state.currentX ? state.currentX : state.startX,
                                              .y                = state.currentY ? state.currentY : state.startY},
                            timestamp, output);
}

bool Engine::pendingPadTap() const {
    return m_pendingTap && m_pendingTap->gesture.zone == Zone::Pad;
}

std::vector<Event> Engine::promoteClaimedContacts() {
    std::vector<Event> events;
    for (size_t index = 0; index < m_slots.size(); ++index) {
        const int32_t slotNumber = m_caps.slotMin + static_cast<int32_t>(index);
        auto&         state      = m_slots[index];
        if (!state.active || state.ownership != Ownership::Claimed)
            continue;
        if (!state.trackingId || !state.currentX || !state.currentY)
            continue;
        state.ownership    = Ownership::Passthrough;
        state.sliderAnchor = std::nullopt;
        events.push_back({EventType::Slot, slotNumber});
        events.push_back({EventType::TrackingId, *state.trackingId});
        events.push_back({EventType::X, *state.currentX});
        events.push_back({EventType::Y, *state.currentY});
    }
    return events;
}

void Engine::emitSliderStepsIfReady(FrameOutput& output) {
    const int32_t slotNumber = m_currentSlot;
    float         position   = 0.0F;
    int32_t       trackingId = 0;
    Zone          zone;
    SliderSpec    spec;
    {
        const auto& state = slot(slotNumber);
        if (state.ownership != Ownership::Claimed)
            return;
        const auto maybeZone = zoneForCurrentSlot();
        if (!maybeZone)
            return;
        const auto maybeSpec = sliderForZone(*maybeZone);
        if (!maybeSpec)
            return;
        const auto maybePosition = sliderPosition(m_caps, maybeSpec->axis, state);
        if (!maybePosition || !state.trackingId)
            return;
        zone       = *maybeZone;
        spec       = *maybeSpec;
        position   = *maybePosition;
        trackingId = *state.trackingId;
    }

    auto& state = slotMut(slotNumber);
    if (!state.sliderAnchor) {
        state.sliderAnchor = position;
        return;
    }

    float anchor = *state.sliderAnchor;
    if (std::abs(position - anchor) >= spec.step)
        flushPendingTap(output);

    auto pushStep = [&](SliderDirection direction) {
        slotMut(slotNumber).contactPhase = ContactPhase::ConsumedBySlider;
        output.sliderSteps.push_back({zone, direction, slotNumber, trackingId});
    };

    while (position - anchor >= spec.step) {
        pushStep(positiveSliderDirection(spec.axis));
        anchor += spec.step;
    }
    while (anchor - position >= spec.step) {
        pushStep(negativeSliderDirection(spec.axis));
        anchor -= spec.step;
    }
    slotMut(slotNumber).sliderAnchor = anchor;
}

std::optional<SliderSpec> Engine::sliderForZone(Zone zone) const {
    for (const auto& slider : m_sliders) {
        if (slider.zone == zone)
            return slider;
    }
    return std::nullopt;
}

std::optional<Zone> Engine::zoneForCurrentSlot() const {
    const auto& state = slot(m_currentSlot);
    if (!state.active || !state.startX || !state.startY)
        return std::nullopt;

    const float x = m_caps.x.normalize(*state.startX);
    const float y = m_caps.y.normalize(*state.startY);

    if (x < m_edges.left)
        return Zone::Left;
    if (x > 1.0F - m_edges.right)
        return Zone::Right;
    if (y < m_edges.top)
        return Zone::Top;
    if (y > 1.0F - m_edges.bottom)
        return Zone::Bottom;
    return std::nullopt;
}

void Engine::ensureSlot(int32_t slotNumber) const {
    if (slotNumber < m_caps.slotMin || slotNumber > m_caps.slotMax)
        throw SlotError("slot out of range");
}

void Engine::resetForResync() {
    for (auto& state : m_slots)
        state = SlotState{};
    m_pressedPhysicalButtons.clear();
    m_currentSlot = m_caps.slotMin;
    m_pendingTap.reset();
}

void Engine::handleClassifiedGesture(const ClassifiedContact& contact, std::optional<Micros> releasedAt, FrameOutput& output) {
    if (contact.gesture.direction != Direction::Tap) {
        flushPendingTap(output);
        output.gestures.push_back(contact.gesture);
        return;
    }

    if (contact.sequenceMatch) {
        if (m_pendingTap) {
            const auto pending = *m_pendingTap;
            m_pendingTap.reset();
            if (pending.gesture.fingers == contact.gesture.fingers) {
                auto doubleTap         = contact.gesture;
                doubleTap.direction    = Direction::DoubleTap;
                output.gestures.push_back(doubleTap);
                return;
            }
            if (pending.emitSingle)
                output.gestures.push_back(pending.gesture);
        }
    }

    if (contact.groupMember)
        return;

    if (!contact.sequenceEligible) {
        flushPendingTap(output);
        if (m_options.singleTapZones & zoneBit(contact.gesture.zone))
            output.gestures.push_back(contact.gesture);
        return;
    }

    const bool canStartSequence = (m_options.doubleTapZones & zoneBit(contact.gesture.zone)) && releasedAt && contact.x && contact.y;
    if (!canStartSequence) {
        output.gestures.push_back(contact.gesture);
        return;
    }

    flushPendingTap(output);
    m_pendingTap = PendingTap{.gesture = contact.gesture,
                              .releasedAt = *releasedAt,
                              .x          = *contact.x,
                              .y          = *contact.y,
                              .emitSingle = (m_options.singleTapZones & zoneBit(contact.gesture.zone)) != 0};
}

bool Engine::currentContactMatchesPendingTap(int32_t slotNumber, Zone zone) const {
    if (!m_pendingTap)
        return false;
    const auto& state = slot(slotNumber);
    if (!state.tapSequenceEligible || m_pendingTap->gesture.zone != zone)
        return false;
    if (!state.startedAt || !state.startX || !state.startY)
        return false;
    if (*state.startedAt < m_pendingTap->releasedAt)
        return false;
    if (*state.startedAt - m_pendingTap->releasedAt >= m_options.doubleTapTimeout)
        return false;

    if (m_pendingTap->gesture.fingers > 1)
        return true;

    const float dx = m_caps.x.normalizeDelta(m_pendingTap->x, *state.startX);
    const float dy = m_caps.y.normalizeDelta(m_pendingTap->y, *state.startY);
    const float maxDistance = m_options.doubleTapMaxDistance;
    return dx * dx + dy * dy <= maxDistance * maxDistance;
}

std::optional<Micros> Engine::matchingContactDeadline() const {
    if (!m_pendingTap)
        return std::nullopt;

    std::optional<Micros> deadline;
    for (const auto& state : m_slots) {
        if (!state.active || !state.tapSequenceEligible || !state.startedAt)
            continue;
        if (*state.startedAt < m_pendingTap->releasedAt)
            continue;
        if (*state.startedAt - m_pendingTap->releasedAt >= m_options.doubleTapTimeout)
            continue;
        const Micros candidate = *state.startedAt + m_options.tapMaxDuration;
        if (!deadline || candidate > *deadline)
            deadline = candidate;
    }
    return deadline;
}

void Engine::expirePendingTap(Micros timestamp, FrameOutput& output) {
    if (!m_pendingTap)
        return;

    const auto secondTapDeadline = matchingContactDeadline();
    const Micros deadline        = secondTapDeadline ? *secondTapDeadline : m_pendingTap->releasedAt + m_options.doubleTapTimeout;
    if (timestamp >= deadline)
        flushPendingTap(output);
}

void Engine::flushPendingTap(FrameOutput& output) {
    for (auto& state : m_slots)
        state.tapSequenceMatch = false;
    if (m_pendingTap) {
        if (m_pendingTap->emitSingle)
            output.gestures.push_back(m_pendingTap->gesture);
        m_pendingTap.reset();
    }
}

size_t Engine::slotIndex(int32_t slotNumber) const {
    ensureSlot(slotNumber);
    return static_cast<size_t>(slotNumber - m_caps.slotMin);
}

const Engine::SlotState& Engine::slot(int32_t slotNumber) const {
    return m_slots[slotIndex(slotNumber)];
}

Engine::SlotState& Engine::slotMut(int32_t slotNumber) {
    return m_slots[slotIndex(slotNumber)];
}

} // namespace hyprpad
