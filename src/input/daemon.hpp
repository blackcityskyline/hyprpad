#pragma once

#include "../actions/actions.hpp"
#include "../config/config.hpp"
#include "../core/engine.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hyprpad {

struct DeviceCandidate {
    std::string path;
    std::string name;
    bool        readable   = false;
    bool        touchpad   = false;
    bool        buttonpad  = false;
    bool        virtualDev = false;
    std::string error;
};

std::vector<DeviceCandidate> discoverDevices();

class Daemon {
  public:
    Daemon() = default;
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    void start();
    void stop();

    // Parsed on the Hyprland thread. The input thread wakes immediately,
    // discards any gesture in flight, and rebuilds the recognizer.
    void applyConfig(std::shared_ptr<const Config> config);

    bool motionShouldBeSwallowed() const;
    // True while a contact the recognizer claims (edge zone) is still down.
    // Hyprland's own touchpad gestures are cancelled on this, so the same
    // fingers cannot drive both hyprpad and the compositor.
    bool contactsClaimed() const;
    bool consumeClickSuppression();
    void noteKeyPressed();

    std::string status() const;

  private:
    struct Device {
        int          fd        = -1;
        std::string  path;
        std::string  name;
        Capabilities caps;
        bool         buttonpad = false;
    };

    void inputLoop();
    void handleOutput(const FrameOutput& output, const Config& config);
    bool openConfiguredDevice(const Config& config);
    void closeDevice();
    void setStatus(std::string state, std::string error = {});
    std::shared_ptr<const Config> configSnapshot() const;
    bool typingBlocked(Micros now) const;
    void wake();
    void waitForWake(int timeoutMs);
    void recordLastGesture(const Gesture& gesture);
    void setClaimed(bool claimed, bool swallowPointer);

    mutable std::mutex            m_configMutex;
    std::shared_ptr<const Config> m_config;
    // Wakes the input thread out of poll() when a config arrives or the plugin
    // unloads, so an idle pad costs zero wakeups instead of a 200 ms tick.
    int m_wakeFd = -1;

    std::atomic<bool>      m_running{false};
    std::thread            m_thread;
    ActionDispatcher       m_actions;

    mutable std::mutex     m_statusMutex;
    std::string            m_state = "no config";
    std::string            m_statusDevice;
    std::string            m_statusName;
    std::string            m_lastGesture;
    std::string            m_error;

    std::atomic<bool>      m_swallowMotion{false};
    std::atomic<bool>      m_claimedContacts{false};
    std::atomic<Micros>    m_lastTouchFrame{0};
    std::atomic<Micros>    m_clickUntil{0};
    std::atomic<Micros>    m_typingUntil{0};
    std::atomic<uint64_t>  m_gestureCount{0};
    std::atomic<uint64_t>  m_sliderStepCount{0};

    Device                 m_device;
    std::optional<Micros>  m_timeOffset;
};

Micros steadyNowUs();

} // namespace hyprpad
