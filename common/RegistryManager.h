#pragma once
#include <filesystem>
#include <string>
namespace fov {
enum class Registration { Missing, Enabled, Disabled, Invalid };
std::filesystem::path moduleDirectory();
void validateManifest(const std::filesystem::path &);
Registration registration(const std::filesystem::path &);
void setRegistration(const std::filesystem::path &, bool enabled);
void unregisterLayer(const std::filesystem::path &);
std::wstring activeRuntime();
int runRegistrationHelper(const std::wstring &action, bool elevate);
} // namespace fov
