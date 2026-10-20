#include "IEdgeRenderer.h"
#include <unordered_map>
#include <array>
#include <map>
namespace fov {
class D3D11EdgeRenderer final : public IEdgeRenderer {
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Buffer> temporalCb;
    TemporalInput temporal;
    TemporalMetrics metrics;
    std::unordered_map<uint64_t, TemporalMetrics> metricsByToken;
    struct Image {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::map<std::pair<IUnknown *, unsigned>, ComPtr<ID3D11RenderTargetView>> rtvs;
    };
    std::unordered_map<uint64_t, Image> images;
    struct Timing {
        ComPtr<ID3D11Query> start, end, disjoint;
        ComPtr<ID3D11Query> completed;
        ComPtr<ID3D11Buffer> counters, readback;
        ComPtr<ID3D11UnorderedAccessView> uav;
        bool metricIssued = false;
        uint64_t token = 0;
        bool issued = false;
    };
    std::array<Timing, 8> timings;
    size_t timingCursor = 0;
    bool timingEnabled = false;
    std::optional<double> elapsed;
    struct TransferTiming {
        ComPtr<ID3D11Query> start, end, disjoint;
        bool issued = false, history = false;
    };
    std::array<TransferTiming, 8> transferTimings;
    size_t transferCursor = 0;
    std::optional<double> sourceElapsed, historyElapsed;
    TransferTiming *beginTransfer(bool history) {
        if (!timingEnabled)
            return nullptr;
        auto &t = transferTimings[transferCursor++ % transferTimings.size()];
        if (t.issued) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            UINT64 start = 0, end = 0;
            if (context->GetData(t.disjoint.Get(), &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !d.Disjoint && d.Frequency &&
                context->GetData(t.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                    S_OK &&
                context->GetData(t.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
                (t.history ? historyElapsed : sourceElapsed) = 1000.0 * (end - start) / d.Frequency;
        }
        t.history = history;
        context->Begin(t.disjoint.Get());
        context->End(t.start.Get());
        return &t;
    }
    void endTransfer(TransferTiming *t) {
        if (t) {
            context->End(t->end.Get());
            context->End(t->disjoint.Get());
            t->issued = true;
        }
    }
    struct StateGuard {
        ID3D11DeviceContext1 *ctx;
        ComPtr<ID3DDeviceContextState> old;
        StateGuard(ID3D11DeviceContext1 *c, ID3DDeviceContextState *s) : ctx(c) {
            ctx->SwapDeviceContextState(s, &old);
        }
        ~StateGuard() {
            ctx->SwapDeviceContextState(old.Get(), nullptr);
        }
    };

  public:
    explicit D3D11EdgeRenderer(ID3D11Device *d) {
        check(d->QueryInterface(IID_PPV_ARGS(&device)));
        device->GetImmediateContext1(&context);
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL chosen;
        check(device->CreateDeviceContextState(0, levels, 2, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                               &chosen, &state));
        auto v = compileShader("VS", "vs_5_0"), p = compileShader("PS", "ps_5_0");
        check(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs));
        check(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sd, &sampler));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ShaderConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&bd, nullptr, &cb));
        bd.ByteWidth = sizeof(TemporalGpu);
        check(device->CreateBuffer(&bd, nullptr, &temporalCb));
        for (auto &t : timings) {
            D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
            check(device->CreateQuery(&q, &t.start));
            check(device->CreateQuery(&q, &t.end));
            q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            check(device->CreateQuery(&q, &t.disjoint));
            q.Query = D3D11_QUERY_EVENT;
            check(device->CreateQuery(&q, &t.completed));
            D3D11_BUFFER_DESC counter{};
            counter.ByteWidth = 16;
            counter.Usage = D3D11_USAGE_DEFAULT;
            counter.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            counter.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            counter.StructureByteStride = 4;
            check(device->CreateBuffer(&counter, nullptr, &t.counters));
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = 4;
            check(device->CreateUnorderedAccessView(t.counters.Get(), &uav, &t.uav));
            counter.Usage = D3D11_USAGE_STAGING;
            counter.BindFlags = 0;
            counter.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            counter.MiscFlags = 0;
            counter.StructureByteStride = 0;
            check(device->CreateBuffer(&counter, nullptr, &t.readback));
            auto &transfer = transferTimings[&t - timings.data()];
            D3D11_QUERY_DESC transferQuery{D3D11_QUERY_TIMESTAMP, 0};
            check(device->CreateQuery(&transferQuery, &transfer.start));
            check(device->CreateQuery(&transferQuery, &transfer.end));
            transferQuery.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            check(device->CreateQuery(&transferQuery, &transfer.disjoint));
        }
    }
    void capture(uint64_t key, IUnknown *source, DXGI_FORMAT format) override {
        ComPtr<ID3D11Texture2D> src;
        check(source->QueryInterface(IID_PPV_ARGS(&src)));
        auto &im = images[key];
        if (!im.texture) {
            D3D11_TEXTURE2D_DESC td;
            src->GetDesc(&td);
            if (td.SampleDesc.Count != 1)
                throw std::runtime_error("MSAA passthrough");
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (depthFormat(format))
                td.Format = depthResourceFormat(format);
            check(device->CreateTexture2D(&td, nullptr, &im.texture));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = sampledFormat(format);
            im.format = sd.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = td.ArraySize;
            check(device->CreateShaderResourceView(im.texture.Get(), &sd, &im.srv));
        }
        StateGuard guard(context.Get(), state.Get());
        auto transfer = beginTransfer(false);
        context->CopyResource(im.texture.Get(), src.Get());
        endTransfer(transfer);
    }
    void render(uint64_t key, IUnknown *output, DXGI_FORMAT format, const ShaderConstants &c,
                const XrRect2Di &dest) override {
        auto &im = images.at(key);
        ComPtr<ID3D11Texture2D> dst;
        check(output->QueryInterface(IID_PPV_ARGS(&dst)));
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.ArraySize = 1;
        rd.Texture2DArray.FirstArraySlice = c.slice;
        auto &rtv = im.rtvs[{output, c.slice}];
        if (!rtv)
            check(device->CreateRenderTargetView(dst.Get(), &rd, &rtv));
        StateGuard guard(context.Get(), state.Get());
        context->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
        auto &t = timings[timingCursor++ % timings.size()];
        if (t.metricIssued &&
            context->GetData(t.completed.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (context->Map(t.readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) ==
                S_OK) {
                auto values = static_cast<uint32_t *>(mapped.pData);
                metrics.available = values[0] > 0;
                metrics.samples = values[0];
                metrics.reconstructedPercent = values[0] ? 100.0 * values[1] / values[0] : 0;
                metrics.fallbackPercent = 100 - metrics.reconstructedPercent;
                metrics.depthUsed = values[2] > 0;
                context->Unmap(t.readback.Get(), 0);
                metricsByToken[t.token] = metrics;
            }
        }
        t.metricIssued = false;
        if (timingEnabled && t.issued) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 start = 0, end = 0;
            if (context->GetData(t.disjoint.Get(), &disjoint, sizeof(disjoint),
                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !disjoint.Disjoint && disjoint.Frequency &&
                context->GetData(t.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                    S_OK &&
                context->GetData(t.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
                elapsed = 1000.0 * (end - start) / disjoint.Frequency;
        }
        if (timingEnabled) {
            context->Begin(t.disjoint.Get());
            context->End(t.start.Get());
        }
        auto rt = rtv.Get();
        if (c.method == 5) {
            UINT zeros[4]{};
            context->ClearUnorderedAccessViewUint(t.uav.Get(), zeros);
            auto uav = t.uav.Get();
            context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rt, nullptr, 1, 1, &uav, nullptr);
        } else
            context->OMSetRenderTargets(1, &rt, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        std::array<ID3D11ShaderResourceView *, 17> views{};
        views[0] = im.srv.Get();
        if (c.method == 5)
            for (size_t i = 0; i < 8; i++) {
                auto color = images.find(temporal.colors[i]);
                if (color != images.end())
                    views[1 + i] = color->second.srv.Get();
                auto depth = images.find(temporal.depths[i]);
                if (depth != images.end())
                    views[9 + i] = depth->second.srv.Get();
            }
        context->PSSetShaderResources(0, (UINT)views.size(), views.data());
        auto s = sampler.Get();
        context->PSSetSamplers(0, 1, &s);
        auto b = cb.Get();
        context->PSSetConstantBuffers(0, 1, &b);
        context->UpdateSubresource(temporalCb.Get(), 0, nullptr, &temporal.gpu, 0, 0);
        b = temporalCb.Get();
        context->PSSetConstantBuffers(1, 1, &b);
        D3D11_VIEWPORT vp{(float)dest.offset.x,
                          (float)dest.offset.y,
                          (float)dest.extent.width,
                          (float)dest.extent.height,
                          0,
                          1};
        context->RSSetViewports(1, &vp);
        context->Draw(3, 0);
        if (c.method == 5) {
            ID3D11UnorderedAccessView *uav = nullptr;
            context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &uav, nullptr);
            context->CopyResource(t.readback.Get(), t.counters.Get());
            context->End(t.completed.Get());
            t.metricIssued = true;
            t.token = temporal.token;
        }
        if (timingEnabled) {
            context->End(t.end.Get());
            context->End(t.disjoint.Get());
            t.issued = true;
        }
        views.fill(nullptr);
        context->PSSetShaderResources(0, (UINT)views.size(), views.data());
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void forget(uint64_t key) override {
        images.erase(key);
        metricsByToken.clear();
    }
    void storeHistory(uint64_t source, uint64_t destination, const XrSwapchainSubImage &region) override {
        auto &src = images.at(source);
        D3D11_TEXTURE2D_DESC desc{};
        src.texture->GetDesc(&desc);
        auto r = region.imageRect;
        auto &dst = images[destination];
        if (dst.texture) {
            D3D11_TEXTURE2D_DESC old{};
            dst.texture->GetDesc(&old);
            if (old.Width != (UINT)r.extent.width || old.Height != (UINT)r.extent.height ||
                old.Format != desc.Format)
                dst = Image{};
        }
        if (!dst.texture) {
            desc.Width = r.extent.width;
            desc.Height = r.extent.height;
            desc.ArraySize = 1;
            check(device->CreateTexture2D(&desc, nullptr, &dst.texture));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = src.format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = sd.Texture2DArray.ArraySize = 1;
            check(device->CreateShaderResourceView(dst.texture.Get(), &sd, &dst.srv));
            dst.format = src.format;
        }
        StateGuard guard(context.Get(), state.Get());
        auto transfer = beginTransfer(true);
        D3D11_BOX box{(UINT)r.offset.x,
                      (UINT)r.offset.y,
                      0,
                      (UINT)(r.offset.x + r.extent.width),
                      (UINT)(r.offset.y + r.extent.height),
                      1};
        context->CopySubresourceRegion(dst.texture.Get(), 0, 0, 0, 0, src.texture.Get(),
                                       region.imageArrayIndex, &box);
        endTransfer(transfer);
    }
    void setTemporal(const TemporalInput &input) override {
        temporal = input;
    }
    TemporalMetrics temporalMetrics() const override {
        auto found = metricsByToken.find(temporal.token);
        return found == metricsByToken.end() ? TemporalMetrics{} : found->second;
    }
    void setTiming(bool enabled) override {
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
std::unique_ptr<IEdgeRenderer> makeD3D11(ID3D11Device *d) {
    return std::make_unique<D3D11EdgeRenderer>(d);
}
} // namespace fov
