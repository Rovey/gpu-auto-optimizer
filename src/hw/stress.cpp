#include "hw/stress.hpp"
#include "core/stress_math.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace gao {

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr int kTile = 16;
constexpr double kTargetBatchMs = 250.0;
constexpr int kMaxDispatches = 4096;
// ~1 ms per pass on an RTX 4070; 20000 passes is far beyond the 2 s TDR limit.
constexpr UINT kTdrPasses = 20000;

// Tiled matmul. Every thread computes one element of C = A * B, compares it
// with the reference, and counts a mismatch. `passes` repeats the whole
// computation; it is 1 except for the TDR self-test.
const char kShader[] = R"(
#define N 1024
#define T 16
StructuredBuffer<float> A : register(t0);
StructuredBuffer<float> B : register(t1);
StructuredBuffer<float> Ref : register(t2);
RWStructuredBuffer<uint> Errors : register(u0);
cbuffer Params : register(b0) { uint passes; uint3 pad; };
groupshared float As[T][T];
groupshared float Bs[T][T];
[numthreads(T, T, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint row = gid.y * T + tid.y;
    uint col = gid.x * T + tid.x;
    float acc = 0;
    [loop] for (uint p = 0; p < passes; ++p) {
        acc = 0;
        [loop] for (uint k0 = 0; k0 < N; k0 += T) {
            As[tid.y][tid.x] = A[row * N + k0 + tid.x];
            Bs[tid.y][tid.x] = B[(k0 + tid.y) * N + col];
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint k = 0; k < T; ++k) acc = mad(As[tid.y][k], Bs[k][tid.x], acc);
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (acc != Ref[row * N + col]) InterlockedAdd(Errors[0], 1);
}
)";

std::string Narrow(const wchar_t* w) {
    char buf[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

std::string Hr(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

}

struct Stress::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11ShaderResourceView> a, b, ref;
    ComPtr<ID3D11Buffer> errors, staging, params;
    ComPtr<ID3D11UnorderedAccessView> errors_uav;
    std::vector<float> ha, hb, href;   // host copies, kept for device recreation
};

Stress::Stress() : impl_(std::make_unique<Impl>()) {}
Stress::~Stress() = default;

bool Stress::Init(StressSelftest selftest) {
    selftest_ = selftest;
    impl_->ha = make_stress_matrix(1);
    impl_->hb = make_stress_matrix(2);
    impl_->href = reference_matmul(impl_->ha, impl_->hb);
    if (selftest_ == StressSelftest::WrongResult) impl_->href[0] += 1.0f;
    return CreateDevice();
}

bool Stress::CreateDevice() {
    Impl& d = *impl_;
    d.device.Reset(); d.ctx.Reset(); d.cs.Reset();
    d.a.Reset(); d.b.Reset(); d.ref.Reset();
    d.errors.Reset(); d.staging.Reset(); d.params.Reset(); d.errors_uav.Reset();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { error_ = Hr("CreateDXGIFactory1", hr); return false; }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == kNvidiaVendorId) { adapter_name_ = Narrow(desc.Description); break; }
        adapter.Reset();
    }
    if (!adapter) { error_ = "no NVIDIA adapter found"; return false; }

    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                           D3D11_SDK_VERSION, &d.device, nullptr, &d.ctx);
    if (FAILED(hr)) { error_ = Hr("D3D11CreateDevice", hr); return false; }

    ComPtr<ID3DBlob> code, log;
    hr = D3DCompile(kShader, sizeof(kShader) - 1, "stress.hlsl", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
    if (FAILED(hr)) {
        error_ = Hr("D3DCompile", hr);
        if (log) error_ += std::string(": ") + static_cast<const char*>(log->GetBufferPointer());
        return false;
    }
    hr = d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.cs);
    if (FAILED(hr)) { error_ = Hr("CreateComputeShader", hr); return false; }

    auto make_srv = [&](const std::vector<float>& host, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(host.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA init{host.data()};
        ComPtr<ID3D11Buffer> buf;
        HRESULT h = d.device->CreateBuffer(&bd, &init, &buf);
        if (SUCCEEDED(h)) h = d.device->CreateShaderResourceView(buf.Get(), nullptr, &out);
        return h;
    };
    if (FAILED(hr = make_srv(d.ha, d.a)) || FAILED(hr = make_srv(d.hb, d.b)) ||
        FAILED(hr = make_srv(d.href, d.ref))) {
        error_ = Hr("input buffer", hr); return false;
    }

    D3D11_BUFFER_DESC ed{};
    ed.ByteWidth = sizeof(UINT);
    ed.Usage = D3D11_USAGE_DEFAULT;
    ed.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ed.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ed.StructureByteStride = sizeof(UINT);
    if (FAILED(hr = d.device->CreateBuffer(&ed, nullptr, &d.errors)) ||
        FAILED(hr = d.device->CreateUnorderedAccessView(d.errors.Get(), nullptr, &d.errors_uav))) {
        error_ = Hr("error counter", hr); return false;
    }
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = sizeof(UINT);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = d.device->CreateBuffer(&sd, nullptr, &d.staging))) {
        error_ = Hr("staging buffer", hr); return false;
    }
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = 16;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(hr = d.device->CreateBuffer(&pd, nullptr, &d.params))) {
        error_ = Hr("constant buffer", hr); return false;
    }
    return true;
}

StressBatch Stress::Batch() {
    StressBatch out;
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&] {
        out.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        return out;
    };
    Impl& d = *impl_;
    if (!d.device && !CreateDevice()) { out.device_lost = true; return finish(); }

    const bool tdr = selftest_ == StressSelftest::Tdr;
    selftest_ = tdr ? StressSelftest::None : selftest_;   // the TDR self-test fires once
    const UINT params[4] = {tdr ? kTdrPasses : 1u, 0, 0, 0};
    d.ctx->UpdateSubresource(d.params.Get(), 0, nullptr, params, 0, 0);

    const UINT zero[4] = {0, 0, 0, 0};
    d.ctx->ClearUnorderedAccessViewUint(d.errors_uav.Get(), zero);
    d.ctx->CSSetShader(d.cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {d.a.Get(), d.b.Get(), d.ref.Get()};
    d.ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = {d.errors_uav.Get()};
    d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = {d.params.Get()};
    d.ctx->CSSetConstantBuffers(0, 1, cbs);

    const int dispatches = tdr ? 1 : dispatches_;
    constexpr UINT groups = kStressN / kTile;
    for (int i = 0; i < dispatches; ++i) d.ctx->Dispatch(groups, groups, 1);
    d.ctx->CopyResource(d.staging.Get(), d.errors.Get());

    // Map blocks until the GPU has finished, so elapsed time is GPU time.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = d.ctx->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || d.device->GetDeviceRemovedReason() != S_OK) {
        out.device_lost = true;
        d.device.Reset();   // next Batch() recreates everything
        return finish();
    }
    UINT count = 0;
    std::memcpy(&count, mapped.pData, sizeof(count));
    // The GPU counter is unsigned and spans every dispatch in the batch; clamp
    // so a huge count can never wrap to a negative "no errors".
    out.wrong_values = count > INT_MAX ? INT_MAX : static_cast<int>(count);
    d.ctx->Unmap(d.staging.Get(), 0);
    out.iterations = dispatches;
    finish();
    if (!tdr && out.elapsed_ms < kTargetBatchMs * 0.6 && dispatches_ < kMaxDispatches) dispatches_ *= 2;
    return out;
}

}
