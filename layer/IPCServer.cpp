#include "common/Platform.h"
#include "IPCServer.h"
#include <chrono>
#include <fstream>
namespace fov {
IPCServer::IPCServer(std::string executable, std::string application)
    : path(std::move(executable)), app(std::move(application)) {
    current.store(std::make_shared<const Profile>(loadProfile(path)));
    diagnostics = {
        {"application", app}, {"executable", path}, {"pid", GetCurrentProcessId()}, {"status", "Starting"}};
    thread = std::jthread([this](std::stop_token t) { run(t); });
}
IPCServer::~IPCServer() {
    thread.request_stop();
    if (thread.joinable())
        thread.join();
}
void IPCServer::report(Json j) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (lock.owns_lock())
        diagnostics.update(j);
}
void IPCServer::run(std::stop_token stop) {
    static std::atomic<bool> processRecorded = false;
    auto pipeName = L"\\\\.\\pipe\\OpenXRFovExtension-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(reinterpret_cast<uintptr_t>(this));
    HANDLE pipe =
        CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                         65536, 0, 0, nullptr);
    bool connected = false, recorded = false;
    std::string lastApi, lastStatus, lastError;
    while (!stop.stop_requested()) {
        try {
            current.store(std::make_shared<const Profile>(loadProfile(path)));
            Json j;
            {
                std::lock_guard lock(mutex);
                j = diagnostics;
            }
            j["timestamp"] = utcNow();
            j["settings"] = profileJson(profile());
            if (!recorded) {
                if (!processRecorded.load()) {
                    recordLaunch(path, app, j.value("graphicsApi", "Unknown"));
                    processRecorded.store(true);
                }
                recorded = true;
            }
            auto api = j.value("graphicsApi", "Unknown");
            if (api != lastApi) {
                updateJson(dataDirectory() / L"history.json", [&](Json &h) { h[path]["graphicsApi"] = api; });
                lastApi = api;
            }
            auto status = j.value("status", ""), errorMessage = j.value("lastError", "");
            if (profile().logging && (status != lastStatus || errorMessage != lastError)) {
                auto logfile =
                    dataDirectory() / (L"layer-" + std::to_wstring(GetCurrentProcessId()) + L".log");
                std::error_code ec;
                auto bytes = std::filesystem::file_size(logfile, ec);
                if (!ec && bytes > 1024 * 1024) {
                    auto previous = logfile;
                    previous += L".previous";
                    MoveFileExW(logfile.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
                std::ofstream out(logfile, std::ios::app);
                out << utcNow() << " " << app << " [" << api << "] " << status << " " << errorMessage << "\n";
            }
            lastStatus = status;
            lastError = errorMessage;
            if (pipe != INVALID_HANDLE_VALUE) {
                if (!connected) {
                    BOOL c = ConnectNamedPipe(pipe, nullptr);
                    auto e = GetLastError();
                    connected = c || e == ERROR_PIPE_CONNECTED;
                }
                if (connected) {
                    auto s = j.dump() + "\n";
                    DWORD written = 0;
                    if (!WriteFile(pipe, s.data(), (DWORD)s.size(), &written, nullptr) ||
                        written != s.size()) {
                        DisconnectNamedPipe(pipe);
                        connected = false;
                    }
                }
            }
        } catch (const std::exception &e) {
            report({{"lastError", e.what()}});
        }
        for (int i = 0; i < 10 && !stop.stop_requested(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
} // namespace fov
