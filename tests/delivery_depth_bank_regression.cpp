// Owned GPU gates prove completion dependencies, not gameplay timing.
#include "pch.h"
#include "runtime.h"
#define VDXR_DELIVERY_TRANSPORT_HELPERS_ONLY
#include "delivery_transport_regression.cpp"
#undef VDXR_DELIVERY_TRANSPORT_HELPERS_ONLY

namespace {
    struct ResolveCopyProbe {
        using CopyRegion = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        inline static CopyRegion original{};
        inline static thread_local ResolveCopyProbe* active{};
        ID3D11DeviceContext* context{};
        ID3D11Resource* destination{};
        ID3D11Resource* source{};
        unsigned calls{}, exactCopies{};

        static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* context,
                                           ID3D11Resource* destination,
                                           UINT destinationSubresource,
                                           UINT x,
                                           UINT y,
                                           UINT z,
                                           ID3D11Resource* source,
                                           UINT sourceSubresource,
                                           const D3D11_BOX* box) {
            if (active && context == active->context) {
                ++active->calls;
                if (destination == active->destination && source == active->source && !destinationSubresource &&
                    !sourceSubresource && !x && !y && !z && !box)
                    ++active->exactCopies;
            }
            original(context, destination, destinationSubresource, x, y, z, source, sourceSubresource, box);
        }

        ResolveCopyProbe(ID3D11DeviceContext4* target, ID3D11Resource* to, ID3D11Resource* from)
            : context(target), destination(to), source(from) {
            check(!active, "one exact producer copy probe");
            // A GPU Wait can switch the driver's context dispatch route. Sample
            // only S's current entry after that Wait; P need not share it.
            original = reinterpret_cast<CopyRegion>((*reinterpret_cast<void***>(target))[deliveryContextSlot(1)]);
            check(DetourTransactionBegin() == NO_ERROR, "begin exact producer copy probe");
            if (DetourUpdateThread(GetCurrentThread()) || DetourAttach(reinterpret_cast<PVOID*>(&original), copy)) {
                DetourTransactionAbort();
                throw std::runtime_error("attach exact producer copy probe");
            }
            check(DetourTransactionCommit() == NO_ERROR, "commit exact producer copy probe");
            active = this;
        }
        ~ResolveCopyProbe() {
            active = nullptr;
            if (DetourTransactionBegin() || DetourUpdateThread(GetCurrentThread()) ||
                DetourDetach(reinterpret_cast<PVOID*>(&original), copy) || DetourTransactionCommit())
                std::terminate();
        }
    };

    ovrResult createDepthBank(DeliveryTransport& transport,
                              const ovrTextureSwapChainDesc& desc,
                              ovrTextureSwapChain* chain) {
#ifdef VDXR_DEPTH_DOMAIN_BASELINE
        // The retained runtime created FG depth through this ordinary route.
        return transport.create(desc, chain);
#else
        return transport.createFrameGenerationDepth(desc, chain);
#endif
    }

    ovrResult acquireDepthBank(DeliveryTransport& transport, ovrTextureSwapChain chain, int* index) {
#ifdef VDXR_DEPTH_DOMAIN_BASELINE
        // The retained frame producer issued its global guard before depth writes.
        transport.orderProducerAfterNativeReads();
        return transport.current(chain, index);
#else
        return transport.acquireFrameGenerationDepthForWrite(chain, index);
#endif
    }

    ovrTextureSwapChainDesc bankDescription(bool depth) {
        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleCount = 1;
        desc.Format = depth ? OVR_FORMAT_D32_FLOAT : OVR_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = depth ? ovrTextureBind_DX_DepthStencil : ovrTextureBind_DX_RenderTarget;
        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
        return desc;
    }

    void writeDepth(ID3D11Device5* device,
                    ID3D11DeviceContext4* context,
                    DeliveryTransport& transport,
                    ovrTextureSwapChain chain,
                    int index,
                    float value) {
        ComPtr<ID3D11Texture2D> image;
        sdk(transport.buffer(chain, index, IID_PPV_ARGS(&image)), "actual depth-bank producer image");
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> view;
        success(device->CreateDepthStencilView(image.Get(), &desc, &view), "depth-bank writer view");
        context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, value, 0);
    }

    ComPtr<ID3D11Fence> producerMarker(Interop& hw) {
        ComPtr<ID3D11Fence> marker;
        success(hw.device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker)), "writer marker");
        success(hw.context->Signal(marker.Get(), 1), "marker after actual producer writes");
        hw.context->Flush();
        return marker;
    }

    void verifyNativeDepth(DeliveryDevice& presentation, ID3D11Texture2D* image, float expected) {
        const auto bytes = presentation.read(image);
        check(bytes.size() == width * height * sizeof(float), "full native depth readback");
        for (size_t offset = 0; offset < bytes.size(); offset += sizeof(float)) {
            float actual{};
            std::memcpy(&actual, bytes.data() + offset, sizeof(actual));
            check(actual == expected, "native depth keeps the original bank pixels after later producer overwrite");
        }
    }

    bool rejectsBankWriter(DeliveryTransport& transport, ovrTextureSwapChain chain) {
        int index = -1;
        try {
            return OVR_FAILURE(acquireDepthBank(transport, chain, &index));
        } catch (...) {
            return true;
        }
    }
} // namespace

namespace virtualdesktop_openxr {
    struct RuntimeFrameGenerationRegression {
        static void runDepthBank(const wchar_t* backend, const std::wstring& scenario) {
            auto hwOwner = std::make_unique<Interop>();
            auto& hw = *hwOwner;
            auto presentationOwner = std::make_unique<DeliveryDevice>(hw.gpu.device.Get());
            auto& presentation = *presentationOwner;
            auto runtimeOwner = std::make_unique<OpenXrRuntime>();
            auto& runtime = *runtimeOwner;
            runtime.stopRegistryWatcher();
            ovrInitParams init{};
            init.Flags = ovrInit_RequestVersion | ovrInit_FocusAware;
            init.RequestedMinorVersion = OVR_MINOR_VERSION;
            sdk(ovr_InitializeWithPathOverride(&init, backend), "initialize private depth-bank SDK");
            ovrGraphicsLuid luid{};
            sdk(ovr_Create(&runtime.m_ovrSession, &luid), "depth-bank SDK session");
            runtime.m_ovrSubmissionDevice = hw.device;
            runtime.m_ovrSubmissionContext = hw.context;
            runtime.m_frameBegun = 1;
            runtime.m_deliveryTransport = std::make_unique<DeliveryTransport>(runtime.m_ovrSession,
                                                                              hw.device.Get(),
                                                                              hw.context.Get(),
                                                                              presentation.device.Get(),
                                                                              presentation.context.Get());
            auto& transport = *runtime.m_deliveryTransport;
            const auto quarantine = [&] {
                (void)runtimeOwner.release();
                (void)presentationOwner.release();
                (void)hwOwner.release();
            };
            // Runtime destruction closes its SDK session before member destruction.
            // Complete the transport first, or retain the entire uncertain graph.
            auto cleanup = MakeScopeGuard([&] {
                if (transport.completionUnknown()) {
                    quarantine();
                    return;
                }
                try {
                    transport.shutdown();
                    runtime.m_deliveryTransport.reset();
                } catch (...) {
                    quarantine();
                    std::_Exit(3);
                }
            });

            const bool singleBank = scenario == L"depth-single";
            const bool depthOnly = scenario == L"depth-only" || singleBank;
            auto bankDesc = bankDescription(true);
            bankDesc.StaticImage = singleBank;
            ovrTextureSwapChain bank{};
            sdk(createDepthBank(transport, bankDesc, &bank), "create lifetime-typed depth bank");
            int bankLength{};
            sdk(transport.length(bank, &bankLength), "actual depth-bank length");
            check(bankLength > 0 && (!singleBank || bankLength == 1), "static depth-bank control owns one allocation");
            int bankIndex{};
            sdk(acquireDepthBank(transport, bank, &bankIndex), "acquire initial depth-bank writer");
            const int originalBankIndex = bankIndex;
            ComPtr<ID3D11Texture2D> originalBankImage;
            sdk(transport.buffer(bank, bankIndex, IID_PPV_ARGS(&originalBankImage)), "original bank allocation");
            writeDepth(hw.device.Get(), hw.context.Get(), transport, bank, bankIndex, .25f);
            sdk(transport.commit(bank), "publish depth producer receipt");

            if (scenario == L"published") {
                int queried{};
                sdk(transport.current(bank, &queried), "generic bank index remains a query after producer Commit");
                check(queried == bankIndex, "writer acquisition does not silently change generic index semantics");
                check(rejectsBankWriter(transport, bank), "producer-committed bank rejects another writer");
                auto batch = transport.prepare({bank}, 1);
                check(rejectsBankWriter(transport, bank), "published depth bank rejects another writer");
                transport.copy(batch);
                presentation.drain();
                check(transport.statistics().nativeCopies == 1 && transport.statistics().nativeCommits == 0,
                      "copy and readDone finish before any FG consumer/native Commit");
                check(rejectsBankWriter(transport, bank),
                      "completed native read cannot release a copied but uncommitted bank");
                transport.commitCopied(batch);
                sdk(acquireDepthBank(transport, bank, &bankIndex), "retired read permits next bank writer");
                writeDepth(hw.device.Get(), hw.context.Get(), transport, bank, bankIndex, .75f);
                check(completed(producerMarker(hw).Get(), 1), "bank writer resumes after actual receipt Commit");
                std::puts("PASS: unpublished FG consumer completion cannot release a reserved depth bank");
                return;
            }

            if (scenario == L"copy-signal-failure") {
                const auto batch = transport.prepare({bank}, 1);
                TransportFaults fault(presentation.context.Get());
                fault.badSignal = true;
                bool failed{};
                try {
                    transport.copy(batch);
                } catch (...) {
                    failed = true;
                }
                check(failed && fault.injections == 1 && transport.statistics().nativeCopies == 1 &&
                          transport.statistics().nativeCommits == 0 && transport.completionUnknown(),
                      "failure after depth CopyResource and before readDone poisons its owners");
                check(rejectsBankWriter(transport, bank), "unknown depth-copy completion rejects bank reuse");
                std::puts("PASS: depth copy Signal failure retains the complete uncertain graph");
                return;
            }

            if (scenario == L"removed-ordinary" || scenario == L"removed-bank") {
                auto batch = transport.prepare({bank}, 1);
                transport.execute(batch);
                presentation.drain();
                ContextHooks hook(
                    hw.context.Get(), presentation.context.Get(), presentation.device.Get(), presentation.fence.Get());
                hook.recordFence = true;
                transport.orderProducerAfterNativeReads();
                check(hook.observedFence, "ordinary guard samples the actual imported native-read fence");
                hook.removedFence = hook.observedFence;
                bool failed{};
                try {
                    if (scenario == L"removed-bank")
                        failed = OVR_FAILURE(acquireDepthBank(transport, bank, &bankIndex));
                    else
                        transport.orderProducerAfterNativeReads();
                } catch (...) {
                    failed = true;
                }
                hook.removedFence = nullptr;
                check(failed && transport.completionUnknown(),
                      "physical fence MAX poisons even a depth-only graph with no ordinary read bound");
                check(rejectsBankWriter(transport, bank), "poisoned physical completion refuses depth writer");
                std::puts("PASS: scripted physical MAX is not hidden by the ordinary high-water value");
                return;
            }

            ovrTextureSwapChain selectedOrdinary{}, writerOrdinary{};
            sdk(transport.create(bankDescription(false), &selectedOrdinary), "ordinary selected source chain");
            sdk(transport.create(bankDescription(false), &writerOrdinary), "disjoint ordinary writer destination");
            sdk(transport.commit(selectedOrdinary), "ordinary producer receipt");

            // Seed a real runtime Swapchain that must take the production copy
            // branch, using only actual producer allocations and SDK handles.
            OpenXrRuntime::Swapchain writer{};
            writer.xrDesc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            writer.xrDesc.width = width;
            writer.xrDesc.height = height;
            writer.xrDesc.arraySize = writer.xrDesc.faceCount = writer.xrDesc.mipCount = writer.xrDesc.sampleCount = 1;
            writer.xrDesc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            writer.ovrDesc = bankDescription(false);
            writer.dxgiFormatForSubmission = DXGI_FORMAT_R8G8B8A8_UNORM;
            writer.lastReleasedIndex = 0;
            const auto pixels = scene(24);
            writer.appSwapchain.images.push_back(hw.texture(&pixels));
            writer.resolvedSlices.resize(1);
            writer.resolvedSlices[0].ovrSwapchain = writerOrdinary;
            sdk(transport.length(writerOrdinary, &writer.ovrSwapchainLength), "runtime writer destination length");
            for (int index = 0; index < writer.ovrSwapchainLength; ++index) {
                ComPtr<ID3D11Texture2D> image;
                sdk(transport.buffer(writerOrdinary, index, IID_PPV_ARGS(&image)), "runtime writer destination");
                writer.resolvedSlices[0].images.push_back(std::move(image));
            }
            int writerIndex{};
            sdk(transport.current(writerOrdinary, &writerIndex), "exact ordinary resolve destination index");
            const auto destination = writer.resolvedSlices[0].images[writerIndex];
            const auto initialPixels = scene(0);
            check(initialPixels != pixels, "ordinary destination sentinel differs from its source");
            hw.context->UpdateSubresource(destination.Get(), 0, nullptr, initialPixels.data(), width * 4, 0);
            check(completed(producerMarker(hw).Get(), 1), "ordinary source and destination initialization completes");
            check(hw.read(destination.Get()) == initialPixels && hw.read(writer.appSwapchain.images[0].Get()) == pixels,
                  "real ordinary destination starts with different pixels before P is held");

            const auto selected = scenario == L"ordinary" ? std::vector<ovrTextureSwapChain>{selectedOrdinary}
                                  : scenario == L"mixed"  ? std::vector<ovrTextureSwapChain>{bank, selectedOrdinary}
                                                          : std::vector<ovrTextureSwapChain>{bank};
            int originalNativeIndex{};
            ComPtr<ID3D11Texture2D> originalNativeDepth;
            if (depthOnly) {
                sdk(ovr_GetTextureSwapChainCurrentIndex(runtime.m_ovrSession, bank, &originalNativeIndex),
                    "original native depth target index");
                sdk(ovr_GetTextureSwapChainBufferDX(
                        runtime.m_ovrSession, bank, originalNativeIndex, IID_PPV_ARGS(&originalNativeDepth)),
                    "original native depth target allocation");
            }
            auto batch = transport.prepare(selected, 1);
            // prepare flushes S readiness before P is held, preventing a gate
            // cycle from manufacturing the tested dependency.
            ProducerHold hold(presentation.device.Get(), presentation.context.Get(), hw.gpu.device.Get());
            transport.execute(batch);
            check(hold.after->GetCompletedValue() == 0, "P dependency is held by the owned GPU queue gate");
            if (scenario == L"bank-wait-failure") {
                TransportFaults fault(hw.context.Get());
                fault.badWait = true;
                const bool refused = rejectsBankWriter(transport, bank);
                const bool stillHeld = hold.after->GetCompletedValue() == 0;
                hold.finish();
                check(stillHeld && refused && fault.injections == 1 && transport.completionUnknown(),
                      "failed S bank-writer Wait on a pending depth-only P read poisons the graph");
                check(rejectsBankWriter(transport, bank), "failed bank-writer dependency cannot permit later reuse");
                std::puts("PASS: source-context bank-writer Wait failure retains the uncertain graph");
                return;
            }
            transport.orderProducerAfterNativeReads();
            OpenXrRuntime::SwapchainImageList resolved;
            unsigned copyCalls{}, exactCopies{};
            {
                ResolveCopyProbe probe(hw.context.Get(), destination.Get(), writer.appSwapchain.images[0].Get());
                runtime.resolveSwapchainImage(writer, 0, resolved);
                copyCalls = probe.calls;
                exactCopies = probe.exactCopies;
            }
            auto ordinaryDone = producerMarker(hw);
            const bool ordinaryProgress = completed(ordinaryDone.Get(), 1, 100);
            const bool pStillHeld = hold.after->GetCompletedValue() == 0;
            ComPtr<ID3D11Fence> bankDone;
            bool bankProgress{};
            if (depthOnly) {
                sdk(acquireDepthBank(transport, bank, &bankIndex), "same depth bank writer after native Commit");
                if (singleBank) {
                    ComPtr<ID3D11Texture2D> reused;
                    sdk(transport.buffer(bank, bankIndex, IID_PPV_ARGS(&reused)), "one-image bank writer allocation");
                    check(bankIndex == originalBankIndex && reused == originalBankImage,
                          "one-image bank rewrites the exact allocation read by pending P work");
                }
                writeDepth(hw.device.Get(), hw.context.Get(), transport, bank, bankIndex, .75f);
                bankDone = producerMarker(hw);
                bankProgress = completed(bankDone.Get(), 1, 20);
            }
            // Release before assertions or readbacks on both red and green runs.
            hold.finish();
            check(completed(ordinaryDone.Get(), 1) && (!bankDone || completed(bankDone.Get(), 1)),
                  "all real producer writes finish after their required P read retires");
            std::cout << "DEPTH_BANK scenario=" << xr::wide_to_utf8(scenario)
                      << " ordinary_progress=" << ordinaryProgress << " p_held=" << pStillHeld
                      << " same_bank_progress=" << bankProgress << " runtime_copies=" << exactCopies
                      << " copy_calls=" << copyCalls << '\n';
            check(pStillHeld && copyCalls == 1 && exactCopies == 1 &&
                      writer.resolvedSlices[0].lastCommittedIndex == writerIndex &&
                      writer.resolvedSlices[0].hasCopyForPreprocessing && writer.resolvedSlices[0].hasCommittedImage &&
                      resolved.size() == 1,
                  "marker follows the actual runtime resolve and producer receipt on a disjoint chain");
            check(hw.read(destination.Get()) == pixels, "actual runtime resolve preserves exact ordinary pixels");
            if (depthOnly) {
                verifyNativeDepth(presentation, originalNativeDepth.Get(), .25f);
                check(ordinaryProgress, "depth-only P read must not block the next ordinary production writer");
                check(!bankProgress, "same-bank depth write remains ordered after its native read");
            } else {
                check(!ordinaryProgress, "ordinary or mixed P batch retains the global producer dependency");
            }
            hw.gpu.noErrors();
            std::puts("PASS: exact depth-bank dependency and production ordinary writer controls");
        }
    };
} // namespace virtualdesktop_openxr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    try {
        check(argc == 3,
              "usage: private-SDK-directory "
              "depth-only|depth-single|ordinary|mixed|published|copy-signal-failure|bank-wait-failure|removed-ordinary|"
              "removed-bank");
        const std::wstring scenario = argv[2];
        check(scenario == L"depth-only" || scenario == L"depth-single" || scenario == L"ordinary" ||
                  scenario == L"mixed" || scenario == L"published" || scenario == L"copy-signal-failure" ||
                  scenario == L"bank-wait-failure" || scenario == L"removed-ordinary" || scenario == L"removed-bank",
              "known depth-bank dependency scenario");
        virtualdesktop_openxr::RuntimeFrameGenerationRegression::runDepthBank(argv[1], scenario);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
