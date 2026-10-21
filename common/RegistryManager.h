#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace fov {
enum class Registration { Missing, Enabled, Disabled, Invalid };
std::filesystem::path moduleDirectory();
void validateManifest(const std::filesystem::path &);
Registration registration(const std::filesystem::path &);
std::vector<std::filesystem::path> activeRegistrationConflicts(const std::filesystem::path &);
std::string registrationFailureMessage(int code);
void setRegistration(const std::filesystem::path &, bool enabled);
void unregisterLayer(const std::filesystem::path &);
std::wstring activeRuntime();
int runRegistrationHelper(const std::wstring &action, bool elevate);
} // namespace fov
