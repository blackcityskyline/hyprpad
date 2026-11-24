#include "actions.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hyprpad {

ActionDispatcher::~ActionDispatcher() {
    stop();
}

void ActionDispatcher::start() {
    if (m_running.exchange(true))
        return;
    m_thread = std::thread([this] { workerLoop(); });
}

void ActionDispatcher::stop() {
    if (!m_running.exchange(false))
        return;
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void ActionDispatcher::enqueue(const std::string& key, std::vector<std::string> argv, uint64_t generation) {
    if (argv.empty())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_pending[key] = Job{std::move(argv), generation};
    }
    m_cv.notify_one();
}

uint64_t ActionDispatcher::executedCount() const {
    return m_executed.load();
}

std::string ActionDispatcher::lastError() const {
    std::lock_guard lock(m_mutex);
    return m_lastError;
}

void ActionDispatcher::workerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running.load() || !m_pending.empty(); });
            if (m_pending.empty()) {
                if (!m_running.load())
                    break;
                continue;
            }
            auto it = m_pending.begin();
            job     = std::move(it->second);
            m_pending.erase(it);
        }

        std::string error;
        if (execute(job.argv, error)) {
            m_executed.fetch_add(1);
        } else {
            std::lock_guard lock(m_mutex);
            m_lastError = std::move(error);
        }
    }

    std::lock_guard lock(m_mutex);
    m_pending.clear();
}

bool ActionDispatcher::execute(const std::vector<std::string>& argv, std::string& error) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& value : argv)
        args.push_back(const_cast<char*>(value.c_str()));
    args.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ);
    if (spawnResult != 0) {
        error = std::string("failed to run ") + argv[0] + ": " + std::strerror(spawnResult);
        return false;
    }

    int   status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(pid, &status, 0)) < 0) {
        if (errno == EINTR)
            continue;
        error = std::string("failed to wait for ") + argv[0] + ": " + std::strerror(errno);
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;

    if (WIFEXITED(status))
        error = argv[0] + " exited with status " + std::to_string(WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        error = argv[0] + " was killed by signal " + std::to_string(WTERMSIG(status));
    else
        error = argv[0] + " failed";
    return false;
}

} // namespace hyprpad
