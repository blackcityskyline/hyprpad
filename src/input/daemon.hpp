#pragma once

#include "../actions/actions.hpp"
#include "../config/config.hpp"
#include "../core/engine.hpp"

#include <atomic>
#include <condition_variable>
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

    // The config is parsed on the Hyprland thread; the input thread picks the
    // newest generation up at an idle frame boundary.
    void applyConfig(std::shared_ptr<const Config> config);

    bool motionShouldBeSwallowed() const;
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

    mutable std::mutex       m_configMutex;
    std::condition_variable  m_configCv;
    std::shared_ptr<const Config> m_config;

    std::atomic<bool>      m_running{false};
    std::thread            m_thread;
    ActionDispatcher       m_actions;

    mutable std::mutex     m_statusMutex;
    std::string            m_state = "no config";
    std::string            m_statusDevice;
    std::string            m_statusName;
    std::string            m_error;

    std::atomic<bool>      m_swallowMotion{false};
    std::atomic<Micros>    m_clickUntil{0};
    std::atomic<Micros>    m_typingUntil{0};
    std::atomic<uint64_t>  m_gestureCount{0};
    std::atomic<uint64_t>  m_sliderStepCount{0};

    Device                 m_device;
    std::optional<Micros>  m_timeOffset;
};

Micros steadyNowUs();

} // namespace hyprpad
