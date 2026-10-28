// Executes the actual prepared source helpers against a frozen pre-change shader.
// GPU execution is deliberately separate from --compile-only validation.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
    constexpr UINT sourceWidth = 67, sourceHeight = 43, queryCount = 1024;
    using Color = std::array<float, 4>;
    void require(bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    }
    void checked(HRESULT hr, const char* message) {
        if (FAILED(hr)) {
            char text[256];
            sprintf_s(text, "%s: %08lx", message, static_cast<unsigned long>(hr));
            throw std::runtime_error(text);
        }
    }
    std::string read(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        require(bool(stream), "open frozen/candidate shader");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    struct Mapping {
        Color rotation{0, 0, 0, 1}, sourceTangents{-1, 1, 1, -1}, targetTangents{-1, 1, 1, -1};
        Color sourceRect{0, 0, float(sourceWidth), float(sourceHeight)};
        Color targetRect{0, 0, float(sourceWidth), float(sourceHeight)};
    };
    struct Parameters {
        UINT width{sourceWidth}, height{sourceHeight}, arraySize{2}, packedWidth{sourceWidth};
        UINT packedHeight{sourceHeight * 2}, eyeStride{sourceHeight}, flowWidth{17}, flowHeight{22};
        UINT blockSize{4}, view{}, gameMotion{}, padding{};
        Color gameRect{};
        std::array<float, 2> gameScale{}, gameJitter{};
        std::array<Mapping, 2> mappings{};
        UINT slice{}, flags{};
    };
    static_assert(sizeof(Parameters) == 62 * sizeof(UINT));
    constexpr const char* probe = R"(
Texture2D<float4> SamplerQueries : register(t7);
[numthreads(64, 1, 1)]
void SamplerRegression(uint3 tid : SV_DispatchThreadID) {
    float4 q = SamplerQueries.Load(int3(int2(tid.xy), 0));
#if PROBE_KIND == 0
    float4 color = bilinear_previous_source(q.xy, uint(q.z));
#elif PROBE_KIND == 1
    float4 color = bilinear_current_source(q.xy, uint(q.z));
#elif PROBE_KIND == 2
    CameraSample sampled = sample_previous_target(q.xy, uint(q.z), uint(q.w));
    float4 color = sampled.valid > 0.5 ? sampled.color : float4(-2, -2, -2, -2);
#else
    CameraSample sampled = sample_current_target(q.xy, uint(q.z), uint(q.w));
    float4 color = sampled.valid > 0.5 ? sampled.color : float4(-2, -2, -2, -2);
#endif
    PackedColor[tid.xy] = color;
}
)";
    ComPtr<ID3DBlob> compile(const std::string& source, UINT kind) {
        const std::string full = source + probe, number = std::to_string(kind);
        const D3D_SHADER_MACRO defines[]{{"PROBE_KIND", number.c_str()}, {nullptr, nullptr}};
        ComPtr<ID3DBlob> code, errors;
        const auto hr =
            D3DCompile(full.data(),
                       full.size(),
                       "prepared sampler",
                       defines,
                       nullptr,
                       "SamplerRegression",
                       "cs_5_0",
                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                       0,
                       &code,
                       &errors);
        if (FAILED(hr) && errors)
            std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        checked(hr, "compile production sampler probe");
        return code;
    }
    void validateOperations(ID3DBlob* code, bool hardware) {
        ComPtr<ID3DBlob> assembly;
        checked(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly),
                "probe disassembly");
        const std::string text(static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize());
        const auto count = [&](const char* opcode) {
            size_t result{}, position{};
            while ((position = text.find(opcode, position)) != std::string::npos) {
                ++result;
                ++position;
            }
            return result;
        };
        // Both variants load one query. Baseline must really contain four manual
        // source loads; candidate must really contain exactly one filtered sample.
        require(count("\nld") == (hardware ? 1u : 5u), "frozen/manual and candidate load operation counts");
        require(count("\nsample_l") == (hardware ? 1u : 0u), "candidate filtered sample operation count");
    }
    struct Fixture {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        std::array<std::array<ComPtr<ID3D11ComputeShader>, 4>, 2> shaders;
        ComPtr<ID3D11Texture2D> output, staging, queries;
        ComPtr<ID3D11UnorderedAccessView> target;
        ComPtr<ID3D11ShaderResourceView> queryView;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11SamplerState> sampler;
        Fixture(const std::array<std::array<ComPtr<ID3DBlob>, 4>, 2>& code) {
            ComPtr<IDXGIFactory6> factory;
            checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapterByGpuPreference(
                        index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                    break;
                DXGI_ADAPTER_DESC1 description{};
                checked(adapter->GetDesc1(&description), "adapter description");
                if (description.VendorId != 0x10de || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    continue;
                const D3D_FEATURE_LEVEL level[]{D3D_FEATURE_LEVEL_11_0};
                checked(D3D11CreateDevice(adapter.Get(),
                                          D3D_DRIVER_TYPE_UNKNOWN,
                                          nullptr,
                                          0,
                                          level,
                                          1,
                                          D3D11_SDK_VERSION,
                                          &device,
                                          nullptr,
                                          &context),
                        "NVIDIA fixture device");
                break;
            }
            require(device != nullptr, "NVIDIA hardware required, no WARP fallback");
            for (UINT version = 0; version < 2; ++version)
                for (UINT kind = 0; kind < 4; ++kind)
                    checked(device->CreateComputeShader(code[version][kind]->GetBufferPointer(),
                                                        code[version][kind]->GetBufferSize(),
                                                        nullptr,
                                                        &shaders[version][kind]),
                            "probe shader");
            D3D11_TEXTURE2D_DESC description{};
            description.Width = queryCount;
            description.Height = description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            description.SampleDesc.Count = 1;
            description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            checked(device->CreateTexture2D(&description, nullptr, &output), "probe target");
            checked(device->CreateUnorderedAccessView(output.Get(), nullptr, &target), "target view");
            description.BindFlags = 0;
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            checked(device->CreateTexture2D(&description, nullptr, &staging), "readback");
            description.Usage = D3D11_USAGE_DEFAULT;
            description.CPUAccessFlags = 0;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            checked(device->CreateTexture2D(&description, nullptr, &queries), "queries");
            checked(device->CreateShaderResourceView(queries.Get(), nullptr, &queryView), "query view");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = (sizeof(Parameters) + 15) & ~15u;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            checked(device->CreateBuffer(&buffer, nullptr, &constants), "constants");
            D3D11_SAMPLER_DESC filter{};
            filter.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            filter.AddressU = filter.AddressV = filter.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            filter.MaxAnisotropy = 1;
            filter.ComparisonFunc = D3D11_COMPARISON_NEVER;
            checked(device->CreateSamplerState(&filter, &sampler), "linear clamp");
        }
        void inputs(UINT pattern, DXGI_FORMAT format) {
            const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (UINT endpoint = 0; endpoint < 2; ++endpoint) {
                std::vector<unsigned char> pixels(sourceWidth * sourceHeight * 2 * 4);
                for (UINT slice = 0; slice < 2; ++slice)
                    for (UINT y = 0; y < sourceHeight; ++y)
                        for (UINT x = 0; x < sourceWidth; ++x)
                            for (UINT channel = 0; channel < 4; ++channel) {
                                UINT value =
                                    pattern == 0   ? 0
                                    : pattern == 1 ? 255
                                    : pattern == 2 ? (x * 3 + y * 5 + slice * 41 + endpoint * 17 + channel * 29) % 256
                                    : pattern == 3 ? ((x + y + channel + slice + endpoint) % 2) * 255
                                    : pattern == 4 ? ((x == 14 || y == 11) ? 255 : 0)
                                                   : ((x < 7 || x > 56 || y < 5 || y > 34) ? 255 : (slice ? 37 : 13));
                                pixels[(((slice * sourceHeight + y) * sourceWidth + x) * 4) + channel] =
                                    static_cast<unsigned char>(value);
                            }
                D3D11_TEXTURE2D_DESC texture{};
                texture.Width = sourceWidth;
                texture.Height = sourceHeight;
                texture.ArraySize = 2;
                texture.MipLevels = 1;
                texture.Format = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
                texture.SampleDesc.Count = 1;
                texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data[2]{};
                for (UINT slice = 0; slice < 2; ++slice) {
                    data[slice].pSysMem = pixels.data() + slice * sourceWidth * sourceHeight * 4;
                    data[slice].SysMemPitch = sourceWidth * 4;
                }
                ComPtr<ID3D11Texture2D> image;
                checked(device->CreateTexture2D(&texture, data, &image), "source texture");
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels = 1;
                view.Texture2DArray.ArraySize = 2;
                ComPtr<ID3D11ShaderResourceView> resource;
                checked(device->CreateShaderResourceView(image.Get(), &view, &resource), "source view");
                ID3D11ShaderResourceView* raw = resource.Get();
                context->CSSetShaderResources(endpoint, 1, &raw);
            }
        }
        std::vector<Color> run(UINT version, UINT kind, Parameters parameters, bool reversed) {
            std::array<Color, queryCount> locations;
            constexpr float fractions[]{0, 0.001f, 0.1273f, 0.498f, 0.5019f, 0.8731f, 0.999f};
            for (UINT index = 0; index < queryCount; ++index) {
                const UINT view = (index / 2) % 2, slice = reversed ? 1 - view : view;
                float x = float(int(index % 73) - 3) + fractions[index % 7];
                float y = float(int((index * 11) % 49) - 3) + fractions[(index / 7) % 7];
                // Literal crop-edge / half-pixel probes exercise source-center clamps.
                if (index < 16) {
                    constexpr float edgeX[]{6.5001f, 6.999f, 7, 7.0001f, 56, 56.0001f, 56.4999f, 57};
                    x = edgeX[index % 8];
                    y = index < 8 ? 5.0f : 34.0f;
                }
                locations[index] = {x, y, float(slice), float(view)};
            }
            context->UpdateSubresource(queries.Get(), 0, nullptr, locations.data(), queryCount * sizeof(Color), 0);
            std::array<unsigned char, (sizeof(Parameters) + 15) & ~15u> padded{};
            std::memcpy(padded.data(), &parameters, sizeof(parameters));
            context->UpdateSubresource(constants.Get(), 0, nullptr, padded.data(), 0, 0);
            ID3D11Buffer* buffer = constants.Get();
            context->CSSetConstantBuffers(0, 1, &buffer);
            ID3D11SamplerState* filter = sampler.Get();
            context->CSSetSamplers(0, 1, &filter);
            ID3D11ShaderResourceView* query = queryView.Get();
            context->CSSetShaderResources(7, 1, &query);
            ID3D11UnorderedAccessView* uav = target.Get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(shaders[version][kind].Get(), nullptr, 0);
            context->Dispatch(queryCount / 64, 1, 1);
            uav = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CopyResource(staging.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "probe readback");
            std::vector<Color> result(queryCount);
            std::memcpy(result.data(), mapped.pData, queryCount * sizeof(Color));
            context->Unmap(staging.Get(), 0);
            return result;
        }
        double time(UINT version) {
            ComPtr<ID3D11Query> disjoint, start, finish;
            D3D11_QUERY_DESC description{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            checked(device->CreateQuery(&description, &disjoint), "timing disjoint query");
            description.Query = D3D11_QUERY_TIMESTAMP;
            checked(device->CreateQuery(&description, &start), "timing start query");
            checked(device->CreateQuery(&description, &finish), "timing finish query");
            ID3D11UnorderedAccessView* uav = target.Get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(shaders[version][0].Get(), nullptr, 0);
            context->Begin(disjoint.Get());
            context->End(start.Get());
            for (UINT repeat = 0; repeat < 1024; ++repeat)
                context->Dispatch(queryCount / 64, 1, 1);
            context->End(finish.Get());
            context->End(disjoint.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            const auto retrieve = [&](ID3D11Query* query, void* data, UINT bytes) {
                HRESULT status{};
                while ((status = context->GetData(query, data, bytes, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
                    require(std::chrono::steady_clock::now() < deadline, "GPU timestamp timeout");
                    std::this_thread::yield();
                }
                checked(status, "timestamp data");
            };
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            UINT64 first{}, last{};
            retrieve(disjoint.Get(), &frequency, sizeof(frequency));
            retrieve(start.Get(), &first, sizeof(first));
            retrieve(finish.Get(), &last, sizeof(last));
            require(!frequency.Disjoint && frequency.Frequency != 0 && last > first, "valid paired GPU timestamps");
            uav = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            return double(last - first) * 1000.0 / double(frequency.Frequency);
        }
    };
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 3 && argc <= 4,
                "usage: fg_sampler_shader_regression baseline.hlsl candidate.hlsl [--compile-only]");
        const std::array<std::string, 2> source{read(argv[1]), read(argv[2])};
        std::array<std::array<ComPtr<ID3DBlob>, 4>, 2> code;
        for (UINT version = 0; version < 2; ++version)
            for (UINT kind = 0; kind < 4; ++kind) {
                code[version][kind] = compile(source[version], kind);
                if (kind < 2)
                    validateOperations(code[version][kind].Get(), version == 1);
            }
        if (argc == 4) {
            require(std::wstring(argv[3]) == L"--compile-only", "unknown option");
            std::puts("PASS: compiled eight real baseline/candidate helper and endpoint probes; no GPU execution");
            return 0;
        }
        Fixture fixture(code);
        float maximumError{};
        UINT comparisons{};
        // D3D11.3 spec 7.18.16.1: >=8 fractional coordinate bits; 3.2.4.1:
        // <=0.6 fixed-point ULP. For [0,1] bilinear inputs, error is bounded
        // by |dx|+|dy|. Allow another 1/255 for normalized filtering arithmetic
        // and 0.0001 for floating-point normalization / sRGB decoding.
        // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
        constexpr float tolerance = 2 * .6f / 256 + 1.0f / 255 + .0001f;
        for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                  DXGI_FORMAT_B8G8R8A8_UNORM,
                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
            for (UINT pattern = 0; pattern < 6; ++pattern) {
                fixture.inputs(pattern, format);
                for (bool crop : {false, true})
                    for (bool reversed : {false, true}) {
                        Parameters parameters;
                        if (crop)
                            for (auto& mapping : parameters.mappings) {
                                mapping.sourceRect = {7, 5, 50, 30};
                                mapping.targetRect = {7, 5, 50, 30};
                                // Expands valid source coverage into a half-pixel strip.
                                mapping.targetTangents = {-1.016f, 1.016f, 1.02f, -1.02f};
                            }
                        if (crop) {
                            auto& otherEye = parameters.mappings[1];
                            otherEye.sourceRect = {9, 7, 45, 25};
                            otherEye.targetRect = {3, 2, 50, 30};
                            otherEye.rotation = {0, std::sin(.013f), 0, std::cos(.013f)};
                        }
                        for (UINT kind = 0; kind < 4; ++kind) {
                            const auto manual = fixture.run(0, kind, parameters, reversed);
                            const auto hardware = fixture.run(1, kind, parameters, reversed);
                            for (UINT query = 0; query < queryCount; ++query)
                                for (UINT channel = 0; channel < 4; ++channel) {
                                    const float error = std::abs(manual[query][channel] - hardware[query][channel]);
                                    maximumError = std::max(maximumError, error);
                                    if (!std::isfinite(hardware[query][channel]) || error > tolerance) {
                                        std::fprintf(stderr,
                                                     "format=%u pattern=%u crop=%u reverse=%u kind=%u query=%u "
                                                     "channel=%u manual=%.9g hardware=%.9g error=%.9g limit=%.9g\n",
                                                     format,
                                                     pattern,
                                                     crop,
                                                     reversed,
                                                     kind,
                                                     query,
                                                     channel,
                                                     manual[query][channel],
                                                     hardware[query][channel],
                                                     error,
                                                     tolerance);
                                        throw std::runtime_error("hardware sampling exceeded fractional-filter bound");
                                    }
                                    if (pattern < 2 && kind < 2)
                                        require(std::abs(hardware[query][channel] - float(pattern)) < 0.00001f,
                                                "black/saturated sampling must preserve literal endpoints");
                                    ++comparisons;
                                }
                        }
                    }
            }
        std::printf("PASS: %u sampler channel comparisons; maximum_error=%.9g tolerance=%.9g\n",
                    comparisons,
                    maximumError,
                    tolerance);
        // Alternate order to reduce a first-run/clock bias; report a probe-level
        // cost, not a frame-generation or headset FPS claim.
        fixture.inputs(3, DXGI_FORMAT_R8G8B8A8_UNORM);
        (void)fixture.time(0);
        (void)fixture.time(1);
        std::array<double, 7> manualTimes{}, hardwareTimes{};
        for (UINT pair = 0; pair < manualTimes.size(); ++pair) {
            if (pair % 2) {
                hardwareTimes[pair] = fixture.time(1);
                manualTimes[pair] = fixture.time(0);
            } else {
                manualTimes[pair] = fixture.time(0);
                hardwareTimes[pair] = fixture.time(1);
            }
        }
        std::sort(manualTimes.begin(), manualTimes.end());
        std::sort(hardwareTimes.begin(), hardwareTimes.end());
        std::printf(
            "paired_sampler_probe_ms manual=%.6f hardware=%.6f ratio=%.6f (1024x1024 samples; median 7 pairs)\n",
            manualTimes[3],
            hardwareTimes[3],
            hardwareTimes[3] / manualTimes[3]);
        require(hardwareTimes[3] <= manualTimes[3] * 1.10 + .01,
                "hardware sampler probe cost regressed beyond 10% plus 0.01 ms noise allowance");
        std::puts("PASS: paired sampler timing gate");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
