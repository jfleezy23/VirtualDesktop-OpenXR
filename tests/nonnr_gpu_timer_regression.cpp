// Exercise the production D3D11 timer on an actual device. Interception is confined to this test's thread/context.
#include "pch.h"
#include "utils.h"
#include "gpu_timers.h"
#include <array>

using namespace virtualdesktop_openxr;
using namespace virtualdesktop_openxr::utils;

namespace {
    void require(bool passed, const char* message) {
        if (!passed)
            throw std::runtime_error(message);
    }
    using GetData = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
    GetData originalGetData{};
    thread_local ID3D11DeviceContext* watchedContext{};
    thread_local bool intercept = false;
    thread_local bool inject = true;
    thread_local unsigned calls = 0;
    thread_local unsigned flushingReads = 0;
    thread_local unsigned failAt = 0;
    thread_local HRESULT injectedResult = S_FALSE;
    thread_local bool disjoint = false;
    HRESULT STDMETHODCALLTYPE
    observedGetData(ID3D11DeviceContext* context, ID3D11Asynchronous* query, void* data, UINT bytes, UINT flags) {
        if (!intercept || context != watchedContext)
            return originalGetData(context, query, data, bytes, flags);
        ++calls;
        if (!(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH))
            ++flushingReads;
        if (!inject)
            return originalGetData(context, query, data, bytes, flags);
        if (calls == failAt)
            return injectedResult;
        if (bytes == sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)) {
            *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data) = {1000000, disjoint};
        } else {
            require(bytes == sizeof(UINT64), "Unexpected timer query data size");
            *static_cast<UINT64*>(data) = calls == 1 ? 100 : 2100;
        }
        return S_OK;
    }
    struct Observer {
        Observer(ID3D11DeviceContext* context) {
            watchedContext = context;
            originalGetData = reinterpret_cast<GetData>((*reinterpret_cast<void***>(context))[29]);
            require(DetourTransactionBegin() == NO_ERROR, "GetData detour begin failed");
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&originalGetData), observedGetData);
            if (updated || attached) {
                DetourTransactionAbort();
                throw std::runtime_error("GetData detour attach failed");
            }
            require(DetourTransactionCommit() == NO_ERROR, "GetData detour commit failed");
        }
        ~Observer() {
            intercept = false;
            const auto begun = DetourTransactionBegin();
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto detached = DetourDetach(reinterpret_cast<PVOID*>(&originalGetData), observedGetData);
            const auto committed = DetourTransactionCommit();
            if (begun || updated || detached || committed)
                std::terminate();
        }
    };
    void resetRead(unsigned failure = 0, HRESULT result = S_FALSE, bool invalid = false) {
        calls = flushingReads = 0;
        failAt = failure;
        injectedResult = result;
        disjoint = invalid;
    }
    void contract(ID3D11Device* device, ID3D11DeviceContext* context) {
        D3D11GpuTimer timer(device, context);
        inject = intercept = true;
        resetRead();
        require(timer.query() == 0 && calls == 0, "Unstarted timer performed a query");
        for (const auto result : {S_FALSE, E_FAIL}) {
            for (unsigned stage = 1; stage <= 3; ++stage) {
                timer.start();
                timer.stop();
                resetRead(stage, result);
                require(timer.query() == 0 && calls == stage, "Unavailable/error query did not short-circuit");
                require(flushingReads == 0, "Timer read can implicitly flush queued GPU work");
                require(timer.query() == 0 && calls == stage, "Default reset did not consume unavailable sample");
            }
        }
        timer.start();
        timer.stop();
        resetRead();
        require(timer.query(false) == 2000 && calls == 3, "Successful timestamp conversion failed");
        require(flushingReads == 0, "Successful timer read can flush GPU work");
        resetRead();
        require(timer.query(false) == 2000 && calls == 3, "Non-resetting query did not retain sample");
        resetRead(2);
        require(timer.query(false) == 0 && calls == 2, "Pending non-resetting query failed");
        resetRead();
        require(timer.query() == 2000 && calls == 3, "Retained pending sample did not recover");
        require(timer.query() == 0 && calls == 3, "Default reset did not consume successful sample");
        timer.start();
        timer.stop();
        resetRead(0, S_FALSE, true);
        require(timer.query() == 0 && calls == 3 && flushingReads == 0, "Disjoint sample handling failed");
        intercept = false;
        // Test submission is independent of GetData; retire actual commands issued by start/stop.
        context->Flush();
        std::cout << "PASS: all three query flags, success, pending/error short circuits, reset and disjoint\n";
    }
    void native(ID3D11Device* device, ID3D11DeviceContext* context, bool benchmark) {
        constexpr unsigned frames = 360;
        constexpr unsigned ringSize = 3;
        std::unique_ptr<D3D11GpuTimer> timers[ringSize];
        for (auto& timer : timers)
            timer = std::make_unique<D3D11GpuTimer>(device, context);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 2048;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        CHECK_HRCMD(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()));
        CHECK_HRCMD(device->CreateRenderTargetView(texture.Get(), nullptr, target.GetAddressOf()));
        std::array<double, frames> queryTimes{};
        unsigned available = 0;
        uint64_t totalUs = 0;
        inject = false;
        resetRead();
        for (unsigned frame = 0; frame < frames; ++frame) {
            auto& timer = timers[frame % ringSize];
            intercept = true;
            const auto started = std::chrono::steady_clock::now();
            const auto value = timer->query();
            queryTimes[frame] =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
            intercept = false;
            if (frame >= ringSize) {
                available += value > 0;
                totalUs += value;
            }
            timer->start();
            const float color[] = {float(frame % 10) / 10.f, 0.1f, 0.2f, 1.f};
            for (unsigned work = 0; work < 8; ++work)
                context->ClearRenderTargetView(target.Get(), color);
            timer->stop();
            // A frame submission analogue after recording work, never a flush performed by a timer read.
            context->Flush();
            Sleep(2);
        }
        std::sort(queryTimes.begin() + ringSize, queryTimes.end());
        std::cout << "MEASURE: native samples=" << frames - ringSize << " available=" << available
                  << " zero=" << frames - ringSize - available << " flushing_reads=" << flushingReads
                  << " gpu_mean_us=" << (available ? double(totalUs) / available : 0.)
                  << " query_median_us=" << queryTimes[ringSize + (frames - ringSize) / 2]
                  << " query_p95_us=" << queryTimes[ringSize + (frames - ringSize) * 95 / 100] << '\n';
        require(available >= (frames - ringSize) * 95 / 100,
                "Native timer samples failed to progress after submission");
        if (!benchmark)
            require(flushingReads == 0, "Native query used flushing GetData");
        std::cout << "PASS: native three-slot timer produces submitted GPU samples\n";
    }
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const std::wstring mode = argc > 1 ? argv[1] : L"contract";
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        CHECK_HRCMD(D3D11CreateDevice(nullptr,
                                      mode == L"contract" ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                      nullptr,
                                      0,
                                      nullptr,
                                      0,
                                      D3D11_SDK_VERSION,
                                      device.GetAddressOf(),
                                      nullptr,
                                      context.GetAddressOf()));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        CHECK_HRCMD(device.As(&dxgi));
        CHECK_HRCMD(dxgi->GetAdapter(adapter.GetAddressOf()));
        DXGI_ADAPTER_DESC description{};
        CHECK_HRCMD(adapter->GetDesc(&description));
        std::wcout << L"ADAPTER: " << description.Description << L'\n';
        Observer observer(context.Get());
        if (mode == L"contract")
            contract(device.Get(), context.Get());
        else if (mode == L"native" || mode == L"benchmark")
            native(device.Get(), context.Get(), mode == L"benchmark");
        else
            throw std::runtime_error("Unknown timer mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
