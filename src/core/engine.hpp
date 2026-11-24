#pragma once

#include "types.hpp"

#include <optional>
#include <stdexcept>
#include <vector>

namespace hyprpad {

class SlotError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Stateful edge recognizer. Feed complete protocol-B frames; call advanceTime
// when nextDeadline() is reached so a pending single tap cannot wait forever.
class Engine {
  public:
    Engine(Capabilities caps, EdgeWidths edges);
    Engine(Capabilities caps, EdgeWidths edges, std::vector<SliderSpec> sliders, EngineOptions options);

    void                 setButtonpad(bool buttonpad);
    std::vector<Event>   updatePhysicalButton(uint16_t code, bool pressed);
    void                 restorePressedPhysicalButtons(const std::vector<uint16_t>& codes);
    std::vector<Gesture> interruptTapSequence();

    FrameOutput processFrame(const std::vector<Event>& frame);
    FrameOutput processFrameAt(const std::vector<Event>& frame, Micros timestamp);

    std::optional<Micros> nextDeadline() const;
    FrameOutput           advanceTime(Micros timestamp);

    bool hasClaimedContacts() const;
    bool hasActiveContacts() const;

    FrameOutput restorePassthroughContacts(const std::vector<ResyncContact>& contacts);

  private:
    enum class Ownership : uint8_t {
        Unknown,
        Claimed,
        Passthrough,
    };

    enum class ContactPhase : uint8_t {
        TapCandidate,
        Motion,
        ConsumedBySlider,
    };

    struct SlotState {
        bool                   active              = false;
        std::optional<int32_t> trackingId;
        Ownership              ownership           = Ownership::Unknown;
        ContactPhase           contactPhase        = ContactPhase::TapCandidate;
        std::optional<int32_t> startX;
        std::optional<int32_t> startY;
        std::optional<int32_t> currentX;
        std::optional<int32_t> currentY;
        std::optional<int32_t> retainedX;
        std::optional<int32_t> retainedY;
        bool                   beganInFrame        = false;
        std::optional<Micros>  startedAt;
        bool                   tapSequenceEligible = true;
        bool                   tapSequenceMatch    = false;
        bool                   tapGroupMember      = false;
        std::optional<float>   sliderAnchor;
        uint8_t                gestureFingers      = 0;
        std::vector<Event>     heldEvents;

        void finishContact();
        void observePosition(Capabilities caps, float swipeMinDistance);
    };

    struct PendingTap {
        Gesture gesture;
        Micros  releasedAt = 0;
        int32_t x          = 0;
        int32_t y          = 0;
        bool    emitSingle = false;
    };

    struct ClassifiedContact {
        Gesture                gesture;
        bool                   sequenceMatch    = false;
        bool                   sequenceEligible = false;
        bool                   groupMember      = false;
        std::optional<int32_t> x;
        std::optional<int32_t> y;
    };

    FrameOutput processFrameWithTime(const std::vector<Event>& frame, std::optional<Micros> timestamp);
    void        finalizeContactsStartedInFrame(FrameOutput& output);
    void        releaseCurrentSlot(const Event& event, std::optional<Micros> timestamp, FrameOutput& output);
    void        routeEventForCurrentSlot(const Event& event, FrameOutput& output);
    void        pushPassthroughEventForCurrentSlot(const Event& event, FrameOutput& output);
    void        pushPassthroughEventForSlot(int32_t slot, const Event& event, FrameOutput& output);
    void        decideOwnershipIfReady(FrameOutput& output);
    void        recordPadFingerCount();
    void        classifyPadTap(int32_t slot, std::optional<Micros> timestamp, FrameOutput& output);
    bool        pendingPadTap() const;
    std::vector<Event> promoteClaimedContacts();
    void        emitSliderStepsIfReady(FrameOutput& output);

    void                      handleClassifiedGesture(const ClassifiedContact& contact, std::optional<Micros> releasedAt,
                                                      FrameOutput& output);
    bool                      currentContactMatchesPendingTap(int32_t slot, Zone zone) const;
    std::optional<Micros>     matchingContactDeadline() const;
    void                      expirePendingTap(Micros timestamp, FrameOutput& output);
    void                      flushPendingTap(FrameOutput& output);
    std::optional<SliderSpec> sliderForZone(Zone zone) const;
    std::optional<Zone>       zoneForCurrentSlot() const;
    void                      resetForResync();

    void ensureSlot(int32_t slot) const;

    size_t           slotIndex(int32_t slot) const;
    const SlotState& slot(int32_t slot) const;
    SlotState&       slotMut(int32_t slot);

    Capabilities              m_caps;
    EdgeWidths                m_edges;
    EngineOptions             m_options;
    std::vector<SliderSpec>   m_sliders;
    std::vector<uint16_t>     m_pressedPhysicalButtons;
    std::vector<SlotState>    m_slots;
    std::optional<PendingTap> m_pendingTap;
    int32_t                   m_currentSlot = 0;
    bool                      m_buttonpad   = false;
};

} // namespace hyprpad
