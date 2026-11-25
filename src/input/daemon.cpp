#include "daemon.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace hyprpad {

Micros steadyNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace {

bool bitSet(const uint8_t* bits, int bit) {
    return (bits[bit / 8] & (1U << (bit % 8))) != 0;
}

struct DeviceQuery {
    bool         typeB      = false;
    bool         pointer    = false;
    bool         direct     = false;
    bool         buttonpad  = false;
    bool         virtualDev = false;
    Capabilities caps;
};

// A uinput device can claim any bus id (edgepad's synthetic touchpad says
// BUS_USB), but its sysfs node always lives under /devices/virtual/.
bool sysfsVirtual(const std::string& eventPath) {
    const auto slash = eventPath.find_last_of('/');
    const auto name  = slash == std::string::npos ? eventPath : eventPath.substr(slash + 1);
    const auto link  = "/sys/class/input/" + name;
    char       target[PATH_MAX] = {};
    const auto length = readlink(link.c_str(), target, sizeof(target) - 1);
    if (length <= 0)
        return false;
    return std::string(target, static_cast<size_t>(length)).find("/devices/virtual/") != std::string::npos;
}

std::optional<DeviceQuery> queryDevice(int fd, std::string& error) {
    uint8_t absBits[(ABS_CNT + 7) / 8] = {};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
        error = std::string("EVIOCGBIT(EV_ABS): ") + std::strerror(errno);
        return std::nullopt;
    }

    uint8_t propBits[(INPUT_PROP_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits) < 0)
        std::memset(propBits, 0, sizeof(propBits));

    DeviceQuery query;
    query.typeB     = bitSet(absBits, ABS_MT_SLOT) && bitSet(absBits, ABS_MT_TRACKING_ID) &&
                  bitSet(absBits, ABS_MT_POSITION_X) && bitSet(absBits, ABS_MT_POSITION_Y);
    query.pointer   = bitSet(propBits, INPUT_PROP_POINTER);
    query.direct    = bitSet(propBits, INPUT_PROP_DIRECT);
    query.buttonpad = bitSet(propBits, INPUT_PROP_BUTTONPAD);

    input_id id{};
    if (ioctl(fd, EVIOCGID, &id) >= 0)
        query.virtualDev = id.bustype == BUS_VIRTUAL;

    auto readAbs = [&](int code, AxisRange& range) {
        input_absinfo info{};
        if (ioctl(fd, EVIOCGABS(code), &info) < 0)
            return false;
        range.min = info.minimum;
        range.max = info.maximum;
        return true;
    };

    input_absinfo slotInfo{};
    if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slotInfo) >= 0) {
        query.caps.slotMin = slotInfo.minimum;
        query.caps.slotMax = slotInfo.maximum;
    }

    if (!readAbs(ABS_MT_POSITION_X, query.caps.x) || !readAbs(ABS_MT_POSITION_Y, query.caps.y)) {
        error = "missing ABS_MT_POSITION_X/Y ranges";
        return std::nullopt;
    }

    return query;
}

std::string deviceName(int fd) {
    char buffer[256] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(buffer) - 1), buffer) < 0)
        return "unknown";
    return buffer;
}

bool isPointerButton(uint16_t code) {
    return code >= BTN_LEFT && code <= BTN_TASK;
}

std::string errorText(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

std::optional<std::vector<int32_t>> mtSlotValues(int fd, const Capabilities& caps, uint16_t code) {
    const int32_t count = caps.slotMax - caps.slotMin + 1;
    if (count <= 0 || count > 64)
        return std::nullopt;
    std::vector<int32_t> request(static_cast<size_t>(count) + 1, 0);
    request[0] = code;
    if (ioctl(fd, EVIOCGMTSLOTS(static_cast<int>(request.size() * sizeof(int32_t))), request.data()) < 0)
        return std::nullopt;
    return std::vector<int32_t>(request.begin() + 1, request.end());
}

std::optional<std::vector<uint16_t>> pressedPointerButtons(int fd) {
    uint8_t keyBits[(KEY_CNT + 7) / 8] = {};
    if (ioctl(fd, EVIOCGKEY(sizeof(keyBits)), keyBits) < 0)
        return std::nullopt;
    std::vector<uint16_t> codes;
    for (int code = BTN_LEFT; code <= BTN_TASK; ++code) {
        if (bitSet(keyBits, code))
            codes.push_back(static_cast<uint16_t>(code));
    }
    return codes;
}

// The kernel dropped events, so the engine cannot know which contacts are
// already down. Read the current slots and hand them to the engine as
// passthrough: their history is gone, so they must not become gestures.
bool resyncEngine(int fd, const Capabilities& caps, Engine& engine) {
    const auto trackingIds = mtSlotValues(fd, caps, ABS_MT_TRACKING_ID);
    const auto xValues     = mtSlotValues(fd, caps, ABS_MT_POSITION_X);
    const auto yValues     = mtSlotValues(fd, caps, ABS_MT_POSITION_Y);
    if (!trackingIds || !xValues || !yValues)
        return false;

    const size_t count = std::min({trackingIds->size(), xValues->size(), yValues->size()});
    std::vector<ResyncContact> contacts;
    for (size_t index = 0; index < count; ++index) {
        if ((*trackingIds)[index] < 0)
            continue;
        contacts.push_back(ResyncContact{caps.slotMin + static_cast<int32_t>(index), (*trackingIds)[index],
                                         (*xValues)[index], (*yValues)[index]});
    }

    try {
        engine.restorePassthroughContacts(contacts);
    } catch (const std::exception&) {
        return false;
    }
    if (const auto buttons = pressedPointerButtons(fd))
        engine.restorePressedPhysicalButtons(*buttons);
    return true;
}

} // namespace

std::vector<DeviceCandidate> discoverDevices() {
    std::vector<DeviceCandidate> result;
    DIR*                         directory = opendir("/dev/input");
    if (!directory)
        return result;

    std::vector<int> numbers;
    while (dirent* entry = readdir(directory)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0)
            continue;
        char* end = nullptr;
        const long number = std::strtol(entry->d_name + 5, &end, 10);
        if (!end || *end != '\0' || number < 0)
            continue;
        numbers.push_back(static_cast<int>(number));
    }
    closedir(directory);
    std::sort(numbers.begin(), numbers.end());

    for (int number : numbers) {
        DeviceCandidate candidate;
        candidate.path = "/dev/input/event" + std::to_string(number);
        const int fd   = open(candidate.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            candidate.error = std::strerror(errno);
            result.push_back(std::move(candidate));
            continue;
        }

        candidate.readable = true;
        candidate.name     = deviceName(fd);
        std::string error;
        if (const auto query = queryDevice(fd, error)) {
            candidate.touchpad   = query->typeB && query->pointer && !query->direct;
            candidate.buttonpad  = query->buttonpad;
            candidate.virtualDev = query->virtualDev || sysfsVirtual(candidate.path);
        } else {
            candidate.error = error;
        }
        close(fd);
        result.push_back(std::move(candidate));
    }

    return result;
}

Daemon::~Daemon() {
    stop();
}

void Daemon::start() {
    if (m_running.exchange(true))
        return;
    m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    m_actions.start();
    m_thread = std::thread([this] {
        pthread_setname_np(pthread_self(), "hyprpad-input");
        inputLoop();
    });
}

void Daemon::stop() {
    if (!m_running.exchange(false))
        return;
    wake();
    if (m_thread.joinable())
        m_thread.join();
    m_actions.stop();
    closeDevice();
    setClaimed(false, false);
    if (m_wakeFd >= 0)
        close(m_wakeFd);
    m_wakeFd = -1;
}

void Daemon::applyConfig(std::shared_ptr<const Config> config) {
    {
        std::lock_guard lock(m_configMutex);
        m_config = std::move(config);
    }
    wake();
}

void Daemon::wake() {
    if (m_wakeFd < 0)
        return;
    const uint64_t one = 1;
    while (write(m_wakeFd, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
}

void Daemon::waitForWake(int timeoutMs) {
    // ponytail: no eventfd (descriptor exhaustion) falls back to timed sleeps;
    // the compositor stays responsive, the thread just wakes a few times a second.
    if (m_wakeFd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeoutMs, 1)));
        return;
    }
    pollfd descriptor{m_wakeFd, POLLIN, 0};
    if (poll(&descriptor, 1, timeoutMs) <= 0)
        return;
    uint64_t value = 0;
    while (read(m_wakeFd, &value, sizeof(value)) > 0) {
    }
}

void Daemon::recordLastGesture(const Gesture& gesture) {
    std::lock_guard lock(m_statusMutex);
    m_lastGesture = std::string(zoneName(gesture.zone)) + "/" + directionName(gesture.direction) + " " +
                    std::to_string(gesture.fingers) + "f";
}

bool Daemon::motionShouldBeSwallowed() const {
    // A palm resting in an edge zone keeps its contact claimed but emits no
    // events. Without the liveness check that would swallow pointer motion
    // from every other device until the palm lifts.
    return m_swallowMotion.load() && steadyNowUs() - m_lastTouchFrame.load() < 250'000;
}

bool Daemon::contactsClaimed() const {
    return m_claimedContacts.load();
}

void Daemon::setClaimed(bool claimed, bool swallowPointer) {
    m_claimedContacts.store(claimed);
    m_swallowMotion.store(claimed && swallowPointer);
}

bool Daemon::consumeClickSuppression() {
    const Micros until = m_clickUntil.load();
    if (until == 0)
        return false;
    if (steadyNowUs() >= until) {
        m_clickUntil.store(0);
        return false;
    }
    m_clickUntil.store(0);
    return true;
}

void Daemon::noteKeyPressed() {
    const auto config = configSnapshot();
    if (!config || !config->blockWhileTyping)
        return;
    m_typingUntil.store(steadyNowUs() + static_cast<Micros>(config->typingTimeoutMs) * 1000);
}

std::string Daemon::status() const {
    std::lock_guard lock(m_statusMutex);
    std::string     text;
    text += "state: " + m_state + "\n";
    text += "device: " + (m_statusDevice.empty() ? std::string("not open") : m_statusDevice) + "\n";
    if (!m_statusName.empty())
        text += "name: " + m_statusName + "\n";
    text += "gestures: " + std::to_string(m_gestureCount.load()) + "\n";
    text += "slider_steps: " + std::to_string(m_sliderStepCount.load()) + "\n";
    text += "actions: " + std::to_string(m_actions.executedCount()) + "\n";
    if (const auto config = configSnapshot())
        text += "bindings: " + std::to_string(config->gestures.size()) + " gestures, " +
                std::to_string(config->sliders.size()) + " sliders\n";
    if (!m_lastGesture.empty())
        text += "last: " + m_lastGesture + "\n";
    if (!m_error.empty())
        text += "error: " + m_error + "\n";
    if (!m_actions.lastError().empty())
        text += "action error: " + m_actions.lastError() + "\n";
    return text;
}

std::shared_ptr<const Config> Daemon::configSnapshot() const {
    std::lock_guard lock(m_configMutex);
    return m_config;
}

bool Daemon::typingBlocked(Micros now) const {
    const auto until = m_typingUntil.load();
    return until != 0 && now < until;
}

void Daemon::setStatus(std::string state, std::string error) {
    std::lock_guard lock(m_statusMutex);
    m_state = std::move(state);
    m_error = std::move(error);
}

void Daemon::closeDevice() {
    if (m_device.fd >= 0)
        close(m_device.fd);
    m_device = Device{};
    m_timeOffset.reset();
    {
        std::lock_guard lock(m_statusMutex);
        m_statusDevice.clear();
        m_statusName.clear();
    }
}

bool Daemon::openConfiguredDevice(const Config& config) {
    std::string path = config.device;

    if (path == "auto") {
        const auto candidates = discoverDevices();
        // Virtual touchpads (edgepad, uinput test rigs) are never the physical
        // device the user means, so auto mode skips them.
        std::vector<DeviceCandidate> touchpads;
        for (const auto& candidate : candidates) {
            if (candidate.touchpad && !candidate.virtualDev)
                touchpads.push_back(candidate);
        }

        if (touchpads.empty()) {
            std::string detail;
            for (const auto& candidate : candidates) {
                if (!candidate.readable)
                    detail += "\n  " + candidate.path + ": " + candidate.error;
            }
            setStatus("waiting for device", "device=auto found no touchpad candidates" + detail);
            return false;
        }
        if (touchpads.size() > 1) {
            std::string detail = "device=auto matched multiple touchpads; set device explicitly:";
            for (const auto& candidate : touchpads)
                detail += "\n  " + candidate.path + " (" + candidate.name + ")";
            setStatus("waiting for device", std::move(detail));
            return false;
        }
        path = touchpads.front().path;
    }

    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        setStatus("waiting for device", errorText("cannot open " + path));
        return false;
    }

    std::string error;
    const auto  query = queryDevice(fd, error);
    if (!query || !query->typeB || !query->pointer || query->direct) {
        close(fd);
        setStatus("waiting for device", path + " is not a protocol-B touchpad" + (error.empty() ? "" : ": " + error));
        return false;
    }

    m_device.fd        = fd;
    m_device.path      = path;
    m_device.name      = deviceName(fd);
    m_device.caps      = query->caps;
    m_device.buttonpad = query->buttonpad;
    m_timeOffset.reset();
    {
        std::lock_guard lock(m_statusMutex);
        m_statusDevice = m_device.path;
        m_statusName   = m_device.name;
    }
    return true;
}

void Daemon::handleOutput(const FrameOutput& output, const Config& config) {
    const Micros now = steadyNowUs();

    if (output.tapLike && config.swallowPointer)
        m_clickUntil.store(now + 250'000);

    for (const auto& gesture : output.gestures) {
        m_gestureCount.fetch_add(1);
        if (typingBlocked(now))
            continue;
        const auto* binding = config.matchGesture(gesture);
        if (!binding)
            continue;
        if (binding->logGesture) {
            recordLastGesture(gesture);
            continue;
        }
        if (binding->action.empty())
            continue;
        static std::atomic<uint64_t> sequence{0};
        const std::string key = std::string("g:") + zoneName(gesture.zone) + ":" + directionName(gesture.direction) +
                                ":" + std::to_string(gesture.fingers) + ":" + std::to_string(sequence.fetch_add(1));
        m_actions.enqueue(key, binding->action);
    }

    for (const auto& step : output.sliderSteps) {
        m_sliderStepCount.fetch_add(1);
        if (typingBlocked(now))
            continue;
        const auto* slider = config.findSlider(step.zone);
        if (!slider)
            continue;
        const auto* action = config.actionForSlider(*slider, step.direction);
        if (!action || action->empty())
            continue;
        const std::string key = std::string("s:") + zoneName(step.zone) + ":" + sliderDirectionName(step.direction);
        m_actions.enqueue(key, *action);
    }
}

void Daemon::inputLoop() {
    std::shared_ptr<const Config> applied;
    std::unique_ptr<Engine>       engine;
    std::vector<Event>            frame;
    bool                          dropped = false;
    std::vector<uint16_t>         pressedButtons;
    std::vector<uint16_t>         releasedButtons;

    while (m_running.load()) {
        auto config = configSnapshot();
        if (!config || (config->gestures.empty() && config->sliders.empty())) {
            closeDevice();
            engine.reset();
            applied.reset();
            setClaimed(false, false);
            setStatus("no bindings");
            waitForWake(250);
            continue;
        }

        if (applied && applied->generation != config->generation) {
            // Dropping the engine mid-gesture discards that gesture but never
            // stalls: waiting for an idle engine would block the reader, and
            // the release events it needs would stay queued in the kernel.
            closeDevice();
            engine.reset();
            applied.reset();
            setClaimed(false, false);
        }

        if (!engine) {
            if (applied && applied->generation == config->generation) {
                waitForWake(500);
                continue;
            }
            if (!openConfiguredDevice(*config)) {
                waitForWake(500);
                continue;
            }

            std::vector<SliderSpec> sliders;
            sliders.reserve(config->sliders.size());
            for (const auto& slider : config->sliders)
                sliders.push_back(SliderSpec{slider.zone, slider.axis, slider.step});

            try {
                engine = std::make_unique<Engine>(m_device.caps, config->edges, std::move(sliders), config->options);
            } catch (const std::exception& exception) {
                setStatus("engine error", exception.what());
                closeDevice();
                waitForWake(500);
                continue;
            }
            engine->setButtonpad(m_device.buttonpad);
            applied = config;
            frame.clear();
            dropped = false;
            pressedButtons.clear();
            releasedButtons.clear();
            setStatus("running");
        }

        int timeoutMs = -1;
        if (const auto deadline = engine->nextDeadline()) {
            const Micros remainingUs = *deadline - steadyNowUs();
            timeoutMs                = static_cast<int>(std::clamp<Micros>(remainingUs / 1000 + 1, 0, 1000));
        }

        pollfd descriptors[2] = {{m_device.fd, POLLIN, 0}, {m_wakeFd, POLLIN, 0}};
        const nfds_t descriptorCount = m_wakeFd >= 0 ? 2 : 1;
        const int    ready           = poll(descriptors, descriptorCount, timeoutMs);
        if (!m_running.load())
            break;

        if (descriptorCount == 2 && (descriptors[1].revents & POLLIN)) {
            uint64_t value = 0;
            while (read(m_wakeFd, &value, sizeof(value)) > 0) {
            }
            continue;
        }

        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) {
                setStatus("poll error", errorText("poll"));
                closeDevice();
                engine.reset();
                applied.reset();
                continue;
            }
            try {
                const auto output = engine->advanceTime(steadyNowUs());
                handleOutput(output, *config);
                setClaimed(engine->hasClaimedContacts(), config->swallowPointer);
            } catch (const std::exception& exception) {
                setStatus("recognizer error", exception.what());
                closeDevice();
                engine.reset();
                applied.reset();
                setClaimed(false, false);
            }
            continue;
        }

        input_event events[64];
        const ssize_t count = read(m_device.fd, events, sizeof(events));
        if (count < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            setStatus("read error", errorText("read"));
            closeDevice();
            engine.reset();
            applied.reset();
            setClaimed(false, false);
            continue;
        }

        const size_t eventCount = static_cast<size_t>(count) / sizeof(input_event);
        if (eventCount == 0)
            continue;

        std::vector<Gesture> interrupted;
        try {
            for (size_t index = 0; index < eventCount; ++index) {
                const input_event& event = events[index];
                const Micros eventUs     = static_cast<Micros>(event.time.tv_sec) * 1'000'000 + event.time.tv_usec;
                if (!m_timeOffset && eventUs > 0)
                    m_timeOffset = steadyNowUs() - eventUs;
                const Micros timestamp = m_timeOffset ? eventUs + *m_timeOffset : steadyNowUs();

                if (event.type == EV_SYN && event.code == SYN_DROPPED) {
                    dropped = true;
                    frame.clear();
                    pressedButtons.clear();
                    releasedButtons.clear();
                    continue;
                }

                if (event.type == EV_SYN && event.code == SYN_REPORT) {
                    m_lastTouchFrame.store(steadyNowUs());
                    if (dropped) {
                        dropped = false;
                        frame.clear();
                        pressedButtons.clear();
                        releasedButtons.clear();
                        if (!resyncEngine(m_device.fd, m_device.caps, *engine))
                            engine->processFrameAt({Event{EventType::SynDropped, 0}}, timestamp);
                        setClaimed(engine->hasClaimedContacts(), config->swallowPointer);
                        continue;
                    }
                    if (frame.empty())
                        continue;

                    if (!pressedButtons.empty()) {
                        auto canceled = engine->interruptTapSequence();
                        interrupted.insert(interrupted.end(), canceled.begin(), canceled.end());
                        for (const auto code : pressedButtons)
                            engine->updatePhysicalButton(code, true);
                    }

                    const auto output = engine->processFrameAt(frame, timestamp);

                    for (const auto code : releasedButtons)
                        engine->updatePhysicalButton(code, false);

                    frame.clear();
                    pressedButtons.clear();
                    releasedButtons.clear();

                    if (!interrupted.empty()) {
                        FrameOutput gesturesOnly;
                        gesturesOnly.gestures = std::move(interrupted);
                        interrupted.clear();
                        handleOutput(gesturesOnly, *config);
                    }
                    handleOutput(output, *config);
                    setClaimed(engine->hasClaimedContacts(), config->swallowPointer);
                    continue;
                }

                if (dropped)
                    continue;

                switch (event.type) {
                    case EV_ABS:
                        switch (event.code) {
                            case ABS_MT_SLOT: frame.push_back({EventType::Slot, event.value}); break;
                            case ABS_MT_TRACKING_ID: frame.push_back({EventType::TrackingId, event.value}); break;
                            case ABS_MT_POSITION_X: frame.push_back({EventType::X, event.value}); break;
                            case ABS_MT_POSITION_Y: frame.push_back({EventType::Y, event.value}); break;
                            default: break;
                        }
                        break;
                    case EV_KEY:
                        if (isPointerButton(event.code)) {
                            if (event.value != 0)
                                pressedButtons.push_back(event.code);
                            else
                                releasedButtons.push_back(event.code);
                        }
                        break;
                    default: break;
                }
            }
        } catch (const std::exception& exception) {
            setStatus("recognizer error", exception.what());
            closeDevice();
            engine.reset();
            applied.reset();
            frame.clear();
            pressedButtons.clear();
            releasedButtons.clear();
            setClaimed(false, false);
        }
    }

    closeDevice();
    setClaimed(false, false);
}

} // namespace hyprpad
