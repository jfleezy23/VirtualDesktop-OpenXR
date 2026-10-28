// Independent scalar contract + real production Pipeline, with synthetic edits.
// This proves shader/lifetime behavior, NOT proprietary NR inference or quality.
#include "../virtualdesktop-openxr/NrSpatialPipeline.h"
#if defined(NR_SPATIAL_TEST_OBSERVER)
#include "nr_spatial_test_observer.h"
#endif
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace virtualdesktop_openxr::nr_spatial;
using Microsoft::WRL::ComPtr;
using Bytes = std::vector<uint8_t>;
std::atomic<size_t> allocationCount{};
void* operator new(size_t bytes) {
    if (void* result = std::malloc(bytes ? bytes : 1)) {
        ++allocationCount;
        return result;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) {
    return ::operator new(bytes);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void hr(HRESULT value, const char* why) {
    if (FAILED(value)) {
        char text[256];
        sprintf_s(text, "%s: %08lx", why, value);
        throw std::runtime_error(text);
    }
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> diagnostics;
    uint64_t serial{};
    explicit Gpu(bool hardware) {
        ComPtr<ID3D12Debug> debug;
        hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer required");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        if (hardware)
            hr(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
               "hardware");
        else
            hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
        DXGI_ADAPTER_DESC1 desc{};
        hr(adapter->GetDesc1(&desc), "adapter");
        std::wprintf(L"Spatial synthetic adapter: %ls\n", desc.Description);
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        hr(device.As(&diagnostics), "diagnostics");
        D3D12_COMMAND_QUEUE_DESC q{};
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    }
    struct Commands {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };
    Commands commands() {
        Commands c;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator)), "allocator");
        hr(device->CreateCommandList(
               0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list)),
           "list");
        return c;
    }
    uint64_t submit(Commands& c) {
        hr(c.list->Close(), "close");
        ID3D12CommandList* lists[]{c.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const auto value = ++serial;
        hr(queue->Signal(fence.Get(), value), "signal");
        return value;
    }
    void wait(uint64_t value) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(event != nullptr, "event");
        const HRESULT result = fence->SetEventOnCompletion(value, event);
        const DWORD status = SUCCEEDED(result) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
        CloseHandle(event);
        hr(result, "completion event");
        check(status == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX, "GPU timeout/removal");
    }
    ComPtr<ID3D12Resource> texture(
        UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, UINT16 slices = 1) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = slices;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(
               &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)),
           "texture");
        return r;
    }
    static void
    barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    }
    Bytes transfer(ID3D12Resource* image, const Bytes* upload = nullptr, UINT slice = 0) {
        auto d = image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes{};
        device->GetCopyableFootprints(&d, slice, 1, 0, &fp, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        hr(device->CreateCommittedResource(&heap,
                                           D3D12_HEAP_FLAG_NONE,
                                           &bd,
                                           upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr,
                                           IID_PPV_ARGS(&buffer)),
           "staging");
        void* mapped{};
        const D3D12_RANGE empty{};
        if (upload) {
            hr(buffer->Map(0, &empty, &mapped), "map upload");
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(static_cast<uint8_t*>(mapped) + y * fp.Footprint.RowPitch,
                            upload->data() + y * d.Width * 4,
                            size_t(d.Width) * 4);
            buffer->Unmap(0, nullptr);
        }
        auto c = commands();
        auto state = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(c.list.Get(), image, D3D12_RESOURCE_STATE_COMMON, state);
        D3D12_TEXTURE_COPY_LOCATION textureLoc{};
        textureLoc.pResource = image;
        textureLoc.SubresourceIndex = slice;
        D3D12_TEXTURE_COPY_LOCATION bufferLoc{};
        bufferLoc.pResource = buffer.Get();
        bufferLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        bufferLoc.PlacedFootprint = fp;
        c.list->CopyTextureRegion(
            upload ? &textureLoc : &bufferLoc, 0, 0, 0, upload ? &bufferLoc : &textureLoc, nullptr);
        barrier(c.list.Get(), image, state, D3D12_RESOURCE_STATE_COMMON);
        wait(submit(c));
        Bytes result(size_t(d.Width) * d.Height * 4);
        if (!upload) {
            hr(buffer->Map(0, nullptr, &mapped), "map readback");
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(result.data() + y * d.Width * 4,
                            static_cast<uint8_t*>(mapped) + y * fp.Footprint.RowPitch,
                            size_t(d.Width) * 4);
            buffer->Unmap(0, &empty);
        }
        return result;
    }
    void noErrors() {
        for (UINT64 n = 0; n < diagnostics->GetNumStoredMessages(); ++n) {
            SIZE_T bytes{};
            hr(diagnostics->GetMessage(n, nullptr, &bytes), "message size");
            Bytes storage(bytes);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            hr(diagnostics->GetMessage(n, msg, &bytes), "message");
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(msg->pDescription);
        }
    }
};

// Double precision oracle integrates piecewise-constant source cells using
// rectangle intersections. It never calls production math/policy helpers.
double decode(double v, bool srgb) {
    return !srgb ? v : v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
}
double encode(double v, bool srgb) {
    return !srgb ? v : v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055;
}
uint8_t quantize(double v) {
    return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255));
}
size_t offset(UINT x, UINT y, UINT w, UINT c, bool bgra = false) {
    return (size_t(y) * w + x) * 4 + (bgra && c < 3 ? 2 - c : c);
}
Bytes areaOracle(const Bytes& native, UINT rowWidth, Rect rect, Extent work, bool srgb, bool bgra) {
    Bytes p(size_t(work.width) * work.height * 4);
    for (UINT y = 0; y < work.height; ++y)
        for (UINT x = 0; x < work.width; ++x) {
            const double x0 = double(x) * rect.width / work.width, x1 = double(x + 1) * rect.width / work.width;
            const double y0 = double(y) * rect.height / work.height, y1 = double(y + 1) * rect.height / work.height;
            for (UINT c = 0; c < 4; ++c) {
                double sum = 0;
                for (UINT j = UINT(y0); j < UINT(std::ceil(y1)); ++j)
                    for (UINT i = UINT(x0); i < UINT(std::ceil(x1)); ++i) {
                        const double a = std::max(0.0, std::min(x1, double(i + 1)) - std::max(x0, double(i))) *
                                         std::max(0.0, std::min(y1, double(j + 1)) - std::max(y0, double(j)));
                        const double v = native[offset(rect.x + i, rect.y + j, rowWidth, c, bgra)] / 255.0;
                        sum += a * (c == 3 ? v : decode(v, srgb));
                    }
                const double mean = sum / ((x1 - x0) * (y1 - y0));
                p[offset(x, y, work.width, c)] = quantize(c == 3 ? mean : encode(mean, srgb));
            }
        }
    return p;
}
Bytes resolveOracle(const Bytes& native,
                    UINT rowWidth,
                    Rect rect,
                    Extent work,
                    const Bytes& p,
                    const Bytes& a,
                    bool srgb,
                    bool sourceBgra,
                    bool outputBgra,
                    Rect processing = {},
                    UINT transition = 0) {
    if (!processing.width && !processing.height)
        processing = {0, 0, rect.width, rect.height};
    Bytes output(size_t(rect.width) * rect.height * 4);
    for (UINT y = 0; y < rect.height; ++y)
        for (UINT x = 0; x < rect.width; ++x) {
            const bool inside = x >= processing.x && y >= processing.y && x < processing.x + processing.width &&
                                y < processing.y + processing.height;
            if (!inside) {
                for (UINT c = 0; c < 4; ++c)
                    output[offset(x, y, rect.width, c, outputBgra)] =
                        native[offset(rect.x + x, rect.y + y, rowWidth, c, sourceBgra)];
                continue;
            }
            const double localX = x - processing.x + .5, localY = y - processing.y + .5;
            const double qx = localX * work.width / processing.width - .5,
                         qy = localY * work.height / processing.height - .5;
            const double edge = std::min({localX, localY, processing.width - localX, processing.height - localY});
            const double t = transition ? std::clamp(edge / transition, 0.0, 1.0) : 1.0;
            const double taper = t * t * (3 - 2 * t);
            const int ix = int(std::floor(qx)), iy = int(std::floor(qy));
            const double fx = qx - ix, fy = qy - iy;
            double n[3], edit[3]{}, total = 0;
            bool zero = true;
            for (UINT c = 0; c < 3; ++c)
                n[c] = decode(native[offset(rect.x + x, rect.y + y, rowWidth, c, sourceBgra)] / 255.0, srgb);
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const UINT sx = UINT(std::clamp(ix + i, 0, int(work.width) - 1)),
                               sy = UINT(std::clamp(iy + j, 0, int(work.height) - 1));
                    double delta[3], distance = 0, guideLevel = 0;
                    for (UINT c = 0; c < 3; ++c) {
                        const auto index = offset(sx, sy, work.width, c);
                        const double guide = decode(p[index] / 255.0, srgb);
                        guideLevel = std::max(guideLevel, guide);
                        delta[c] = decode(a[index] / 255.0, srgb) - guide;
                        zero = zero && a[index] == p[index];
                        distance += (guide - n[c]) * (guide - n[c]) / 3;
                    }
                    // Independent double-precision policy; original guide normalization is fixed.
                    const double level = std::min(std::max({n[0], n[1], n[2]}), guideLevel);
                    if (level < 1.0 / 128) {
                        const auto toe = [level](double end) {
                            const double z = std::clamp(level / end, 0.0, 1.0);
                            return z * z * (3 - 2 * z);
                        };
                        const double least = std::min({delta[0], delta[1], delta[2]});
                        const double greatest = std::max({delta[0], delta[1], delta[2]});
                        const double gray = least > 0 ? least : greatest < 0 ? greatest : 0;
                        for (auto& d : delta)
                            d = toe(1.0 / 1024) * gray + toe(1.0 / 128) * (d - gray);
                    }
                    const double weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy) / (1 + 1024 * distance);
                    for (UINT c = 0; c < 3; ++c)
                        edit[c] += weight * delta[c];
                    total += weight;
                }
            for (UINT c = 0; c < 4; ++c) {
                const auto raw = native[offset(rect.x + x, rect.y + y, rowWidth, c, sourceBgra)];
                output[offset(x, y, rect.width, c, outputBgra)] =
                    c == 3 || zero ? raw : quantize(encode(n[c] + taper * edit[c] / total, srgb));
            }
        }
    return output;
}
void compare(const Bytes& actual, const Bytes& expected, int tolerance, const char* label) {
    check(actual.size() == expected.size(), "comparison shape");
    int largest = 0;
    size_t failures = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        const int error = std::abs(int(actual[i]) - int(expected[i]));
        largest = std::max(largest, error);
        failures += error > tolerance;
    }
    if (failures) {
        std::fprintf(stderr, "%s: %zu wrong channels; max error %d\n", label, failures, largest);
        throw std::runtime_error(label);
    }
}

void formatValidation(Gpu& gpu) {
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                DXGI_FORMAT_UNKNOWN,
                                DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                DXGI_FORMAT_B8G8R8A8_TYPELESS,
                                DXGI_FORMAT_R16G16B16A16_FLOAT};
    auto allocation = [](DXGI_FORMAT format) {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                       format == DXGI_FORMAT_B8G8R8A8_TYPELESS
                   ? DXGI_FORMAT_B8G8R8A8_TYPELESS
                   : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    };
    for (UINT left = 0; left < std::size(formats); ++left)
        for (UINT right = 0; right < std::size(formats); ++right)
            for (UINT output = 0; output < std::size(formats); ++output) {
                auto a = gpu.texture(132, 129, allocation(formats[left]));
                auto b = gpu.texture(132, 129, allocation(formats[right]));
                auto target =
                    gpu.texture(264, 129, allocation(formats[output]), D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
                FrameInput frame{};
                frame.output = target.Get();
                frame.outputFormat = formats[output];
                frame.outputEyeStride = 132;
                frame.processingPercent = 80;
                frame.eyes[0] = {a.Get(), formats[left], {0, 0, 132, 129}};
                frame.eyes[1] = {b.Get(), formats[right], {0, 0, 132, 129}};
                // Literal supported pair matrix, independent of production policy.
                const bool allowed = left == right && left < 4 && output < 4 &&
                                     (left == output || (left == 2 && output == 0) || (left == 3 && output == 1));
                Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
                check(SUCCEEDED(pipeline.Prepare(frame)) == allowed,
                      "typed view matrix: same encoding, BGRA to RGBA only, identical stereo source formats");
                pipeline.DiscardRecording();
            }
    for (bool sourceMismatch : {false, true}) {
        auto source =
            gpu.texture(132, 129, sourceMismatch ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS);
        auto output = gpu.texture(264,
                                  129,
                                  sourceMismatch ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        FrameInput frame{};
        frame.output = output.Get();
        frame.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.outputEyeStride = 132;
        frame.processingPercent = 80;
        for (auto& eye : frame.eyes)
            eye = {source.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, {0, 0, 132, 129}};
        Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
        check(FAILED(pipeline.Prepare(frame)),
              "canonical conversion still rejects source/output view backing family mismatch");
    }
    gpu.noErrors();
    std::puts("PASS all 512 source-eye/output typed view qualification combinations");
}

void runFormat(Gpu& gpu,
               DXGI_FORMAT format,
               DXGI_FORMAT outputFormat,
               UINT percent,
               bool arraySource = false,
               UINT coverage = 100) {
    const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto allocation = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool outputBgra =
        outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM || outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto outputAllocation = outputBgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr UINT inputW = 311, inputH = 161, outputW = 281, outputH = 137, stride = 144;
    const std::array<Rect, 2> rects{{{3, 5, 137, 133}, {161, 13, 129, 127}}};
    // Independently hand-calculated 50%/80%/90% ceil-and-align grids.
    const std::array<Extent, 2> work = coverage == 95 ? std::array<Extent, 2>{{{108, 104}, {100, 96}}}
                                       : coverage == 90 && percent == 80 ? std::array<Extent, 2>{{{100, 96}, {96, 96}}}
                                       : coverage == 90 ? std::array<Extent, 2>{{{108, 104}, {104, 100}}}
                                       : percent == 50  ? std::array<Extent, 2>{{{72, 68}, {68, 64}}}
                                       : percent == 80  ? std::array<Extent, 2>{{{112, 108}, {104, 104}}}
                                                        : std::array<Extent, 2>{{{124, 120}, {120, 116}}};
    const std::array<Rect, 2> processing = coverage == 95   ? std::array<Rect, 2>{{{2, 2, 132, 128}, {2, 3, 124, 120}}}
                                           : coverage == 90 ? std::array<Rect, 2>{{{6, 6, 124, 120}, {4, 5, 120, 116}}}
                                                            : std::array<Rect, 2>{{{0, 0, 137, 133}, {0, 0, 129, 127}}};
    const UINT transition = coverage == 100 ? 0 : coverage == 95 ? 2 : 3;
    const UINT16 slices = arraySource ? 2 : 1;
    auto source = gpu.texture(inputW, inputH, allocation, D3D12_RESOURCE_FLAG_NONE, slices);
    auto target = gpu.texture(outputW, outputH, outputAllocation, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = gpu.texture(183, 145, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, slices);
    FrameInput frame{};
    frame.output = target.Get();
    frame.outputFormat = outputFormat;
    frame.outputEyeStride = stride;
    frame.processingPercent = percent;
    const std::array<Rect, 2> depths{{{7, 11, 75, 99}, {91, 3, 83, 131}}};
    for (UINT e = 0; e < 2; ++e)
        frame.eyes[e] = {source.Get(), format, rects[e], depth.Get(), DXGI_FORMAT_R32_FLOAT, depths[e]};
#if !defined(NR_SPATIAL_CROP_BASELINE)
    if (coverage != 100)
        for (UINT e = 0; e < 2; ++e) {
            frame.eyes[e].processingRect = processing[e];
            frame.eyes[e].transitionWidth = transition;
        }
#endif
    Bytes native(inputW * inputH * 4, 255), depthData(183 * 145 * 4);
    for (UINT y = 0; y < 145; ++y)
        for (UINT x = 0; x < 183; ++x) {
            const float v = float(x + y * 183) / 30000;
            std::memcpy(depthData.data() + (y * 183 + x) * 4, &v, 4);
        }
    gpu.transfer(depth.Get(), &depthData);
    if (arraySource) {
        const Bytes poisonDepth(depthData.size(), 0);
        gpu.transfer(depth.Get(), &poisonDepth, 1);
    }
    Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
    // Reject invalid/depth-missing-view/native requests before recording.
    FrameInput invalid = frame;
    invalid.processingPercent = 100;
    check(FAILED(pipeline.Prepare(invalid)), "native100 rejected by optional module");
    invalid = frame;
    invalid.eyes[0].depthFormat = DXGI_FORMAT_UNKNOWN;
    check(FAILED(pipeline.Prepare(invalid)), "never silently drop invalid depth");
    invalid = frame;
    invalid.eyes[1].colorRect.x = inputW;
    check(FAILED(pipeline.Prepare(invalid)), "reject outside eye rect");
#if !defined(NR_SPATIAL_CROP_BASELINE)
    invalid = frame;
    invalid.eyes[0].processingRect = {136, 0, 2, 133};
    check(FAILED(pipeline.Prepare(invalid)), "reject local crop beyond full valid eye");
    invalid = frame;
    invalid.eyes[0].processingRect = {1, 0, 0, 0};
    check(FAILED(pipeline.Prepare(invalid)), "only entirely empty legacy crop defaults to full eye");
    invalid = frame;
    invalid.eyes[0].transitionWidth = 100;
    check(FAILED(pipeline.Prepare(invalid)), "reject feather wider than crop quarter");
#endif
    for (UINT bank = 0; bank < 3; ++bank) {
        if (bank) {
            const auto bankFormat = bank == 2 ? format : allocation; // typed as well as typeless imports
            source = gpu.texture(inputW, inputH, bankFormat, D3D12_RESOURCE_FLAG_NONE, slices);
            const auto targetFormat = bank == 2 ? outputFormat : outputAllocation;
            target = gpu.texture(outputW, outputH, targetFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            frame.output = target.Get();
            for (auto& eye : frame.eyes)
                eye.color = source.Get();
        }
        for (UINT e = 0; e < 2; ++e)
            for (UINT y = 0; y < rects[e].height; ++y)
                for (UINT x = 0; x < rects[e].width; ++x)
                    for (UINT c = 0; c < 4; ++c) {
                        // Dark codes, lines, checkerboard, opposite eye colors, nonopaque alpha.
                        const UINT v = c == 3 ? (x + 3 * y) % 256
                                              : (x % 9 == 0 ? 240 : (x * (c + 1) + y * 7 + e * 73 + bank * 31) % 180);
                        native[offset(rects[e].x + x, rects[e].y + y, inputW, c, bgra)] = uint8_t(v);
                    }
        gpu.transfer(source.Get(), &native);
        if (arraySource) {
            const Bytes poisonColor(native.size(), 0);
            gpu.transfer(source.Get(), &poisonColor, 1);
        }
        if (bank)
            check(pipeline.Matches(frame), "same-size new native bank matches generation");
        for (UINT mode = 0; mode < 3; ++mode) {
            const Bytes guard(size_t(outputW) * outputH * 4, 37);
            gpu.transfer(target.Get(), &guard);
            hr(pipeline.Prepare(frame), "prepare");
#if !defined(NR_SPATIAL_CROP_BASELINE)
            auto changed = frame;
            changed.eyes[0].transitionWidth = transition + 1;
            check(!pipeline.Matches(changed), "transition changes must invalidate generation/reset matching");
            changed = frame;
            changed.eyes[0].processingRect = processing[0];
            changed.eyes[0].processingRect.width -= 4;
            check(!pipeline.Matches(changed), "crop changes must invalidate even if aligned work extent is unchanged");
#endif
            check(FAILED(pipeline.Prepare(frame)), "unsubmitted/uncertain batch cannot be reused");
            pipeline.MarkSubmitted(0);
            pipeline.MarkSubmitted(UINT64_MAX);
            check(FAILED(pipeline.Prepare(frame)), "unknown completion tokens must keep prepared ownership");
            const auto left = pipeline.GetWorkingEye(0), right = pipeline.GetWorkingEye(1);
            check(left.extent.width == work[0].width && right.extent.width == work[1].width &&
                      left.proxy != right.proxy,
                  "independent eye geometry");
            auto c = gpu.commands();
            const auto beforeInputs = allocationCount.load();
            pipeline.RecordInputs(c.list.Get());
            check(allocationCount.load() == beforeInputs, "RecordInputs must allocate nothing");
            gpu.wait(gpu.submit(c));
            std::array<Bytes, 2> expected;
            for (UINT e = 0; e < 2; ++e) {
                auto eye = pipeline.GetWorkingEye(e);
                const Bytes p = gpu.transfer(eye.proxy);
                const Rect sampled{rects[e].x + processing[e].x,
                                   rects[e].y + processing[e].y,
                                   processing[e].width,
                                   processing[e].height};
                compare(p,
                        areaOracle(native, inputW, sampled, work[e], srgb, bgra),
                        1,
                        "crop area-filter/encoding/eye bounds");
                const Bytes sampledDepth = gpu.transfer(eye.depth);
                for (UINT y = 0; y < work[e].height; ++y)
                    for (UINT x = 0; x < work[e].width; ++x) {
                        const UINT dx =
                            depths[e].x +
                            (coverage == 100
                                 ? UINT((double(x) + .5) * depths[e].width / work[e].width)
                                 : UINT((processing[e].x + (double(x) + .5) * processing[e].width / work[e].width) *
                                        depths[e].width / rects[e].width));
                        const UINT dy =
                            depths[e].y +
                            (coverage == 100
                                 ? UINT((double(y) + .5) * depths[e].height / work[e].height)
                                 : UINT((processing[e].y + (double(y) + .5) * processing[e].height / work[e].height) *
                                        depths[e].height / rects[e].height));
                        check(std::memcmp(sampledDepth.data() + (y * work[e].width + x) * 4,
                                          depthData.data() + (dy * 183 + dx) * 4,
                                          4) == 0,
                              "point depth valid rect/inversion retention");
                    }
                Bytes answer = p;
                for (size_t i = 0; i < answer.size(); i += 4)
                    for (size_t channel = 0; channel < 3; ++channel) {
                        const int edit = mode == 0   ? 0
                                         : mode == 1 ? (channel == 1 ? -13 : 17)
                                                     : int((i / 4 + channel) % 23) - 11;
                        answer[i + channel] = uint8_t(std::clamp(int(p[i + channel]) + edit, 0, 255));
                    }
                gpu.transfer(eye.answer, &answer);
                expected[e] = resolveOracle(
                    native, inputW, rects[e], work[e], p, answer, srgb, bgra, outputBgra, processing[e], transition);
            }
            // Set deliberately unrelated state so resolve must bind everything.
            c = gpu.commands();
            c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            const auto beforeResolve = allocationCount.load();
            pipeline.RecordResolve(c.list.Get());
            check(allocationCount.load() == beforeResolve, "RecordResolve must allocate nothing");
            const auto value = gpu.submit(c);
            const auto beforeMark = allocationCount.load();
            pipeline.MarkSubmitted(value);
            check(allocationCount.load() == beforeMark, "MarkSubmitted must allocate nothing");
            gpu.wait(value);
            const Bytes actual = gpu.transfer(target.Get());
            for (UINT y = 0; y < outputH; ++y)
                for (UINT x = 0; x < outputW; ++x) {
                    const bool inLeft = x < rects[0].width && y < rects[0].height;
                    const bool inRight = x >= stride && x < stride + rects[1].width && y < rects[1].height;
                    if (!inLeft && !inRight)
                        for (UINT channel = 0; channel < 4; ++channel)
                            check(actual[offset(x, y, outputW, channel)] == 37, "packed output guard pixels intact");
                }
            for (UINT e = 0; e < 2; ++e) {
                Bytes crop(size_t(rects[e].width) * rects[e].height * 4);
                for (UINT y = 0; y < rects[e].height; ++y)
                    std::memcpy(crop.data() + size_t(y) * rects[e].width * 4,
                                actual.data() + (size_t(y) * outputW + e * stride) * 4,
                                size_t(rects[e].width) * 4);
                compare(crop, expected[e], mode ? 1 : 0, mode ? "guided signed residual" : "exact zero-edit raw/alpha");
                for (size_t i = 3; i < crop.size(); i += 4)
                    check(crop[i] == expected[e][i], "native alpha exact under edit");
                for (UINT y = 0; y < rects[e].height; ++y)
                    for (UINT x = 0; x < rects[e].width; ++x)
                        if (x < processing[e].x || y < processing[e].y || x >= processing[e].x + processing[e].width ||
                            y >= processing[e].y + processing[e].height)
                            for (UINT channel = 0; channel < 4; ++channel) {
                                const auto i = offset(x, y, rects[e].width, channel, outputBgra);
                                check(crop[i] == expected[e][i], "outside crop original color and alpha must be exact");
                            }
            }
        }
    }
    hr(pipeline.Prepare(frame), "prepare to discard");
    pipeline.DiscardRecording();
    hr(pipeline.Prepare(frame), "discard permits retry");
    pipeline.DiscardRecording();
    pipeline.Clear();
    gpu.noErrors();
    std::printf(
        "PASS source format %u output format %u percent %u slices %u coverage %u: odd/offset eyes, poison borders, "
        "zero/signed edits, depth, bank "
        "rotation\n",
        unsigned(format),
        unsigned(outputFormat),
        percent,
        slices,
        coverage);
}

// A real GPU queue is held behind a CPU-signalled fence. Three recordings are
// prepared while their completion fence cannot advance. Any early descriptor
// rewrite, input release, or per-batch scratch allocation breaks this contract.
void delayedBatches(Gpu& gpu, bool withDepth, bool canonical = false, bool srgb = false, bool partial = false) {
    const auto sourceFormat =
        canonical ? (srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM) : DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto outputFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    auto a = gpu.texture(264, 129, sourceFormat);
    auto b = gpu.texture(264, 129, sourceFormat);
    auto outA = gpu.texture(264, 129, outputFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto outB = gpu.texture(264, 129, outputFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depthA = withDepth ? gpu.texture(264, 129, DXGI_FORMAT_R32_FLOAT) : ComPtr<ID3D12Resource>{};
    auto depthB = withDepth ? gpu.texture(264, 129, DXGI_FORMAT_R32_FLOAT) : ComPtr<ID3D12Resource>{};
    Bytes pa(264 * 129 * 4), pb(pa.size());
    for (size_t i = 0; i < pa.size(); ++i) {
        pa[i] = uint8_t((i * 13) % 256);
        pb[i] = uint8_t((i * 7 + 91) % 256);
    }
    gpu.transfer(a.Get(), &pa);
    gpu.transfer(b.Get(), &pb);
    // Upload layouts describe real source bytes; expected target bytes are RGBA.
    Bytes expectedA = pa, expectedB = pb;
    if (canonical)
        for (size_t i = 0; i < pa.size(); i += 4) {
            std::swap(expectedA[i], expectedA[i + 2]);
            std::swap(expectedB[i], expectedB[i + 2]);
        }
    if (withDepth) {
        Bytes depthPixels(pa.size());
        const float value = .25f;
        for (size_t i = 0; i < depthPixels.size(); i += 4)
            std::memcpy(depthPixels.data() + i, &value, 4);
        gpu.transfer(depthA.Get(), &depthPixels);
        gpu.transfer(depthB.Get(), &depthPixels);
    }
    FrameInput input{};
    input.output = outA.Get();
    input.outputFormat = outputFormat;
    input.outputEyeStride = 132;
    input.processingPercent = 50;
    for (UINT e = 0; e < 2; ++e)
        input.eyes[e] = {a.Get(), sourceFormat, {e * 132, 0, 132, 129}};
#if !defined(NR_SPATIAL_CROP_BASELINE)
    if (partial)
        for (auto& eye : input.eyes) {
            eye.processingRect = {2, 2, 128, 124};
            eye.transitionWidth = 2;
        }
#else
    (void)partial;
#endif
    Pipeline p(gpu.device.Get(), gpu.fence.Get());
    ComPtr<ID3D12Fence> gate;
    hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
    hr(gpu.queue->Wait(gate.Get(), 1), "queue hold");
    std::vector<Gpu::Commands> pending;
    pending.reserve(3);
    ID3D12Resource* scratch = nullptr;
    uint64_t last{};
    for (UINT n = 0; n < 3; ++n) {
        input.output = n == 1 ? outB.Get() : outA.Get();
        for (auto& eye : input.eyes) {
            eye.color = n == 1 ? b.Get() : a.Get();
            eye.depth = n == 1 ? depthB.Get() : depthA.Get();
            eye.depthFormat = DXGI_FORMAT_R32_FLOAT;
            eye.depthRect = eye.colorRect;
        }
#if defined(NR_SPATIAL_TEST_OBSERVER)
        test_observer::Reset();
#endif
        hr(p.Prepare(input), "prepare while prior completion blocked");
#if defined(NR_SPATIAL_TEST_OBSERVER)
        size_t writes = 0;
        for (size_t i = 0; i < test_observer::count; ++i)
            writes += test_observer::events[i].kind == test_observer::Kind::Descriptor;
        check(writes == 13, "blocked completion, including repeated identity, requires a new complete batch");
        test_observer::enabled = false;
#endif
        if (n == 0)
            scratch = p.GetWorkingEye(0).proxy;
        else
            check(scratch == p.GetWorkingEye(0).proxy, "one scratch set per geometry, not bank");
        check(bool(p.GetWorkingEye(0).depth) == withDepth, "paired and absent depth contracts retained");
        pending.push_back(gpu.commands());
        auto* list = pending.back().list.Get();
        p.RecordInputs(list);
        for (UINT e = 0; e < 2; ++e) {
            const auto eye = p.GetWorkingEye(e);
            Gpu::barrier(list, eye.proxy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Gpu::barrier(list, eye.answer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(eye.answer, eye.proxy);
            Gpu::barrier(list, eye.proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Gpu::barrier(list, eye.answer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        p.RecordResolve(list);
        last = gpu.submit(pending.back());
        check(gpu.fence->GetCompletedValue() < last, "completion is actually blocked");
        p.MarkSubmitted(last);
    }
    // Drop caller references before any recorded read has executed. Immutable
    // descriptor batches must keep both source banks alive through completion.
    a.Reset();
    b.Reset();
    depthA.Reset();
    depthB.Reset();
    hr(gate->Signal(1), "release queue hold");
    gpu.wait(last);
    compare(gpu.transfer(outA.Get()), expectedA, 0, "delayed first bank exact pixels");
    compare(gpu.transfer(outB.Get()), expectedB, 0, "delayed second bank exact pixels");
    if (withDepth) {
        const auto actual = gpu.transfer(p.GetWorkingEye(0).depth);
        const float expected = .25f;
        for (size_t i = 0; i < actual.size(); i += 4)
            check(std::memcmp(actual.data() + i, &expected, 4) == 0, "delayed depth ownership and point sample");
    }
    // Same geometry with newly allocated banks must recycle completed descriptor
    // slots, rather than retaining every retired swapchain resource indefinitely.
    auto replacement = gpu.texture(264, 129, sourceFormat);
    auto replacementDepth = withDepth ? gpu.texture(264, 129, DXGI_FORMAT_R32_FLOAT) : ComPtr<ID3D12Resource>{};
    auto replacementOutput = gpu.texture(264, 129, outputFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    gpu.transfer(replacement.Get(), &pb);
    if (withDepth) {
        Bytes depthPixels(pa.size());
        const float value = .75f;
        for (size_t i = 0; i < depthPixels.size(); i += 4)
            std::memcpy(depthPixels.data() + i, &value, 4);
        gpu.transfer(replacementDepth.Get(), &depthPixels);
    }
    input.output = replacementOutput.Get();
    for (auto& eye : input.eyes) {
        eye.color = replacement.Get();
        eye.depth = replacementDepth.Get();
    }
#if defined(NR_SPATIAL_TEST_OBSERVER)
    test_observer::Reset();
#endif
    const auto before = allocationCount.load();
    hr(p.Prepare(input), "prepare completed slot with changed resource identity");
    check(allocationCount.load() == before, "warmed Prepare must recycle descriptor slots without allocations");
#if defined(NR_SPATIAL_TEST_OBSERVER)
    size_t writes = 0;
    for (size_t i = 0; i < test_observer::count; ++i)
        if (test_observer::events[i].kind == test_observer::Kind::Descriptor) {
            ++writes;
            auto* resource = test_observer::events[i].resource;
            check(resource == input.output || resource == replacement.Get() || resource == replacementDepth.Get(),
                  "actual-fence rebinding never rewrites scratch descriptors");
        }
    check(writes == 5, "positively completed submitted slot rewrites exactly five native descriptors");
    test_observer::enabled = false;
    std::printf("COMPLETED_REBIND source=%u output=%u depth=%u fence=%llu writes=%zu allocations=0\n",
                unsigned(sourceFormat),
                unsigned(outputFormat),
                withDepth,
                static_cast<unsigned long long>(last),
                writes);
#endif
    auto commands = gpu.commands();
    p.RecordInputs(commands.list.Get());
    for (UINT e = 0; e < 2; ++e) {
        const auto eye = p.GetWorkingEye(e);
        Gpu::barrier(commands.list.Get(), eye.proxy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Gpu::barrier(commands.list.Get(), eye.answer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        commands.list->CopyResource(eye.answer, eye.proxy);
        Gpu::barrier(commands.list.Get(), eye.proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Gpu::barrier(commands.list.Get(), eye.answer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    p.RecordResolve(commands.list.Get());
    const auto replacementFence = gpu.submit(commands);
    p.MarkSubmitted(replacementFence);
    gpu.wait(replacementFence);
    compare(gpu.transfer(replacementOutput.Get()), expectedB, 0, "completed rebind writes current mixed-format bank");
    if (withDepth) {
        const auto actual = gpu.transfer(p.GetWorkingEye(0).depth);
        const float expected = .75f;
        for (size_t i = 0; i < actual.size(); i += 4)
            check(std::memcmp(actual.data() + i, &expected, 4) == 0, "completed rebind reads current depth bank");
    }
    gpu.noErrors();
    std::printf("PASS delayed completion, shared scratch, immutable bank bindings, source ownership, depth=%u "
                "source=%u output=%u\n",
                withDepth,
                unsigned(sourceFormat),
                unsigned(outputFormat));
}

void residualInvariants(Gpu& gpu) {
    constexpr UINT w = 256, h = 128;
    auto source = gpu.texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto target = gpu.texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    Bytes native(w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
            for (UINT c = 0; c < 4; ++c)
                native[offset(x, y, w, c)] = uint8_t(c == 3 ? (x + y) % 256 : 64 + ((x * 17 + y * 11 + c * 31) % 128));
    gpu.transfer(source.Get(), &native);
    FrameInput frame{};
    frame.output = target.Get();
    frame.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    frame.outputEyeStride = 128;
    frame.processingPercent = 50;
    for (UINT e = 0; e < 2; ++e)
        frame.eyes[e] = {source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, {e * 128, 0, 128, 128}};
    Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
    for (UINT mode = 0; mode < 2; ++mode) {
        hr(pipeline.Prepare(frame), "invariant prepare");
        auto commands = gpu.commands();
        pipeline.RecordInputs(commands.list.Get());
        gpu.wait(gpu.submit(commands));
        std::array<Bytes, 2> proxies, answers;
        const int constant[3]{16, -8, 4};
        for (UINT e = 0; e < 2; ++e) {
            const auto eye = pipeline.GetWorkingEye(e);
            proxies[e] = gpu.transfer(eye.proxy);
            answers[e] = proxies[e];
            for (UINT y = 0; y < 64; ++y)
                for (UINT x = 0; x < 64; ++x)
                    for (UINT c = 0; c < 3; ++c) {
                        const auto i = offset(x, y, 64, c);
                        const int delta = mode == 0 ? constant[c] : int((x * 3 + y * 7 + c * 11) % 41) - 20;
                        answers[e][i] = uint8_t(int(proxies[e][i]) + delta);
                    }
            gpu.transfer(eye.answer, &answers[e]);
        }
        commands = gpu.commands();
        pipeline.RecordResolve(commands.list.Get());
        const auto value = gpu.submit(commands);
        pipeline.MarkSubmitted(value);
        gpu.wait(value);
        const Bytes result = gpu.transfer(target.Get());
        for (UINT e = 0; e < 2; ++e)
            for (UINT y = 0; y < 128; ++y)
                for (UINT x = 0; x < 128; ++x)
                    for (UINT c = 0; c < 3; ++c) {
                        const auto i = offset(x + e * 128, y, w, c);
                        const int actual = int(result[i]) - int(native[i]);
                        if (mode == 0)
                            check(std::abs(actual - constant[c]) <= 1,
                                  "constant signed residual is preserved across native edges");
                        else {
                            const int sx = int(std::floor((x + .5) / 2 - .5)), sy = int(std::floor((y + .5) / 2 - .5));
                            int minimum = 255, maximum = -255;
                            for (int j = 0; j < 2; ++j)
                                for (int k = 0; k < 2; ++k) {
                                    const auto q =
                                        offset(UINT(std::clamp(sx + k, 0, 63)), UINT(std::clamp(sy + j, 0, 63)), 64, c);
                                    const int delta = int(answers[e][q]) - int(proxies[e][q]);
                                    minimum = std::min(minimum, delta);
                                    maximum = std::max(maximum, delta);
                                }
                            check(actual >= minimum - 1 && actual <= maximum + 1,
                                  "positive guided residual stays within local convex bounds");
                        }
                    }
    }
    gpu.noErrors();
    std::puts("PASS analytic constant signed edits and local convex residual bounds");
}

#if defined(NR_SPATIAL_TEST_OBSERVER)
// Catch redundant immutable description queries, descriptor rewrites, lost or
// reordered transitions, and barriers moved across actual dispatch/draw use.
int operationCounts(Gpu& gpu, bool depth, bool shared, bool partial = false) {
    namespace trace = test_observer;
    using Kind = trace::Kind;
    std::array<ComPtr<ID3D12Resource>, 2> colors, depths;
    for (UINT e = 0; e < 2; ++e) {
        colors[e] = shared && e ? colors[0] : gpu.texture(132, 129, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (depth)
            depths[e] = shared && e ? depths[0] : gpu.texture(132, 129, DXGI_FORMAT_R32_FLOAT);
    }
    auto target = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    FrameInput frame{};
    frame.output = target.Get();
    frame.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    frame.outputEyeStride = 132;
    frame.processingPercent = 80;
    for (UINT e = 0; e < 2; ++e)
        frame.eyes[e] = {colors[e].Get(),
                         DXGI_FORMAT_R8G8B8A8_UNORM,
                         {0, 0, 132, 129},
                         depths[e].Get(),
                         DXGI_FORMAT_R32_FLOAT,
                         {0, 0, 132, 129}};
#if !defined(NR_SPATIAL_CROP_BASELINE)
    if (partial)
        for (auto& eye : frame.eyes) {
            eye.processingRect = {2, 2, 128, 124};
            eye.transitionWidth = 2;
        }
#else
    (void)partial;
#endif
    auto count = [](Kind kind) {
        return size_t(std::count_if(trace::events.begin(), trace::events.begin() + trace::count, [=](const auto& e) {
            return e.kind == kind;
        }));
    };
    int failures = 0;
    auto expect = [&](bool value, const char* why) {
        if (!value) {
            std::fprintf(stderr, "FAIL operation counts depth=%u shared=%u: %s\n", depth, shared, why);
            ++failures;
        }
    };
    Pipeline p(gpu.device.Get(), gpu.fence.Get());
    trace::Reset();
    hr(p.Prepare(frame), "operation prepare new batch");
    const auto descriptions = count(Kind::Description), created = count(Kind::Descriptor);
    expect(descriptions <= (depth ? 5u : 3u), "one description per output/color/depth input");
    expect(created == 13, "new batch initializes all 13 descriptors");
    // Resource descriptions are local to Validate, including when both eyes
    // alias the same resource. No output query is repeated for bounds checks.
    size_t nextDescription = 0;
    std::array<ID3D12Resource*, 5> described{};
    described[nextDescription++] = frame.output;
    for (const auto& eye : frame.eyes) {
        described[nextDescription++] = eye.color;
        if (eye.depth)
            described[nextDescription++] = eye.depth;
    }
    for (size_t i = 0; i < descriptions && i < nextDescription; ++i)
        expect(trace::events[i].kind == Kind::Description && trace::events[i].resource == described[i],
               "description queries cover exactly output and each eye input");
    p.DiscardRecording();
    trace::Reset();
    const auto beforeHit = allocationCount.load();
    hr(p.Prepare(frame), "operation identity hit");
    expect(allocationCount.load() == beforeHit, "identity hit allocates nothing");
    expect(count(Kind::Descriptor) == 0, "identity hit writes no descriptors");
    p.DiscardRecording();
    auto replacement = gpu.texture(132, 129, DXGI_FORMAT_R8G8B8A8_UNORM);
    frame.eyes[0].color = replacement.Get();
    if (shared)
        frame.eyes[1].color = replacement.Get();
    trace::Reset();
    const auto beforeRebind = allocationCount.load();
    hr(p.Prepare(frame), "operation completed changed-bank rebind");
    expect(allocationCount.load() == beforeRebind, "completed rebinding allocates nothing");
    const auto rebound = count(Kind::Descriptor);
    expect(rebound == 5, "completed rebinding writes only four native SRVs and output RTV");
    for (size_t i = 0; i < trace::count; ++i)
        if (trace::events[i].kind == Kind::Descriptor) {
            const auto* r = trace::events[i].resource;
            expect(r == frame.output || r == frame.eyes[0].color || r == frame.eyes[1].color ||
                       r == frame.eyes[0].depth || r == frame.eyes[1].depth,
                   "rebinding never rewrites scratch descriptors");
        }
    auto commands = gpu.commands();
    trace::Reset();
    const auto beforeRecording = allocationCount.load();
    p.RecordInputs(commands.list.Get());
    p.RecordResolve(commands.list.Get());
    expect(allocationCount.load() == beforeRecording, "recording allocates nothing");
    const auto calls = trace::barrierCalls;
    const auto transitions = count(Kind::Transition);
    expect(calls == 6, "adjacent transitions batched at six GPU-use boundaries");

    // Hand-derived contract: every original transition, in original order,
    // including COMMON boundaries between input, vendor access and resolve.
    std::array<trace::Event, 40> expected{};
    size_t n = 0;
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        expected[n++] = {Kind::Transition, r, from, to};
    };
    const auto common = D3D12_RESOURCE_STATE_COMMON;
    const auto compute = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto pixel = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto native = [&](D3D12_RESOURCE_STATES state, bool entering, bool includeDepth) {
        for (UINT e = 0; e < (shared ? 1u : 2u); ++e) {
            transition(frame.eyes[e].color, entering ? common : state, entering ? state : common);
            if (depth && includeDepth)
                transition(frame.eyes[e].depth, entering ? common : state, entering ? state : common);
        }
    };
    native(compute, true, true);
    for (UINT e = 0; e < 2; ++e) {
        const auto s = p.GetWorkingEye(e);
        transition(s.proxy, common, uav);
        if (depth)
            transition(s.depth, common, uav);
        expected[n++] = {Kind::Dispatch};
        transition(s.proxy, uav, common);
        if (depth)
            transition(s.depth, uav, common);
    }
    native(compute, false, true);
    native(pixel, true, false);
    transition(frame.output, common, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT e = 0; e < 2; ++e) {
        const auto s = p.GetWorkingEye(e);
        transition(s.proxy, common, pixel);
        transition(s.answer, common, pixel);
        expected[n++] = {Kind::Draw};
        transition(s.proxy, pixel, common);
        transition(s.answer, pixel, common);
    }
    transition(frame.output, D3D12_RESOURCE_STATE_RENDER_TARGET, common);
    native(pixel, false, false);
    expect(trace::count == n, "all transition/use events retained");
    bool ordered = trace::count == n;
    for (size_t i = 0; i < std::min(n, trace::count); ++i) {
        const auto& a = trace::events[i];
        const auto& b = expected[i];
        const bool same = a.kind == b.kind && a.resource == b.resource && a.before == b.before && a.after == b.after;
        ordered = ordered && same;
        expect(same, "transition order, deduplication and dispatch/draw boundary");
    }
    trace::enabled = false;
    hr(commands.list->Close(), "close unsubmitted operation recording");
    p.DiscardRecording();
    gpu.noErrors();
    std::printf("OPERATIONS depth=%u shared=%u partial=%u GetDesc=%zu create=%zu identity=0 rebind=%zu "
                "barrierCalls=%zu transitions=%zu order=%s\n",
                depth,
                shared,
                partial,
                descriptions,
                created,
                rebound,
                calls,
                transitions,
                ordered ? "retained" : "FAIL");
    return failures;
}
#endif
void shadowToeContract(Gpu& gpu) {
    constexpr UINT eyeSize = 128, width = eyeSize * 2;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool srgb = IsSrgb(format);
        auto source = gpu.texture(width, eyeSize, bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS);
        auto output =
            gpu.texture(width, eyeSize, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        FrameInput frame{};
        frame.output = output.Get();
        frame.outputFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.outputEyeStride = eyeSize;
        frame.processingPercent = 80;
        for (UINT e = 0; e < 2; ++e)
            frame.eyes[e] = {source.Get(), format, {e * eyeSize, 0, eyeSize, eyeSize}};
        Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
        int previousSpread = -1;
        for (UINT code : {1u, 2u, 4u, 8u, 16u, 22u, 32u, 64u, 128u}) {
            // Identity, biased gray, neutral structure, real green identity,
            // and a bright native pixel whose model guide contains no signal.
            for (UINT mode = 0; mode < 5; ++mode) {
                Bytes native(size_t(width) * eyeSize * 4);
                for (size_t i = 0; i < native.size(); i += 4) {
                    for (UINT c = 0; c < 3; ++c)
                        native[i + (bgra ? 2 - c : c)] = uint8_t(mode == 3 && c != 1 ? 0 : code);
                    native[i + 3] = uint8_t((i / 4) % 256);
                }
                gpu.transfer(source.Get(), &native);
                hr(pipeline.Prepare(frame), "shadow toe prepare");
                auto commands = gpu.commands();
                pipeline.RecordInputs(commands.list.Get());
                gpu.wait(gpu.submit(commands));
                for (UINT e = 0; e < 2; ++e) {
                    const auto eye = pipeline.GetWorkingEye(e);
                    Bytes proxy = gpu.transfer(eye.proxy);
                    if (mode == 4) {
                        std::fill(proxy.begin(), proxy.end(), uint8_t(0));
                        gpu.transfer(eye.proxy, &proxy);
                    }
                    Bytes answer = proxy;
                    for (size_t i = 0; i < answer.size(); i += 4) {
                        if (mode == 1 || mode == 4)
                            answer[i + 1] += 8;
                        if (mode == 2)
                            for (UINT c = 0; c < 3; ++c)
                                answer[i + c] += 2;
                        answer[i + 3] = 255;
                    }
                    gpu.transfer(eye.answer, &answer);
                }
                commands = gpu.commands();
                pipeline.RecordResolve(commands.list.Get());
                const auto serial = gpu.submit(commands);
                pipeline.MarkSubmitted(serial);
                gpu.wait(serial);
                const auto result = gpu.transfer(output.Get());
                for (size_t i = 0; i < result.size(); i += 4) {
                    check(result[i + 3] == native[i + 3], "shadow toe preserves native alpha");
                    if (mode == 0 || mode == 3 || mode == 4)
                        for (UINT c = 0; c < 3; ++c)
                            check(result[i + c] == native[i + (bgra ? 2 - c : c)],
                                  "shadow identity, legitimate dark green and unsupported guide edits retain source");
                    if (mode == 2 && (code >= 4 || !srgb))
                        for (UINT c = 0; c < 3; ++c)
                            check(std::abs(int(result[i + c]) - int(code + 2)) <= 1,
                                  "neutral NR structure survives outside narrow black toe");
                    if (mode == 1 && code >= 22)
                        check(result[i] == code && result[i + 1] == code + 8 && result[i + 2] == code,
                              "midtone NR color edit remains fully active");
                }
                if (mode == 1) {
                    const int spread = int(result[1]) - int(result[0]);
                    check(spread >= 0 && spread <= 8, "shadow color edit remains bounded without sign inversion");
                    check(spread >= previousSpread, "uniform near-black chroma transition has no backward step");
                    if (srgb && code <= 4)
                        check(spread <= 1, "deep sRGB shadows reject strong model green bias");
                    previousSpread = spread;
                }
            }
        }
    }
    gpu.noErrors();
    std::puts("PASS analytic shadow ramps, green identity, neutral structure, source/proxy support and midtone edits");
}

void blackShadowContract(Gpu& gpu) {
    constexpr UINT eyeSize = 128, width = eyeSize * 2;
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        auto source = gpu.texture(width, eyeSize, bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS);
        auto output =
            gpu.texture(width, eyeSize, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        Bytes native(size_t(width) * eyeSize * 4);
        for (UINT y = 0; y < eyeSize; ++y)
            for (UINT x = 0; x < width; ++x)
                native[(size_t(y) * width + x) * 4 + 3] = uint8_t((x + 3 * y) % 256);
        gpu.transfer(source.Get(), &native);
        FrameInput frame{};
        frame.output = output.Get();
        frame.outputFormat = IsSrgb(format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.outputEyeStride = eyeSize;
        frame.processingPercent = 80;
        for (UINT e = 0; e < 2; ++e)
            frame.eyes[e] = {source.Get(),
                             format,
                             {e * eyeSize, 0, eyeSize, eyeSize},
                             nullptr,
                             DXGI_FORMAT_UNKNOWN,
                             {},
                             {4, 4, 120, 120},
                             2};
        Pipeline pipeline(gpu.device.Get(), gpu.fence.Get());
        hr(pipeline.Prepare(frame), "black shadow prepare");
        auto commands = gpu.commands();
        pipeline.RecordInputs(commands.list.Get());
        gpu.wait(gpu.submit(commands));
        for (UINT e = 0; e < 2; ++e) {
            const auto eye = pipeline.GetWorkingEye(e);
            Bytes tinted(size_t(eye.extent.width) * eye.extent.height * 4);
            for (size_t i = 0; i < tinted.size(); i += 4) {
                tinted[i + 1] = 8;
                tinted[i + 3] = 255;
            }
            gpu.transfer(eye.answer, &tinted);
        }
        commands = gpu.commands();
        pipeline.RecordResolve(commands.list.Get());
        const auto serial = gpu.submit(commands);
        pipeline.MarkSubmitted(serial);
        gpu.wait(serial);
        const auto result = gpu.transfer(output.Get());
        compare(result, native, 0, "black shadow must retain original RGB and alpha under tinted NR answer");
    }
    gpu.noErrors();
    std::puts("PASS analytic black preservation with tinted NR answers across RGBA/BGRA and linear/sRGB");
}

#include "nr_color_contract_cases.h"

int main(int argc, char** argv) {
    try {
        Gpu gpu(argc > 1 && std::strcmp(argv[1], "--hardware") == 0);
        colorChannelContract(gpu, false);
        blackShadowContract(gpu);
        shadowToeContract(gpu);
        for (const auto combined : {std::array<UINT, 2>{95, 80}, {90, 80}, {90, 85}})
            for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
                for (bool arraySource : {false, true})
                    runFormat(gpu,
                              format,
                              format == DXGI_FORMAT_B8G8R8A8_UNORM        ? DXGI_FORMAT_R8G8B8A8_UNORM
                              : format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                          : format,
                              combined[1],
                              arraySource,
                              combined[0]);
#if defined(NR_SPATIAL_TEST_OBSERVER)
        int operationFailures = 0;
        for (bool depth : {false, true})
            for (bool shared : {false, true})
                for (bool partial : {false, true})
                    operationFailures += operationCounts(gpu, depth, shared, partial);
#endif
        for (auto percent : {50u, 80u, 90u})
            for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
                for (bool arraySource : {false, true})
                    runFormat(gpu, format, format, percent, arraySource);
        for (auto percent : {50u, 80u, 90u})
            for (bool srgb : {false, true})
                for (bool arraySource : {false, true})
                    runFormat(gpu,
                              srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
                              srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                              percent,
                              arraySource);
        delayedBatches(gpu, false);
        delayedBatches(gpu, true);
        for (bool depth : {false, true})
            for (bool srgb : {false, true})
                for (bool partial : {false, true})
                    delayedBatches(gpu, depth, true, srgb, partial);
        formatValidation(gpu);
        residualInvariants(gpu);
#if defined(NR_SPATIAL_TEST_OBSERVER)
        check(operationFailures == 0, "CPU operation-count regression");
#endif
        std::puts("PASS production spatial pipeline synthetic GPU suite (no vendor inference)");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
