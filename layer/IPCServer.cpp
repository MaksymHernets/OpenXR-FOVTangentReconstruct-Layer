#include "common/Platform.h"
#include "IPCServer.h"
#include <chrono>
#include <fstream>
#include <array>
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
    struct Client {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        bool connected = false;
    };
    std::array<Client, 4> clients;
    for (auto &client : clients)
        client.pipe =
            CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             UINT(clients.size()), 65536, 0, 0, nullptr);
    bool recorded = false;
    std::string lastApi, lastStatus, lastError, lastFFR;
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
            auto ffr = j.value("ffr", Json::object());
            ffr.erase("displayTime");
            if (ffr.contains("automaticScene")) {
                auto automatic = ffr["automaticScene"];
                ffr["automaticScene"] = {
                    {"installed", automatic.value("installed", false)},
                    {"executionObserved", automatic.value("executionConfirmedByFence", false)},
                    {"reason", automatic.value("reason", std::string{})}};
            }
            auto ffrText = ffr.empty() ? std::string{} : ffr.dump();
            if (profile().logging &&
                (status != lastStatus || errorMessage != lastError || ffrText != lastFFR)) {
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
                if (ffrText != lastFFR && ffr.contains("eyes"))
                    for (auto &eye : ffr["eyes"]) {
                        out << "[FFR] Eye " << eye.value("eye", 0)
                            << " Effective FOV Scale: " << eye["effectiveScale"].dump()
                            << " Mode: " << eye.value("mode", "")
                            << " Zone 1: Full Quality; Zone 2: Reduced Quality; "
                            << "Outside: " << eye.value("outsideRegion", "unknown")
                            << "; Outside scene suppression: unsupported, scene savings 0%; "
                            << " Reconstruction Mode: " << ffr.value("reconstructionMode", "")
                            << " Shading Rate Backend: " << ffr.value("capabilities", Json::object()).dump()
                            << " Applied Shading Rates: pixel frequency not sampled in game; "
                            << "Automatic execution evidence: "
                            << ffr.value("automaticScene", Json::object()).dump() << "\n";
                    }
            }
            lastFFR = ffrText;
            lastStatus = status;
            lastError = errorMessage;
            for (auto &client : clients) {
                auto pipe = client.pipe;
                auto &connected = client.connected;
                if (pipe == INVALID_HANDLE_VALUE)
                    continue;
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
    for (auto &client : clients)
        if (client.pipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(client.pipe);
            CloseHandle(client.pipe);
        }
}
} // namespace fov
