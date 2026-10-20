#pragma once
#include "common/ConfigManager.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
namespace fov {
class IPCServer {
    std::jthread thread;
    std::atomic<std::shared_ptr<const Profile>> current;
    std::mutex mutex;
    Json diagnostics;
    std::string path, app;
    void run(std::stop_token);

  public:
    IPCServer(std::string executable, std::string application);
    ~IPCServer();
    Profile profile() const {
        return *current.load();
    }
    void report(Json status);
};
} // namespace fov
