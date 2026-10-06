// Runtime-linked GL rollback checks. D3D textures, submission devices, and fences are real.
// WGL calls and every GL operation are spies; no native GL context or settings writes.
#include "pch.h"
#include "runtime.h"
#include <exception>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void checkHr(HRESULT result) { require(SUCCEEDED(result), "D3D fixture operation failed"); }
const HDC fakeDC = reinterpret_cast<HDC>(0x1010);
const HGLRC fakeRC = reinterpret_cast<HGLRC>(0x2020);
struct GlSpy {
    bool rejectContext{};
    bool deleteError{};
    unsigned failStorageAt{};
    unsigned failMemoryCreateAt{};
    unsigned failImageCreateAt{};
    bool memoryCreationFailed{};
    bool imageCreationFailed{};
    bool failSemaphoreCreate{};
    bool failSemaphoreImport{};
    HDC currentDC{};
    HGLRC currentRC{};
    LUID adapterLuid{};
    GLenum pendingError{GL_NO_ERROR};
    unsigned invalidCalls{};
    unsigned bindCalls{};
    unsigned restoreCalls{};
    unsigned errorReads{};
    unsigned memoryCreates{};
    unsigned memoryImports{};
    unsigned imageCreates{};
    unsigned storageCalls{};
    unsigned imageDeletes{};
    unsigned memoryDeletes{};
    unsigned invalidImports{};
    unsigned invalidStorage{};
    unsigned invalidDeletes{};
    unsigned nestedContextBinds{};
    unsigned errorsClearedInNestedContext{};
    unsigned semaphoreCreates{};
    unsigned semaphoreImports{};
    unsigned semaphoreDeletes{};
    unsigned invalidSemaphoreImports{};
    unsigned queryCreates{};
    unsigned queryDeletes{};
    std::set<GLuint> memory;
    std::set<GLuint> images;
    std::set<GLuint> storedImages;
    std::set<GLuint> semaphores;
    std::set<GLuint> importedSemaphores;
    std::set<GLuint> queries;
} spy;
bool hookCleanupFailed;
decltype(&wglMakeCurrent) originalMakeCurrent = wglMakeCurrent;
decltype(&glGetError) originalGetError = glGetError;
decltype(&glDeleteTextures) originalDeleteTextures = glDeleteTextures;
decltype(&wglGetProcAddress) originalGetProcAddress = wglGetProcAddress;
decltype(&wglGetCurrentDC) originalGetCurrentDC = wglGetCurrentDC;
decltype(&wglGetCurrentContext) originalGetCurrentContext = wglGetCurrentContext;
decltype(&glFinish) originalFinish = glFinish;

BOOL WINAPI makeCurrent(HDC dc, HGLRC rc) {
    if (dc == fakeDC && rc == fakeRC) {
        ++spy.bindCalls;
        if (spy.rejectContext) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
        if (spy.currentDC == fakeDC && spy.currentRC == fakeRC) ++spy.nestedContextBinds;
        spy.currentDC = dc;
        spy.currentRC = rc;
        return TRUE;
    }
    if (dc == nullptr && rc == nullptr) {
        ++spy.restoreCalls;
        spy.currentDC = nullptr;
        spy.currentRC = nullptr;
        return TRUE;
    }
    ++spy.invalidCalls;
    return FALSE;
}
HDC WINAPI currentDC() { return spy.currentDC; }
HGLRC WINAPI currentRC() { return spy.currentRC; }
void APIENTRY finish() {}
GLenum APIENTRY getError() {
    ++spy.errorReads;
    const auto result = spy.pendingError;
    if (result != GL_NO_ERROR && spy.nestedContextBinds) ++spy.errorsClearedInNestedContext;
    spy.pendingError = GL_NO_ERROR;
    return result;
}
void APIENTRY createMemory(GLsizei count, GLuint* names) {
    if (count != 1 || !names) { ++spy.invalidCalls; return; }
    if (++spy.memoryCreates == spy.failMemoryCreateAt) {
        spy.memoryCreationFailed = true;
        spy.pendingError = GL_OUT_OF_MEMORY;
        return; // Creation failure need not provide an output name; model an untouched output.
    }
    *names = 200 + spy.memoryCreates;
    spy.memory.insert(*names);
}
void APIENTRY importMemory(GLuint memory, GLuint64, GLenum type, void* handle) {
    if (spy.memoryCreationFailed || !spy.memory.count(memory) || type != GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT || !handle) {
        ++spy.invalidCalls;
        ++spy.invalidImports;
    }
    ++spy.memoryImports;
}
void APIENTRY createTextures(GLenum target, GLsizei count, GLuint* names) {
    if (target != GL_TEXTURE_2D || count != 1 || !names) { ++spy.invalidCalls; return; }
    if (++spy.imageCreates == spy.failImageCreateAt) {
        spy.imageCreationFailed = true;
        spy.pendingError = GL_OUT_OF_MEMORY;
        return; // Do not manufacture a texture name when creation failed.
    }
    *names = 100 + spy.imageCreates;
    spy.images.insert(*names);
}
void APIENTRY storage(GLuint image, GLsizei levels, GLenum format, GLsizei width, GLsizei height,
                      GLuint memory, GLuint64 offset) {
    if (spy.memoryCreationFailed || spy.imageCreationFailed || !spy.images.count(image) ||
        !spy.memory.count(memory) || levels != 1 || format != GL_RGBA8 || width != 8 || height != 8 || offset != 0) {
        ++spy.invalidCalls;
        ++spy.invalidStorage;
    }
    if (++spy.storageCalls == spy.failStorageAt) spy.pendingError = GL_OUT_OF_MEMORY;
    else spy.storedImages.insert(image);
}
void APIENTRY deleteTextures(GLsizei count, const GLuint* names) {
    if (count == 1 && names && *names == 0) return; // GL ignores deletion of name zero.
    if (count != 1 || !names || !spy.images.erase(*names)) { ++spy.invalidCalls; ++spy.invalidDeletes; }
    if (names) spy.storedImages.erase(*names);
    ++spy.imageDeletes;
    if (spy.deleteError) spy.pendingError = GL_INVALID_OPERATION;
}
void APIENTRY deleteMemory(GLsizei count, const GLuint* names) {
    if (count == 1 && names && *names == 0) return;
    if (count != 1 || !names || !spy.memory.erase(*names)) { ++spy.invalidCalls; ++spy.invalidDeletes; }
    ++spy.memoryDeletes;
}
void APIENTRY deviceLuid(GLenum pname, GLubyte* bytes) {
    if (pname != GL_DEVICE_LUID_EXT || !bytes) { ++spy.invalidCalls; return; }
    memcpy(bytes, &spy.adapterLuid, sizeof(LUID));
}
void APIENTRY createSemaphores(GLsizei count, GLuint* names) {
    if (count != 1 || !names) { ++spy.invalidCalls; return; }
    ++spy.semaphoreCreates;
    if (spy.failSemaphoreCreate) { spy.pendingError = GL_OUT_OF_MEMORY; return; }
    *names = 600 + spy.semaphoreCreates;
    spy.semaphores.insert(*names);
}
void APIENTRY importSemaphore(GLuint semaphore, GLenum type, void* handle) {
    ++spy.semaphoreImports;
    if (!spy.semaphores.count(semaphore) || type != GL_HANDLE_TYPE_D3D12_FENCE_EXT || !handle) {
        ++spy.invalidSemaphoreImports;
        if (spy.pendingError == GL_NO_ERROR) spy.pendingError = GL_INVALID_VALUE;
        return;
    }
    if (spy.failSemaphoreImport) { spy.pendingError = GL_INVALID_OPERATION; return; }
    spy.importedSemaphores.insert(semaphore);
}
void APIENTRY deleteSemaphores(GLsizei count, const GLuint* names) {
    if (count == 1 && names && *names == 0) return;
    if (count != 1 || !names || !spy.semaphores.erase(*names)) { ++spy.invalidCalls; return; }
    spy.importedSemaphores.erase(*names);
    ++spy.semaphoreDeletes;
}
void APIENTRY semaphoreParameter(GLuint semaphore, GLenum pname, const GLuint64* value) {
    if (!spy.importedSemaphores.count(semaphore) || pname != GL_D3D12_FENCE_VALUE_EXT || !value) ++spy.invalidCalls;
}
void APIENTRY signalSemaphore(GLuint semaphore, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) {
    if (!spy.importedSemaphores.count(semaphore)) ++spy.invalidCalls;
}
void APIENTRY createQueries(GLsizei count, GLuint* names) {
    if (count != 2 || !names) { ++spy.invalidCalls; return; }
    for (GLsizei i = 0; i < count; ++i) {
        names[i] = 1000 + (++spy.queryCreates);
        spy.queries.insert(names[i]);
    }
}
void APIENTRY deleteQueries(GLsizei count, const GLuint* names) {
    if (count != 2 || !names) { ++spy.invalidCalls; return; }
    for (GLsizei i = 0; i < count; ++i) {
        if (!spy.queries.erase(names[i])) ++spy.invalidCalls;
        ++spy.queryDeletes;
    }
}
void APIENTRY queryCounter(GLuint id, GLenum target) {
    if (!spy.queries.count(id) || target != GL_TIMESTAMP) ++spy.invalidCalls;
}
void APIENTRY queryAvailable(GLuint id, GLenum pname, GLint* value) {
    if (!spy.queries.count(id) || pname != GL_QUERY_RESULT_AVAILABLE || !value) { ++spy.invalidCalls; return; }
    *value = 1;
}
void APIENTRY queryValue(GLuint id, GLenum pname, GLuint64* value) {
    if (!spy.queries.count(id) || pname != GL_QUERY_RESULT || !value) { ++spy.invalidCalls; return; }
    *value = 0;
}
// These storage variants are resolved by initialization but unused by the 2D fixture.
void APIENTRY unusedStorage2DMS(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLboolean, GLuint, GLuint64) {
    ++spy.invalidCalls;
}
void APIENTRY unusedStorage3D(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLuint, GLuint64) {
    ++spy.invalidCalls;
}
void APIENTRY unusedStorage3DMS(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean, GLuint, GLuint64) {
    ++spy.invalidCalls;
}
PROC WINAPI getProcAddress(LPCSTR name) {
#define GL_PROC(api, replacement) if (!strcmp(name, #api)) return reinterpret_cast<PROC>(replacement)
    GL_PROC(glGetUnsignedBytevEXT, deviceLuid);
    GL_PROC(glCreateTextures, createTextures);
    GL_PROC(glCreateMemoryObjectsEXT, createMemory);
    GL_PROC(glDeleteMemoryObjectsEXT, deleteMemory);
    GL_PROC(glTextureStorageMem2DEXT, storage);
    GL_PROC(glTextureStorageMem2DMultisampleEXT, unusedStorage2DMS);
    GL_PROC(glTextureStorageMem3DEXT, unusedStorage3D);
    GL_PROC(glTextureStorageMem3DMultisampleEXT, unusedStorage3DMS);
    GL_PROC(glGenSemaphoresEXT, createSemaphores);
    GL_PROC(glDeleteSemaphoresEXT, deleteSemaphores);
    GL_PROC(glSemaphoreParameterui64vEXT, semaphoreParameter);
    GL_PROC(glSignalSemaphoreEXT, signalSemaphore);
    GL_PROC(glImportMemoryWin32HandleEXT, importMemory);
    GL_PROC(glImportSemaphoreWin32HandleEXT, importSemaphore);
    GL_PROC(glGenQueries, createQueries);
    GL_PROC(glDeleteQueries, deleteQueries);
    GL_PROC(glQueryCounter, queryCounter);
    GL_PROC(glGetQueryObjectiv, queryAvailable);
    GL_PROC(glGetQueryObjectui64v, queryValue);
#undef GL_PROC
    ++spy.invalidCalls;
    return nullptr;
}

class GlHooks {
public:
    GlHooks() {
        require(wglGetCurrentContext() == nullptr && wglGetCurrentDC() == nullptr,
                "GL spy fixture requires no native context on its thread");
        require(DetourTransactionBegin() == NO_ERROR, "Detour begin failed");
        require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "Detour thread failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalMakeCurrent), makeCurrent) == NO_ERROR,
                "WGL detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalGetError), getError) == NO_ERROR,
                "GL error detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalDeleteTextures), deleteTextures) == NO_ERROR,
                "GL delete detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalGetProcAddress), getProcAddress) == NO_ERROR,
                "GL proc-address detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalGetCurrentDC), currentDC) == NO_ERROR,
                "WGL current-DC detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalGetCurrentContext), currentRC) == NO_ERROR,
                "WGL current-context detour attach failed");
        require(DetourAttach(reinterpret_cast<PVOID*>(&originalFinish), finish) == NO_ERROR,
                "GL finish detour attach failed");
        require(DetourTransactionCommit() == NO_ERROR, "Detour commit failed");
    }
    ~GlHooks() {
        LONG result = DetourTransactionBegin();
        if (result == NO_ERROR) result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalMakeCurrent), makeCurrent);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalGetError), getError);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalDeleteTextures), deleteTextures);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalGetProcAddress), getProcAddress);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalGetCurrentDC), currentDC);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalGetCurrentContext), currentRC);
        if (result == NO_ERROR) result = DetourDetach(reinterpret_cast<PVOID*>(&originalFinish), finish);
        if (result == NO_ERROR) result = DetourTransactionCommit();
        else DetourTransactionAbort();
        if (result != NO_ERROR) {
            hookCleanupFailed = true;
            std::cerr << "ERROR: GL hook cleanup failed: " << result << '\n';
        }
    }
    GlHooks(const GlHooks&) = delete;
    GlHooks& operator=(const GlHooks&) = delete;
};
}

namespace virtualdesktop_openxr {
struct RuntimeInputRegression {
    static void import(const std::string& mode) {
        OpenXrRuntime runtime;
        runtime.stopRegistryWatcher();
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level;
        checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, &level, &context));
        checkHr(device.As(&runtime.m_ovrSubmissionDevice));
        checkHr(context.As(&runtime.m_ovrSubmissionContext));
        OpenXrRuntime::Swapchain chain{};
        chain.ovrSwapchainLength = 2;
        chain.xrDesc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain.xrDesc.format = GL_RGBA8;
        chain.xrDesc.width = chain.xrDesc.height = 8;
        chain.xrDesc.arraySize = chain.xrDesc.mipCount = chain.xrDesc.faceCount = chain.xrDesc.sampleCount = 1;
        chain.xrDesc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 8;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        for (unsigned i = 0; i < 2; ++i) {
            ComPtr<ID3D11Texture2D> texture;
            checkHr(device->CreateTexture2D(&desc, nullptr, &texture));
            chain.appSwapchain.images.push_back(texture);
        }
        runtime.m_glContext = {fakeDC, fakeRC, true};
        runtime.m_glDispatch.glCreateMemoryObjectsEXT = createMemory;
        runtime.m_glDispatch.glImportMemoryWin32HandleEXT = importMemory;
        runtime.m_glDispatch.glCreateTextures = createTextures;
        runtime.m_glDispatch.glTextureStorageMem2DEXT = storage;
        runtime.m_glDispatch.glDeleteMemoryObjectsEXT = deleteMemory;
        spy = {};
        spy.rejectContext = mode == "wgl";
        spy.deleteError = mode == "cleanup";
        const bool memoryFailure = mode == "memory-create";
        const bool imageFailure = mode == "texture-create";
        const bool creationFailure = memoryFailure || imageFailure;
        spy.failStorageAt = mode == "storage" || mode == "cleanup" ? 2 : 0;
        spy.failMemoryCreateAt = memoryFailure ? 2 : 0;
        spy.failImageCreateAt = imageFailure ? 2 : 0;
        {
            GlHooks hooks;
            XrSwapchainImageOpenGLKHR output[2]{{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR}, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR}};
            bool threw = false;
            try { runtime.getSwapchainImagesOpenGL(chain, output, 2); }
            catch (const std::exception&) { threw = true; }
            const bool empty = chain.glImages.empty() && chain.glMemory.empty() && spy.images.empty() && spy.memory.empty();
            const auto imagesDeleted = spy.imageDeletes;
            const auto memoryDeleted = spy.memoryDeletes;
            const auto contextRestores = spy.restoreCalls;
            const auto errorReads = spy.errorReads;
            const auto memoryCreates = spy.memoryCreates;
            std::cout << "gl-" << mode << " threw=" << threw << " cachedImages=" << chain.glImages.size()
                      << " cachedMemory=" << chain.glMemory.size() << " imageDeletes=" << imagesDeleted
                      << " memoryDeletes=" << memoryDeleted << " restores=" << contextRestores
                      << " errorReads=" << errorReads << " memoryCreates=" << memoryCreates
                      << " invalidImports=" << spy.invalidImports << " invalidStorage=" << spy.invalidStorage
                      << " invalidDeletes=" << spy.invalidDeletes << '\n';
            // Clean retained baseline names without injecting a second error into test teardown.
            spy.rejectContext = spy.deleteError = false;
            if (!empty) runtime.cleanupSwapchainImagesOpenGL(chain);
            if (mode == "wgl") {
                runtime.m_glContext.valid = false;
                require(threw && empty && contextRestores == 1 && errorReads == 0 && memoryCreates == 0 &&
                        !spy.invalidCalls, "Failed WGL switch performed GL work or did not restore the previous context");
            } else {
                const unsigned expectedImages = creationFailure ? 1 : 2;
                const unsigned expectedMemory = memoryFailure ? 1 : 2;
                const unsigned expectedStorage = creationFailure ? 1 : 2;
                require(threw && spy.storageCalls == expectedStorage && empty && imagesDeleted == expectedImages &&
                        memoryDeleted == expectedMemory &&
                        contextRestores == 1 && !spy.invalidCalls,
                        creationFailure ? "Failed GL creation used or deleted an uncreated name, or leaked a valid candidate" :
                                          "Failed GL storage left published images/memory or did not delete every candidate");
                spy.failStorageAt = spy.failMemoryCreateAt = spy.failImageCreateAt = 0;
                spy.memoryCreationFailed = spy.imageCreationFailed = false;
                require(runtime.getSwapchainImagesOpenGL(chain, output, 2) == XR_SUCCESS,
                        "GL import retry failed after the allocation error was cleared");
                require(chain.glImages.size() == 2 && chain.glMemory.size() == 2 && output[0].image != output[1].image &&
                        spy.images.count(output[0].image) && spy.images.count(output[1].image) &&
                        spy.storedImages.count(output[0].image) && spy.storedImages.count(output[1].image) &&
                        spy.images.size() == 2 && spy.memory.size() == 2 && !spy.invalidCalls,
                        "GL import retry returned incomplete or invalid texture names");
                const GLuint first = output[0].image;
                const GLuint second = output[1].image;
                require(runtime.getSwapchainImagesOpenGL(chain, output, 2) == XR_SUCCESS &&
                        output[0].image == first && output[1].image == second && spy.imageCreates == (memoryFailure ? 3u : 4u),
                        "Completed GL imports were recreated on repeated enumeration");
                runtime.cleanupSwapchainImagesOpenGL(chain);
                runtime.m_glContext.valid = false;
                require(spy.images.empty() && spy.memory.empty() && spy.imageDeletes == expectedImages + 2 &&
                        spy.memoryDeletes == expectedMemory + 2 &&
                        !spy.invalidCalls, "Successful GL retry cleanup leaked or double-deleted names");
            }
        }
        checkHr(device->GetDeviceRemovedReason());
        std::cout << "PASS: GL " << mode << " failure preserves ownership and context\n";
    }
    static void doubleFault() {
        spy = {};
        GlHooks hooks;
        bool primaryCaught = false;
        try {
            GlContext context{fakeDC, fakeRC, true};
            GlContextSwitch current(context);
            spy.pendingError = GL_OUT_OF_MEMORY;
            throw std::logic_error("Primary test exception");
        } catch (const std::logic_error&) { primaryCaught = true; }
        std::cout << "gl-double-fault primaryCaught=" << primaryCaught << " restores=" << spy.restoreCalls << '\n';
        require(primaryCaught && spy.restoreCalls == 1 && !spy.invalidCalls,
                "Secondary GL error replaced the primary exception or skipped context restoration");
        std::cout << "PASS: GL context restoration preserves the primary exception\n";
    }
    static void semaphoreInitialization(bool failCreate) {
        OpenXrRuntime runtime;
        runtime.stopRegistryWatcher();
        ComPtr<ID3D11Device> probeDevice;
        ComPtr<ID3D11DeviceContext> probeContext;
        D3D_FEATURE_LEVEL level;
        checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &probeDevice, &level, &probeContext));
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDescription{};
        checkHr(probeDevice.As(&dxgiDevice));
        checkHr(dxgiDevice->GetAdapter(&adapter));
        checkHr(adapter->GetDesc(&adapterDescription));
        runtime.m_adapterLuid = adapterDescription.AdapterLuid;
        spy = {};
        spy.adapterLuid = adapterDescription.AdapterLuid;
        spy.failSemaphoreCreate = failCreate;
        spy.failSemaphoreImport = !failCreate;
        {
            GlHooks hooks;
            XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
            binding.hDC = fakeDC;
            binding.hGLRC = fakeRC;
            bool threw = false;
            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            try { result = runtime.initializeOpenGL(binding); }
            catch (const std::exception&) { threw = true; }
            require(spy.semaphoreCreates == 1 && (failCreate || spy.semaphoreImports == 1) && !spy.invalidCalls,
                    "OpenGL initialization did not reach the injected semaphore failure");
            const auto firstQueries = spy.queryCreates;
            const auto firstImports = spy.semaphoreImports;
            const auto invalidImports = spy.invalidSemaphoreImports;
            const auto clearedByNestedContext = spy.errorsClearedInNestedContext;
            const bool failed = threw || XR_FAILED(result);
            std::cout << "gl-semaphore-" << (failCreate ? "create" : "import") << " failed=" << failed
                      << " result=" << int(result) << " imports=" << firstImports
                      << " invalidImports=" << invalidImports << " queries=" << firstQueries
                      << " errorsClearedInNestedContext=" << clearedByNestedContext << '\n';
            // These are the actual cleanup routines used by session initialization rollback.
            runtime.cleanupOpenGL();
            runtime.cleanupSubmissionDevice();
            require(spy.semaphores.empty() && spy.importedSemaphores.empty() && spy.queries.empty() &&
                    spy.queryDeletes == firstQueries && spy.semaphoreDeletes == (failCreate ? 0u : 1u) &&
                    !runtime.m_glContext.valid && !runtime.m_fenceHandleForAMDWorkaround && !spy.invalidCalls,
                    "Failed OpenGL initialization cleanup leaked a semaphore, query, or shared fence handle");
            require(failed && firstQueries == 0 && invalidImports == 0 && clearedByNestedContext == 0 &&
                    firstImports == (failCreate ? 0u : 1u),
                    "Semaphore error was consumed by timer context initialization instead of failing initialization");
            spy.failSemaphoreCreate = spy.failSemaphoreImport = false;
            require(runtime.initializeOpenGL(binding) == XR_SUCCESS && runtime.m_glSemaphore != 0 &&
                    spy.importedSemaphores.count(runtime.m_glSemaphore) &&
                    spy.queryCreates == 2 * OpenXrRuntime::k_numGpuTimers && !spy.invalidCalls,
                    "OpenGL initialization retry did not create a usable semaphore and complete timers");
            runtime.cleanupOpenGL();
            runtime.cleanupSubmissionDevice();
            require(spy.semaphores.empty() && spy.importedSemaphores.empty() && spy.queries.empty() &&
                    spy.queryDeletes == spy.queryCreates && spy.semaphoreDeletes == (failCreate ? 1u : 2u) &&
                    !runtime.m_fenceHandleForAMDWorkaround && !spy.invalidCalls,
                    "Successful OpenGL initialization retry leaked or double-deleted resources");
        }
        checkHr(probeDevice->GetDeviceRemovedReason());
        std::cout << "PASS: GL semaphore " << (failCreate ? "creation" : "import")
                  << " failure is reported before timers and allows a complete retry\n";
    }
};
}

int main(int argc, char** argv) {
    // Report a double exception as an ordinary failing test without native crash UI.
    std::set_terminate([] {
        std::cerr << "FAIL: GL context cleanup threw a secondary exception during unwinding\n";
        ExitProcess(3);
    });
    try {
        require(argc == 2, "usage: nonnr_gl_regression storage | cleanup | wgl | double-fault | memory-create | texture-create | semaphore-create | semaphore-import");
        const std::string mode = argv[1];
        if (mode == "double-fault") virtualdesktop_openxr::RuntimeInputRegression::doubleFault();
        else if (mode == "semaphore-create" || mode == "semaphore-import")
            virtualdesktop_openxr::RuntimeInputRegression::semaphoreInitialization(mode == "semaphore-create");
        else if (mode == "storage" || mode == "cleanup" || mode == "wgl" || mode == "memory-create" || mode == "texture-create")
            virtualdesktop_openxr::RuntimeInputRegression::import(mode);
        else throw std::runtime_error("Unknown GL fixture mode");
        require(!hookCleanupFailed, "GL hooks were not restored");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
