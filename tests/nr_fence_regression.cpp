// Exercise the production command-context fence waits on real D3D12 queues.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <winrt/base.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;
inline void checkHr(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Direct3D call failed");
}
#define CHECK_HRCMD(expression) checkHr(expression)
#define CHECK_MSG(condition, message)                                                                                  \
    if (!(condition))                                                                                                  \
    throw std::runtime_error(message)
#include "../virtualdesktop-openxr/D3D12Utils.h"

int main() {
    try {
        ComPtr<ID3D12Device> device;
        checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())));
        // A completed fence on one queue must never release another queue's Flush.
        for (int attempt = 0; attempt < 32; ++attempt) {
            D3D12Utils::CommandContext a(device.Get(), L"Fence regression A");
            D3D12Utils::CommandContext b(device.Get(), L"Fence regression B");
            ComPtr<ID3D12Fence> gateA, gateB;
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gateA.GetAddressOf())));
            checkHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gateB.GetAddressOf())));
            checkHr(a.GetCommandQueue()->Wait(gateA.Get(), 1));
            checkHr(b.GetCommandQueue()->Wait(gateB.Get(), 1));
            a.SubmitCommandList(a.GetCommandList());
            b.SubmitCommandList(b.GetCommandList());
            std::atomic<bool> aReturned{false}, bReturned{false};
            std::atomic<bool> invalidCompletion{false};
            std::thread tb([&] {
                b.Flush();
                invalidCompletion = b.GetCompletionFence()->GetCompletedValue() < 1;
                bReturned = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::thread ta([&] {
                a.Flush();
                aReturned = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            checkHr(gateA->Signal(1));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (!aReturned && !bReturned && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // Always unblock and join both workers before reporting failure.
            checkHr(gateB->Signal(1));
            ta.join();
            tb.join();
            if (invalidCompletion) {
                std::cerr << "FAIL: Flush returned before its own GPU fence completed (attempt " << attempt << ")\n";
                return 1;
            }
        }
        std::cout << "PASS: 32 independent concurrent GPU fence waits\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
