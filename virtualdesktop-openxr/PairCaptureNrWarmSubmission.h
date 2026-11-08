#pragma once
#include "PairCaptureNrOwner.h"
#include "../nr-frame-generation/include/pair_capture_nr_abi.h"
namespace virtualdesktop_openxr::nr_capture {
    // Caller-owned raw scalar observations. This is not an owner, reservation or
    // completion proof. The caller clears it before entering the original pool.
    struct NrWarmSubmissionFacts {
        uint64_t context{}, contextLifetime{};
        vdxrfg::pair_capture::nr::NrCallFacts calls{};
    };
    inline bool warmSubmissionComplete(const NrWarmSubmissionFacts& f,
                                       uint64_t context,
                                       uint64_t lifetime,
                                       const vdxrfg::pair_capture::nr::NrSubmissionKey& borrowed,
                                       uint64_t returnedValue) noexcept {
        using vdxrfg::pair_capture::nr::sameSubmission;
        return context && lifetime && lifetime != UINT64_MAX && f.context == context && f.contextLifetime == lifetime &&
               borrowed.queue && borrowed.fence && borrowed.list && borrowed.allocator && borrowed.recordingEpoch &&
               borrowed.recordingEpoch != UINT64_MAX && returnedValue && returnedValue != UINT64_MAX &&
               f.calls.close.observed && f.calls.close.result >= 0 && sameSubmission(f.calls.close.actual, borrowed) &&
               f.calls.executeObservations == 1 && sameSubmission(f.calls.lastExecuteActual, borrowed) &&
               f.calls.original.observed && !f.calls.original.recovery && f.calls.original.result >= 0 &&
               f.calls.original.value == returnedValue && sameSubmission(f.calls.original.actual, borrowed);
    }
    // Candidate companion meaning for Close/Execute/Signal only. Other call kinds
    // retain their own flags semantics (for example the NT shared-handle variant).
    inline constexpr uint64_t nrSubmitQpcEnclosing = UINT64_C(0x1);
    inline bool projectNrSubmission(const NrWarmSubmissionFacts& facts,
                                    const vdxrfg::pair_capture::nr::NrSubmissionKey& borrowed,
                                    uint64_t returned,
                                    uint64_t device,
                                    uint64_t module,
                                    uint64_t begin,
                                    uint64_t end,
                                    uint64_t thread,
                                    NrCallV1 (&rows)[3]) noexcept {
        for (auto& row : rows)
            row = {};
        if (!device || module >= 8 || !begin || end < begin || !thread ||
            !warmSubmissionComplete(facts, facts.context, facts.contextLifetime, borrowed, returned))
            return false;
        const auto populate = [&](NrCallV1& row,
                                  uint64_t kind,
                                  uint64_t resultType,
                                  int32_t result,
                                  const vdxrfg::pair_capture::nr::NrSubmissionKey& actual) noexcept {
            row.kind = kind;
            row.resultType = resultType;
            row.resultBits = static_cast<uint32_t>(result);
            row.classifiedSuccess = resultType != NR_VOID;
            row.beginObserved = row.endObserved = 1;
            row.eye = 2;
            row.qpcBegin = begin;
            row.qpcEnd = end;
            row.thread = thread;
            row.flags = nrSubmitQpcEnclosing;
            row.deviceAddress = device;
            row.moduleIndex = module;
            row.queueAddress = actual.queue;
            row.fenceAddress = actual.fence;
            row.listAddress = actual.list;
            row.allocatorAddress = actual.allocator;
            row.recordingEpoch = actual.recordingEpoch;
            row.contextAddress = facts.context;
            row.contextLifetimeEpoch = facts.contextLifetime;
            row.observedMask = NR_CALL_OBS;
        };
        populate(rows[0], NR_CLOSE, NR_HRESULT, facts.calls.close.result, facts.calls.close.actual);
        // The actual original Submit executes one list. Its later observed Signal
        // proves that the void Execute call returned; it does not prove completion.
        populate(rows[1], NR_EXECUTE, NR_VOID, 0, facts.calls.lastExecuteActual);
        rows[1].actual.actualCountArgument = 1;
        populate(rows[2], NR_SIGNAL, NR_HRESULT, facts.calls.original.result, facts.calls.original.actual);
        rows[2].signalValue = facts.calls.original.value;
        return true;
    }

} // namespace virtualdesktop_openxr::nr_capture
