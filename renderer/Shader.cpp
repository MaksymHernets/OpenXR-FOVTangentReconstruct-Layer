#include "IEdgeRenderer.h"
#include <d3dcompiler.h>
#include "ShaderSource.h"
namespace fov {
ComPtr<ID3DBlob> compileShader(const char *entry, const char *target) {
    ComPtr<ID3DBlob> code, errors;
    auto hr =
        D3DCompile(shaderSource, sizeof(shaderSource) - 1, "EdgeFill.hlsl", nullptr, nullptr, entry, target,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr))
        throw std::runtime_error(
            errors ? std::string((char *)errors->GetBufferPointer(), errors->GetBufferSize())
                   : "Shader compilation failed");
    return code;
}
} // namespace fov
