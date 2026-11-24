#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hyprpad {

// Runs argv arrays without a shell. One worker keeps a latest-value slot per
// key, so a burst of slider steps collapses to the newest step instead of
// replaying a process per event.
class ActionDispatcher {
  public:
    ActionDispatcher() = default;
    ~ActionDispatcher();

    ActionDispatcher(const ActionDispatcher&) = delete;
    ActionDispatcher& operator=(const ActionDispatcher&) = delete;

    void start();
    void stop();

    void enqueue(const std::string& key, std::vector<std::string> argv, uint64_t generation);

    uint64_t executedCount() const;
    std::string lastError() const;

  private:
    struct Job {
        std::vector<std::string> argv;
        uint64_t                 generation = 0;
    };

    void workerLoop();
    bool execute(const std::vector<std::string>& argv, std::string& error);

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, Job> m_pending;
    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<uint64_t>   m_executed{0};
    std::string             m_lastError;
};

} // namespace hyprpad
