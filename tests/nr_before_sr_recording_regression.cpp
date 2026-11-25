#include "../virtualdesktop-openxr/NrBeforeSrRecording.h"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace virtualdesktop_openxr::nr_before_sr;

namespace {
    constexpr uintptr_t queue = 100, first = 101, second = 102, allocatorA = 201, allocatorB = 202;

    void require(bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    }
    void bind(RecordingLedger& ledger, uintptr_t list, uintptr_t allocator) {
        require(ledger.bind(list, allocator, std::make_shared<int>(1)), "verified successful list Reset");
    }
    void resetAllocator(RecordingLedger& ledger, uintptr_t allocator, bool success = true) {
        const auto epoch = ledger.beforeAllocatorReset(allocator);
        ledger.afterAllocatorReset(allocator, epoch, success);
    }

    void unobservedQueueAndDeferredDestruction() {
        RecordingLedger ledger(queue);
        require(!ledger.attach(first, std::make_shared<int>(1)), "unobserved creation allocator must decline");
        require(!ledger.bind(first, allocatorA, {}), "addresses need a strong identity lease");
        bind(ledger, first, allocatorA);
        const auto modelThread = std::this_thread::get_id();
        std::thread::id destroyedOn;
        auto owner = std::shared_ptr<int>(new int(1), [&](int* p) {
            delete p;
            destroyedOn = std::this_thread::get_id();
            (void)ledger.empty(); // reentering the ledger must be safe.
        });
        std::weak_ptr<int> probe = owner;
        auto original = ledger.attach(first, owner);
        owner.reset();
        require(bool(original), "observed allocator binding admits model work");
        // The game executes its list through an unhooked implementation, then resets
        // that list onto B while the GPU can still consume commands allocated from A.
        ledger.afterReset(original, false);
        require(ledger.current(first) == original, "failed Reset preserves binding");
        ledger.afterReset(original, true);
        bind(ledger, first, allocatorB);
        auto next = ledger.attach(first, std::make_shared<int>(2));
        ledger.afterReset(original, true);
        require(next != original && ledger.current(first) == next, "stale list Reset cannot end newer recording");
        resetAllocator(ledger, allocatorB);
        resetAllocator(ledger, allocatorA, false);
        ledger.afterAllocatorReset(allocatorA, ledger.beforeAllocatorReset(allocatorB), true);
        ledger.poll(64);
        ledger.drain();
        require(!probe.expired() && original->outcome() == RecordingOutcome::Pending,
                "wrong allocator, failed allocator Reset, list Reset and queue progress cannot retire");
        std::thread resetThread([&] { resetAllocator(ledger, allocatorA); });
        resetThread.join();
        require(!probe.expired(), "successful allocator Reset never destroys model owners in the Reset hook");
        ledger.drain();
        require(probe.expired() && destroyedOn == modelThread && original->outcome() == RecordingOutcome::Abandoned,
                "unconfirmed execution discards history only after correct allocator proof");
        require(ledger.empty(), "model drain retires every old owner");
    }

    void sessionRestart() {
        RecordingLedger ledger(queue);
        bind(ledger, first, allocatorA);
        auto owner = std::make_shared<int>(42);
        std::weak_ptr<int> retained = owner;
        auto prior = ledger.attach(first, owner);
        owner.reset();
        ledger.stop();
        require(!ledger.attach(first, std::make_shared<int>(1)), "stopped session rejects new recording");
        require(ledger.resume() && !retained.expired(), "resuming preserves replayable owners");
        bind(ledger, second, allocatorB);
        require(bool(ledger.attach(second, std::make_shared<int>(2))), "new session admits a fresh recording");
        auto submitted = ledger.beforeExecute(queue, &first, 1);
        require(bool(submitted), "prior session work still tracked");
        ledger.afterSignal(submitted, 1, true);
        ledger.poll(1);
        resetAllocator(ledger, allocatorA);
        ledger.drain();
        require(prior->outcome() == RecordingOutcome::Executed, "prior session submission retires normally");
        ledger.quarantine();
        require(!ledger.resume(), "quarantine survives a session restart");
    }

    void sharedAllocatorEpochs() {
        RecordingLedger ledger(queue);
        bind(ledger, first, allocatorA);
        bind(ledger, second, allocatorA);
        auto a = ledger.attach(first, std::make_shared<int>(1));
        auto b = ledger.attach(second, std::make_shared<int>(2));
        const uintptr_t pair[] = {first, second};
        auto both = ledger.beforeExecute(queue, pair, 2);
        require(bool(both), "multiple closed lists sharing an allocator");
        ledger.afterSignal(both, 1, true);
        ledger.poll(1);
        ledger.drain();
        require(a->outcome() == RecordingOutcome::Pending, "fenced lists still replay");
        auto oldEpoch = ledger.beforeAllocatorReset(allocatorA);
        ledger.afterAllocatorReset(allocatorA, oldEpoch, true);
        bind(ledger, first, allocatorA);
        auto next = ledger.attach(first, std::make_shared<int>(3));
        ledger.afterAllocatorReset(allocatorA, oldEpoch, true);
        ledger.drain();
        require(a->outcome() == RecordingOutcome::Executed && b->outcome() == RecordingOutcome::Executed &&
                    next->outcome() == RecordingOutcome::Pending && ledger.current(first) == next,
                "old allocator Reset retires both submitted lists and preserves the newer binding");
        resetAllocator(ledger, allocatorA);
        ledger.drain();
        require(ledger.empty(), "all epochs retired");
    }

    void arbitraryBatches() {
        RecordingLedger ledger(queue);
        bind(ledger, first, allocatorA);
        auto g = ledger.attach(first, std::make_shared<int>(1));
        std::vector<uintptr_t> batch(4096, second);
        batch.front() = 0;
        auto at = [&](size_t i) { return batch[i]; };
        require(!ledger.beforeExecute(queue + 1, batch.size(), at) && !ledger.quarantined(), "large unrelated batch");
        batch.back() = first;
        auto matched = ledger.beforeExecute(queue, batch.size(), at);
        require(bool(matched), "matching tail of arbitrary batch");
        ledger.afterSignal(matched, 9, true);
        ledger.poll(9);
        ledger.afterReset(g, true);
        ledger.drain();
        require(g->outcome() == RecordingOutcome::Pending, "closed and completed work still needs allocator proof");
        resetAllocator(ledger, allocatorA);
        ledger.drain();
        require(g->outcome() == RecordingOutcome::Executed, "observed successful game submission");
    }

    void observeOnlyRotation() {
        RecordingLedger ledger(queue);
        bool released = false;
        auto identity = std::shared_ptr<int>(new int(1), [&](int* p) {
            delete p;
            (void)ledger.current(first); // COM identity destruction may reenter.
            released = true;
        });
        require(ledger.bind(first, allocatorA, identity), "first observed list Reset");
        identity.reset();
        const auto original = ledger.current(first);
        for (unsigned i = 0; i < 160; ++i)
            bind(ledger, first, i % 2 ? allocatorA : allocatorB);
        require(released && original->outcome() == RecordingOutcome::Abandoned &&
                    ledger.current(first)->allocator == allocatorA,
                "stale observation leases are reusable without a model-thread drain");

        auto model = ledger.attach(first, std::make_shared<int>(2));
        require(bool(model), "initially disabled session can later attach real work");
        for (uintptr_t i = 0; i < 160; ++i)
            bind(ledger, second + i, allocatorB);
        require(!ledger.current(second) && !ledger.attach(second, std::make_shared<int>(3)) &&
                    ledger.current(second + 159) && ledger.current(first) == model,
                "bound ownerless observations under churn without displacing GPU work");
        resetAllocator(ledger, allocatorA);
        resetAllocator(ledger, allocatorB);
        ledger.drain();
        require(ledger.empty(), "rotated bindings and model retire normally");
    }

    void pressureAndQuarantine() {
        RecordingLedger ledger(queue);
        size_t bound = 0;
        while (bound < 1024 && ledger.bind(first + bound, allocatorA, std::make_shared<int>(1))) {
            require(bool(ledger.attach(first + bound, std::make_shared<int>(2))), "fill slots with GPU leases");
            ++bound;
        }
        require(bound && bound < 1024 && !ledger.quarantined(), "bounded pre-record admission");
        resetAllocator(ledger, allocatorA);
        ledger.drain();
        require(ledger.empty(), "released identities free capacity");
        bind(ledger, first, allocatorA);
        auto g = ledger.attach(first, std::make_shared<int>(2));
        ledger.stop();
        require(!ledger.attach(second, std::make_shared<int>(3)), "stop disables new work");
        auto submitted = ledger.beforeExecute(queue, &first, 1);
        ledger.afterSignal(submitted, 1, false);
        resetAllocator(ledger, allocatorA);
        ledger.drain();
        require(ledger.quarantined() && g->outcome() == RecordingOutcome::Quarantined && !ledger.empty(),
                "failed signal never certifies model ownership");
    }

    void quarantineOnUnreliableCompletion() {
        for (unsigned fault = 0; fault < 5; ++fault) {
            std::weak_ptr<int> probe;
            {
                RecordingLedger ledger(queue);
                bind(ledger, first, allocatorA);
                auto owner = std::make_shared<int>(42);
                probe = owner;
                auto g = ledger.attach(first, owner);
                owner.reset();
                if (fault == 0) {
                    ledger.poll(UINT64_MAX);
                } else if (fault == 1) {
                    auto submit = ledger.beforeExecute(queue, &first, 1);
                    ledger.afterSignal(submit, 20, true);
                    ledger.poll(20);
                    bind(ledger, second, allocatorB);
                    require(bool(ledger.attach(second, std::make_shared<int>(1))), "attach distinct submitted list");
                    auto secondSubmit = ledger.beforeExecute(queue, &second, 1);
                    ledger.afterSignal(secondSubmit, 19, true);
                } else if (fault == 2) {
                    auto firstSubmit = ledger.beforeExecute(queue, &first, 1);
                    require(bool(firstSubmit), "first execution accepted");
                    ledger.afterSignal(firstSubmit, 1, true);
                    require(!ledger.beforeExecute(queue, &first, 1), "same recorded list replay quarantined");
                } else if (fault == 3) {
                    const uintptr_t duplicate[] = {first, first};
                    require(!ledger.beforeExecute(queue, duplicate, 2), "duplicate entries in one batch quarantined");
                } else {
                    require(!ledger.beforeExecute(queue + 1, &first, 1), "owned work on a foreign queue quarantined");
                }
                resetAllocator(ledger, allocatorA);
                ledger.drain();
                require(g->outcome() == RecordingOutcome::Quarantined && !probe.expired(), "unreliable completion");
            }
            require(!probe.expired(), "unsafe teardown retains SDK and GPU owners");
        }
    }
} // namespace

int main() {
    try {
        unobservedQueueAndDeferredDestruction();
        sessionRestart();
        sharedAllocatorEpochs();
        arbitraryBatches();
        observeOnlyRotation();
        pressureAndQuarantine();
        quarantineOnUnreliableCompletion();
        std::cout << "Pre-SR recording lifetime regression passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
