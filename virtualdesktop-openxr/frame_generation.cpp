#include "frame_generation.h"
#include "PairCaptureNrWarmLayouts.h"
#include "PairCaptureNrEngine.h"
#include "PairCaptureNrPrivate.h"
#include "PairCaptureNrSources.h"
#include "../nr-frame-generation/include/pair_capture_private.hpp"
#include "FrameGenerationCopyCS.h"
#include "FrameGenerationDepthCS.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>

namespace virtualdesktop_openxr {
    using Microsoft::WRL::ComPtr;

    namespace {
        struct BgraCopy {
            ComPtr<ID3D11Texture2D> source, output;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11UnorderedAccessView> uav;
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3DDeviceContextState> contextState;
            HRESULT initialize(const FrameGeneration::InitInfo& info) noexcept;
            void copy(ID3D11DeviceContext4* context,
                      ID3D11Texture2D* destination,
                      uint32_t eyeWidth,
                      uint32_t bgraMask,
                      const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                      const std::array<VDXRFG_ViewV1, 2>& views) const;
        };

        struct DepthCopy {
            struct Eye {
                ComPtr<ID3D11Texture2D> source, snapshot;
                ComPtr<ID3D11ShaderResourceView> srv;
                uint32_t slice{};
            };
            std::array<Eye, 2> eyes;
            ComPtr<ID3D11Texture2D> atlas;
            ComPtr<ID3D11UnorderedAccessView> uav;
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3D11Buffer> constants;
            ComPtr<ID3DDeviceContextState> contextState;
            HRESULT initialize(const FrameGeneration::InitInfo& info) noexcept;
            bool prepare(ID3D11Device5* device, const std::array<FrameGenerationDepthSource, 2>& sources) noexcept;
            void copy(ID3D11DeviceContext4* context,
                      uint32_t width,
                      uint32_t height,
                      const std::array<FrameGenerationDepthSource, 2>& sources,
                      vdxrfg::pair_capture::HostHook hook = nullptr,
                      void* captureContext = nullptr,
                      uint64_t serial = 0) const;
        };

        bool isBgra(DXGI_FORMAT format) {
            return format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                   format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
    } // namespace

    struct FrameGeneration::State {
        HMODULE module{};
        nr_capture::ReceiverCall nrReceiver{};
        nr_capture::ReceiverReady nrReady{};
        vdxrfg::pair_capture::HostHook pairHook{};
        uint32_t(WINAPI* pairStatus)(uint32_t) noexcept {};
        HRESULT(WINAPI* pairOutputWait)(void*, uint64_t, uint32_t, ID3D11DeviceContext4*) noexcept {};
        bool outputObservationEnabled{true};
        VDXRFG_ApiV1 api{};
        VDXRFG_ApiV3 api3{};
        VDXRFG_ApiV4 api4{};
        VDXRFG_ApiV5 api5{};
        void* context{};
        InitInfo info;
        ComPtr<ID3D11Device5> device11;
        ComPtr<ID3D11DeviceContext4> context11;
        ComPtr<ID3D11Device5> deliveryDevice11;
        ComPtr<ID3D11DeviceContext4> deliveryContext11;
        ComPtr<ID3D11Texture2D> deliveryInput11;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 4>, 2> deliveryOutputs11;
        ComPtr<ID3D11Fence> deliveryReady11, deliveryFirstReady11, deliveryConsumed11, consumerProducer11;
        uint64_t canonicalReadValue{};
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D11Texture2D> input11;
        ComPtr<ID3D12Resource> input12;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 4>, 2> outputs11;
        std::array<std::array<ComPtr<ID3D12Resource>, 4>, 2> outputs12;
        ComPtr<ID3D11Fence> producer11, consumed11, ready11, firstReady11, consumer11;
        ComPtr<ID3D12Fence> firstReady12;
        ComPtr<ID3D12Fence> producer12;
        std::array<uint64_t, 2> liveTokens{};
        std::array<uint64_t, 2> consumerValues{};
        std::array<std::array<uint64_t, 4>, 2> roleConsumerValues{};
        uint64_t outputCallId{};
        std::array<std::array<vdxrfg::pair_capture::SuccessfulCopy, 4>, 2> successfulCopies{};
        std::array<std::array<uint64_t, 4>, 2> registeredCopyValues{};
        std::array<uint64_t, 2> registeredRetirementCopyValues{};
        std::array<VDXRFG_TicketV1, 2> tickets{};
        std::array<std::array<ComPtr<ID3D11Texture2D>, 4>, 2> retainedDestinations;
        std::array<std::array<ComPtr<ID3D11Texture2D>, 2>, 2> capturedSources;
        std::array<std::array<VDXRFG_ViewV1, 2>, 2> capturedViews{};
        std::array<uint32_t, 2> capturedBgraMask{};
        BgraCopy bgra;
        DepthCopy depth;
        ComPtr<ID3D12Resource> guide12;
        std::array<FrameGenerationDepthSource, 2> producerDepth;
        uint64_t guideEpoch{}, guideSignature{};
        std::array<ComPtr<ID3D11Texture2D>, 2> producerSources;
        uint64_t generation{};
        uint64_t producerValue{}, consumerValue{}, consumedValue{}, previousSerial{};
        uint64_t inputOwnerToken{}, inputOwnerGeneration{};
        std::atomic<bool> poisoned{};
        bool hostCompletionUnknown{};
        bool hostOwnsReal{}, opaqueInput{};
        HANDLE copyCompletionEvent{};
        HANDLE readyCompletionEvent{};
        std::array<uint64_t, 2> registeredReadyValues{};
        std::array<uint64_t, 2> firstReadyValues{}, registeredFirstReadyValues{};
        std::array<bool, 2> queuedFullReadyWait{}, queuedFirstReadyWait{};

        ~State() {
            // FrameGeneration::shutdown has already released context ownership.
            if (copyCompletionEvent) {
                CloseHandle(copyCompletionEvent);
            }
            if (readyCompletionEvent) {
                CloseHandle(readyCompletionEvent);
            }
            if (module) {
                FreeLibrary(module);
            }
        }
    };

    void FrameGeneration::diagnostic(
        const char* name, uint64_t serial, void* object, void* other, uint64_t value, HRESULT result) const noexcept {
        if (m_state)
            vdxrfg::pair_capture::emit(m_state->pairHook, name, m_state->context, serial, object, other, value, result);
    }
    bool FrameGeneration::diagnosticEntryEnabled() const noexcept {
        if (!m_state || !m_state->pairStatus)
            return false;
        const auto flags = m_state->pairStatus(0);
        return (flags & 16u) && !(flags & (1u | 2u | 64u));
    }

    namespace {
        std::atomic<uint64_t> nextGeneration{1};
        HRESULT shareTexture(ID3D11Texture2D* texture,
                             ID3D12Device* device,
                             ID3D12Resource** result,
                             vdxrfg::pair_capture::HostHook hook = nullptr,
                             ID3D11Device5* delivery = nullptr,
                             ID3D11Texture2D** deliveryView = nullptr) {
            ComPtr<IDXGIResource1> dxgi;
            HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&dxgi));
            if (FAILED(hr)) {
                return hr;
            }
            HANDLE handle{};
            hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
            vdxrfg::pair_capture::emit(
                hook, "texture-export11", nullptr, 0, texture, nullptr, reinterpret_cast<uintptr_t>(handle), hr);
            if (FAILED(hr)) {
                return hr;
            }
            hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(result));
            vdxrfg::pair_capture::emit(
                hook, "texture-import12", nullptr, 0, *result, device, reinterpret_cast<uintptr_t>(handle), hr);
            if (SUCCEEDED(hr) && delivery) {
                hr = delivery->OpenSharedResource1(handle, IID_PPV_ARGS(deliveryView));
            }
            CloseHandle(handle);
            return hr;
        }

        HRESULT importFence(ID3D12Device* device,
                            ID3D11Device5* consumer,
                            ID3D12Fence* fence,
                            ID3D11Fence** result,
                            vdxrfg::pair_capture::HostHook hook = nullptr,
                            ID3D11Device5* delivery = nullptr,
                            ID3D11Fence** deliveryFence = nullptr) {
            HANDLE handle{};
            HRESULT hr = device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle);
            vdxrfg::pair_capture::emit(
                hook, "consumer-fence-export12", nullptr, 0, fence, device, reinterpret_cast<uintptr_t>(handle), hr);
            if (FAILED(hr)) {
                return hr;
            }
            hr = consumer->OpenSharedFence(handle, IID_PPV_ARGS(result));
            vdxrfg::pair_capture::emit(hook,
                                       "consumer-fence-import11",
                                       nullptr,
                                       0,
                                       *result,
                                       consumer,
                                       reinterpret_cast<uintptr_t>(handle),
                                       hr);
            if (SUCCEEDED(hr) && delivery) {
                hr = delivery->OpenSharedFence(handle, IID_PPV_ARGS(deliveryFence));
            }
            CloseHandle(handle);
            return hr;
        }

        bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* texture) {
            ComPtr<ID3D11Device> owner;
            ComPtr<IUnknown> a, b;
            texture->GetDevice(&owner);
            if (owner.Get() == expected) {
                return true;
            }
            // Different interface pointers may still identify the same COM device.
            return SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&a))) && SUCCEEDED(owner.As(&b)) &&
                   a.Get() == b.Get();
        }

        // Sources and view bounds have already been validated and retained.
        // Copy only subresource zero, including for array/mip-backed inputs.
        void copyStereo(ID3D11DeviceContext* context,
                        ID3D11Texture2D* destination,
                        uint32_t eyeWidth,
                        const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                        const std::array<VDXRFG_ViewV1, 2>& views) {
            const auto& left = views[0].rect;
            const auto& right = views[1].rect;
            if (sources[0].Get() == sources[1].Get() && left.width == eyeWidth && right.width == eyeWidth &&
                left.y == right.y && left.height == right.height && uint64_t(left.x) + left.width == right.x) {
                const D3D11_BOX box{left.x, left.y, 0, right.x + right.width, left.y + left.height, 1};
                context->CopySubresourceRegion(destination, 0, 0, 0, 0, sources[0].Get(), 0, &box);
            } else {
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    const auto& rect = views[eye].rect;
                    const D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
                    context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, sources[eye].Get(), 0, &box);
                }
            }
        }

        HRESULT BgraCopy::initialize(const FrameGeneration::InitInfo& info) noexcept {
            if (shader) {
                return S_OK;
            }
            BgraCopy candidate;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = info.eyeWidth;
            desc.Height = info.height;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = info.device11->CreateTexture2D(&desc, nullptr, &candidate.source);
            if (FAILED(hr))
                return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            hr = info.device11->CreateShaderResourceView(candidate.source.Get(), &srvDesc, &candidate.srv);
            if (FAILED(hr))
                return hr;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            hr = info.device11->CreateTexture2D(&desc, nullptr, &candidate.output);
            if (FAILED(hr))
                return hr;
            hr = info.device11->CreateUnorderedAccessView(candidate.output.Get(), nullptr, &candidate.uav);
            if (FAILED(hr))
                return hr;
            hr = info.device11->CreateComputeShader(
                g_FrameGenerationCopyCS, sizeof(g_FrameGenerationCopyCS), nullptr, &candidate.shader);
            if (FAILED(hr))
                return hr;
            const auto level = info.device11->GetFeatureLevel();
            const auto flags = (info.device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                   ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                   : 0u;
            hr = info.device11->CreateDeviceContextState(
                flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &candidate.contextState);
            if (FAILED(hr))
                return hr;
            *this = std::move(candidate);
            return S_OK;
        }

        void BgraCopy::copy(ID3D11DeviceContext4* context,
                            ID3D11Texture2D* destination,
                            uint32_t eyeWidth,
                            uint32_t bgraMask,
                            const std::array<ComPtr<ID3D11Texture2D>, 2>& sources,
                            const std::array<VDXRFG_ViewV1, 2>& views) const {
            // One eye-sized cache also supports different source families in
            // the two eyes, with no per-frame views, constants or CPU waits.
            struct Restore {
                ID3D11DeviceContext4* context;
                ComPtr<ID3DDeviceContextState> saved;
                ~Restore() {
                    context->SwapDeviceContextState(saved.Get(), nullptr);
                }
            } restore{context};
            context->SwapDeviceContextState(contextState.Get(), &restore.saved);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto& rect = views[eye].rect;
                const D3D11_BOX box{rect.x, rect.y, 0, rect.x + rect.width, rect.y + rect.height, 1};
                if (!(bgraMask & (1u << eye))) {
                    context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, sources[eye].Get(), 0, &box);
                    continue;
                }
                context->CopySubresourceRegion(source.Get(), 0, 0, 0, 0, sources[eye].Get(), 0, &box);
                context->CSSetShader(shader.Get(), nullptr, 0);
                context->CSSetShaderResources(0, 1, srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                context->Dispatch((eyeWidth + 15) / 16, (rect.height + 15) / 16, 1);
                ID3D11ShaderResourceView* nullSrv{};
                ID3D11UnorderedAccessView* nullUav{};
                context->CSSetShaderResources(0, 1, &nullSrv);
                context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
                context->CopySubresourceRegion(destination, 0, eye * eyeWidth, 0, 0, output.Get(), 0, nullptr);
            }
        }

        HRESULT DepthCopy::initialize(const FrameGeneration::InitInfo& info) noexcept {
            DepthCopy candidate;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 2 * info.eyeWidth;
            desc.Height = info.height;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            HRESULT hr = info.device11->CreateTexture2D(&desc, nullptr, &candidate.atlas);
            if (FAILED(hr))
                return hr;
            nr_capture::nrBankTextureObserved(candidate.atlas.Get(), info.device11);
            hr = info.device11->CreateUnorderedAccessView(candidate.atlas.Get(), nullptr, &candidate.uav);
            if (FAILED(hr))
                return hr;
            hr = info.device11->CreateComputeShader(
                g_FrameGenerationDepthCS, sizeof(g_FrameGenerationDepthCS), nullptr, &candidate.shader);
            if (FAILED(hr))
                return hr;
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 80;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            hr = info.device11->CreateBuffer(&buffer, nullptr, &candidate.constants);
            if (FAILED(hr))
                return hr;
            const auto level = info.device11->GetFeatureLevel();
            const auto flags = (info.device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                   ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                   : 0u;
            hr = info.device11->CreateDeviceContextState(
                flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &candidate.contextState);
            if (FAILED(hr))
                return hr;
            *this = std::move(candidate);
            return S_OK;
        }

        bool DepthCopy::prepare(ID3D11Device5* device,
                                const std::array<FrameGenerationDepthSource, 2>& sources) noexcept {
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& input = sources[eye];
                if (!frameGenerationDepthSourceValid(device, input))
                    return false;
            }
            // All allocation/view failures happen before a source read or atlas
            // write. A failure therefore submits a deliberate no-guide frame.
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& input = sources[eye];
                auto& cached = eyes[eye];
                if (cached.source.Get() == input.source.Get() && cached.slice == input.arraySlice && cached.srv)
                    continue;
                Eye candidate;
                candidate.source = input.source;
                candidate.slice = input.arraySlice;
                auto desc = input.description;
                const auto srvFormat = frameGenerationDepthSrvFormat(desc.Format);
                DXGI_FORMAT typeless{};
                switch (srvFormat) {
                case DXGI_FORMAT_R16_UNORM:
                    typeless = DXGI_FORMAT_R16_TYPELESS;
                    break;
                case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                    typeless = DXGI_FORMAT_R24G8_TYPELESS;
                    break;
                case DXGI_FORMAT_R32_FLOAT:
                    typeless = DXGI_FORMAT_R32_TYPELESS;
                    break;
                default:
                    typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
                    break;
                }
                ID3D11Texture2D* viewed = candidate.source.Get();
                const bool direct = desc.Format == typeless && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE);
                if (!direct) {
                    // App images rotate, but a private copy has no dependence
                    // on their COM identity or selected source slice. The
                    // producer fence already completed before prepare, so a
                    // matching cache can safely serve the new original image.
                    if (cached.snapshot && cached.srv) {
                        D3D11_TEXTURE2D_DESC snapshotDesc{};
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                        cached.snapshot->GetDesc(&snapshotDesc);
                        cached.srv->GetDesc(&srvDesc);
                        if (snapshotDesc.Width == desc.Width && snapshotDesc.Height == desc.Height &&
                            snapshotDesc.Format == typeless && srvDesc.Format == srvFormat) {
                            cached.source = input.source;
                            cached.slice = input.arraySlice;
                            continue;
                        }
                    }
                    desc.Format = typeless;
                    desc.MipLevels = desc.ArraySize = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.CPUAccessFlags = desc.MiscFlags = 0;
                    if (FAILED(device->CreateTexture2D(&desc, nullptr, &candidate.snapshot)))
                        return false;
                    viewed = candidate.snapshot.Get();
                }
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = srvFormat;
                view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels = 1;
                view.Texture2DArray.FirstArraySlice = direct ? input.arraySlice : 0;
                view.Texture2DArray.ArraySize = 1;
                if (FAILED(device->CreateShaderResourceView(viewed, &view, &candidate.srv)))
                    return false;
                cached = std::move(candidate);
            }
            return true;
        }

        void DepthCopy::copy(ID3D11DeviceContext4* context,
                             uint32_t width,
                             uint32_t height,
                             const std::array<FrameGenerationDepthSource, 2>& sources,
                             vdxrfg::pair_capture::HostHook hook,
                             void* captureContext,
                             uint64_t serial) const {
            struct Restore {
                ID3D11DeviceContext4* context;
                ComPtr<ID3DDeviceContextState> saved;
                ~Restore() {
                    context->SwapDeviceContextState(saved.Get(), nullptr);
                }
            } restore{context};
            context->SwapDeviceContextState(contextState.Get(), &restore.saved);
            struct Constants {
                float colour[4], crop[4], depth[4], range[4];
                uint32_t output[4];
            };
            static_assert(sizeof(Constants) == 80);
            const auto rect = [](float* output, const VDXRFG_RectV1& value) {
                output[0] = static_cast<float>(value.x);
                output[1] = static_cast<float>(value.y);
                output[2] = static_cast<float>(value.width);
                output[3] = static_cast<float>(value.height);
            };
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& input = sources[eye];
                const auto& cached = eyes[eye];
                if (cached.snapshot) {
                    // Depth/stencil copies cover the complete selected mip/slice.
                    context->CopySubresourceRegion(
                        cached.snapshot.Get(),
                        0,
                        0,
                        0,
                        0,
                        input.source.Get(),
                        D3D11CalcSubresource(0, input.arraySlice, input.description.MipLevels),
                        nullptr);
                    vdxrfg::pair_capture::emit(hook,
                                               "normalizer-snapshot-Copy-void",
                                               captureContext,
                                               serial,
                                               cached.snapshot.Get(),
                                               input.source.Get(),
                                               D3D11CalcSubresource(0, input.arraySlice, input.description.MipLevels));
                }
                Constants data{};
                rect(data.colour, input.colourRect);
                rect(data.crop, input.colourCrop);
                rect(data.depth, input.depthRect);
                data.range[0] = input.minDepth;
                data.range[1] = input.maxDepth;
                data.range[2] = 1.f / input.nearZ;
                data.range[3] = 1.f / input.farZ;
                data.output[0] = width;
                data.output[1] = height;
                data.output[2] = eye;
                data.output[3] = (std::isinf(input.nearZ) ? 1u : 0u) | (std::isinf(input.farZ) ? 2u : 0u);
                context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
                context->CSSetShader(shader.Get(), nullptr, 0);
                context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
                context->CSSetShaderResources(0, 1, cached.srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                context->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
                nr_capture::sourceBankWriteObserved(atlas.Get(), context, serial, NR_DISPATCH);
                if (hook) {
                    vdxrfg::pair_capture::Host packet;
                    packet.operation = 5;
                    packet.context = captureContext;
                    packet.serial = serial;
                    packet.eventObject = shader.Get();
                    packet.eventOther = cached.srv.Get();
                    packet.eventValue = eye;
                    cached.srv->GetDesc(&packet.normalizerSrv);
                    ComPtr<ID3D11Resource> viewed;
                    cached.srv->GetResource(&viewed);
                    packet.normalizerTexture = viewed.Get();
                    hook(&packet);
                }
                ID3D11ShaderResourceView* nullSrv{};
                ID3D11UnorderedAccessView* nullUav{};
                context->CSSetShaderResources(0, 1, &nullSrv);
                context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            }
        }

        uint64_t depthSignature(const FinalProjectionInput& input) noexcept {
            // Hash explicit semantic fields, excluding padding, moving poses,
            // released image indices and rotating COM allocation identities.
            uint64_t hash = 14695981039346656037ull;
            const auto add = [&](const void* data, size_t size) {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (size_t i = 0; i < size; ++i) {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
            };
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& source = input.depth[eye];
                add(&source.colourRect, sizeof(source.colourRect));
                add(&source.colourCrop, sizeof(source.colourCrop));
                add(&source.depthRect, sizeof(source.depthRect));
                add(&source.arraySlice, sizeof(source.arraySlice));
                add(&source.description.Width, sizeof(source.description.Width));
                add(&source.description.Height, sizeof(source.description.Height));
                add(&source.description.Format, sizeof(source.description.Format));
                add(&source.minDepth, sizeof(source.minDepth));
                add(&source.maxDepth, sizeof(source.maxDepth));
                add(&source.nearZ, sizeof(source.nearZ));
                add(&source.farZ, sizeof(source.farZ));
                add(input.views[eye].fov, sizeof(input.views[eye].fov));
                add(&input.views[eye].rect.width, sizeof(input.views[eye].rect.width));
                add(&input.views[eye].rect.height, sizeof(input.views[eye].rect.height));
            }
            return hash;
        }
    } // namespace

    FrameGeneration::FrameGeneration() = default;

    FrameGeneration::~FrameGeneration() {
        if (FAILED(shutdown())) {
            // Retain the complete module/COM graph on uncertain GPU completion.
            // No destructor may release only part of an owned transaction.
            (void)m_state.release();
        }
    }

    HRESULT FrameGeneration::initialize(const InitInfo& info) noexcept {
#ifndef _WIN64
        return E_NOTIMPL;
#else
        if (m_state || !info.device11 || !info.context11 || !info.device12 || !info.modulePath.is_absolute() ||
            !info.eyeWidth || info.eyeWidth > UINT32_MAX / 2 || !info.height ||
            (info.multiplier != 2 && info.multiplier != 3 && info.multiplier != 4) || info.depthMode > 2 ||
            (info.format != DXGI_FORMAT_R8G8B8A8_UNORM && info.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            return VDXRFG_E_REJECTED;
        }
        ComPtr<ID3D11Device> contextDevice;
        ComPtr<IUnknown> contextIdentity, deviceIdentity;
        info.context11->GetDevice(&contextDevice);
        if (info.context11->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || FAILED(contextDevice.As(&contextIdentity)) ||
            FAILED(info.device11->QueryInterface(IID_PPV_ARGS(&deviceIdentity))) ||
            contextIdentity.Get() != deviceIdentity.Get()) {
            return VDXRFG_E_REJECTED;
        }
        if (bool(info.deliveryDevice11) != bool(info.deliveryContext11)) {
            return VDXRFG_E_REJECTED;
        }
        if (info.deliveryDevice11) {
            ComPtr<ID3D11Device> owner;
            ComPtr<IUnknown> ownerIdentity, deliveryIdentity;
            ComPtr<IDXGIDevice> producerDxgi, deliveryDxgi;
            ComPtr<IDXGIAdapter> producerAdapter, deliveryAdapter;
            DXGI_ADAPTER_DESC producerDesc{}, deliveryDesc{};
            info.deliveryContext11->GetDevice(&owner);
            if (info.deliveryContext11->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                FAILED(owner.As(&ownerIdentity)) ||
                FAILED(info.deliveryDevice11->QueryInterface(IID_PPV_ARGS(&deliveryIdentity))) ||
                ownerIdentity.Get() != deliveryIdentity.Get() || deliveryIdentity.Get() == deviceIdentity.Get() ||
                FAILED(info.device11->QueryInterface(IID_PPV_ARGS(&producerDxgi))) ||
                FAILED(info.deliveryDevice11->QueryInterface(IID_PPV_ARGS(&deliveryDxgi))) ||
                FAILED(producerDxgi->GetAdapter(&producerAdapter)) ||
                FAILED(deliveryDxgi->GetAdapter(&deliveryAdapter)) || FAILED(producerAdapter->GetDesc(&producerDesc)) ||
                FAILED(deliveryAdapter->GetDesc(&deliveryDesc)) ||
                producerDesc.AdapterLuid.HighPart != deliveryDesc.AdapterLuid.HighPart ||
                producerDesc.AdapterLuid.LowPart != deliveryDesc.AdapterLuid.LowPart ||
                nr_capture::hostRequest().valid) {
                return VDXRFG_E_REJECTED;
            }
        }
        try {
            auto state = std::make_unique<State>();
            state->generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
            if (!state->generation || state->generation == UINT64_MAX) {
                return VDXRFG_E_REJECTED;
            }
            state->info = info;
            state->device11 = info.device11;
            state->context11 = info.context11;
            state->deliveryDevice11 = info.deliveryDevice11;
            state->deliveryContext11 = info.deliveryContext11;
            state->device12 = info.device12;
            state->copyCompletionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state->copyCompletionEvent) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            state->readyCompletionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state->readyCompletionEvent) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            state->module = LoadLibraryExW(
                info.modulePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!state->module) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (nr_capture::hostRequest().valid) {
                state->nrReceiver =
                    reinterpret_cast<nr_capture::ReceiverCall>(GetProcAddress(state->module, "VDXRFG_PrivatePairNRV1"));
                state->nrReady = reinterpret_cast<nr_capture::ReceiverReady>(
                    GetProcAddress(state->module, "VDXRFG_PrivatePairNRReadyV1"));
            }
            state->pairHook = reinterpret_cast<vdxrfg::pair_capture::HostHook>(
                GetProcAddress(state->module, "VDXRFG_PrivatePairCaptureV1"));
            state->pairStatus = reinterpret_cast<decltype(state->pairStatus)>(
                GetProcAddress(state->module, "VDXRFG_PrivatePairCaptureStatusV1"));
            state->pairOutputWait = reinterpret_cast<decltype(state->pairOutputWait)>(
                GetProcAddress(state->module, "VDXRFG_PrivatePairOutputWaitV1"));
            // Selected-output diagnostics have a fixed producer-context
            // contract. They are not qualified for the separate-domain proof.
            if (state->deliveryDevice11 && (!state->pairStatus || state->pairStatus(0) != 0)) {
                return VDXRFG_E_REJECTED;
            }
            auto getApi = reinterpret_cast<VDXRFG_GetApiV1Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV1"));
            if (!getApi) {
                return VDXRFG_E_REJECTED;
            }
            state->api.struct_size = sizeof(state->api);
            state->api.api_version = VDXRFG_API_VERSION;
            HRESULT hr = getApi(&state->api);
            if (FAILED(hr) || state->api.api_version != VDXRFG_API_VERSION ||
                state->api.struct_size != sizeof(state->api) || !state->api.Create || !state->api.Submit ||
                !state->api.Retire || !state->api.Reset || !state->api.GetStatistics || !state->api.Destroy) {
                return VDXRFG_E_REJECTED;
            }
            VDXRFG_ApiV2 api2{};
            const auto getApi2 = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV2"));
            if (getApi2) {
                api2.struct_size = sizeof(api2);
                api2.api_version = VDXRFG_API_VERSION_2;
                hr = getApi2(&api2);
                if (FAILED(hr) || api2.struct_size != sizeof(api2) || api2.api_version != VDXRFG_API_VERSION_2 ||
                    api2.reserved || !api2.Create || !api2.Submit || !api2.Retire || !api2.Reset ||
                    !api2.GetStatistics || !api2.Destroy) {
                    return VDXRFG_E_REJECTED;
                }
                state->api.Submit = api2.Submit;
                state->api.Retire = api2.Retire;
                state->api.Reset = api2.Reset;
                state->api.GetStatistics = api2.GetStatistics;
                state->api.Destroy = api2.Destroy;
            }
            const auto getApi3 = reinterpret_cast<VDXRFG_GetApiV3Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV3"));
            auto& api3 = state->api3;
            if (getApi3) {
                api3.struct_size = sizeof(api3);
                api3.api_version = VDXRFG_API_VERSION_3;
                hr = getApi3(&api3);
                if (FAILED(hr) || api3.struct_size != sizeof(api3) || api3.api_version != VDXRFG_API_VERSION_3 ||
                    api3.max_multiplier < info.multiplier || !api3.Create || !api3.Submit || !api3.Retire ||
                    !api3.Reset || !api3.GetStatistics || !api3.Destroy)
                    return VDXRFG_E_REJECTED;
                state->api.Retire = api3.Retire;
                state->api.Reset = api3.Reset;
                state->api.GetStatistics = api3.GetStatistics;
                state->api.Destroy = api3.Destroy;
            }
            const auto getApi4 = reinterpret_cast<VDXRFG_GetApiV4Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV4"));
            if (getApi4) {
                auto& api4 = state->api4;
                api4.struct_size = sizeof(api4);
                api4.api_version = VDXRFG_API_VERSION_4;
                hr = getApi4(&api4);
                if (FAILED(hr) || api4.struct_size != sizeof(api4) || api4.api_version != VDXRFG_API_VERSION_4 ||
                    api4.max_multiplier < info.multiplier || !api4.Create || !api4.Submit || !api4.Retire ||
                    !api4.Reset || !api4.GetStatistics || !api4.Destroy || !api4.GetOutputReadiness)
                    return VDXRFG_E_REJECTED;
                // Normalize the V3-payload dispatch. V4's extra query stays
                // separate; every legacy full-ticket fence keeps its meaning.
                api3 = {sizeof(api3),
                        VDXRFG_API_VERSION_3,
                        api4.supported_create_flags,
                        api4.max_multiplier,
                        api4.Create,
                        api4.Submit,
                        api4.Retire,
                        api4.Reset,
                        api4.GetStatistics,
                        api4.Destroy};
                state->api.Retire = api4.Retire;
                state->api.Reset = api4.Reset;
                state->api.GetStatistics = api4.GetStatistics;
                state->api.Destroy = api4.Destroy;
            }
            const auto getApi5 = reinterpret_cast<VDXRFG_GetApiV5Fn>(GetProcAddress(state->module, "VDXRFG_GetApiV5"));
            if (getApi5 && info.depthMode != 0 && info.enableDepthGuidance && !info.useFidelityFxFull) {
                auto& api5 = state->api5;
                api5.struct_size = sizeof(api5);
                api5.api_version = VDXRFG_API_VERSION_5;
                hr = getApi5(&api5);
                if (FAILED(hr) || api5.struct_size != sizeof(api5) || api5.api_version != VDXRFG_API_VERSION_5 ||
                    api5.reserved || api5.max_multiplier < info.multiplier || !api5.Create || !api5.Submit ||
                    !api5.Retire || !api5.Reset || !api5.GetStatistics || !api5.Destroy || !api5.GetOutputReadiness) {
                    // V5 guidance is optional. Keep the independently validated
                    // older color API when its extension is malformed/unavailable.
                    api5 = {};
                }
                const uint32_t required =
                    VDXRFG_DEPTH_CANONICAL_TRANSPORT |
                    (info.depthMode == 2 ? VDXRFG_DEPTH_RECONSTRUCTION : VDXRFG_DEPTH_ENDPOINT_PREFERENCE);
                if ((api5.supported_depth_capabilities & required) != required ||
                    (info.depthMode == 2 && !(api5.supported_create_flags & VDXRFG_CREATE_DEPTH_RECONSTRUCTION))) {
                    api5 = {};
                } else {
                    hr = state->depth.initialize(info);
                    if (SUCCEEDED(hr))
                        hr = shareTexture(state->depth.atlas.Get(), info.device12, &state->guide12, state->pairHook);
                    if (FAILED(hr)) {
                        // Optional guidance allocation failure preserves the
                        // established older module path before any GPU work.
                        api5 = {};
                        state->depth = {};
                        state->guide12.Reset();
                    } else {
                        state->api.Retire = api5.Retire;
                        state->api.Reset = api5.Reset;
                        state->api.GetStatistics = api5.GetStatistics;
                        state->api.Destroy = api5.Destroy;
                        state->api4.GetOutputReadiness = api5.GetOutputReadiness;
                    }
                }
            }
            const bool extended = getApi3 || getApi4 || state->api5.Create;
            if (info.multiplier == 4 && !extended)
                return VDXRFG_E_REJECTED;
            const uint32_t supportedFlags = state->api5.Create ? state->api5.supported_create_flags
                                            : extended         ? api3.supported_create_flags
                                                               : api2.supported_create_flags;
            uint32_t createFlags = supportedFlags & (VDXRFG_CREATE_HOST_OWNS_REAL |
                                                     (info.enableGpuTiming ? VDXRFG_CREATE_GPU_TIMING : 0u));
            if (info.useFidelityFxFull)
                createFlags |= VDXRFG_CREATE_FIDELITYFX_FULL;
            if (extended && (createFlags & VDXRFG_CREATE_HOST_OWNS_REAL) &&
                (supportedFlags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS))
                createFlags |= VDXRFG_CREATE_OMIT_REAL_OUTPUTS;
            if (state->api5.Create && info.opaqueInput && (supportedFlags & VDXRFG_CREATE_COLOR_OPAQUE) &&
                (state->api5.supported_depth_capabilities & VDXRFG_DEPTH_OPACITY_RESET))
                createFlags |= VDXRFG_CREATE_COLOR_OPAQUE;
            if (state->api5.Create && info.depthMode == 2)
                createFlags |= VDXRFG_CREATE_DEPTH_RECONSTRUCTION;
            state->hostOwnsReal = (createFlags & VDXRFG_CREATE_HOST_OWNS_REAL) != 0;
            if (state->deliveryDevice11 && !state->hostOwnsReal) {
                return VDXRFG_E_REJECTED;
            }
            state->opaqueInput = (createFlags & VDXRFG_CREATE_COLOR_OPAQUE) != 0;
            // Backend choice is required, unlike optional diagnostics/real
            // ownership. An old module must not silently substitute NVIDIA.
            if (info.useFidelityFxFull && (!(supportedFlags & VDXRFG_CREATE_FIDELITYFX_FULL))) {
                return VDXRFG_E_REJECTED;
            }
            D3D12_COMMAND_QUEUE_DESC queue{};
            hr = info.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&state->queue));
            if (FAILED(hr)) {
                return hr;
            }
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = 2 * info.eyeWidth;
            texture.Height = info.height;
            texture.MipLevels = texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            texture.SampleDesc.Count = 1;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            hr = info.device11->CreateTexture2D(&texture, nullptr, &state->input11);
            if (FAILED(hr)) {
                return hr;
            }
            hr = shareTexture(state->input11.Get(),
                              info.device12,
                              &state->input12,
                              state->pairHook,
                              state->deliveryDevice11.Get(),
                              &state->deliveryInput11);
            if (FAILED(hr)) {
                return hr;
            }
            VDXRFG_CreateInfoV3 create{};
            create.struct_size = sizeof(create);
            create.api_version = VDXRFG_API_VERSION_3;
            create.flags = createFlags;
            create.device = info.device12;
            create.queue = state->queue.Get();
            create.input = state->input12.Get();
            create.width = texture.Width;
            create.height = texture.Height;
            create.format = info.format;
            create.multiplier = info.multiplier;
            for (uint32_t bank = 0; bank < 2; ++bank) {
                for (uint32_t role = 0; role < 4; ++role) {
                    if ((role == 1 && info.multiplier == 2) || (role == 3 && info.multiplier != 4) ||
                        (role == 2 && (createFlags & VDXRFG_CREATE_OMIT_REAL_OUTPUTS))) {
                        continue;
                    }
                    hr = info.device11->CreateTexture2D(&texture, nullptr, &state->outputs11[bank][role]);
                    if (FAILED(hr)) {
                        return hr;
                    }
                    hr = shareTexture(state->outputs11[bank][role].Get(),
                                      info.device12,
                                      &state->outputs12[bank][role],
                                      state->pairHook,
                                      state->deliveryDevice11.Get(),
                                      &state->deliveryOutputs11[bank][role]);
                    if (FAILED(hr)) {
                        return hr;
                    }
                    create.outputs[bank][role] = state->outputs12[bank][role].Get();
                }
            }
            hr = info.device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->producer11));
            if (FAILED(hr)) {
                return hr;
            }
            HANDLE handle{};
            hr = state->producer11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
            vdxrfg::pair_capture::emit(state->pairHook,
                                       "producer-fence-export11",
                                       nullptr,
                                       0,
                                       state->producer11.Get(),
                                       info.device11,
                                       reinterpret_cast<uintptr_t>(handle),
                                       hr);
            if (FAILED(hr)) {
                return hr;
            }
            hr = info.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&state->producer12));
            vdxrfg::pair_capture::emit(state->pairHook,
                                       "producer-fence-import12",
                                       nullptr,
                                       0,
                                       state->producer12.Get(),
                                       info.device12,
                                       reinterpret_cast<uintptr_t>(handle),
                                       hr);
            CloseHandle(handle);
            if (FAILED(hr)) {
                return hr;
            }
            auto* consumerDevice = state->deliveryDevice11 ? state->deliveryDevice11.Get() : info.device11;
            hr = consumerDevice->CreateFence(0,
                                             state->deliveryDevice11 ? D3D11_FENCE_FLAG_SHARED : D3D11_FENCE_FLAG_NONE,
                                             IID_PPV_ARGS(&state->consumer11));
            if (FAILED(hr)) {
                return hr;
            }
            if (state->deliveryDevice11) {
                HANDLE consumerHandle{};
                hr = state->consumer11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &consumerHandle);
                if (FAILED(hr)) {
                    return hr;
                }
                hr = info.device11->OpenSharedFence(consumerHandle, IID_PPV_ARGS(&state->consumerProducer11));
                CloseHandle(consumerHandle);
                if (FAILED(hr)) {
                    return hr;
                }
            }
            if (state->api5.Create) {
                VDXRFG_CreateInfoV5 guide{};
                guide.struct_size = sizeof(guide);
                guide.api_version = VDXRFG_API_VERSION_5;
                guide.base = create;
                guide.guide_input = state->guide12.Get();
                hr = state->api5.Create(&guide, &state->context);
            } else if (extended) {
                hr = api3.Create(&create, &state->context);
            } else {
                VDXRFG_CreateInfoV1 legacy{};
                legacy.struct_size = sizeof(legacy);
                legacy.api_version = VDXRFG_API_VERSION;
                legacy.device = create.device;
                legacy.queue = create.queue;
                legacy.input = create.input;
                legacy.width = create.width;
                legacy.height = create.height;
                legacy.format = create.format;
                legacy.multiplier = create.multiplier;
                for (unsigned bank = 0; bank < 2; ++bank)
                    for (unsigned role = 0; role < 3; ++role)
                        legacy.outputs[bank][role] = create.outputs[bank][role];
                if (getApi2) {
                    VDXRFG_CreateInfoV2 options{};
                    options.struct_size = sizeof(options);
                    options.api_version = VDXRFG_API_VERSION_2;
                    options.base = legacy;
                    options.flags = createFlags;
                    hr = api2.Create(&options, &state->context);
                } else {
                    hr = state->api.Create(&legacy, &state->context);
                }
            }
            if (SUCCEEDED(hr) && !state->context)
                hr = VDXRFG_E_REJECTED;
            if (FAILED(hr)) {
                if (state->api5.Create && !state->context && hr != VDXRFG_E_POISONED) {
                    // A rejected optional Create owns no context/GPU work.
                    // Rebuild the entire attempt so legacy flags and required
                    // output allocations come from the older API's own contract.
                    // Disabling the extension bounds this retry to one attempt.
                    const bool requested = info.enableDepthGuidance;
                    auto fallbackInfo = info;
                    fallbackInfo.enableDepthGuidance = false;
                    state.reset();
                    const auto result = initialize(fallbackInfo);
                    if (m_state)
                        m_state->info.enableDepthGuidance = requested;
                    return result;
                }
                if (state->context || hr == VDXRFG_E_POISONED) {
                    state->poisoned = true;
                    // A poisoned module without a returned settlement context
                    // cannot prove when its attempted GPU work is safe to free.
                    if (!state->context)
                        state->hostCompletionUnknown = true;
                    m_state = std::move(state);
                }
                return hr;
            }
            // Request eligibility is immutable for this module's lifetime.
            // Only an authoritative disabled result can suppress optional
            // output observation; absent/unknown status preserves compatibility.
            if (state->pairStatus)
                state->outputObservationEnabled = state->pairStatus(0) != 0;
            m_state = std::move(state);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
#endif
    }

    HRESULT FrameGeneration::capture(const FinalProjectionInput& input, OwnedFrameTransaction& transaction) noexcept {
        return capture(input, transaction, nullptr);
    }

    HRESULT FrameGeneration::capture(const FinalProjectionInput& input,
                                     OwnedFrameTransaction& transaction,
                                     CaptureDiagnostics* diagnostics) noexcept {
        if (diagnostics)
            *diagnostics = {};
        if (!m_state) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        // Packing can overwrite the shared atlas before Submit rejects. No
        // failed capture may leave an older ticket claiming those pixels.
        if (!state.deliveryDevice11) {
            state.inputOwnerToken = state.inputOwnerGeneration = 0;
        }
        if (state.poisoned || transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        if (state.deliveryDevice11 && state.inputOwnerToken) {
            for (unsigned bank = 0; bank < state.liveTokens.size(); ++bank) {
                if (state.liveTokens[bank] == state.inputOwnerToken && !state.roleConsumerValues[bank][2]) {
                    // The canonical atlas is this live B's only delivery view.
                    // A rejected attempt must not invalidate that owner.
                    return VDXRFG_E_BUSY;
                }
            }
        }
        if (state.producerValue == UINT64_MAX || input.slots.count == 0 || input.slots.count > 4 ||
            (input.slots.count != 1 && input.slots.count != state.info.multiplier) || !input.serial ||
            input.time <= 0 || input.period <= 0 ||
            input.slots.roles[input.slots.count - 1] != FrameGenerationRole::real ||
            input.slots.targets[input.slots.count - 1] != input.time) {
            return VDXRFG_E_REJECTED;
        }
        auto producerCompleted = state.producer11->GetCompletedValue();
        if (producerCompleted == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (producerCompleted < state.producerValue) {
            producerCompleted = state.producer11->GetCompletedValue();
            if (producerCompleted == UINT64_MAX) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        if (producerCompleted < state.producerValue ||
            std::all_of(state.liveTokens.begin(), state.liveTokens.end(), [](uint64_t token) { return token != 0; })) {
            return VDXRFG_E_BUSY;
        }
        VDXRFG_SubmitInfoV3 submit{};
        submit.struct_size = sizeof(submit);
        submit.api_version = VDXRFG_API_VERSION_3;
        submit.input = state.input12.Get();
        submit.producer_fence = state.producer12.Get();
        submit.serial = input.serial;
        submit.display_time_ns = input.time;
        submit.display_period_ns = input.period;
        D3D11_TEXTURE2D_DESC description{};
        uint32_t bgraMask{};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& view = input.views[eye];
            const auto& rect = view.rect;
            if (!input.colour[eye]) {
                return VDXRFG_E_REJECTED;
            }
            // Final NR eyes share one packed texture. Its device and immutable
            // description need checking once, but each view still needs validation.
            if (eye == 0 || input.colour[eye].Get() != input.colour[0].Get()) {
                if (!sameDevice(state.device11.Get(), input.colour[eye].Get())) {
                    return VDXRFG_E_REJECTED;
                }
                input.colour[eye]->GetDesc(&description);
            }
            // Runtime-resolved array slice zero can alias the app array; its
            // selected image is subresource zero. Higher slices are resolved
            // to standalone images before reaching this interface.
            const bool eyeBgra = isBgra(description.Format);
            if (description.SampleDesc.Count != 1 || !description.ArraySize || !rect.width || !rect.height ||
                rect.width != state.info.eyeWidth || rect.height != state.info.height ||
                uint64_t(rect.x) + rect.width > description.Width ||
                uint64_t(rect.y) + rect.height > description.Height ||
                (!eyeBgra && description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                 description.Format != state.info.format && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
                return VDXRFG_E_REJECTED;
            }
            if (eyeBgra)
                bgraMask |= 1u << eye;
            submit.views[eye] = view;
            submit.views[eye].rect.x = eye * state.info.eyeWidth;
            submit.views[eye].rect.y = 0;
            float norm{};
            for (const float component : view.orientation) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
                norm += component * component;
            }
            if (std::abs(norm - 1.f) > .02f) {
                return VDXRFG_E_REJECTED;
            }
            for (const float component : view.position) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
            }
            for (const float component : view.fov) {
                if (!std::isfinite(component)) {
                    return VDXRFG_E_REJECTED;
                }
            }
            if (view.fov[0] >= view.fov[1] || view.fov[3] >= view.fov[2]) {
                return VDXRFG_E_REJECTED;
            }
        }
        for (uint32_t slot = 0; slot < input.slots.count; ++slot) {
            submit.target_time_ns[slot] = input.slots.targets[slot];
            if (slot < input.slots.count - 1) {
                if (input.slots.roles[slot] != (slot == 0   ? FrameGenerationRole::synthetic1
                                                : slot == 1 ? FrameGenerationRole::synthetic2
                                                            : FrameGenerationRole::synthetic3) ||
                    !std::isfinite(input.slots.fractions[slot]) || input.slots.fractions[slot] <= 0 ||
                    input.slots.fractions[slot] >= 1 || input.slots.targets[slot] >= input.slots.targets[slot + 1]) {
                    return VDXRFG_E_REJECTED;
                }
                submit.fractions[slot] = input.slots.fractions[slot];
            }
        }
        if (bgraMask) {
            const auto result = state.bgra.initialize(state.info);
            // No GPU work or history change occurred. Optional conversion
            // allocation failure must preserve the complete real-frame path.
            if (FAILED(result))
                return VDXRFG_E_REJECTED;
        }
        const bool hasGuide = state.api5.Submit && state.depth.prepare(state.device11.Get(), input.depth);
        uint64_t guideEpoch{};
        if (hasGuide) {
            const auto signature = depthSignature(input);
            if (!state.guideEpoch || signature != state.guideSignature) {
                if (state.guideEpoch == UINT64_MAX)
                    return VDXRFG_E_REJECTED;
                ++state.guideEpoch;
                state.guideSignature = signature;
            }
            guideEpoch = state.guideEpoch;
        }
        if (input.slots.count == 1 && state.previousSerial) {
            const auto resetResult = state.api.Reset(state.context, 0);
            if (FAILED(resetResult)) {
                return resetResult;
            }
            state.previousSerial = 0;
        }
        // A subsequent producer never overwrites a registered input still read
        // by the FG queue. This is a GPU dependency, not a CPU completion wait.
        if (state.consumed11 && state.consumedValue) {
            HRESULT hr = state.context11->Wait(state.consumed11.Get(), state.consumedValue);
            vdxrfg::pair_capture::emit(state.pairHook,
                                       "prior-input-consumed-Wait-result",
                                       state.context,
                                       input.serial,
                                       state.context11.Get(),
                                       state.consumed11.Get(),
                                       state.consumedValue,
                                       hr);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        if (state.deliveryDevice11 && state.canonicalReadValue) {
            const auto result = state.context11->Wait(state.consumerProducer11.Get(), state.canonicalReadValue);
            if (FAILED(result)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        vdxrfg::pair_capture::Host pairPacket;
        pairPacket.context = state.context;
        pairPacket.producer = state.context11.Get();
        pairPacket.serial = input.serial;
        pairPacket.displayTime = input.time;
        pairPacket.nrEnabled = input.nrEnabled;
        pairPacket.entry = input.entry;
        pairPacket.hasGuide = hasGuide;
        pairPacket.canonical = state.depth.atlas.Get();
        pairPacket.guideEpoch = guideEpoch;
        pairPacket.priorConsumedFence = state.consumed11.Get();
        pairPacket.priorConsumedValue = state.consumedValue;
        pairPacket.producerFence = state.producer11.Get();
        pairPacket.importedProducerFence = state.producer12.Get();
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& from = input.depth[eye];
            auto& to = pairPacket.eyes[eye];
            to.source = from.source.Get();
            to.description = from.description;
            to.colour = from.colourRect;
            to.crop = from.colourCrop;
            to.depth = from.depthRect;
            to.view = from.captureView;
            to.releasedIndex = from.releasedIndex;
            to.arraySlice = from.arraySlice;
            to.minDepth = from.minDepth;
            to.maxDepth = from.maxDepth;
            to.nearZ = from.nearZ;
            to.farZ = from.farZ;
        }
        if (state.pairHook)
            state.pairHook(&pairPacket);
        if (nr_capture::hostRequest().valid)
            for (uint64_t eye = 0; eye < 2; ++eye)
                nr_capture::warmLayout11(input.serial, NR_FINAL_COLOUR, eye, input.colour[eye].Get());
        auto& nrEngine = nr_capture::captureEngine();
        const auto nrFrame = nrEngine.selectedFrame(input.serial);
        auto* nrReturnGuard = nr_capture::SourceReturnGuard::current();
        if (nrFrame < 2) {
            ID3D11Texture2D* nrFinal[2]{input.colour[0].Get(), input.colour[1].Get()};
            if (!nrEngine.observeFinalDimensions(nrFrame, input.views[0].rect.width, input.views[0].rect.height)) {
                nr_capture::hostCaptureTerminal(NR_BAD_IDENTITY, true);
                return VDXRFG_E_POISONED;
            }
            if (!hasGuide || !nrReturnGuard) {
                nr_capture::hostCaptureTerminal(NR_BAD_ROLE, true);
                return VDXRFG_E_POISONED;
            }
            if (!nrEngine.postFxReady(nrFrame)) {
                nr_capture::CapturePostFxInput actual{};
                actual.finalColour[0] = nrFinal[0];
                actual.finalColour[1] = nrFinal[1];
                actual.canonical = state.depth.atlas.Get();
                actual.canonicalLifetime = nr_capture::nrBankTextureLifetime(actual.canonical);
                actual.fgContext = reinterpret_cast<uint64_t>(state.context);
                if (!nrEngine.acquirePostFx(nrFrame, actual, *nrReturnGuard)) {
                    nr_capture::hostCaptureTerminal(NR_BAD_GROUP, true);
                    return VDXRFG_E_POISONED;
                }
            }
            if (!nrEngine.matchesFinalColour(nrFrame, nrFinal)) {
                nr_capture::hostCaptureTerminal(NR_BAD_ROLE, true);
                return VDXRFG_E_POISONED;
            }
            for (uint32_t eye = 0; eye < 2; ++eye) {
                nr_capture::SnapshotRoleMetadata geometry{};
                geometry.hostSerial = input.serial;
                geometry.geometryObserved = true;
                geometry.rectXbits = input.views[eye].rect.x;
                geometry.rectYbits = input.views[eye].rect.y;
                geometry.rectWidth = input.views[eye].rect.width;
                geometry.rectHeight = input.views[eye].rect.height;
                geometry.cropXbits = geometry.rectXbits;
                geometry.cropYbits = geometry.rectYbits;
                geometry.cropWidth = geometry.rectWidth;
                geometry.cropHeight = geometry.rectHeight;
                if (!nrEngine.capturePostFxRole(nrFrame, NR_FINAL_COLOUR, eye, *nrReturnGuard, geometry)) {
                    nr_capture::hostCaptureTerminal(NR_BAD_ROLE, true);
                    return VDXRFG_E_POISONED;
                }
            }
        }
        state.producerSources = input.colour;
        state.producerDepth = hasGuide ? input.depth : std::array<FrameGenerationDepthSource, 2>{};
        if (state.deliveryDevice11) {
            state.inputOwnerToken = state.inputOwnerGeneration = 0;
        }
        if (bgraMask) {
            state.bgra.copy(
                state.context11.Get(), state.input11.Get(), state.info.eyeWidth, bgraMask, input.colour, input.views);
        } else {
            copyStereo(state.context11.Get(), state.input11.Get(), state.info.eyeWidth, input.colour, input.views);
        }
        pairPacket.operation = 6;
        pairPacket.eventObject = state.input11.Get();
        pairPacket.eventOther = state.input12.Get();
        pairPacket.eventValue = bgraMask;
        for (unsigned eye = 0; eye < 2; ++eye) {
            pairPacket.colourSources[eye] = input.colour[eye].Get();
            pairPacket.colourViews[eye] = input.views[eye];
        }
        if (state.pairHook)
            state.pairHook(&pairPacket);
        if (hasGuide)
            state.depth.copy(state.context11.Get(),
                             state.info.eyeWidth,
                             state.info.height,
                             input.depth,
                             state.pairHook,
                             state.context,
                             input.serial);
        if (nrFrame < 2) {
            nr_capture::SnapshotRoleMetadata geometry{};
            geometry.hostSerial = input.serial;
            geometry.geometryObserved = true;
            geometry.rectWidth = uint64_t(state.info.eyeWidth) * 2;
            geometry.rectHeight = state.info.height;
            geometry.cropWidth = geometry.rectWidth;
            geometry.cropHeight = geometry.rectHeight;
            if (!nrEngine.capturePostFxRole(nrFrame, NR_CANONICAL_DEPTH, 2, *nrReturnGuard, geometry)) {
                nr_capture::hostCaptureTerminal(NR_BAD_ROLE, true);
                return VDXRFG_E_POISONED;
            }
            if (!nrEngine.finishFrameTransfer(nrFrame, *nrReturnGuard)) {
                nr_capture::hostCaptureTerminal(NR_BAD_OPERATION, true);
                return VDXRFG_E_POISONED;
            }
        }
        if (nr_capture::hostRequest().valid && hasGuide) {
            nr_capture::warmLayout11(input.serial, NR_CANONICAL_DEPTH, 2, state.depth.atlas.Get());
            nr_capture::warmLayoutFinish(input.serial);
        }
        pairPacket.operation = 1;
        if (state.pairHook)
            state.pairHook(&pairPacket);
        submit.producer_value = ++state.producerValue;
        HRESULT hr = state.context11->Signal(state.producer11.Get(), submit.producer_value);
        vdxrfg::pair_capture::emit(state.pairHook,
                                   "producer-Signal",
                                   state.context,
                                   input.serial,
                                   state.context11.Get(),
                                   state.producer11.Get(),
                                   submit.producer_value,
                                   hr);
        if (FAILED(hr)) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        state.context11->Flush(); // Dispatch the cross-API producer dependency; does not drain the GPU.
        vdxrfg::pair_capture::emit(state.pairHook,
                                   "producer-Flush-void",
                                   state.context,
                                   input.serial,
                                   state.context11.Get(),
                                   state.device11.Get(),
                                   submit.producer_value);
        pairPacket.operation = 3;
        pairPacket.producerValue = submit.producer_value;
        if (state.pairHook)
            state.pairHook(&pairPacket);
        if (state.api5.Submit) {
            VDXRFG_SubmitInfoV5 guide{};
            guide.struct_size = sizeof(guide);
            guide.api_version = VDXRFG_API_VERSION_5;
            guide.base = submit;
            if (hasGuide) {
                guide.guide_serial = input.serial;
                guide.guide_epoch = guideEpoch;
                guide.guide_mask = 3;
                guide.guide_encoding = VDXRFG_GUIDE_ENCODING_INVERSE_Z;
            }
            vdxrfg::pair_capture::emit(state.pairHook,
                                       "original-SubmitV5-enter",
                                       state.context,
                                       input.serial,
                                       state.input12.Get(),
                                       state.producer12.Get(),
                                       submit.producer_value);
            hr = state.api5.Submit(state.context, &guide, &transaction.ticket);
            if (diagnostics) {
                diagnostics->apiVersion = guide.api_version;
                diagnostics->guideMask = guide.guide_mask;
                diagnostics->guideSerial = guide.guide_serial;
                diagnostics->guideEpoch = guide.guide_epoch;
                diagnostics->guideEncoding = guide.guide_encoding;
            }
        } else if (state.api3.Submit) {
            hr = state.api3.Submit(state.context, &submit, &transaction.ticket);
            if (diagnostics)
                diagnostics->apiVersion = submit.api_version;
        } else {
            VDXRFG_SubmitInfoV1 legacy{};
            legacy.struct_size = sizeof(legacy);
            legacy.api_version = VDXRFG_API_VERSION;
            legacy.input = submit.input;
            legacy.producer_fence = submit.producer_fence;
            legacy.producer_value = submit.producer_value;
            legacy.serial = submit.serial;
            legacy.display_time_ns = submit.display_time_ns;
            legacy.display_period_ns = submit.display_period_ns;
            for (unsigned i = 0; i < 3; ++i)
                legacy.target_time_ns[i] = submit.target_time_ns[i];
            for (unsigned i = 0; i < 2; ++i) {
                legacy.fractions[i] = submit.fractions[i];
                legacy.views[i] = submit.views[i];
            }
            hr = state.api.Submit(state.context, &legacy, &transaction.ticket);
            if (diagnostics)
                diagnostics->apiVersion = legacy.api_version;
        }
        if (nr_capture::hostRequest().valid) {
            NrReadyV1 ready{};
            ready.size = sizeof ready;
            ready.version = 1;
            const bool bound =
                state.nrReady && state.nrReady(&ready) == 1 &&
                nr_capture::hostBindReceiver(
                    ready, state.nrReceiver, state.module, state.device12.Get(), state.queue.Get(), state.context);
            // Warm file hashing runs under the same producer owner leases as
            // selection, before readiness. Selected callbacks never rehash.
            const bool attested =
                hr == S_OK && transaction.ticket.token && bound && nr_capture::captureEngine().attestWarmModules();
            nr_capture::hostModuleAttested(input.serial, transaction.ticket.generation, attested);
            // Preserve the genuine original admission even if attestation fails.
            nr_capture::hostOriginalAdmission(input.serial,
                                              transaction.ticket.token,
                                              transaction.ticket.previous_serial,
                                              transaction.ticket.generation,
                                              hr);
        }
        pairPacket.operation = 2;
        pairPacket.originalResult = hr;
        pairPacket.ticket = transaction.ticket;
        if (state.pairHook)
            state.pairHook(&pairPacket);
        if (diagnostics) {
            diagnostics->submitCalled = true;
            diagnostics->submitResult = hr;
            if (SUCCEEDED(hr)) {
                diagnostics->previousSerial = transaction.ticket.previous_serial;
                diagnostics->currentSerial = transaction.ticket.current_serial;
                diagnostics->ticketToken = transaction.ticket.token;
            }
        }
        if (transaction.ticket.token) {
            transaction.ownerGeneration = state.generation;
            transaction.slots = input.slots;
            if (transaction.ticket.bank >= state.liveTokens.size() || state.liveTokens[transaction.ticket.bank]) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
            state.liveTokens[transaction.ticket.bank] = transaction.ticket.token;
            state.tickets[transaction.ticket.bank] = transaction.ticket;
            state.capturedSources[transaction.ticket.bank] = input.colour;
            state.capturedViews[transaction.ticket.bank] = input.views;
            state.capturedBgraMask[transaction.ticket.bank] = bgraMask;
        }
        if (FAILED(hr)) {
            state.poisoned = hr == VDXRFG_E_POISONED;
            return hr;
        }
        const bool prime = state.previousSerial == 0;
        const uint32_t expectedMask = (state.hostOwnsReal ? 0u : 4u) | (prime                        ? 0u
                                                                        : state.info.multiplier == 4 ? 11u
                                                                        : state.info.multiplier == 3 ? 3u
                                                                                                     : 1u);
        if (!transaction.ticket.token || transaction.ticket.prime != static_cast<uint32_t>(prime) ||
            transaction.ticket.role_mask != expectedMask || transaction.ticket.reserved ||
            transaction.ticket.current_serial != input.serial ||
            transaction.ticket.previous_serial != state.previousSerial || !transaction.ticket.generation ||
            !transaction.ticket.input_consumed_fence || !transaction.ticket.output_ready_fence ||
            !transaction.ticket.input_consumed_value || !transaction.ticket.output_ready_value) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        transaction.slots = input.slots;
        state.previousSerial = input.serial;
        state.liveTokens[transaction.ticket.bank] = transaction.ticket.token;
        if (!state.consumed11) {
            hr = importFence(state.device12.Get(),
                             state.device11.Get(),
                             static_cast<ID3D12Fence*>(transaction.ticket.input_consumed_fence),
                             &state.consumed11,
                             state.pairHook,
                             state.deliveryDevice11.Get(),
                             &state.deliveryConsumed11);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        state.consumedValue = transaction.ticket.input_consumed_value;
        if (!state.ready11) {
            hr = importFence(state.device12.Get(),
                             state.device11.Get(),
                             static_cast<ID3D12Fence*>(transaction.ticket.output_ready_fence),
                             &state.ready11,
                             state.pairHook,
                             state.deliveryDevice11.Get(),
                             &state.deliveryReady11);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        if (state.api4.GetOutputReadiness) {
            for (const unsigned role : {0u, 1u, 3u}) {
                if (!(transaction.ticket.role_mask & (1u << role)))
                    continue;
                VDXRFG_OutputReadinessV4 output{sizeof(output), VDXRFG_API_VERSION_4, nullptr, 0};
                hr = state.api4.GetOutputReadiness(state.context, transaction.ticket.token, role, &output);
                if (FAILED(hr) || output.struct_size != sizeof(output) || output.api_version != VDXRFG_API_VERSION_4 ||
                    !output.fence || !output.value) {
                    state.poisoned = state.hostCompletionUnknown = true;
                    return VDXRFG_E_POISONED;
                }
                if (output.fence == transaction.ticket.output_ready_fence) {
                    if (output.value == transaction.ticket.output_ready_value)
                        continue;
                } else if (role == 0) {
                    const auto fence = static_cast<ID3D12Fence*>(output.fence);
                    if (!state.firstReady11) {
                        hr = importFence(state.device12.Get(),
                                         state.device11.Get(),
                                         fence,
                                         &state.firstReady11,
                                         state.pairHook,
                                         state.deliveryDevice11.Get(),
                                         &state.deliveryFirstReady11);
                        if (FAILED(hr)) {
                            state.poisoned = state.hostCompletionUnknown = true;
                            return VDXRFG_E_POISONED;
                        }
                        state.firstReady12 = fence;
                    }
                    if (state.firstReady12.Get() == fence) {
                        state.firstReadyValues[transaction.ticket.bank] = output.value;
                        continue;
                    }
                }
                // Later roles must use the authoritative full completion.
                // Reject a broken negotiation while retaining all owned work.
                state.poisoned = state.hostCompletionUnknown = true;
                return VDXRFG_E_POISONED;
            }
        }
        state.inputOwnerToken = transaction.ticket.token;
        state.inputOwnerGeneration = transaction.ticket.generation;
        return S_OK;
    }

    bool FrameGeneration::ready(const OwnedFrameTransaction& transaction) const noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || !transaction.ticket.output_ready_fence ||
            transaction.ticket.bank >= 2 || transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return false;
        }
        const auto& ticket = m_state->tickets[transaction.ticket.bank];
        const auto value = m_state->ready11->GetCompletedValue();
        return value != UINT64_MAX && value >= ticket.output_ready_value;
    }

    HRESULT FrameGeneration::prepareOutputReadyWait(const OwnedFrameTransaction& transaction,
                                                    HANDLE& completionEvent,
                                                    FrameGenerationRole role,
                                                    bool registerPending) noexcept {
        completionEvent = nullptr;
        if (!m_state || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        if (state.poisoned) {
            return VDXRFG_E_POISONED;
        }
        const auto& ticket = state.tickets[transaction.ticket.bank];
        const auto index = static_cast<unsigned>(role);
        if (index >= 4 || (role != FrameGenerationRole::real && !(ticket.role_mask & (1u << index))))
            return VDXRFG_E_REJECTED;
        const bool first = role == FrameGenerationRole::synthetic1 && state.firstReadyValues[ticket.bank] != 0;
        const auto fence = first ? state.firstReady11.Get() : state.ready11.Get();
        const auto value = first ? state.firstReadyValues[ticket.bank] : ticket.output_ready_value;
        if (!fence || !ticket.output_ready_fence || !value) {
            return VDXRFG_E_REJECTED;
        }
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (completed >= value) {
            return S_OK;
        }
        if (!registerPending) {
            return VDXRFG_E_BUSY;
        }
        auto& registered =
            first ? state.registeredFirstReadyValues[ticket.bank] : state.registeredReadyValues[ticket.bank];
        if (registered != value) {
            const auto hr = fence->SetEventOnCompletion(value, state.readyCompletionEvent);
            if (FAILED(hr)) {
                return hr;
            }
            registered = value;
        }
        // A timeout keeps the kernel registration live. Never reset or close
        // this event here: an older target may still signal it after a timeout.
        completionEvent = state.readyCompletionEvent;
        return VDXRFG_E_BUSY;
    }

    HRESULT FrameGeneration::copyOutput(OwnedFrameTransaction& transaction,
                                        FrameGenerationRole role,
                                        ID3D11Texture2D* destination) noexcept {
        std::optional<vdxrfg::pair_capture::Host> packet;
        const auto clock = []() noexcept {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            return static_cast<uint64_t>(qpc.QuadPart);
        };
        if (m_state && m_state->pairHook && m_state->outputObservationEnabled) {
            packet.emplace();
            packet->operation = 7;
            packet->ticket = transaction.ticket;
            packet->serial = transaction.ticket.current_serial;
            packet->context = m_state->context;
            auto& observation = packet->output;
            observation.callQpc = clock();
            observation.ownerGeneration = transaction.ownerGeneration;
            observation.role = static_cast<uint32_t>(role);
            observation.destination11 = reinterpret_cast<uintptr_t>(destination);
            observation.callId = ++m_state->outputCallId;
            observation.hostGeneration = m_state->generation;
            observation.hostContext11 = reinterpret_cast<uintptr_t>(m_state->context11.Get());
            observation.fence11 = reinterpret_cast<uintptr_t>(m_state->consumer11.Get());
            if (transaction.ticket.bank < 2 && observation.role < 4) {
                observation.roleValueBefore = m_state->roleConsumerValues[transaction.ticket.bank][observation.role];
                observation.retainedDestination11Before = reinterpret_cast<uintptr_t>(
                    m_state->retainedDestinations[transaction.ticket.bank][observation.role].Get());
                if (role != FrameGenerationRole::real) {
                    observation.source11 = reinterpret_cast<uintptr_t>(
                        m_state->outputs11[transaction.ticket.bank][observation.role].Get());
                    observation.source12 = reinterpret_cast<uintptr_t>(
                        m_state->outputs12[transaction.ticket.bank][observation.role].Get());
                }
            }
        }
        const auto identity = [&]() noexcept {
            const auto& observation = packet->output;
            return vdxrfg::pair_capture::OutputIdentity{reinterpret_cast<uintptr_t>(packet->context),
                                                        observation.hostGeneration,
                                                        packet->ticket.token,
                                                        packet->ticket.generation,
                                                        packet->ticket.bank,
                                                        observation.role,
                                                        observation.source11,
                                                        observation.source12,
                                                        observation.destination11,
                                                        observation.fence11};
        };
        const auto finish = [&](HRESULT result) noexcept {
            if (packet) {
                packet->originalResult = result;
                packet->output.consumerValue = transaction.consumerValue;
                packet->output.returnQpc = clock();
                m_state->pairHook(&*packet);
            }
            return result;
        };
        if (!m_state || m_state->poisoned || !destination || !transaction.ticket.token ||
            transaction.ticket.bank >= 2 || transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return finish(VDXRFG_E_REJECTED);
        }
        auto& state = *m_state;
        auto* copyContext = state.deliveryContext11 ? state.deliveryContext11.Get() : state.context11.Get();
        auto* copyDevice = state.deliveryDevice11 ? state.deliveryDevice11.Get() : state.device11.Get();
        if (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
            role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3) {
            return finish(VDXRFG_E_REJECTED);
        }
        const uint32_t index = static_cast<unsigned>(role);
        const auto& ticket = state.tickets[transaction.ticket.bank];
        const bool available =
            (role == FrameGenerationRole::real && state.hostOwnsReal) || (ticket.role_mask & (1u << index));
        if (!available || state.consumerValue == UINT64_MAX || !sameDevice(copyDevice, destination)) {
            return finish(VDXRFG_E_REJECTED);
        }
        D3D11_TEXTURE2D_DESC description{};
        destination->GetDesc(&description);
        if (packet) {
            packet->output.destinationDescription = description;
            packet->output.descriptionObserved = true;
        }
        if (description.Width != 2 * state.info.eyeWidth || description.Height != state.info.height ||
            description.ArraySize != 1 || description.MipLevels != 1 || description.SampleDesc.Count != 1 ||
            (description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && description.Format != state.info.format &&
             description.Format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
            return finish(VDXRFG_E_REJECTED);
        }
        if (state.roleConsumerValues[transaction.ticket.bank][index]) {
            if (packet) {
                auto& observation = packet->output;
                observation.prepared = true;
                const auto& prior = state.successfulCopies[transaction.ticket.bank][index];
                observation.preparedReferenceValid = vdxrfg::pair_capture::preparedMatches(
                    identity(), prior, observation.roleValueBefore, observation.retainedDestination11Before);
                observation.copyId = observation.preparedReferenceValid ? prior.copyId : 0;
                observation.destinationRetentionEpoch = observation.copyId;
            }
            // A prepared role identifies one SDK image for the ticket's entire
            // lifetime. A cloned transaction must not redirect that GPU copy.
            if (state.retainedDestinations[transaction.ticket.bank][index].Get() != destination) {
                return finish(VDXRFG_E_REJECTED);
            }
            transaction.destinations[index] = destination;
            transaction.consumerValue = state.roleConsumerValues[transaction.ticket.bank][index];
            return finish(S_OK);
        }
        for (unsigned bank = 0; bank < state.liveTokens.size(); ++bank) {
            for (const auto& alias : state.deliveryOutputs11[bank]) {
                if (alias.Get() == destination) {
                    return finish(VDXRFG_E_REJECTED);
                }
            }
            for (const auto& prepared : state.retainedDestinations[bank]) {
                if (prepared.Get() == destination) {
                    return finish(VDXRFG_E_REJECTED);
                }
            }
            for (const auto& source : state.capturedSources[bank]) {
                if (source.Get() == destination) {
                    return finish(VDXRFG_E_REJECTED);
                }
            }
        }
        HRESULT hr = S_OK;
        if (state.deliveryDevice11 && destination == state.deliveryInput11.Get()) {
            return finish(VDXRFG_E_REJECTED);
        }
        if (state.deliveryDevice11 && role == FrameGenerationRole::real &&
            (state.inputOwnerToken != ticket.token || state.inputOwnerGeneration != ticket.generation)) {
            return finish(VDXRFG_E_REJECTED);
        }
        bool early = role == FrameGenerationRole::synthetic1 && state.firstReadyValues[ticket.bank] != 0;
        if (early) {
            const auto completed = state.ready11->GetCompletedValue();
            if (completed == UINT64_MAX) {
                state.poisoned = state.hostCompletionUnknown = true;
                return finish(VDXRFG_E_POISONED);
            }
            // A completed whole batch needs just the existing shared wait;
            // do not add a separate early wait when it cannot buy time.
            early = completed < ticket.output_ready_value;
        }
        if (role != FrameGenerationRole::real && !state.queuedFullReadyWait[ticket.bank] &&
            (!early || !state.queuedFirstReadyWait[ticket.bank])) {
            const auto fence = state.deliveryDevice11
                                   ? (early ? state.deliveryFirstReady11.Get() : state.deliveryReady11.Get())
                                   : (early ? state.firstReady11.Get() : state.ready11.Get());
            const auto value = early ? state.firstReadyValues[ticket.bank] : ticket.output_ready_value;
            hr = copyContext->Wait(fence, value);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return finish(VDXRFG_E_POISONED);
            }
            if (early)
                state.queuedFirstReadyWait[ticket.bank] = true;
            else
                state.queuedFullReadyWait[ticket.bank] = true;
        }
        // Test-only selected B role0 readback changes shared output state after
        // the product fence. Queue an imported private-fence GPU wait before
        // the original D3D11 read; never CPU-drain while holding this lease.
        if (state.outputObservationEnabled && state.pairOutputWait) {
            hr = state.pairOutputWait(state.context, ticket.token, index, state.context11.Get());
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return finish(VDXRFG_E_POISONED);
            }
        }
        transaction.destinations[index] = destination;
        state.retainedDestinations[transaction.ticket.bank][index] = destination;
        if (state.deliveryDevice11 && role == FrameGenerationRole::real) {
            // input_consumed is the COMMON-state handoff for this packed
            // atlas, not the later full synthesis completion fence.
            hr = copyContext->Wait(state.deliveryConsumed11.Get(), ticket.input_consumed_value);
            if (FAILED(hr)) {
                state.poisoned = state.hostCompletionUnknown = true;
                return finish(VDXRFG_E_POISONED);
            }
            auto views = state.capturedViews[ticket.bank];
            for (uint32_t eye = 0; eye < 2; ++eye) {
                views[eye].rect.x = eye * state.info.eyeWidth;
                views[eye].rect.y = 0;
            }
            const std::array<ComPtr<ID3D11Texture2D>, 2> sources{state.deliveryInput11, state.deliveryInput11};
            copyStereo(copyContext, destination, state.info.eyeWidth, sources, views);
        } else if (role == FrameGenerationRole::real) {
            if (packet) {
                auto& observation = packet->output;
                observation.sourcePath = "real-copyStereo";
                observation.source11Eyes[0] =
                    reinterpret_cast<uintptr_t>(state.capturedSources[transaction.ticket.bank][0].Get());
                observation.source11Eyes[1] =
                    reinterpret_cast<uintptr_t>(state.capturedSources[transaction.ticket.bank][1].Get());
            }
            // B is already finalized by NR, sharpening and upscaling. Reading
            // that retained source avoids waiting for synthesis just to fall
            // back to the real frame. It still obeys the producer's NR fence.
            if (state.capturedBgraMask[transaction.ticket.bank]) {
                const auto completed = state.hostOwnsReal && state.inputOwnerToken == ticket.token &&
                                               state.inputOwnerGeneration == ticket.generation && state.consumed11
                                           ? state.consumed11->GetCompletedValue()
                                           : UINT64_MAX;
                if (completed != UINT64_MAX && completed >= ticket.input_consumed_value) {
                    auto views = state.capturedViews[transaction.ticket.bank];
                    for (uint32_t eye = 0; eye < 2; ++eye) {
                        views[eye].rect.x = eye * state.info.eyeWidth;
                        views[eye].rect.y = 0;
                    }
                    // Only the written canonical eye rectangles are valid;
                    // coalesce them only when copyStereo proves adjacency.
                    const std::array<ComPtr<ID3D11Texture2D>, 2> sources{state.input11, state.input11};
                    if (packet)
                        packet->output.source11Eyes[0] = packet->output.source11Eyes[1] =
                            reinterpret_cast<uintptr_t>(state.input11.Get());
                    copyStereo(state.context11.Get(), destination, state.info.eyeWidth, sources, views);
                } else {
                    if (packet)
                        packet->output.sourcePath = "real-bgra";
                    state.bgra.copy(state.context11.Get(),
                                    destination,
                                    state.info.eyeWidth,
                                    state.capturedBgraMask[transaction.ticket.bank],
                                    state.capturedSources[transaction.ticket.bank],
                                    state.capturedViews[transaction.ticket.bank]);
                }
            } else {
                copyStereo(state.context11.Get(),
                           destination,
                           state.info.eyeWidth,
                           state.capturedSources[transaction.ticket.bank],
                           state.capturedViews[transaction.ticket.bank]);
            }
        } else {
            if (packet) {
                auto& observation = packet->output;
                observation.sourcePath = "generated-CopyResource";
                observation.source11Eyes[0] = observation.source11Eyes[1] = observation.source11;
            }
            copyContext->CopyResource(destination,
                                      state.deliveryDevice11
                                          ? state.deliveryOutputs11[transaction.ticket.bank][index].Get()
                                          : state.outputs11[transaction.ticket.bank][index].Get());
        }
        if (packet) {
            auto& observation = packet->output;
            observation.copyCalled = true;
            observation.copyQpc = clock();
            observation.copyId = observation.destinationRetentionEpoch = observation.callId;
        }
        transaction.consumerValue = ++state.consumerValue;
        state.consumerValues[transaction.ticket.bank] = transaction.consumerValue;
        state.roleConsumerValues[transaction.ticket.bank][index] = transaction.consumerValue;
        hr = copyContext->Signal(state.consumer11.Get(), transaction.consumerValue);
        if (packet) {
            packet->output.signalCalled = true;
            packet->output.signalResult = hr;
            packet->output.signalQpc = clock();
        }
        if (FAILED(hr)) {
            state.poisoned = state.hostCompletionUnknown = true;
            return finish(VDXRFG_E_POISONED);
        }
        copyContext->Flush(); // Queue the copy before presenting; never a CPU drain.
        if (state.deliveryDevice11 && role == FrameGenerationRole::real) {
            state.canonicalReadValue = transaction.consumerValue;
        }
        if (packet) {
            packet->output.flushCalled = true;
            packet->output.flushQpc = clock();
            state.successfulCopies[transaction.ticket.bank][index] = {
                identity(), packet->output.callId, transaction.consumerValue};
        }
        return finish(S_OK);
    }

    bool FrameGeneration::copyReady(const OwnedFrameTransaction& transaction, FrameGenerationRole role) const noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token ||
            (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
             role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3)) {
            return false;
        }
        const unsigned index = static_cast<unsigned>(role);
        const auto value = m_state->roleConsumerValues[transaction.ticket.bank][index];
        const auto completed = m_state->consumer11->GetCompletedValue();
        return value && completed != UINT64_MAX && completed >= value;
    }

    HRESULT FrameGeneration::waitForCopy(const OwnedFrameTransaction& transaction,
                                         FrameGenerationRole role,
                                         DWORD timeoutMs) noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token ||
            (role != FrameGenerationRole::real && role != FrameGenerationRole::synthetic1 &&
             role != FrameGenerationRole::synthetic2 && role != FrameGenerationRole::synthetic3)) {
            return VDXRFG_E_REJECTED;
        }
        const unsigned index = static_cast<unsigned>(role);
        auto& state = *m_state;
        const auto value = state.roleConsumerValues[transaction.ticket.bank][index];
        if (!value || timeoutMs == INFINITE) {
            return VDXRFG_E_REJECTED;
        }
        const auto initialCompleted = state.consumer11->GetCompletedValue();
        if (initialCompleted == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (initialCompleted >= value) {
            return S_OK;
        }
        auto& registered = state.registeredCopyValues[transaction.ticket.bank][index];
        if (registered != value) {
            const auto hr = state.consumer11->SetEventOnCompletion(value, state.copyCompletionEvent);
            if (FAILED(hr)) {
                return hr;
            }
            registered = value;
        }
        const auto result = WaitForSingleObject(state.copyCompletionEvent, timeoutMs);
        if (result == WAIT_FAILED) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        // A prior timed-out registration may wake this reusable event. Only
        // the authoritative current role fence decides whether it is ready.
        const auto completed = state.consumer11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        return completed >= value ? S_OK : VDXRFG_E_BUSY;
    }

    HRESULT FrameGeneration::prepareRetirementWait(const OwnedFrameTransaction& transaction,
                                                   HANDLE& completionEvent) noexcept {
        completionEvent = nullptr;
        if (!m_state || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        auto& state = *m_state;
        if (state.poisoned) {
            return VDXRFG_E_POISONED;
        }
        const auto bank = transaction.ticket.bank;
        const auto completed = state.consumer11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            state.poisoned = state.hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        const auto value = state.consumerValues[bank];
        if (completed < value) {
            auto& registered = state.registeredRetirementCopyValues[bank];
            const auto& roleRegistrations = state.registeredCopyValues[bank];
            if (registered != value &&
                std::find(roleRegistrations.begin(), roleRegistrations.end(), value) == roleRegistrations.end()) {
                const auto result = state.consumer11->SetEventOnCompletion(value, state.copyCompletionEvent);
                if (FAILED(result)) {
                    return result;
                }
            }
            registered = value;
            completionEvent = state.copyCompletionEvent;
            return VDXRFG_E_BUSY;
        }
        // Real B can finish copying before synthesis. Retirement must also
        // observe the full-output fence, never only S1's early readiness.
        return prepareOutputReadyWait(transaction, completionEvent, FrameGenerationRole::real);
    }

    HRESULT FrameGeneration::retire(OwnedFrameTransaction& transaction) noexcept {
        if (!m_state || m_state->poisoned || !transaction.ticket.token || transaction.ticket.bank >= 2 ||
            transaction.ownerGeneration != m_state->generation ||
            m_state->liveTokens[transaction.ticket.bank] != transaction.ticket.token) {
            return VDXRFG_E_REJECTED;
        }
        const auto completed = m_state->consumer11->GetCompletedValue();
        if (completed == UINT64_MAX) {
            m_state->poisoned = m_state->hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (completed < m_state->consumerValues[transaction.ticket.bank]) {
            return VDXRFG_E_BUSY;
        }
        vdxrfg::pair_capture::emitTicket(
            m_state->pairHook, "original-Retire-enter", m_state->context, transaction.ticket, E_PENDING);
        const auto hr = m_state->api.Retire(m_state->context, transaction.ticket.token);
        vdxrfg::pair_capture::emitTicket(
            m_state->pairHook, "original-Retire-result", m_state->context, transaction.ticket, hr);
        nr_capture::hostOriginalRetirement(
            transaction.ticket.current_serial, transaction.ticket.token, transaction.ticket.generation, hr);
        if (SUCCEEDED(hr)) {
            if (m_state->inputOwnerToken == transaction.ticket.token) {
                m_state->inputOwnerToken = m_state->inputOwnerGeneration = 0;
            }
            m_state->liveTokens[transaction.ticket.bank] = 0;
            m_state->consumerValues[transaction.ticket.bank] = 0;
            m_state->roleConsumerValues[transaction.ticket.bank] = {};
            m_state->successfulCopies[transaction.ticket.bank] = {};
            m_state->registeredCopyValues[transaction.ticket.bank] = {};
            m_state->registeredRetirementCopyValues[transaction.ticket.bank] = 0;
            m_state->registeredReadyValues[transaction.ticket.bank] = 0;
            m_state->registeredFirstReadyValues[transaction.ticket.bank] = 0;
            m_state->firstReadyValues[transaction.ticket.bank] = 0;
            m_state->queuedFirstReadyWait[transaction.ticket.bank] = false;
            m_state->queuedFullReadyWait[transaction.ticket.bank] = false;
            m_state->tickets[transaction.ticket.bank] = {};
            m_state->retainedDestinations[transaction.ticket.bank] = {};
            m_state->capturedSources[transaction.ticket.bank] = {};
            m_state->capturedViews[transaction.ticket.bank] = {};
            transaction = {};
        }
        return hr;
    }

    HRESULT FrameGeneration::reset(uint32_t reason) noexcept {
        if (!m_state || m_state->poisoned) {
            return VDXRFG_E_REJECTED;
        }
        if (!m_state->deliveryDevice11) {
            m_state->inputOwnerToken = m_state->inputOwnerGeneration = 0;
        }
        const auto hr = m_state->api.Reset(m_state->context, reason);
        if (SUCCEEDED(hr)) {
            m_state->previousSerial = 0;
            m_state->inputOwnerToken = m_state->inputOwnerGeneration = 0;
        }
        return hr;
    }

    HRESULT FrameGeneration::shutdown() noexcept {
        if (!m_state) {
            return S_OK;
        }
        if (m_state->hostCompletionUnknown) {
            return VDXRFG_E_POISONED;
        }
        const auto produced = m_state->producer11->GetCompletedValue();
        if (produced == UINT64_MAX) {
            m_state->poisoned = m_state->hostCompletionUnknown = true;
            return VDXRFG_E_POISONED;
        }
        if (produced < m_state->producerValue) {
            return VDXRFG_E_BUSY;
        }
        if (std::any_of(
                m_state->liveTokens.begin(), m_state->liveTokens.end(), [](uint64_t token) { return token != 0; })) {
            return VDXRFG_E_BUSY;
        }
        if (m_state->context) {
            const auto hr = m_state->api.Destroy(m_state->context);
            if (FAILED(hr)) {
                return hr;
            }
            m_state->context = nullptr;
        }
        m_state.reset();
        return S_OK;
    }

    bool FrameGeneration::borrowPrivateNrCanonical(PrivateNrCanonicalBorrow& out) const noexcept {
        out = {};
        if (!nr_capture::hostRequest().valid || !m_state || !m_state->context || m_state->poisoned ||
            !m_state->api5.Submit || !m_state->depth.atlas || !m_state->device11 || !m_state->context11)
            return false;
        const auto lifetime = nr_capture::nrBankTextureLifetime(m_state->depth.atlas.Get());
        if (!lifetime)
            return false;
        out.context = m_state->context;
        out.device = m_state->device11.Get();
        out.producer = m_state->context11.Get();
        out.device12 = m_state->device12.Get();
        out.texture = m_state->depth.atlas.Get();
        out.resourceLifetime = lifetime;
        return true;
    }
    bool FrameGeneration::initialized() const noexcept {
        return m_state && m_state->context && !m_state->poisoned;
    }

    bool FrameGeneration::depthGuidanceEnabled() const noexcept {
        return m_state && m_state->context && m_state->api5.Submit && m_state->depth.atlas;
    }

    bool FrameGeneration::depthGuidanceRequested() const noexcept {
        return m_state && m_state->info.depthMode != 0 && m_state->info.enableDepthGuidance;
    }

    uint32_t FrameGeneration::negotiatedDepthMode() const noexcept {
        // A guided context is not proof that any particular submitted pair
        // contains usable depth. Per-frame guide masks remain separate.
        return depthGuidanceEnabled() ? m_state->info.depthMode : 0;
    }

    bool FrameGeneration::opaqueInputEnabled() const noexcept {
        return depthGuidanceEnabled() && m_state->opaqueInput;
    }

    HRESULT FrameGeneration::updateOpaqueInput(bool opaqueInput) noexcept {
        if (!initialized())
            return VDXRFG_E_REJECTED;
        auto& state = *m_state;
        // Older consumers retain conservative alpha-aware behavior. Enabling
        // this extension must not require tearing down the hardware backend.
        if (!depthGuidanceEnabled() || !(state.api5.supported_create_flags & VDXRFG_CREATE_COLOR_OPAQUE) ||
            !(state.api5.supported_depth_capabilities & VDXRFG_DEPTH_OPACITY_RESET) || state.opaqueInput == opaqueInput)
            return S_FALSE;
        const auto hr =
            state.api5.Reset(state.context, opaqueInput ? VDXRFG_RESET_COLOR_OPAQUE : VDXRFG_RESET_COLOR_ALPHA);
        if (SUCCEEDED(hr)) {
            state.inputOwnerToken = state.inputOwnerGeneration = 0;
            state.previousSerial = 0;
            state.info.opaqueInput = state.opaqueInput = opaqueInput;
            return S_OK;
        }
        return hr;
    }

    HRESULT FrameGeneration::getStatistics(VDXRFG_StatisticsV1& statistics) const noexcept {
        if (!m_state || !m_state->context) {
            return VDXRFG_E_REJECTED;
        }
        statistics = {};
        statistics.struct_size = sizeof(statistics);
        statistics.api_version = VDXRFG_API_VERSION;
        return m_state->api.GetStatistics(m_state->context, &statistics);
    }
} // namespace virtualdesktop_openxr
