#pragma once
#include "PairCaptureNrViewDescriptions.h"
#include "../nr-frame-generation/include/pair_capture_nr_postfx_abi.h"
#include <array>
#include <d3d11_4.h>
namespace virtualdesktop_openxr::nr_capture {
    enum class PostFxBinding : uint32_t { shader, buffer, srv, uav, sampler };
    struct PostFxRecord {
        uint64_t serial{}, shaderLifetime{}, bufferLifetime{}, dispatchThread{}, dispatchQpc{};
        NrDrawV1 draw{};
        std::array<NrDescriptorV1, 3> descriptors{};
        std::array<uint8_t, 112> constants{};
        std::array<uint8_t, 32> shaderHash{};
        std::array<uint64_t, 5> bindingEpochs{};
        uint64_t samplerAddress{};
        uint64_t mapResultBits{}, mapThread{}, mapEvent{}, copyEvent{}, unmapEvent{}, dispatchEvent{};
        std::array<uint64_t, 5> bindingEvents{};
        bool mapObserved{};
        uint32_t descriptorCount{};
        uint64_t constantContextAddress{}, constantBufferAddress{}, constantBufferLifetime{}, constantWriteSequence{};
        int32_t inputX{}, inputY{}, inputWidth{}, inputHeight{}, sourceWidth{}, sourceHeight{}, outputWidth{},
            outputHeight{};
        bool upscaling{}, sharpening{}, neuralInput{}, geometryObserved{};
        bool constantsObserved{};
    };
    inline bool postFxRecordComplete(const PostFxRecord& r) noexcept {
        const auto& d = r.draw;
        if (!r.mapObserved || r.mapResultBits > UINT32_MAX || (r.mapResultBits & 0x80000000u) || !r.mapThread ||
            r.mapThread != r.dispatchThread || !r.mapEvent ||
            !(r.mapEvent < r.copyEvent && r.copyEvent < r.unmapEvent && r.unmapEvent < r.bindingEvents[1] &&
              r.bindingEvents[1] < r.dispatchEvent) ||
            !r.constantsObserved || !r.constantWriteSequence || d.batchEpoch != r.constantWriteSequence ||
            !r.constantBufferAddress || r.constantBufferAddress != d.rootAddress ||
            r.constantBufferLifetime != r.bufferLifetime || r.constantContextAddress != d.listOrContext ||
            !r.geometryObserved || r.inputX < 0 || r.inputY < 0 || r.inputWidth <= 0 || r.inputHeight <= 0 ||
            r.sourceWidth <= 0 || r.sourceHeight <= 0 || r.outputWidth <= 0 || r.outputHeight <= 0 ||
            r.inputX > r.sourceWidth - r.inputWidth || r.inputY > r.sourceHeight - r.inputHeight || !r.neuralInput ||
            (d.kind == 3 && !r.upscaling) || (d.kind == 4 && !r.sharpening) ||
            d.dispatchX != (static_cast<uint64_t>(r.outputWidth) + 15) / 16 ||
            d.dispatchY != (static_cast<uint64_t>(r.outputHeight) + 15) / 16 || !r.serial ||
            (d.kind != 3 && d.kind != 4) || d.eye > 1 || !d.listOrContext || !d.psoAddress || !d.rootAddress ||
            !d.rootTable0 || !d.rootTable1 || !r.shaderLifetime || !r.bufferLifetime || !d.bindEpoch ||
            d.constantsBytes != (d.kind == 3 ? 112 : 80) || r.descriptorCount != (d.kind == 3 ? 3u : 2u) ||
            !d.dispatchX || !d.dispatchY || d.dispatchZ != 1 || d.vertexCount || d.instanceCount || d.viewportWbits ||
            d.viewportHbits || d.scissorRightBits || d.scissorBottomBits || !d.resourceWriteEpoch ||
            d.observedMask != 15)
            return false;
        for (size_t i = 0; i < (d.kind == 3 ? 5u : 4u); ++i)
            if (!r.bindingEpochs[i] || !r.bindingEvents[i] || r.bindingEvents[i] >= r.dispatchEvent)
                return false;
        if (d.kind == 4 && (r.bindingEpochs[4] || r.bindingEvents[4] || r.samplerAddress))
            return false;
        bool hash = false;
        for (auto b : r.shaderHash)
            hash |= b != 0;
        if (!hash)
            return false;
        if (r.descriptors[0].kind != NR_DESC_SRV || r.descriptors[0].objectAddress != d.rootTable0 ||
            r.descriptors[1].kind != NR_DESC_UAV || r.descriptors[1].objectAddress != d.rootTable1 ||
            (d.kind == 3 &&
             (r.descriptors[2].kind != NR_DESC_SAMPLER || r.descriptors[2].objectAddress != r.samplerAddress)))
            return false;
        for (size_t i = 0; i < r.descriptorCount; ++i) {
            const auto& v = r.descriptors[i];
            if (!v.objectAddress || !v.objectLifetimeEpoch || !v.writeEpoch || !v.bindEpoch ||
                v.bindEpoch > d.bindEpoch || v.observedMask != 15)
                return false;
            if (i < 2 && (!v.resourceAddress || !v.resourceLifetimeEpoch || !v.sourceWriteEpoch))
                return false;
        }
        return r.descriptors[1].sourceWriteEpoch == d.resourceWriteEpoch;
    }
    void postFxShaderCreated(ID3D11ComputeShader*, const void* actualBytecode, size_t byteCount) noexcept;
    void postFxBufferCreated(ID3D11Buffer*) noexcept;
    void postFxSrvCreated(ID3D11Texture2D*, ID3D11ShaderResourceView*, const D3D11_SHADER_RESOURCE_VIEW_DESC&) noexcept;
    void postFxUavCreated(ID3D11Texture2D*,
                          ID3D11UnorderedAccessView*,
                          const D3D11_UNORDERED_ACCESS_VIEW_DESC&) noexcept;
    void postFxSamplerCreated(ID3D11SamplerState*, const D3D11_SAMPLER_DESC&) noexcept;
    void postFxMapReturned(PostFxRecord&, ID3D11DeviceContext4*, ID3D11Buffer*, HRESULT) noexcept;
    void postFxUnmapReturned(PostFxRecord&, ID3D11DeviceContext4*, ID3D11Buffer*) noexcept;
    inline bool postFxExportRow(const PostFxRecord& r, NrPostFxRowV1& out) noexcept {
        out = {};
        if (!postFxRecordComplete(r))
            return false;
        out.eye = r.draw.eye;
        out.kind = r.draw.kind;
        out.hostSerial = r.serial;
        out.contextAddress = r.constantContextAddress;
        out.bufferAddress = r.constantBufferAddress;
        out.bufferLifetime = r.constantBufferLifetime;
        out.constantWriteSequence = r.constantWriteSequence;
        out.mapResultBits = r.mapResultBits;
        out.mapType = 4;
        out.mapEvent = r.mapEvent;
        out.copyEvent = r.copyEvent;
        out.unmapEvent = r.unmapEvent;
        out.dispatchEvent = r.dispatchEvent;
        out.mapThread = r.mapThread;
        out.shaderAddress = r.draw.psoAddress;
        out.shaderLifetime = r.shaderLifetime;
        out.srvAddress = r.draw.rootTable0;
        out.uavAddress = r.draw.rootTable1;
        out.samplerAddress = r.samplerAddress;
        for (size_t i = 0; i < 5; ++i) {
            out.bindingEpochs[i] = r.bindingEpochs[i];
            out.bindingEvents[i] = r.bindingEvents[i];
        }
        out.drawBindEpoch = r.draw.bindEpoch;
        out.dispatchX = r.draw.dispatchX;
        out.dispatchY = r.draw.dispatchY;
        out.dispatchZ = r.draw.dispatchZ;
        out.dispatchThread = r.dispatchThread;
        out.dispatchQpc = r.dispatchQpc;
        out.inputXbits = static_cast<uint32_t>(r.inputX);
        out.inputYbits = static_cast<uint32_t>(r.inputY);
        out.inputWidth = r.inputWidth;
        out.inputHeight = r.inputHeight;
        out.sourceWidth = r.sourceWidth;
        out.sourceHeight = r.sourceHeight;
        out.outputWidth = r.outputWidth;
        out.outputHeight = r.outputHeight;
        out.stageFlags =
            (r.upscaling ? NR_POSTFX_EASU : 0u) | (r.sharpening ? NR_POSTFX_CAS : 0u) | NR_POSTFX_NEURAL_INPUT;
        out.constantsBytes = r.draw.constantsBytes;
        const auto& srv = r.descriptors[0];
        const auto& uav = r.descriptors[1];
        out.srvLifetime = srv.objectLifetimeEpoch;
        out.uavLifetime = uav.objectLifetimeEpoch;
        out.inputResourceAddress = srv.resourceAddress;
        out.inputResourceLifetime = srv.resourceLifetimeEpoch;
        out.inputSourceWriteEpoch = srv.sourceWriteEpoch;
        out.outputResourceAddress = uav.resourceAddress;
        out.outputResourceLifetime = uav.resourceLifetimeEpoch;
        out.outputSourceWriteEpoch = uav.sourceWriteEpoch;
        out.srvWriteEpoch = srv.writeEpoch;
        out.uavWriteEpoch = uav.writeEpoch;
        if (r.draw.kind == 3) {
            out.samplerLifetime = r.descriptors[2].objectLifetimeEpoch;
            out.samplerWriteEpoch = r.descriptors[2].writeEpoch;
        }
        for (size_t i = 0; i < 32; ++i)
            out.shaderSha256[i] = r.shaderHash[i];
        for (size_t i = 0; i < out.constantsBytes; ++i)
            out.constants[i] = r.constants[i];
        return true;
    }
    void postFxConstants(PostFxRecord&,
                         ID3D11DeviceContext4* actualContext,
                         ID3D11Buffer* actualBuffer,
                         const void* actualMappedBytes,
                         size_t count) noexcept;
    void postFxGeometry(PostFxRecord&,
                        int32_t x,
                        int32_t y,
                        int32_t width,
                        int32_t height,
                        int32_t sourceWidth,
                        int32_t sourceHeight,
                        int32_t outputWidth,
                        int32_t outputHeight,
                        bool upscaling,
                        bool sharpening,
                        bool neuralInput) noexcept;
    void postFxBindingObserved(PostFxRecord&, PostFxBinding, const void* actualBoundObject) noexcept;
    bool observePostFxDispatch(PostFxRecord&,
                               uint64_t serial,
                               uint32_t eye,
                               uint32_t kind,
                               ID3D11DeviceContext4*,
                               uint32_t x,
                               uint32_t y,
                               uint32_t z) noexcept;
    bool postFxFrameComplete(uint64_t serial, bool upscaling, bool sharpening) noexcept;
    bool copyPostFxSidecar(const NrPacketV1&, bool actualUpscaling, bool actualSharpening, NrPostFxSidecarV1&) noexcept;
    bool copyPostFxRecord(uint64_t serial, uint32_t eye, uint32_t kind, PostFxRecord&) noexcept;
} // namespace virtualdesktop_openxr::nr_capture
