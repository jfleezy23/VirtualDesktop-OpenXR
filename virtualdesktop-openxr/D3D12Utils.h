#include "PairCaptureNrContracts.h"
#include "PairCaptureNrWarmSubmission.h"
// MIT License
//
// Copyright(c) 2024-2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once
#include "NrOwnershipViews.h"
#include "PairCaptureNrOwner.h"

namespace D3D12Utils {

    class CommandList {
      public:
        ComPtr<ID3D12GraphicsCommandList> Commands;

      private:
        ComPtr<ID3D12CommandAllocator> Allocator;
        uint64_t CompletedFenceValue{0};
        vdxrfg::pair_capture::nr::NrRecordingEpochFacts m_RecordingFacts{};

        friend class CommandContext;
    };

    class CommandContext {
      public:
        CommandContext(ID3D12Device* Device, const std::wstring& DebugName = L"Unnamed")
            : m_Device(Device), m_DebugName(DebugName) {
            // Create a command queue for our commands.
            D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
            commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            CHECK_HRCMD(
                m_Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(m_CommandQueue.ReleaseAndGetAddressOf())));
            m_CommandQueue->SetName((DebugName + L" Command Queue").c_str());

            CHECK_HRCMD(m_Device->CreateFence(
                0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(m_CompletionFence.ReleaseAndGetAddressOf())));
            m_CompletionFence->SetName((DebugName + L" Completion Fence").c_str());
            vdxrfg::pair_capture::nr::TryNextObservedEpoch(s_NextContextLifetime, m_ContextLifetimeEpoch);
        }

        ~CommandContext() {
            Flush();
        }

        CommandList GetCommandList() {
            std::unique_lock lock(m_CommandListPoolMutex);

            if (m_AvailableCommandList.empty()) {
                // Recycle completed command lists.
                while (!m_PendingCommandList.empty() &&
                       IsCommandListCompleted(m_PendingCommandList.front().CompletedFenceValue)) {
                    m_AvailableCommandList.push_back(std::move(m_PendingCommandList.front()));
                    m_PendingCommandList.pop_front();
                }
            }

            CommandList commandList;
            if (m_AvailableCommandList.empty()) {
                // Allocate a new command list if needed.
                CHECK_HRCMD(m_Device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandList.Allocator.ReleaseAndGetAddressOf())));
                vdxrfg::pair_capture::nr::TryNextObservedEpoch(s_NextAllocatorObject,
                                                               commandList.m_RecordingFacts.nrAllocatorObjectEpoch);
                CHECK_HRCMD(m_Device->CreateCommandList(0,
                                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        commandList.Allocator.Get(),
                                                        nullptr,
                                                        IID_PPV_ARGS(commandList.Commands.ReleaseAndGetAddressOf())));
                commandList.Commands->SetName((m_DebugName + L" Command List").c_str());
                vdxrfg::pair_capture::nr::TryNextObservedEpoch(s_NextListObject,
                                                               commandList.m_RecordingFacts.nrListObjectEpoch);
                commandList.m_RecordingFacts.originatingCommandContext = this;
                commandList.m_RecordingFacts.nrCommandContextLifetimeEpoch = m_ContextLifetimeEpoch;
            } else {
                commandList = std::move(m_AvailableCommandList.front());
                m_AvailableCommandList.pop_front();

                // Reset the command list before reuse.
                CHECK_HRCMD(commandList.Allocator->Reset());
                CHECK_HRCMD(commandList.Commands->Reset(commandList.Allocator.Get(), nullptr));
            }

            // Published only after original Create/Reset success, including reused pointers.
            vdxrfg::pair_capture::nr::TryNextObservedEpoch(s_NextRecording,
                                                           commandList.m_RecordingFacts.nrRecordingEpoch);
            return commandList;
        }

        bool BorrowRecordingOwnership(const CommandList& commandList,
                                      vdxrfg::pair_capture::nr::NrRecordingOwnership& out) const noexcept {
            using namespace vdxrfg::pair_capture::nr;
            out = {};
            NrRecordingOwnership view{};
            view.device = m_Device.Get();
            view.queue = m_CommandQueue.Get();
            view.fence = m_CompletionFence.Get();
            view.list = commandList.Commands.Get();
            view.allocator = commandList.Allocator.Get();
            auto facts = commandList.m_RecordingFacts;
            facts.observed =
                (view.device ? NrRecordingEpochFacts::device : 0u) | (view.queue ? NrRecordingEpochFacts::queue : 0u) |
                (view.fence ? NrRecordingEpochFacts::fence : 0u) | (view.list ? NrRecordingEpochFacts::list : 0u) |
                (view.allocator ? NrRecordingEpochFacts::allocator : 0u);
            if (!TryBorrowRecordingEpochFacts(this, m_ContextLifetimeEpoch, facts, view.facts))
                return false;
            out = view;
            return true;
        }

        uint64_t GetObservedLifetimeEpoch() const noexcept {
            return m_ContextLifetimeEpoch;
        }

        // Caller has already obtained the actual list and holds the audited outer
        // serialization lease through Submit. This scalar observation reserves nothing.
        bool TryObserveNextSubmission(uint64_t& contextLifetime, uint64_t& nextValue) {
            contextLifetime = nextValue = 0;
            std::unique_lock lock(m_CommandListPoolMutex);
            return virtualdesktop_openxr::nr_capture::observeProspective(
                m_ContextLifetimeEpoch, m_CompletionFenceValue, contextLifetime, nextValue);
        }

        uint64_t SubmitCommandList(CommandList CommandList,
                                   vdxrfg::pair_capture::nr::NrSubmissionToken* observation = nullptr,
                                   virtualdesktop_openxr::nr_capture::NrWarmSubmissionFacts* warmFacts = nullptr) {
            // End only the scalar call observation, including original failure paths.
            // Construct before the pool lock so its destruction cannot run under it.
            struct EndObservation {
                vdxrfg::pair_capture::nr::NrSubmissionToken* token;
                ~EndObservation() noexcept {
                    if (token)
                        token->endCall();
                }
            } endObservation{observation};
            std::unique_lock lock(m_CommandListPoolMutex);

            // Scalar-only tees under the original pool lock. The caller must arm
            // and reconcile outside this lock; original callers use nullptr.
            using vdxrfg::pair_capture::nr::NrSubmissionKey;
            const auto makeActualKey = [&](const D3D12Utils::CommandList& actual) noexcept -> NrSubmissionKey {
                return {reinterpret_cast<uint64_t>(m_CommandQueue.Get()),
                        reinterpret_cast<uint64_t>(m_CompletionFence.Get()),
                        reinterpret_cast<uint64_t>(actual.Commands.Get()),
                        reinterpret_cast<uint64_t>(actual.Allocator.Get()),
                        actual.m_RecordingFacts.nrRecordingEpoch};
            };
            const auto actualLocal = (observation || warmFacts) ? makeActualKey(CommandList) : NrSubmissionKey{};
            if (warmFacts) {
                warmFacts->context = reinterpret_cast<uint64_t>(this);
                warmFacts->contextLifetime = m_ContextLifetimeEpoch;
            }
            const auto closeResult = CommandList.Commands->Close();
            if (observation)
                observation->recordClose(actualLocal, closeResult);
            if (warmFacts)
                warmFacts->calls.close = {true, false, actualLocal, closeResult};
            CHECK_HRESULT(closeResult, "CommandList.Commands->Close()");
            CommandList.CompletedFenceValue = m_CompletionFenceValue + 1;
            // Own submitted work before Execute: retaining it must not allocate after the GPU can use it.
            m_PendingCommandList.push_back(std::move(CommandList));
            auto& pending = m_PendingCommandList.back();
            m_CompletionFenceValue = pending.CompletedFenceValue;
            const auto actualPending = (observation || warmFacts) ? makeActualKey(pending) : NrSubmissionKey{};
            if (observation)
                observation->recordExecute(actualPending);
            if (warmFacts) {
                warmFacts->calls.executeObservations = 1;
                warmFacts->calls.lastExecuteActual = actualPending;
            }
            // TODO: This call goes through the Detoured version, which causes unwanted tracing.
            m_CommandQueue->ExecuteCommandLists(1,
                                                reinterpret_cast<ID3D12CommandList**>(pending.Commands.GetAddressOf()));
            const auto signalResult = m_CommandQueue->Signal(m_CompletionFence.Get(), m_CompletionFenceValue);
            if (observation)
                observation->recordSignal(actualPending, m_CompletionFenceValue, signalResult);
            if (warmFacts)
                warmFacts->calls.original = {true, false, false, actualPending, m_CompletionFenceValue, signalResult};
            CHECK_HRESULT(signalResult, "m_CommandQueue->Signal(m_CompletionFence.Get(), m_CompletionFenceValue)");
            m_SignaledFenceValue = m_CompletionFenceValue;

            return m_CompletionFenceValue;
        }

        // Drain the captured submission boundary. Callers retiring resources must exclude new submissions.
        void Flush() {
            uint64_t fenceValue;
            {
                std::unique_lock lock(m_CommandListPoolMutex);
                // A successful prior drain is also teardown proof. Do not query
                // a device that may have been removed after that completed boundary.
                if (m_DrainedFenceValue >= m_CompletionFenceValue)
                    return;
                if (m_SignaledFenceValue < m_CompletionFenceValue) {
                    // A failed Submit signal cannot be published to consumers. A later queue signal drains its work.
                    const auto recoveryValue = m_CompletionFenceValue + 1;
                    CHECK_HRCMD(m_CommandQueue->Signal(m_CompletionFence.Get(), recoveryValue));
                    m_CompletionFenceValue = m_SignaledFenceValue = recoveryValue;
                }
                fenceValue = m_CompletionFenceValue;
            }
            if (fenceValue && !IsCommandListCompleted(fenceValue)) {
                winrt::handle handle;
                // Each wait must own its event; named auto-reset events can wake an unrelated queue.
                *handle.put() = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
                CHECK_MSG(handle.get(), "Failed to create completion event");
                CHECK_HRCMD(m_CompletionFence->SetEventOnCompletion(fenceValue, handle.get()));
                CHECK_MSG(WaitForSingleObject(handle.get(), INFINITE) == WAIT_OBJECT_0, "Completion wait failed");
                // Device removal may wake an event without proving normal command completion.
                CHECK_MSG(IsCommandListCompleted(fenceValue), "Completion fence did not reach the requested value");
            }
            std::unique_lock lock(m_CommandListPoolMutex);
            if (m_DrainedFenceValue < fenceValue)
                m_DrainedFenceValue = fenceValue;
        }

        bool IsCommandListCompleted(uint64_t CompletedFenceValue) {
            const auto completedValue = m_CompletionFence->GetCompletedValue();
            CHECK_MSG(completedValue != UINT64_MAX, "Command completion device was removed");
            return completedValue >= CompletedFenceValue;
        }

        ID3D12Fence* GetCompletionFence() const {
            return m_CompletionFence.Get();
        }

        ID3D12CommandQueue* GetCommandQueue() const {
            return m_CommandQueue.Get();
        }

      private:
        inline static std::atomic<uint64_t> s_NextContextLifetime{1}, s_NextListObject{1}, s_NextAllocatorObject{1},
            s_NextRecording{1};
        uint64_t m_ContextLifetimeEpoch{};
        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12CommandQueue> m_CommandQueue;

        std::mutex m_CommandListPoolMutex;
        std::deque<CommandList> m_AvailableCommandList;
        std::deque<CommandList> m_PendingCommandList;
        ComPtr<ID3D12Fence> m_CompletionFence;
        uint64_t m_CompletionFenceValue{0};
        uint64_t m_SignaledFenceValue{0};
        uint64_t m_DrainedFenceValue{0};

        const std::wstring m_DebugName;
    };

    class DescriptorHeap {
      public:
        DescriptorHeap(ID3D12Device* Device,
                       D3D12_DESCRIPTOR_HEAP_TYPE HeapType,
                       UINT NumDescriptors = 128u,
                       const std::wstring& DebugName = L"Unnamed")
            : m_Device(Device) {
            // Create the descriptor heap.
            D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
            descriptorHeapDesc.Type = HeapType;
            descriptorHeapDesc.NumDescriptors = NumDescriptors;
            if (HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            }
            CHECK_HRCMD(
                m_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(m_Heap.ReleaseAndGetAddressOf())));
            m_Heap->SetName((DebugName + L" Descriptor Heap").c_str());

            // Retrieve the heap base.
            m_CPUHandleStart = m_Heap->GetCPUDescriptorHandleForHeapStart();
            m_GPUHandleStart = m_Heap->GetGPUDescriptorHandleForHeapStart();
            m_DescriptorSize = m_Device->GetDescriptorHandleIncrementSize(HeapType);

            // Populate the allocator.
            for (UINT i = 0; i < descriptorHeapDesc.NumDescriptors; i++) {
                m_AvailableDescriptor.push_back(i);
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDescriptor() {
            std::unique_lock lock(m_AvailableDescriptorMutex);

            CHECK_MSG(!m_AvailableDescriptor.empty(), "Out of descriptors");

            const UINT allocated = m_AvailableDescriptor.front();
            m_AvailableDescriptor.pop_front();
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CPUHandleStart, allocated, m_DescriptorSize);
        }

        void ReturnDescriptor(const D3D12_CPU_DESCRIPTOR_HANDLE& CpuHandle) {
            std::unique_lock lock(m_AvailableDescriptorMutex);

            const SIZE_T offset = (CpuHandle.ptr - m_CPUHandleStart.ptr) / m_DescriptorSize;
            m_AvailableDescriptor.push_back(static_cast<UINT>(offset));
        }

        D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptor(const D3D12_CPU_DESCRIPTOR_HANDLE& CpuHandle) const {
            const SIZE_T offset = (CpuHandle.ptr - m_CPUHandleStart.ptr) / m_DescriptorSize;
            return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_GPUHandleStart, static_cast<UINT>(offset), m_DescriptorSize);
        }

        ID3D12DescriptorHeap* GetDescriptorHeap() const {
            return m_Heap.Get();
        }

      private:
        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12DescriptorHeap> m_Heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_CPUHandleStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_GPUHandleStart{};
        UINT m_DescriptorSize{0};

        std::mutex m_AvailableDescriptorMutex;
        std::deque<UINT> m_AvailableDescriptor;
    };

} // namespace D3D12Utils
