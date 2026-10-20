#include "IEdgeRenderer.h"
#include <array>
#include <unordered_map>
#include <vector>
namespace fov {
class D3D12EdgeRenderer final : public IEdgeRenderer {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3DBlob> vs, ps;
    ComPtr<ID3D12Fence> fence;
    UniqueHandle event;
    UINT64 serial = 0;
    size_t cursor = 0;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestamps;
    UINT64 *mappedTimes = nullptr;
    UINT64 frequency = 0;
    bool timingEnabled = false;
    std::optional<double> elapsed;
    std::optional<double> sourceElapsed, historyElapsed;
    TemporalInput temporal;
    TemporalMetrics metrics;
    std::unordered_map<uint64_t, TemporalMetrics> metricsByToken;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12DescriptorHeap> srv, rtv;
        UINT64 done = 0;
        UINT index = 0;
        bool measured = false;
        int timingStage = 0;
        ComPtr<ID3D12Resource> parameters, counters, readback;
        BYTE *parameterBytes = nullptr;
        uint32_t *counterValues = nullptr;
        bool metricIssued = false;
        uint64_t token = 0;
    };
    std::array<Slot, 8> slots;
    struct Image {
        ComPtr<ID3D12Resource> texture;
        DXGI_FORMAT format;
    };
    std::unordered_map<uint64_t, Image> images;
    std::unordered_map<int, ComPtr<ID3D12PipelineState>> pipelines;
    void wait(UINT64 value) {
        if (value && fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, event.get()));
            if (WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0)
                throw std::runtime_error("D3D12 fence wait failed");
        }
    }
    Slot &begin() {
        auto &s = slots[cursor++ % slots.size()];
        wait(s.done);
        if (s.metricIssued) {
            auto values = s.counterValues;
            metrics.available = values[0] > 0;
            metrics.samples = values[0];
            metrics.reconstructedPercent = values[0] ? 100.0 * values[1] / values[0] : 0;
            metrics.fallbackPercent = 100 - metrics.reconstructedPercent;
            metrics.depthUsed = values[2] > 0;
            metricsByToken[s.token] = metrics;
            s.metricIssued = false;
        }
        if (s.measured && timingEnabled && frequency) {
            auto a = mappedTimes[s.index * 2], b = mappedTimes[s.index * 2 + 1];
            auto duration = 1000.0 * (b - a) / frequency;
            if (s.timingStage == 0)
                elapsed = duration;
            else if (s.timingStage == 1)
                sourceElapsed = duration;
            else
                historyElapsed = duration;
        }
        s.measured = false;
        check(s.allocator->Reset());
        check(s.list->Reset(s.allocator.Get(), nullptr));
        return s;
    }
    void submit(Slot &s) {
        check(s.list->Close());
        ID3D12CommandList *l = s.list.Get();
        queue->ExecuteCommandLists(1, &l);
        s.done = ++serial;
        check(queue->Signal(fence.Get(), serial));
    }
    void beginTimed(Slot &s, int stage) {
        s.timingStage = stage;
        if (timingEnabled && frequency)
            s.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.index * 2);
    }
    void endTimed(Slot &s) {
        if (timingEnabled && frequency) {
            s.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.index * 2 + 1);
            s.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s.index * 2, 2,
                                     timestamps.Get(), s.index * 16);
            s.measured = true;
        }
    }
    static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        l->ResourceBarrier(1, &b);
    }
    ID3D12PipelineState *pipeline(DXGI_FORMAT format) {
        auto &p = pipelines[(int)format];
        if (!p) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = root.Get();
            d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            d.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            d.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
            d.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            d.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            d.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
            d.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = format;
            d.SampleDesc.Count = 1;
            check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p)));
        }
        return p.Get();
    }

  public:
    D3D12EdgeRenderer(ID3D12Device *d, ID3D12CommandQueue *q) : device(d), queue(q) {
        if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::runtime_error("D3D12 requires DIRECT queue");
        vs = compileShader("VS", "vs_5_0");
        ps = compileShader("PS", "ps_5_0");
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 17, 0, 0,
                                     D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, sizeof(ShaderConstants) / 4};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor = {1, 0};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor = {1, 0};
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{4, params, 1, &sampler,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event.get())
            throw std::runtime_error("Cannot create D3D12 fence event");
        for (auto &s : slots) {
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)));
            check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&s.list)));
            check(s.list->Close());
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 17;
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.srv)));
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 1;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv)));
            auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES initial,
                              D3D12_RESOURCE_FLAGS flags) {
                ComPtr<ID3D12Resource> resource;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = type;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width = bytes;
                desc.Height = 1;
                desc.DepthOrArraySize = desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                desc.Flags = flags;
                check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
                                                      IID_PPV_ARGS(&resource)));
                return resource;
            };
            s.parameters = buffer(D3D12_HEAP_TYPE_UPLOAD, 1024, D3D12_RESOURCE_STATE_GENERIC_READ,
                                  D3D12_RESOURCE_FLAG_NONE);
            check(s.parameters->Map(0, nullptr, reinterpret_cast<void **>(&s.parameterBytes)));
            memset(s.parameterBytes, 0, 1024);
            s.counters = buffer(D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            s.readback = buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_FLAG_NONE);
            check(s.readback->Map(0, nullptr, reinterpret_cast<void **>(&s.counterValues)));
        }
    }
    ~D3D12EdgeRenderer() {
        try {
            wait(serial);
        } catch (...) {
        }
        if (mappedTimes)
            timestamps->Unmap(0, nullptr);
        for (auto &s : slots) {
            if (s.parameterBytes)
                s.parameters->Unmap(0, nullptr);
            if (s.counterValues)
                s.readback->Unmap(0, nullptr);
        }
    }
    void capture(uint64_t key, IUnknown *source, DXGI_FORMAT format) override {
        ComPtr<ID3D12Resource> src;
        check(source->QueryInterface(IID_PPV_ARGS(&src)));
        auto &im = images[key];
        bool fresh = !im.texture;
        if (fresh) {
            auto d = src->GetDesc();
            if (d.SampleDesc.Count != 1)
                throw std::runtime_error("MSAA passthrough");
            d.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (depthFormat(format))
                d.Format = depthResourceFormat(format);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&im.texture)));
            im.format = sampledFormat(format);
        }
        auto &s = begin();
        beginTimed(s, 1);
        if (!fresh)
            barrier(s.list.Get(), im.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto sourceState =
            depthFormat(format) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier(s.list.Get(), src.Get(), sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        s.list->CopyResource(im.texture.Get(), src.Get());
        barrier(s.list.Get(), src.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
        barrier(s.list.Get(), im.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        endTimed(s);
        submit(s);
    }
    void render(uint64_t key, IUnknown *output, DXGI_FORMAT format, const ShaderConstants &c,
                const XrRect2Di &dest) override {
        auto &im = images.at(key);
        ComPtr<ID3D12Resource> dst;
        check(output->QueryInterface(IID_PPV_ARGS(&dst)));
        auto p = pipeline(format);
        auto &s = begin();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = im.format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = im.texture->GetDesc().DepthOrArraySize;
        auto descriptor = s.srv->GetCPUDescriptorHandleForHeapStart();
        auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(im.texture.Get(), &sd, descriptor);
        for (size_t i = 0; i < 16; i++) {
            descriptor.ptr += stride;
            auto textureKey = i < 8 ? temporal.colors[i] : temporal.depths[i - 8];
            auto image = c.method == 5 ? images.find(textureKey) : images.end();
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = image == images.end() ? (i < 8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT)
                                                : image->second.format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = 1;
            device->CreateShaderResourceView(image == images.end() ? nullptr : image->second.texture.Get(),
                                             &view, descriptor);
        }
        memcpy(s.parameterBytes, &temporal.gpu, sizeof(temporal.gpu));
        if (c.method == 5) {
            barrier(s.list.Get(), s.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            s.list->CopyBufferRegion(s.counters.Get(), 0, s.parameters.Get(), 768, 16);
            barrier(s.list.Get(), s.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        D3D12_RENDER_TARGET_VIEW_DESC rt{};
        rt.Format = format;
        rt.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rt.Texture2DArray.FirstArraySlice = c.slice;
        rt.Texture2DArray.ArraySize = 1;
        device->CreateRenderTargetView(dst.Get(), &rt, s.rtv->GetCPUDescriptorHandleForHeapStart());
        beginTimed(s, 0);
        s.list->SetPipelineState(p);
        s.list->SetGraphicsRootSignature(root.Get());
        auto heap = s.srv.Get();
        s.list->SetDescriptorHeaps(1, &heap);
        s.list->SetGraphicsRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
        s.list->SetGraphicsRootDescriptorTable(1, s.srv->GetGPUDescriptorHandleForHeapStart());
        s.list->SetGraphicsRootConstantBufferView(2, s.parameters->GetGPUVirtualAddress());
        s.list->SetGraphicsRootUnorderedAccessView(3, s.counters->GetGPUVirtualAddress());
        auto handle = s.rtv->GetCPUDescriptorHandleForHeapStart();
        s.list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        D3D12_VIEWPORT vp{(float)dest.offset.x,
                          (float)dest.offset.y,
                          (float)dest.extent.width,
                          (float)dest.extent.height,
                          0,
                          1};
        D3D12_RECT sc{dest.offset.x, dest.offset.y, dest.offset.x + dest.extent.width,
                      dest.offset.y + dest.extent.height};
        s.list->RSSetViewports(1, &vp);
        s.list->RSSetScissorRects(1, &sc);
        s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        s.list->DrawInstanced(3, 1, 0, 0);
        if (c.method == 5) {
            barrier(s.list.Get(), s.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            s.list->CopyBufferRegion(s.readback.Get(), 0, s.counters.Get(), 0, 16);
            barrier(s.list.Get(), s.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.metricIssued = true;
            s.token = temporal.token;
        }
        endTimed(s);
        submit(s);
    }
    void forget(uint64_t key) override {
        wait(serial);
        images.erase(key);
        metricsByToken.clear();
    }
    void storeHistory(uint64_t source, uint64_t destination, const XrSwapchainSubImage &region) override {
        auto &src = images.at(source);
        auto desc = src.texture->GetDesc();
        auto r = region.imageRect;
        auto &dst = images[destination];
        if (dst.texture) {
            auto old = dst.texture->GetDesc();
            if (old.Width != (UINT)r.extent.width || old.Height != (UINT)r.extent.height ||
                old.Format != desc.Format) {
                wait(serial);
                dst = Image{};
            }
        }
        bool fresh = !dst.texture;
        if (fresh) {
            desc.Width = r.extent.width;
            desc.Height = r.extent.height;
            desc.DepthOrArraySize = 1;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&dst.texture)));
            dst.format = src.format;
        }
        auto &s = begin();
        beginTimed(s, 2);
        if (!fresh)
            barrier(s.list.Get(), dst.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(s.list.Get(), src.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = src.texture.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = region.imageArrayIndex;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = dst.texture.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{(UINT)r.offset.x,
                      (UINT)r.offset.y,
                      0,
                      (UINT)(r.offset.x + r.extent.width),
                      (UINT)(r.offset.y + r.extent.height),
                      1};
        s.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        // Depth/stencil resources have two planes; copy both when present to retain valid resource contents.
        if (src.format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
            src.format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
            from.SubresourceIndex += src.texture->GetDesc().DepthOrArraySize;
            to.SubresourceIndex = 1;
            s.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }
        barrier(s.list.Get(), src.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(s.list.Get(), dst.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        endTimed(s);
        submit(s);
    }
    void setTemporal(const TemporalInput &input) override {
        temporal = input;
    }
    TemporalMetrics temporalMetrics() const override {
        auto found = metricsByToken.find(temporal.token);
        return found == metricsByToken.end() ? TemporalMetrics{} : found->second;
    }
    void setTiming(bool enabled) override {
        if (enabled && !mappedTimes) {
            ComPtr<ID3D12QueryHeap> newQueries;
            ComPtr<ID3D12Resource> newTimestamps;
            check(queue->GetTimestampFrequency(&frequency));
            D3D12_QUERY_HEAP_DESC q{};
            q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            q.Count = 16;
            check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&newQueries)));
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 128;
            rd.Height = 1;
            rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&newTimestamps)));
            check(newTimestamps->Map(0, nullptr, reinterpret_cast<void **>(&mappedTimes)));
            queries = std::move(newQueries);
            timestamps = std::move(newTimestamps);
            for (UINT i = 0; i < slots.size(); i++)
                slots[i].index = i;
        }
        timingEnabled = enabled;
        if (!enabled)
            elapsed.reset();
    }
    std::optional<double> gpuMilliseconds() const override {
        return elapsed;
    }
    std::optional<double> sourceCopyMilliseconds() const override {
        return sourceElapsed;
    }
    std::optional<double> historyCopyMilliseconds() const override {
        return historyElapsed;
    }
};
std::unique_ptr<IEdgeRenderer> makeD3D12(ID3D12Device *d, ID3D12CommandQueue *q) {
    return std::make_unique<D3D12EdgeRenderer>(d, q);
}
} // namespace fov
