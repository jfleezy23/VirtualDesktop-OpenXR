#pragma once
#include "PairCaptureNrWarmLayouts.h"
#include "PairCaptureNrSources.h"
#include "PairCaptureNrEngineFacts.h"
#include "PairCaptureNrReturnGuard.h"
#include "PairCaptureNrPipelineFacts.h"
#include "PairCaptureNrPacketBuilder.h"
#include "PairCaptureNrModulePacket.h"
#include "PairCaptureNrEnginePacket.h"
#include "PairCaptureNrDrawPacket.h"
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    struct CaptureRawSource {
        const void* swapchain{};
        uint32_t releasedIndex{}, selectedArraySlice{};
        uint64_t role{}, eye{};
        ID3D11Texture2D* texture{};
        SnapshotRoleMetadata metadata{};
    };
    struct CapturePreflightInput {
        uint64_t serial{}, displayTime{}, nrContextLifetime{};
        const void* nrContext{};
        ID3D11Device5* producerDevice{};
        ID3D11DeviceContext4* producerContext{};
        ID3D11Device5* applicationDevice{};
        ID3D11DeviceContext4* applicationContext{};
        ID3D12Device* nrDevice{};
        ID3D12Device* fgDevice{};
        ID3D11Fence* originalInputProducerFence{};
        ID3D12Fence* originalInputAlias{};
        ID3D12Fence* originalNrFence{};
        ID3D11Fence* originalOutputAlias{};
        bool upscaling{}, sharpening{};
        NrRawSettingsV1 rawSettings{};
        uint64_t entryQpc{}, leaseQpc{}, frameThread{}, stackCount{}, stack[48]{};
        CaptureRawSource raw[4]{};
    };
    struct CaptureNrInput {
        const void* pipelineObject{};
        vdxrfg::pair_capture::nr::NrRecordingOwnership recording{};
        vdxrfg::pair_capture::nr::NrPipelineOwnership pipeline{};
        PipelineProgramFacts programs{};
        ID3D12Resource* nativeDepth[2]{};
        uint64_t prospectiveContextLifetime{}, prospectiveSignalValue{};
    };
    struct CapturePostFxBinding {
        uint64_t kind{}, eye{};
        ID3D11ComputeShader* shader{};
        ID3D11Buffer* buffer{};
        ID3D11ShaderResourceView* srv{};
        ID3D11UnorderedAccessView* uav{};
        ID3D11SamplerState* sampler{};
    };
    struct CapturePostFxInput {
        ID3D11Texture2D* easu[2]{};
        ID3D11Texture2D* cas[2]{};
        ID3D11Texture2D* finalColour[2]{};
        ID3D11Texture2D* canonical{};
        uint64_t canonicalLifetime{}, fgContext{};
        CapturePostFxBinding bindings[4]{};
        uint32_t bindingCount{};
        bool originalGlobalUnbindReturned{};
    };
    struct PrivateFencePreflight {
        uint64_t producerDevice{}, applicationDevice{}, fgDevice{};
        uint64_t producerFence{}, applicationFence{}, fgFence{}, handle{};
        uint64_t createBegin{}, createEnd{}, exportBegin{}, exportEnd{};
        uint64_t appImportBegin{}, appImportEnd{}, fgImportBegin{}, fgImportEnd{}, thread{};
        int32_t createResult{}, exportResult{}, appImportResult{}, fgImportResult{};
        uint64_t producerLifetime{}, applicationLifetime{}, fgLifetime{};
        bool createObserved{}, exportObserved{}, appImportObserved{}, fgImportObserved{}, handleClosed{};
    };
    // Caller-serialized owner of the complete unexposed two-frame allocation plan.
    // prepareResources does not arm a group, issue a command, or claim a complete
    // transferable packet. Groups must acquire exact refs before initial refs drop.
    class CaptureEngine final {
      public:
        using BudgetCall = uint32_t(NR_PRIVATE_CALL*)(NrFgReadbackBudgetV1*) noexcept;
        using SubmitCall = uint32_t(NR_PRIVATE_CALL*)(const NrPacketV1*, const NrPostFxSidecarV1*, NrAckV1*) noexcept;
        using SourceCall = uint32_t(NR_PRIVATE_CALL*)(const NrPacketV1*, const NrSourceWitnessV1*) noexcept;
        bool attestWarmModules() noexcept;
        void releaseModuleFiles() noexcept;
        bool prepareResources(const CapturePreflightInput&) noexcept;
        bool acquireRaw(uint32_t frame, const CapturePreflightInput&, SourceReturnGuard&) noexcept;
        bool acquireNr(uint32_t frame, const CaptureNrInput&) noexcept;
        bool captureNrRole(
            uint32_t frame, uint64_t role, uint64_t eye, SourceReturnGuard&, const SnapshotRoleMetadata&) noexcept;
        vdxrfg::pair_capture::nr::NrSubmissionToken* beginNrSubmission(uint32_t frame) noexcept;
        bool finishNrSubmission(uint32_t frame,
                                const NrWarmSubmissionFacts&,
                                uint64_t returnedValue,
                                SourceReturnGuard&) noexcept;
        bool acquirePostFx(uint32_t frame, const CapturePostFxInput&, SourceReturnGuard&) noexcept;
        bool capturePostFxRole(
            uint32_t frame, uint64_t role, uint64_t eye, SourceReturnGuard&, const SnapshotRoleMetadata&) noexcept;
        bool matchesFinalColour(uint32_t frame, ID3D11Texture2D* const actual[2]) const noexcept;
        bool observeFinalDimensions(uint32_t frame, uint64_t width, uint64_t height) noexcept;
        bool finishFrameTransfer(uint32_t frame, SourceReturnGuard&) noexcept;
        bool postFxReady(uint32_t frame) const noexcept {
            return frame < 2 && postFxAcquired_[frame];
        }
        bool resourcesPrepared() const noexcept {
            return prepared_;
        }
        uint32_t plannedFrame(uint64_t serial) const noexcept {
            return prepared_ && serial >= firstSerial_ && serial - firstSerial_ < 2
                       ? static_cast<uint32_t>(serial - firstSerial_)
                       : UINT32_MAX;
        }
        uint32_t selectedFrame(uint64_t serial) const noexcept {
            const auto frame = plannedFrame(serial);
            return frame < 2 && groups_[frame][0].id.valid() ? frame : UINT32_MAX;
        }
        const NrBudgetV1& budget() const noexcept {
            return budget_;
        }
        const NrFgReadbackBudgetV1& fgBudgetFacts() const noexcept {
            return fgBudget_;
        }
        const PrivateFencePreflight& fenceFacts() const noexcept {
            return fenceFacts_;
        }

      private:
        ID3D11Texture2D* postFxRoleSource(uint32_t frame, uint64_t role, uint64_t eye) const noexcept;
        ID3D12Resource* nrRoleSource(uint32_t frame, uint64_t role, uint64_t eye) const noexcept;
        bool observeRawCohort(uint32_t frame, const CapturePreflightInput&) noexcept;
        bool createPrivateFence(const CapturePreflightInput&) noexcept;
        bool prepareModules() noexcept;
        bool completePreflight(const CapturePreflightInput&) noexcept;
        bool projectFrameIdentity(uint32_t frame, SourceReturnGuard&) noexcept;
        bool projectFrameGroups(uint32_t frame, SourceReturnGuard&) noexcept;
        bool projectFrameControls(uint32_t frame, SourceReturnGuard&) noexcept;
        bool projectFrameRoles(uint32_t frame) noexcept;
        bool projectFrameSdk(uint32_t frame) noexcept;
        bool appendControl(uint32_t frame, const NrCallV1&, uint64_t& index) noexcept;
        bool appendAlias(uint32_t frame, const NrAliasV1&, const NrCallV1&, const NrCallV1&, uint64_t& index) noexcept;
        bool rejectUnexposed(uint64_t reason) noexcept;
        SnapshotStore snapshots_{};
        WarmFrameLayout warm_{};
        NrReadyV1 ready_{};
        NrHostStatusV1 status_{};
        NrFgReadbackBudgetV1 fgBudget_{};
        NrBudgetV1 budget_{};
        NrSourceWitnessV1 sources_[2]{};
        SourceTextureFacts rawFacts_[2][4]{};
        uint32_t rawCopyIndex_[2][4]{};
        uint64_t rawDescriptorEpoch_[2]{}, sourceGeneration_[2]{};
        NrRefV1 refs_[2][89]{};
        uint32_t refCounts_[2]{};
        OwnedGroupBoundary groups_[2][3]{};
        vdxrfg::pair_capture::nr::NrGroupIdentity groupIdentities_[2][3]{};
        CapturePreflightInput preparedInput_{}, frameInputs_[2]{};
        NrCallV1 luidCalls_[2]{};
        uint64_t luidLow_{}, luidHigh_{};
        CaptureNrInput nrInputs_[2]{};
        CapturePostFxInput postFxInputs_[2]{};
        bool postFxAcquired_[2]{}, postFxExposed_[2]{};
        vdxrfg::pair_capture::nr::NrSubmissionToken nrSubmission_[2]{};
        ResourceWriteFact roleWriters_[2][28]{};
        SnapshotRoleMetadata roleMetadata_[2][28]{};
        uint64_t roleWriterAlias_[2][28]{};
        bool nrAcquired_[2]{}, nrSubmitted_[2]{};
        NrOutputFenceFacts nrFenceFacts_{}, nrInputFenceFacts_{};
        NrModuleV1 pinned_[2]{};
        NrCallV1 pinCalls_[2]{};
        NrModuleV1 modules_[8]{};
        NrCallV1 modulePinCalls_[8]{};
        ModuleExpectation moduleExpectations_[8]{};
        ManualModuleObservation manualModule_{};
        // File cleanup may be called by the settled release export, which does not
        // own the producer lease. This mutex protects file-cache state only.
        std::mutex moduleFilesMutex_;
        ModuleFileLease moduleFiles_[6]{};
        NrModuleV1 processPinModules_[2]{};
        NrCallV1 processPinCalls_[2]{};
        SdkWarmFacts moduleSdkIdentity_{};
        bool modulesAttested_{};
        SdkObservation sdkLifecycle_[64]{}, sdkWarmRows_[6]{};
        uint32_t moduleCount_{}, sdkLifecycleCount_{}, sdkWarmRowCount_{};
        uint64_t warmSdkWitness_{};
        PrivateFencePreflight fenceFacts_{};
        Microsoft::WRL::ComPtr<ID3D11Fence> initialProducerFence_, initialAppFence_;
        Microsoft::WRL::ComPtr<ID3D12Fence> initialFgFence_;
        BudgetCall fgBudgetCall_{};
        SubmitCall submit_{};
        SourceCall source_{};
        vdxrfg::pair_capture::nr::NrIdentity ownerIdentity_{};
        ID3D11Fence* producerFence_{};
        ID3D11Fence* appFence_{};
        ID3D12Fence* fgFence_{};
        HostLifetimeFacts hostLifetime_{}, reservedHost_{};
        uint64_t firstSerial_{}, ownerHostEpoch_{}, reservationObservationEpoch_{}, reservedOwnerNumericId_{},
            reservedOwnerAttempt_{};
        struct FrameProjection {
            NrPacketV1 packet{};
            NrHeaderV1 header{};
            NrIdentityV1 identity{};
            NrGroupV1 groups[3]{};
            NrCallV1 controls[128]{};
            NrAliasV1 aliases[32]{};
            EnginePacketModuleInput modules[8]{};
            EnginePacketSdkInput sdk[70]{};
            SdkObservation sdkRows[6]{};
            EnginePacketRoleInput roles[28]{};
            PipelineDrawFacts pipelineDraws[4]{};
            PostFxRecord postFxRecords[4]{};
            DrawPacketProjection drawProjection{};
            NrPostFxSidecarV1 postFx{};
            struct DigestInput {
                NrPacketV1 packet;
                NrPostFxSidecarV1 postFx;
            } digestInput{};
            NrAckV1 ack{};
            uint64_t digest[4]{}, groupTransfers[3]{};
            uint32_t controlCount{}, aliasCount{}, sdkCount{}, roleCount{};
            bool identityProjected{}, groupsProjected{}, assembled{}, transferred{};
        } projection_[2]{};
        bool attempted_{}, prepared_{}, ownerReserved_{};
    };
    CaptureEngine& captureEngine() noexcept;
} // namespace virtualdesktop_openxr::nr_capture
