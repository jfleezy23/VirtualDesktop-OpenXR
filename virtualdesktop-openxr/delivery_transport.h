#pragma once

#include <OVR_CAPI_D3D.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <vector>
#include "sdk_gate_lease.h"

namespace virtualdesktop_openxr {
    struct DeliverySdkTraceObserver {
        static bool enabled() noexcept;
        static int64_t now() noexcept;
        static void emit(const detail::SdkGateTiming&) noexcept;
    };
    // Genuine SDK handles stay on P. Producer-facing images are private S
    // allocations; their final contents are selected after preprocessing.
    class DeliveryTransport {
        enum class ChainKind { ordinary, nativeOnly, frameGenerationDepth };
        struct Chain;
        struct State;

      public:
        struct Transfer {
            std::shared_ptr<Chain> owner;
            int sourceIndex{};
        };
        struct Batch {
            std::vector<Transfer> images;
            uint64_t sourceReady{}, serial{};
        };
        struct Statistics {
            uint64_t privateExports{}, nativeCopies{}, nativeCommits{}, batches{};
        };

        DeliveryTransport(ovrSession session,
                          ID3D11Device5* producer,
                          ID3D11DeviceContext4* producerContext,
                          ID3D11Device5* presentation,
                          ID3D11DeviceContext4* presentationContext);
        ~DeliveryTransport();
        DeliveryTransport(const DeliveryTransport&) = delete;
        DeliveryTransport& operator=(const DeliveryTransport&) = delete;

        ovrResult create(const ovrTextureSwapChainDesc& desc, ovrTextureSwapChain* chain, bool nativeOnly = false);
        ovrResult createFrameGenerationDepth(const ovrTextureSwapChainDesc& desc, ovrTextureSwapChain* chain);
        // The depth-bank writer owns the producer context and must acquire its
        // index here. Ordinary current/index peeks do not order GPU writes.
        ovrResult acquireFrameGenerationDepthForWrite(ovrTextureSwapChain chain, int* index);
        void destroy(ovrTextureSwapChain chain);
        ovrResult length(ovrTextureSwapChain chain, int* length);
        ovrResult description(ovrTextureSwapChain chain, ovrTextureSwapChainDesc* desc);
        ovrResult current(ovrTextureSwapChain chain, int* index);
        // Producer-only readiness peek: never enter the SDK or wait for metadata.
        bool tryProducerIndex(ovrTextureSwapChain chain, int& index);
        ovrResult buffer(ovrTextureSwapChain chain, int index, REFIID iid, void** output);
        // S: receipt only. Native-only P: actual SDK Commit, after P writes.
        ovrResult commit(ovrTextureSwapChain chain);
        // Final layer selection: never transport unused FG intermediates.
        Batch prepare(const std::vector<ovrTextureSwapChain>& selected, uint64_t serial);
        // Submission owner only. A current batch's S wait must not be queued
        // ahead of older ready FG work on P.
        void copy(const Batch& batch);
        void commitCopied(const Batch& batch);
        void execute(const Batch& batch);
        bool fgDepthReady(const Batch& batch);
        // Discard an exact scheduled FG depth write before any native read.
        // The next writer runs on the same producer context.
        void discardUncopiedFgDepth(const Batch& batch);
        void orderProducerAfterNativeReads();
        // One resource export, multiple owned duplicate handles/importers.
        HRESULT duplicateSharedHandle(ID3D11Texture2D* image, HANDLE* output);
        void drain();
        // Caller owns both context-use leases. Rare resize/destruction path.
        void collectRetired();
        void shutdown();
        bool completionUnknown() const noexcept;
        Statistics statistics() const;
        using SdkLease = detail::SdkGateLease<DeliverySdkTraceObserver>;
        SdkLease sdkLease(const char* operation = "Unspecified", long long nativeId = -1);
        ID3D11Device5* presentationDevice() const noexcept;
        ID3D11DeviceContext4* presentationContext() const noexcept;

      private:
        ovrResult createChain(const ovrTextureSwapChainDesc& desc, ovrTextureSwapChain* chain, ChainKind kind);
        std::unique_ptr<State> m_state;
    };
} // namespace virtualdesktop_openxr
