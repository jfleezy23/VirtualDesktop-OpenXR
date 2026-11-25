#include "../virtualdesktop-openxr/NrBeforeSrPipeline.h"

#include <iostream>
#include <stdexcept>

using namespace virtualdesktop_openxr::nr_before_sr;

namespace {
    struct Counted {
        ULONG refs{};
        ULONG AddRef() noexcept {
            return ++refs;
        }
        ULONG Release() noexcept {
            return --refs;
        }
    };
    struct Pipeline : Counted {};
    struct List : Counted {
        Pipeline* current{};
        unsigned sets{};
        void SetPipelineState(Pipeline* pso) noexcept {
            current = pso;
            ++sets;
        }
    };
    void require(bool value, const char* text) {
        if (!value)
            throw std::runtime_error(text);
    }

    void recordingAndLifetime() {
        List left, right, unsupported;
        Pipeline initial, later, privateModel;
        unsigned qualifications = 0;
        const auto qualify = [&](List* list) {
            ++qualifications;
            return list != &unsupported;
        };
        PipelineStateTracker<List, Pipeline> tracker;
        tracker.set(&left, &later);
        require(!tracker.capture(&left, 1), "a setter before an observed reset cannot adopt a list");
        tracker.reset(&unsupported, 8, &initial, qualify);
        tracker.set(&unsupported, &later);
        require(!tracker.capture(&unsupported, 8) && !unsupported.refs, "unqualified interface cannot enter tracking");
        tracker.reset(&left, 1, nullptr, qualify);
        require(!tracker.capture(&left, 1), "the implicit default PSO has no replayable pointer");
        tracker.set(&left, &later);
        require(left.refs == 1 && later.refs == 1 && tracker.capture(&left, 1).Get() == &later,
                "verified list and last host PSO are held strongly");
        tracker.reset(&right, 2, &initial, qualify);
        require(tracker.capture(&right, 2).Get() == &initial && !tracker.capture(&right, 1),
                "initial PSO belongs to its exact list and recording");
        tracker.reset(&left, 3, &initial, qualify);
        require(!later.refs && !tracker.capture(&left, 1) && tracker.capture(&left, 3).Get() == &initial,
                "successful reset replaces old state and generation");
        tracker.reset(&left, 1, &later, qualify);
        require(tracker.capture(&left, 3).Get() == &initial && qualifications == 3,
                "old generation cannot replace a live entry and retained lists avoid requalification");

        auto replayOwner = tracker.capture(&left, 3);
        tracker.set(&left, &privateModel);
        require(replayOwner.Get() == &initial && tracker.capture(&left, 3).Get() == &privateModel,
                "private recording cannot overwrite the strong capture");
        tracker.set(&left, replayOwner.Get());
        require(!privateModel.refs && tracker.capture(&left, 3).Get() == &initial, "restore rebinds the captured PSO");
        tracker.set(&left, nullptr);
        require(!tracker.capture(&left, 3) && tracker.capture(&right, 2).Get() == &initial,
                "opaque state changes invalidate only the affected list");
        tracker.reset(&right, 4, nullptr, qualify);
        require(initial.refs == 1, "the replayable recording alone still owns the restored PSO");
        replayOwner.Reset();
        require(!initial.refs, "last replay owner releases the old PSO");
        tracker.set(&left, &later);
        require(tracker.capture(&left, 3).Get() == &later, "explicit legacy bind recovers from opaque state");
    }

    void boundedAdoption() {
        List a, b, c;
        Pipeline first, second, third;
        auto qualify = [](List*) { return true; };
        {
            PipelineStateTracker<List, Pipeline, 2> tracker;
            tracker.reset(&a, 1, &first, qualify);
            tracker.reset(&b, 2, &second, qualify);
            tracker.reset(&c, 3, &third, qualify);
            require(!tracker.capture(&a, 1) && !first.refs && !a.refs, "eviction releases state and identity together");
            tracker.set(&a, &first);
            require(!tracker.capture(&a, 1), "late setter on evicted list cannot resurrect the old identity");
            require(tracker.capture(&b, 2).Get() == &second && tracker.capture(&c, 3).Get() == &third,
                    "capacity limits do not cross list states");
            tracker.reset(&a, 4, nullptr, qualify);
            require(!tracker.capture(&a, 1) && !tracker.capture(&a, 4) && !b.refs,
                    "readopted list starts with the new exact epoch");
        }
        require(!a.refs && !c.refs && !second.refs && !third.refs, "teardown releases remaining CPU owners");
    }

    void restoreBeforeNative() {
        List list;
        Pipeline host, model;
        for (int path = 0; path != 4; ++path) {
            list.SetPipelineState(&host);
            const auto before = list.sets;
            auto record = [&] {
                auto restore = RestorePipelineOnExit(&list, &host);
                list.SetPipelineState(&model); // Private CreateFeature may write before failing.
                if (path == 0)
                    return false;
                list.SetPipelineState(&model); // Encode and model evaluation.
                if (path == 1)
                    return false;
                if (path == 2)
                    throw std::runtime_error("fixture");
                return true;
            };
            bool recorded = false, threw = false;
            try {
                recorded = record();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            require(threw == (path == 2) && recorded == (path == 3), "expected private recording outcome");
            require(list.current == &host && list.sets == before + (path == 0 ? 2u : 3u),
                    "caller sees the exact host PSO restored once before native SR");
        }
    }
} // namespace

int main() try {
    recordingAndLifetime();
    boundedAdoption();
    restoreBeforeNative();
    std::cout << "Before-SR pipeline tracking passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
