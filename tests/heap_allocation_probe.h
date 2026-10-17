#pragma once

// The fixtures arm this probe only around warmed runtime calls. Counters are
// thread local so backend/driver workers cannot contaminate the measurement.
namespace allocation_test {
    static DWORD allocationThreadId = 0;
    static thread_local bool countHeapAllocations = false;
    static thread_local size_t heapAllocations = 0;
    using AllocateHeap = void*(NTAPI*)(HANDLE, ULONG, SIZE_T);
    inline AllocateHeap originalAllocateHeap = nullptr;
    inline void* NTAPI countAllocation(HANDLE heap, ULONG flags, SIZE_T bytes) {
        // Do not touch executable TLS from driver worker startup/teardown.
        // Only the fixture thread is ever observed.
        if (GetCurrentThreadId() == allocationThreadId && countHeapAllocations)
            ++heapAllocations;
        return originalAllocateHeap(heap, flags, bytes);
    }

    struct AllocationObserver {
        bool hookAttached = false;
        AllocationObserver() {
            allocationThreadId = GetCurrentThreadId();
            originalAllocateHeap =
                reinterpret_cast<AllocateHeap>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAllocateHeap"));
            if (!originalAllocateHeap || DetourTransactionBegin() != NO_ERROR)
                throw std::runtime_error("Allocation detour begin failed");
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&originalAllocateHeap), countAllocation);
            if (updated || attached) {
                DetourTransactionAbort();
                throw std::runtime_error("Allocation detour attach failed");
            }
            if (DetourTransactionCommit() != NO_ERROR)
                throw std::runtime_error("Allocation detour commit failed");
            hookAttached = true;
        }
        void detach() {
            countHeapAllocations = false;
            if (!hookAttached)
                return;
            const auto begun = DetourTransactionBegin();
            const auto updated = DetourUpdateThread(GetCurrentThread());
            const auto detached = DetourDetach(reinterpret_cast<PVOID*>(&originalAllocateHeap), countAllocation);
            const auto committed = DetourTransactionCommit();
            if (begun || updated || detached || committed)
                std::terminate();
            hookAttached = false;
        }
        ~AllocationObserver() {
            detach();
        }
    };
} // namespace allocation_test
