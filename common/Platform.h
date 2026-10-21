#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <stdexcept>
#include <string>
namespace fov {
class UniqueHandle {
    HANDLE value = nullptr;

  public:
    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    ~UniqueHandle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void reset(HANDLE handle) {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = handle;
    }
    HANDLE get() const {
        return value;
    }
};
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("DirectX HRESULT " + std::to_string(static_cast<unsigned long>(hr)));
}
} // namespace fov
