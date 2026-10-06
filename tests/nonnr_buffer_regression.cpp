// CPU-only two-call buffer contracts against runtime methods, including failure-path counts.
#include "pch.h"
#include "runtime.h"

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams*, const wchar_t*);

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
ovrResult OVR_CDECL fixtureStencil(ovrSession, const ovrFovStencilDesc*, ovrFovStencilMeshBuffer* buffer) {
    buffer->UsedVertexCount = 3;
    buffer->UsedIndexCount = 3;
    if (buffer->AllocVertexCount >= 3 && buffer->VertexBuffer) {
        buffer->VertexBuffer[0] = {0.f, 0.f};
        buffer->VertexBuffer[1] = {1.f, 0.f};
        buffer->VertexBuffer[2] = {0.f, 1.f};
    }
    if (buffer->AllocIndexCount >= 3 && buffer->IndexBuffer) {
        buffer->IndexBuffer[0] = 0;
        buffer->IndexBuffer[1] = 1;
        buffer->IndexBuffer[2] = 2;
    }
    return ovrSuccess;
}
struct StencilProvider {
    decltype(&ovr_GetFovStencil) original{};
    StencilProvider(const wchar_t* backendDirectory) {
        ovrInitParams init{};
        init.Flags = ovrInit_RequestVersion;
        init.RequestedMinorVersion = OVR_MINOR_VERSION;
        const auto directory = std::filesystem::absolute(backendDirectory).wstring() + L"\\";
        require(OVR_SUCCESS(ovr_InitializeWithPathOverride(&init, directory.c_str())), "OVRNull initialization failed");
        original = reinterpret_cast<decltype(original)>(
            GetProcAddress(GetModuleHandleW(L"LibOVRRT64_1.dll"), "ovr_GetFovStencil"));
        require(original != nullptr, "OVRNull stencil export missing");
        require(DetourTransactionBegin() == NO_ERROR, "Stencil detour begin failed");
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&original), fixtureStencil);
        if (updated || attached) {
            DetourTransactionAbort();
            throw std::runtime_error("Stencil detour attach failed");
        }
        require(DetourTransactionCommit() == NO_ERROR, "Stencil detour commit failed");
    }
    ~StencilProvider() {
        const auto begun = DetourTransactionBegin();
        const auto updated = DetourUpdateThread(GetCurrentThread());
        const auto detached = DetourDetach(reinterpret_cast<PVOID*>(&original), fixtureStencil);
        const auto committed = DetourTransactionCommit();
        if (begun || updated || detached || committed) std::terminate();
    }
};
}

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
    inline static unsigned failures = 0;
    static XrInstance instance() { return reinterpret_cast<XrInstance>(1); }
    static XrSession session() { return reinterpret_cast<XrSession>(1); }
    static void expect(bool condition, const std::string& label) {
        std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
        if (!condition) ++failures;
    }
    static void seed(OpenXrRuntime& runtime) {
        runtime.stopRegistryWatcher();
        runtime.m_instanceCreated = true;
        runtime.m_systemCreated = true;
        runtime.m_sessionCreated = true;
        runtime.m_isHeadless = false;
        runtime.m_apiMinor = 0;
        runtime.has_XR_KHR_vulkan_enable = true;
    }
    static XrPath path(OpenXrRuntime& runtime, const char* text) {
        XrPath result{};
        require(runtime.xrStringToPath(instance(), text, &result) == XR_SUCCESS, "Fixture path creation failed");
        return result;
    }
    static XrAction boundAction(OpenXrRuntime& runtime) {
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "buffers");
        strcpy_s(setInfo.localizedActionSetName, "Buffers");
        XrActionSet set{};
        require(runtime.xrCreateActionSet(instance(), &setInfo, &set) == XR_SUCCESS, "Fixture action set failed");
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(actionInfo.actionName, "select");
        strcpy_s(actionInfo.localizedActionName, "Select");
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        XrAction action{};
        require(runtime.xrCreateAction(set, &actionInfo, &action) == XR_SUCCESS, "Fixture action failed");
        auto& internal = *reinterpret_cast<OpenXrRuntime::Action*>(action);
        OpenXrRuntime::ActionSource source{};
        source.realPath = "/user/hand/left/input/x/click";
        internal.actionSources[source.realPath] = source;
        source.realPath = "/user/hand/right/input/a/click";
        internal.actionSources[source.realPath] = source;
        runtime.m_attachedActionSets.insert(set);
        return action;
    }

    template<class Item, class Call>
    static void arrayCount(const char* name, Item initial, Call call, uint32_t expected = 0,
                           bool fullBufferControl = true) {
        uint32_t required = 0xabababab;
        const auto query = call(0, &required, nullptr);
        expect(query == XR_SUCCESS && required > 1 && (!expected || required == expected),
               std::string(name) + " zero capacity returns required count");
        require(query == XR_SUCCESS && required > 1 && required < 1000, "Array count query fixture failed");
        std::vector<Item> items(required + 1, initial);
        uint32_t count = 0xabababab;
        const auto shortResult = call(required - 1, &count, items.data());
        std::cout << name << " short result=" << shortResult << " count=" << count << " required=" << required << '\n';
        expect(shortResult == XR_ERROR_SIZE_INSUFFICIENT && count == required,
               std::string(name) + " insufficient capacity returns required count");
        if (fullBufferControl) {
            count = 0xabababab;
            const auto full = call(required, &count, items.data());
            expect(full == XR_SUCCESS && count == required, std::string(name) + " exact capacity succeeds");
        }
    }

    template<class Call>
    static void stringBuffer(const char* name, const std::string& expected, Call call, bool controlsOnly) {
        const auto required = uint32_t(expected.size() + 1);
        uint32_t count = 0xabababab;
        const auto query = call(0, &count, nullptr);
        expect(query == XR_SUCCESS && count == required, std::string(name) + " count includes terminator");
        const uint32_t capacities[] = {required - 2, required - 1, required, required + 8};
        for (const auto capacity : capacities) {
            if (controlsOnly && capacity < required) continue;
            std::vector<char> buffer(capacity + 8, '#');
            count = 0xabababab;
            const auto result = call(capacity, &count, buffer.data());
            const auto expectedResult = capacity < required ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
            std::cout << name << " capacity=" << capacity << " result=" << result << " count=" << count
                      << " required=" << required << '\n';
            expect(result == expectedResult && count == required,
                   std::string(name) + " capacity " + std::to_string(capacity) + " result and required count");
            expect(buffer[capacity] == '#', std::string(name) + " respects buffer capacity");
            if (capacity >= required) {
                expect(std::equal(expected.begin(), expected.end(), buffer.begin()) && buffer[expected.size()] == '\0',
                       std::string(name) + " returns full terminated string");
            }
        }
    }

    static void counts() {
        OpenXrRuntime runtime;
        seed(runtime);
        const auto action = boundAction(runtime);
        XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        bound.action = action;
        arrayCount("BoundSourcesForAction", XrPath{}, [&](uint32_t capacity, uint32_t* count, XrPath* items) {
            return runtime.xrEnumerateBoundSourcesForAction(session(), &bound, capacity, count, items);
        }, 2);
        runtime.has_XR_HTCX_vive_tracker_interaction = true;
        runtime.m_supportsBodyTracking = true;
        runtime.m_emulateViveTrackers = true;
        for (size_t i = 0; i < std::size(runtime.m_isTrackerDisabled); ++i) runtime.m_isTrackerDisabled[i] = i >= 2;
        arrayCount("ViveTrackerPathsHTCX", XrViveTrackerPathsHTCX{XR_TYPE_VIVE_TRACKER_PATHS_HTCX},
                   [&](uint32_t capacity, uint32_t* count, XrViveTrackerPathsHTCX* items) {
            return runtime.xrEnumerateViveTrackerPathsHTCX(instance(), capacity, count, items);
        }, 2);
        arrayCount("InstanceExtensionProperties", XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES},
                   [&](uint32_t capacity, uint32_t* count, XrExtensionProperties* items) {
            return runtime.xrEnumerateInstanceExtensionProperties(nullptr, capacity, count, items);
        });
        runtime.has_XR_EXT_local_floor = false;
        arrayCount("ReferenceSpaces", XR_REFERENCE_SPACE_TYPE_MAX_ENUM,
                   [&](uint32_t capacity, uint32_t* count, XrReferenceSpaceType* items) {
            return runtime.xrEnumerateReferenceSpaces(session(), capacity, count, items);
        }, 3);
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
        XrSpace base{};
        require(runtime.xrCreateReferenceSpace(session(), &spaceInfo, &base) == XR_SUCCESS, "LocateViews base fixture failed");
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = 1000000000;
        locate.space = base;
        XrViewState state{XR_TYPE_VIEW_STATE};
        // Query and undersized paths return before pose retrieval, so no external pose backend is needed.
        arrayCount("LocateViews", XrView{XR_TYPE_VIEW}, [&](uint32_t capacity, uint32_t* count, XrView* items) {
            return runtime.xrLocateViews(session(), &locate, &state, capacity, count, items);
        }, 2, false);
        require(runtime.xrDestroySpace(base) == XR_SUCCESS, "LocateViews base cleanup failed");
        // ensureOVRSession returns immediately for the token; neither tested capacity fetches device data.
        runtime.m_ovrSession = reinterpret_cast<ovrSession>(1);
        try {
            arrayCount("ViewConfigurationViews", XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                       [&](uint32_t capacity, uint32_t* count, XrViewConfigurationView* items) {
                return runtime.xrEnumerateViewConfigurationViews(instance(), 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                 capacity, count, items);
            }, 2, false);
        } catch (...) { runtime.m_ovrSession = nullptr; throw; }
        runtime.m_ovrSession = nullptr;
        arrayCount("SwapchainFormats", int64_t{}, [&](uint32_t capacity, uint32_t* count, int64_t* items) {
            return runtime.xrEnumerateSwapchainFormats(session(), capacity, count, items);
        });
        OpenXrRuntime::Swapchain chain{};
        chain.ovrDesc.StaticImage = false;
        chain.ovrSwapchainLength = 3;
        const auto swapchain = reinterpret_cast<XrSwapchain>(&chain);
        runtime.m_swapchains.insert(swapchain);
        arrayCount("SwapchainImages", XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR},
                   [&](uint32_t capacity, uint32_t* count, XrSwapchainImageD3D11KHR* items) {
            return runtime.xrEnumerateSwapchainImages(swapchain, capacity, count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(items));
        }, 3, false);
        runtime.m_swapchains.erase(swapchain);
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Count fixture session cleanup failed");
    }

    static void strings(bool controlsOnly) {
        OpenXrRuntime runtime;
        seed(runtime);
        boundAction(runtime);
        const std::string handPath = "/user/hand/left";
        const auto left = path(runtime, handPath.c_str());
        stringBuffer("PathToString", handPath, [&](uint32_t capacity, uint32_t* count, char* buffer) {
            return runtime.xrPathToString(instance(), left, capacity, count, buffer);
        }, controlsOnly);
        XrInputSourceLocalizedNameGetInfo localized{XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
        localized.sourcePath = path(runtime, "/user/hand/left/input/x/click");
        localized.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT;
        stringBuffer("InputSourceLocalizedName", "Left Hand", [&](uint32_t capacity, uint32_t* count, char* buffer) {
            return runtime.xrGetInputSourceLocalizedName(session(), &localized, capacity, count, buffer);
        }, controlsOnly);
        stringBuffer("VulkanInstanceExtensions",
            "VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities "
            "VK_KHR_external_fence_capabilities VK_KHR_get_physical_device_properties2",
            [&](uint32_t capacity, uint32_t* count, char* buffer) {
                return runtime.xrGetVulkanInstanceExtensionsKHR(instance(), 1, capacity, count, buffer);
            }, controlsOnly);
        stringBuffer("VulkanDeviceExtensions",
            "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory "
            "VK_KHR_external_memory_win32 VK_KHR_timeline_semaphore VK_KHR_external_semaphore "
            "VK_KHR_external_semaphore_win32",
            [&](uint32_t capacity, uint32_t* count, char* buffer) {
                return runtime.xrGetVulkanDeviceExtensionsKHR(instance(), 1, capacity, count, buffer);
            }, controlsOnly);
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "String fixture session cleanup failed");
    }

    static void visibility(const wchar_t* backendDirectory) {
        OpenXrRuntime runtime;
        seed(runtime);
        StencilProvider provider(backendDirectory);
        runtime.has_XR_KHR_visibility_mask = true;
        runtime.m_cachedEyeInfo[0].Fov = {1.f, 1.f, 1.f, 1.f};
        struct Case { uint32_t vertices; uint32_t indices; XrResult expected; };
        const Case cases[] = {{0, 0, XR_SUCCESS}, {2, 3, XR_ERROR_SIZE_INSUFFICIENT},
                              {3, 2, XR_ERROR_SIZE_INSUFFICIENT}, {3, 3, XR_SUCCESS}, {4, 5, XR_SUCCESS}};
        for (const auto& item : cases) {
            XrVector2f vertices[5]{};
            uint32_t indices[5]{99, 99, 99, 99, 99};
            XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
            mask.vertexCapacityInput = item.vertices;
            mask.indexCapacityInput = item.indices;
            mask.vertices = item.vertices ? vertices : nullptr;
            mask.indices = item.indices ? indices : nullptr;
            mask.vertexCountOutput = mask.indexCountOutput = 0xabababab;
            const auto result = runtime.xrGetVisibilityMaskKHR(session(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
            std::cout << "VisibilityMask capacities=" << item.vertices << ',' << item.indices << " result=" << result
                      << " counts=" << mask.vertexCountOutput << ',' << mask.indexCountOutput << " required=3,3\n";
            expect(result == item.expected && mask.vertexCountOutput == 3 && mask.indexCountOutput == 3,
                   "VisibilityMask capacities " + std::to_string(item.vertices) + "," + std::to_string(item.indices) +
                   " return both required counts");
            if (item.vertices >= 3 && item.indices >= 3)
                expect(indices[0] == 0 && indices[1] == 1 && indices[2] == 2 && indices[3] == 99,
                       "VisibilityMask full capacity returns the mesh within bounds");
        }
        require(runtime.xrDestroySession(session()) == XR_SUCCESS, "Visibility fixture session cleanup failed");
    }

    static int run(int argc, wchar_t** argv) {
        const std::wstring mode = argc > 1 ? argv[1] : L"cpu";
        if (mode == L"cpu" || mode == L"all") { counts(); strings(false); }
        else if (mode == L"counts") counts();
        else if (mode == L"strings" || mode == L"string-controls") strings(mode == L"string-controls");
        else require(mode == L"visibility", "Unknown mode: cpu, all, counts, strings, string-controls, visibility");
        if (mode == L"visibility" || mode == L"all") {
            require(argc == 3, "Visibility mode requires the explicit OVRNull directory");
            visibility(argv[2]);
        }
        std::cout << "failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try { return virtualdesktop_openxr::RuntimeInputRegression::run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 2; }
}
