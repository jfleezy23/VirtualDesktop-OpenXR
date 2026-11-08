#pragma once
#include "pair_capture_nr_abi.h"
#include "pair_capture_nr_postfx_validate.h"
#include "PairCaptureNrOwner.h"
#include <array>
#include <atomic>
#include <mutex>

namespace vdxrfg::pair_capture::nr {
    struct RequestBinding {
        uint64_t attempt{}, mode{}, nonce[2]{}, pid{}, creation{}, session[2]{};
        uint64_t context{}, lifetime{}, history{}, minimumSerial{}, minimumTime{}, budget{};
    };
    // Bounded same-process custody. Numeric metadata is never a worker object graph.
    class Receiver final {
      public:
        bool bind(const RequestBinding&) noexcept;
        bool attachSource(const NrPacketV1&, const NrSourceWitnessV1&) noexcept;
        const NrPacketV1* inspect(unsigned frame) const noexcept;
        const NrSourceWitnessV1* source(unsigned frame) const noexcept;
        NrAckV1 acknowledgement(unsigned frame) const noexcept;
        const NrPostFxSidecarV1* postFx(unsigned frame) const noexcept;
        void fail() noexcept;
        bool artifactPublished() noexcept;
        uint32_t transact(const NrPacketV1*, NrAckV1*, const NrPostFxSidecarV1* = nullptr) noexcept;
        bool committed(unsigned frame) const noexcept;
        const NrPacketV1* beginRead(unsigned frame) noexcept;
        void* borrow(unsigned frame, uint64_t refIndex, uint64_t interfaceKind) const noexcept;
        bool completed(unsigned frame, unsigned group, const NrSubmissionKey& actual, uint64_t value) noexcept;
        bool closeReaders(unsigned frame) noexcept;
        bool endWorker(unsigned frame) noexcept;
        // kind1 reserves Map's single QI reference; kind2 reserves the two
        // GetDevice/As references for an output-fence import. No unnamed slots.
        bool claimScratch(unsigned frame, unsigned kind) noexcept;
        bool returnScratch(unsigned frame, unsigned kind) noexcept;
        // Call only after the FG product/copy/output-Wait/retirement gate also
        // permits it. This owner independently enforces all transferred host work.
        bool release(unsigned frame) noexcept;

      private:
        enum class Phase { empty, receiving, committed, releasing, settled, uncertain };
        struct Frame {
            NrPacketV1 packet{};
            NrPostFxSidecarV1 postFx{};
            NrAckV1 ack{};
            NrSourceWitnessV1 source{};
            uint64_t sourceTransfer{};
            std::array<NrGroupId, 3> groups{};
            std::array<IUnknown*, 128> held{};
            std::array<uint8_t, 32> digest{};
            Phase phase{Phase::empty};
            uint64_t epoch{};
            std::array<uint64_t, 3> completion{};
            bool workerStarted{}, workerEnded{}, readersClosed{};
            unsigned scratch{};
        };
        bool acquire(Frame&, const NrPacketV1&) noexcept;
        mutable std::mutex mutex_;
        std::array<Frame, 2> frames_{};
        NrWorkOwner owner_;
        std::atomic_flag transaction_ = ATOMIC_FLAG_INIT;
        uint64_t nextEpoch_{};
        bool negotiated_{};
        bool terminal_{};
        RequestBinding binding_{};
        bool bound_{};
        bool artifactPublished_{};
    };
    bool packetDigest(const NrPacketV1&, std::array<uint8_t, 32>&) noexcept;
    bool transferDigest(const NrPacketV1&, const NrPostFxSidecarV1&, std::array<uint8_t, 32>&) noexcept;
} // namespace vdxrfg::pair_capture::nr
