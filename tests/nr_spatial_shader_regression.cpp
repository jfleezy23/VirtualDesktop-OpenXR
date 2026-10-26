// Independent scalar contract + real production Pipeline, with synthetic edits.
// This proves shader/lifetime behavior, NOT proprietary NR inference or quality.
#include "../virtualdesktop-openxr/NrSpatialPipeline.h"
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
Bytes resolveOracle(
    const Bytes& native, UINT rowWidth, Rect rect, Extent work, const Bytes& p, const Bytes& a, bool srgb, bool bgra) {
    Bytes output(size_t(rect.width) * rect.height * 4);
    for (UINT y = 0; y < rect.height; ++y)
        for (UINT x = 0; x < rect.width; ++x) {
            const double qx = (x + .5) * work.width / rect.width - .5, qy = (y + .5) * work.height / rect.height - .5;
            const int ix = int(std::floor(qx)), iy = int(std::floor(qy));
            const double fx = qx - ix, fy = qy - iy;
            double n[3], edit[3]{}, total = 0;
            bool zero = true;
            for (UINT c = 0; c < 3; ++c)
                n[c] = decode(native[offset(rect.x + x, rect.y + y, rowWidth, c, bgra)] / 255.0, srgb);
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const UINT sx = UINT(std::clamp(ix + i, 0, int(work.width) - 1)),
                               sy = UINT(std::clamp(iy + j, 0, int(work.height) - 1));
                    double delta[3], distance = 0;
                    for (UINT c = 0; c < 3; ++c) {
                        const auto index = offset(sx, sy, work.width, c);
                        const double guide = decode(p[index] / 255.0, srgb);
                        delta[c] = decode(a[index] / 255.0, srgb) - guide;
                        zero = zero && a[index] == p[index];
                        distance += (guide - n[c]) * (guide - n[c]) / 3;
                    }
                    const double weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy) / (1 + 1024 * distance);
                    for (UINT c = 0; c < 3; ++c)
                        edit[c] += weight * delta[c];
                    total += weight;
                }
            for (UINT c = 0; c < 4; ++c) {
                const auto raw = native[offset(rect.x + x, rect.y + y, rowWidth, c, bgra)];
                output[offset(x, y, rect.width, c, bgra)] =
                    c == 3 || zero ? raw : quantize(encode(n[c] + edit[c] / total, srgb));
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

void runFormat(Gpu& gpu, DXGI_FORMAT format, UINT percent, bool arraySource = false) {
    const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto allocation = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr UINT inputW = 311, inputH = 161, outputW = 281, outputH = 137, stride = 144;
    const std::array<Rect, 2> rects{{{3, 5, 137, 133}, {161, 13, 129, 127}}};
    // Independently hand-calculated 50%/80%/90% ceil-and-align grids.
    const std::array<Extent, 2> work = percent == 50   ? std::array<Extent, 2>{{{72, 68}, {68, 64}}}
                                       : percent == 80 ? std::array<Extent, 2>{{{112, 108}, {104, 104}}}
                                                       : std::array<Extent, 2>{{{124, 120}, {120, 116}}};
    const UINT16 slices = arraySource ? 2 : 1;
    auto source = gpu.texture(inputW, inputH, allocation, D3D12_RESOURCE_FLAG_NONE, slices);
    auto target = gpu.texture(outputW, outputH, allocation, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = gpu.texture(183, 145, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, slices);
    FrameInput frame{};
    frame.output = target.Get();
    frame.outputFormat = format;
    frame.outputEyeStride = stride;
    frame.processingPercent = percent;
    const std::array<Rect, 2> depths{{{7, 11, 75, 99}, {91, 3, 83, 131}}};
    for (UINT e = 0; e < 2; ++e)
        frame.eyes[e] = {source.Get(), format, rects[e], depth.Get(), DXGI_FORMAT_R32_FLOAT, depths[e]};
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
    for (UINT bank = 0; bank < 3; ++bank) {
        if (bank) {
            const auto bankFormat = bank == 2 ? format : allocation; // typed as well as typeless imports
            source = gpu.texture(inputW, inputH, bankFormat, D3D12_RESOURCE_FLAG_NONE, slices);
            target = gpu.texture(outputW, outputH, bankFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
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
            hr(pipeline.Prepare(frame), "prepare");
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
                compare(
                    p, areaOracle(native, inputW, rects[e], work[e], srgb, bgra), 1, "area-filter/encoding/eye bounds");
                const Bytes sampledDepth = gpu.transfer(eye.depth);
                for (UINT y = 0; y < work[e].height; ++y)
                    for (UINT x = 0; x < work[e].width; ++x) {
                        const UINT dx = depths[e].x + UINT((double(x) + .5) * depths[e].width / work[e].width);
                        const UINT dy = depths[e].y + UINT((double(y) + .5) * depths[e].height / work[e].height);
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
                expected[e] = resolveOracle(native, inputW, rects[e], work[e], p, answer, srgb, bgra);
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
            for (UINT e = 0; e < 2; ++e) {
                Bytes crop(size_t(rects[e].width) * rects[e].height * 4);
                for (UINT y = 0; y < rects[e].height; ++y)
                    std::memcpy(crop.data() + size_t(y) * rects[e].width * 4,
                                actual.data() + (size_t(y) * outputW + e * stride) * 4,
                                size_t(rects[e].width) * 4);
                compare(crop, expected[e], mode ? 1 : 0, mode ? "guided signed residual" : "exact zero-edit raw/alpha");
                for (size_t i = 3; i < crop.size(); i += 4)
                    check(crop[i] == expected[e][i], "native alpha exact under edit");
            }
        }
    }
    hr(pipeline.Prepare(frame), "prepare to discard");
    pipeline.DiscardRecording();
    hr(pipeline.Prepare(frame), "discard permits retry");
    pipeline.DiscardRecording();
    pipeline.Clear();
    gpu.noErrors();
    std::printf("PASS format %u percent %u slices %u: odd/offset eyes, poison borders, zero/signed edits, depth, bank "
                "rotation\n",
                unsigned(format),
                percent,
                slices);
}

// A real GPU queue is held behind a CPU-signalled fence. Three recordings are
// prepared while their completion fence cannot advance. Any early descriptor
// rewrite, input release, or per-batch scratch allocation breaks this contract.
void delayedBatches(Gpu& gpu) {
    auto a = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto b = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto outA = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto outB = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    Bytes pa(264 * 129 * 4), pb(pa.size());
    for (size_t i = 0; i < pa.size(); ++i) {
        pa[i] = uint8_t((i * 13) % 256);
        pb[i] = uint8_t((i * 7 + 91) % 256);
    }
    gpu.transfer(a.Get(), &pa);
    gpu.transfer(b.Get(), &pb);
    FrameInput input{};
    input.output = outA.Get();
    input.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    input.outputEyeStride = 132;
    input.processingPercent = 50;
    for (UINT e = 0; e < 2; ++e)
        input.eyes[e] = {a.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, {e * 132, 0, 132, 129}};
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
        for (auto& eye : input.eyes)
            eye.color = n == 1 ? b.Get() : a.Get();
        hr(p.Prepare(input), "prepare while prior completion blocked");
        if (n == 0)
            scratch = p.GetWorkingEye(0).proxy;
        else
            check(scratch == p.GetWorkingEye(0).proxy, "one scratch set per geometry, not bank");
        check(p.GetWorkingEye(0).depth == nullptr, "missing paired depth preserves no-depth contract");
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
    hr(gate->Signal(1), "release queue hold");
    gpu.wait(last);
    compare(gpu.transfer(outA.Get()), pa, 0, "delayed first bank exact pixels");
    compare(gpu.transfer(outB.Get()), pb, 0, "delayed second bank exact pixels");
    // Same geometry with newly allocated banks must recycle completed descriptor
    // slots, rather than retaining every retired swapchain resource indefinitely.
    auto replacement = gpu.texture(264, 129, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto& eye : input.eyes)
        eye.color = replacement.Get();
    const auto before = allocationCount.load();
    hr(p.Prepare(input), "prepare completed slot with changed resource identity");
    check(allocationCount.load() == before, "warmed Prepare must recycle descriptor slots without allocations");
    p.DiscardRecording();
    gpu.noErrors();
    std::puts("PASS delayed completion, shared scratch, immutable bank bindings, source ownership, no depth");
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
int main(int argc, char** argv) {
    try {
        Gpu gpu(argc > 1 && std::strcmp(argv[1], "--hardware") == 0);
        for (auto percent : {50u, 80u, 90u})
            for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
                for (bool arraySource : {false, true})
                    runFormat(gpu, format, percent, arraySource);
        delayedBatches(gpu);
        residualInvariants(gpu);
        std::puts("PASS production spatial pipeline synthetic GPU suite (no vendor inference)");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
