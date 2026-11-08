// CPU-only ownership/timing regressions using a real recursive mutex.
#include "../virtualdesktop-openxr/sdk_gate_lease.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace virtualdesktop_openxr::detail;
namespace {
    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }
    struct Observer {
        inline static std::atomic<bool> on{false};
        inline static std::atomic<unsigned> enabledCalls{}, nowCalls{};
        inline static std::atomic<int64_t> ticks{100};
        inline static std::mutex sync;
        inline static std::condition_variable changed;
        inline static bool pauseBeforeAcquisition{}, beforeGate{}, proceed{};
        inline static std::vector<SdkGateTiming> events;
        static bool enabled() noexcept {
            ++enabledCalls;
            return on.load();
        }
        static int64_t now() noexcept {
            const auto stamp = ticks.fetch_add(1);
            if (++nowCalls == 1 && pauseBeforeAcquisition) {
                std::unique_lock lock(sync);
                beforeGate = true;
                changed.notify_all();
                changed.wait(lock, [] { return proceed; });
            }
            return stamp;
        }
        static void emit(const SdkGateTiming& event) noexcept {
            std::lock_guard lock(sync);
            events.push_back(event);
        }
        static void reset(bool tracing) {
            on = tracing;
            enabledCalls = nowCalls = 0;
            ticks = 100;
            pauseBeforeAcquisition = beforeGate = proceed = false;
            events.clear();
            events.reserve(4);
        }
    };
    using Lease = SdkGateLease<Observer>;
    static_assert(!std::is_copy_constructible_v<Lease> && !std::is_copy_assignable_v<Lease>);
    static_assert(std::is_move_constructible_v<Lease>);

    bool availableFromOtherThread(std::recursive_mutex& gate) {
        bool available{};
        std::thread probe([&] {
            available = gate.try_lock();
            if (available)
                gate.unlock();
        });
        probe.join();
        return available;
    }

    void disabledSnapshotAndEmptyLease() {
        Observer::reset(false);
        std::recursive_mutex gate;
        {
            Lease empty;
            require(!empty.tracing(), "empty lease cannot trace");
        }
        require(Observer::enabledCalls == 0 && Observer::nowCalls == 0 && Observer::events.empty(),
                "empty lease must not observe or emit");
        {
            Lease lease(gate, "disabled", 11);
            require(!lease.tracing() && !availableFromOtherThread(gate), "disabled lease still owns the gate");
            Observer::on = true;
            lease.enterCall();
            lease.leaveCall();
        }
        require(Observer::enabledCalls == 1 && Observer::nowCalls == 0 && Observer::events.empty(),
                "disabled acquisition snapshot must perform zero clock reads and emissions");
        require(availableFromOtherThread(gate), "disabled lease releases ownership");
    }

    void acquisitionAndCallAreSeparate() {
        Observer::reset(true);
        Observer::pauseBeforeAcquisition = true;
        std::recursive_mutex gate;
        std::unique_lock blocker(gate);
        std::thread worker([&] {
            Lease lease(gate, "blocked", 17);
            Observer::on = false; // The enabled acquisition snapshot survives a later toggle.
            lease.enterCall();
            lease.leaveCall();
        });
        bool reached{};
        {
            std::unique_lock lock(Observer::sync);
            reached = Observer::changed.wait_for(lock, std::chrono::seconds(2), [] { return Observer::beforeGate; });
            Observer::ticks = 200;
            Observer::proceed = true;
            Observer::changed.notify_all();
        }
        blocker.unlock();
        worker.join(); // Always release/join before an assertion can unwind.
        require(reached && Observer::events.size() == 1, "enabled acquisition must reach its pre-lock checkpoint");
        const auto& event = Observer::events.front();
        require(std::strcmp(event.staticOp, "blocked") == 0 && event.nativeId == 17 &&
                    event.gate == reinterpret_cast<uintptr_t>(&gate),
                "timing identifies the operation and actual gate");
        require(event.gateEnter == 100 && event.gateAcquired == 200 && event.callEnter == 201 &&
                    event.callReturn == 202 && event.releaseEnter == 203 && event.released == 204,
                "gate acquisition, operation and release must retain separate ordered timestamps");
        require(event.callEntered && event.callCompleted && Observer::enabledCalls == 1 && Observer::nowCalls == 6,
                "successful call emits once from the enabled acquisition snapshot");
        require(availableFromOtherThread(gate), "timed lease releases its gate");
    }

    void nestedAcquisitionsRetainOuterOwnership() {
        Observer::reset(true);
        std::recursive_mutex gate;
        {
            Lease outer(gate, "outer");
            {
                Lease inner(gate, "inner");
                inner.enterCall();
                inner.leaveCall();
            }
            require(Observer::events.size() == 1 && !availableFromOtherThread(gate),
                    "inner acquisition release cannot claim the recursive gate is globally free");
        }
        require(Observer::events.size() == 2 && availableFromOtherThread(gate), "outermost release frees the gate");
        const auto& inner = Observer::events[0];
        const auto& outer = Observer::events[1];
        require(std::strcmp(inner.staticOp, "inner") == 0 && std::strcmp(outer.staticOp, "outer") == 0 &&
                    inner.gate == outer.gate && outer.gateAcquired < inner.gateEnter &&
                    inner.released < outer.releaseEnter,
                "nested records describe each acquisition independently and in release order");
        require(!outer.callEntered && !outer.callCompleted, "an unentered outer call remains explicitly unentered");
    }

    void moveTransfersSingleOwnership() {
        Observer::reset(true);
        std::recursive_mutex gate;
        {
            Lease source(gate, "moved", 23);
            source.enterCall();
            {
                Lease destination(std::move(source));
                destination.leaveCall();
            }
            require(Observer::events.size() == 1 && availableFromOtherThread(gate),
                    "move destination owns and releases the single acquisition");
        }
        require(Observer::events.size() == 1 && Observer::nowCalls == 6 && Observer::enabledCalls == 1 &&
                    Observer::events[0].callCompleted && Observer::events[0].nativeId == 23,
                "moved-from destruction must not unlock or emit a second record");
    }

    void exceptionRecordsIncompleteCallAndReleases() {
        Observer::reset(true);
        std::recursive_mutex gate;
        try {
            Lease lease(gate, "throwing", 31);
            lease.enterCall();
            throw 31;
        } catch (int value) {
            require(value == 31, "operation exception propagates unchanged");
        }
        require(Observer::events.size() == 1 && availableFromOtherThread(gate),
                "exception unwinding releases ownership and emits once");
        const auto& event = Observer::events[0];
        require(event.callEntered && !event.callCompleted && event.callReturn == 0 &&
                    event.callEnter < event.releaseEnter && event.releaseEnter < event.released,
                "an exception records an entered but incomplete operation without inventing a return timestamp");
    }
} // namespace

int main() {
    try {
        disabledSnapshotAndEmptyLease();
        acquisitionAndCallAreSeparate();
        nestedAcquisitionsRetainOuterOwnership();
        moveTransfersSingleOwnership();
        exceptionRecordsIncompleteCallAndReleases();
        std::cout << "PASS: SDK gate lease ownership, timing, moves and exceptions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
