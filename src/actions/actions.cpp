#include "actions.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
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
    m_thread = std::thread([this] {
        pthread_setname_np(pthread_self(), "hyprpad-actions");
        workerLoop();
    });
}

void ActionDispatcher::stop() {
    if (!m_running.exchange(false))
        return;
    m_cv.notify_all();
    if (const pid_t pid = m_activePid.load(); pid > 0)
        kill(-pid, SIGKILL);
    if (m_thread.joinable())
        m_thread.join();
}

void ActionDispatcher::enqueue(const std::string& key, std::vector<std::string> argv) {
    if (argv.empty())
        return;
    {
        std::lock_guard lock(m_mutex);
        m_pending[key] = std::move(argv);
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
        std::vector<std::string> job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running.load() || !m_pending.empty(); });
            if (!m_running.load() || m_pending.empty())
                break;
            auto it = m_pending.begin();
            job     = std::move(it->second);
            m_pending.erase(it);
        }

        std::string error;
        if (execute(job, error)) {
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

    // Own process group, so unload can kill the command and anything it forked.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t     pid         = 0;
    const int spawnResult = posix_spawnp(&pid, args[0], nullptr, &attributes, args.data(), environ);
    posix_spawnattr_destroy(&attributes);
    if (spawnResult != 0) {
        error = std::string("failed to run ") + argv[0] + ": " + std::strerror(spawnResult);
        return false;
    }

    m_activePid.store(pid);
    if (!m_running.load())
        kill(-pid, SIGKILL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        // Hyprland's own SIGCHLD handling can reap our children first; the
        // command still ran, only its exit status is gone.
        if (errno == ECHILD) {
            m_activePid.store(0);
            return true;
        }
        error = std::string("failed to wait for ") + argv[0] + ": " + std::strerror(errno);
        m_activePid.store(0);
        return false;
    }
    m_activePid.store(0);

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
