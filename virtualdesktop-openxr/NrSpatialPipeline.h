#pragma once

#include "NrSpatialPolicy.h"
#include <array>
#include <memory>
#include <d3d12.h>

namespace virtualdesktop_openxr::nr_spatial {
    struct EyeInput {
        ID3D12Resource* color{};
        DXGI_FORMAT colorFormat{DXGI_FORMAT_UNKNOWN};
        Rect colorRect{};
        ID3D12Resource* depth{};
        DXGI_FORMAT depthFormat{DXGI_FORMAT_UNKNOWN};
        Rect depthRect{};
        Rect processingRect{};      // Local to the full valid colorRect; empty means the complete eye.
        uint32_t transitionWidth{}; // Native pixels, inside processingRect only.
    };
    struct FrameInput {
        std::array<EyeInput, 2> eyes{};
        ID3D12Resource* output{};
        DXGI_FORMAT outputFormat{DXGI_FORMAT_UNKNOWN};
        uint32_t outputEyeStride{}; // Horizontal native output offset of eye 1; eye 0 starts at (0,0).
        uint32_t processingPercent{100};
        bool nativeAnswer{}; // Direct packed vendor answer; requires 100%, no downsample/depth scratch.
    };
    struct WorkingEye {
        ID3D12Resource* proxy{};
        ID3D12Resource* answer{};
        ID3D12Resource* depth{};
        Extent extent{};
    };

    // DIRECT command lists only; all resources have COMMON entry/exit state.
    // Between RecordInputs and RecordResolve, the vendor owns proxy/answer/depth
    // access and must return them to COMMON. Proxy/answer are RGBA8 UNORM storage
    // containing the declared input encoding. Depth is R32_FLOAT, unchanged in sign.
    // In nativeAnswer mode there are no input/depth scratch passes: the vendor
    // writes the packed GetNativeAnswer() crop and returns it to COMMON before
    // RecordResolve. Original sources remain separate from the final render target.
    // All resources must be single-sample, single-mip 2D resources. Native inputs
    // read slice zero, including the host's array alias for resolved slice zero;
    // other eye slices are flattened by the host. Output must be single-slice.
    // Rect never selects a slice.
    // Prepare allocates before recording and retains immutable descriptor batches
    // and resource references until the supplied fence completes. Matches compares
    // geometry/encoding, while resource identities select batches inside a generation.
    // Callers must drain before destroying, clearing, or replacing a generation.
    // DiscardRecording is legal only when commands definitely were NOT submitted;
    // on uncertain submission keep this object alive until queue recovery/drain.
    class Pipeline {
      public:
        Pipeline(ID3D12Device*, ID3D12Fence*);
        ~Pipeline();
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        bool Matches(const FrameInput&) const;
        HRESULT Prepare(const FrameInput&);
        WorkingEye GetWorkingEye(uint32_t) const;
        ID3D12Resource* GetNativeAnswer() const;
        void RecordInputs(ID3D12GraphicsCommandList*);
        void RecordResolve(ID3D12GraphicsCommandList*);
        void MarkSubmitted(uint64_t);
        void DiscardRecording();
        void Clear();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace virtualdesktop_openxr::nr_spatial
