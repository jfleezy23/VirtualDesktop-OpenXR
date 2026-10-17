// Observe real COM calls without substituting proxy objects into the GPU pipeline.
struct NrLookupProbe {
    using GetResourceFn = void(STDMETHODCALLTYPE*)(ID3D11ShaderResourceView*, ID3D11Resource**);
    using QueryFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**);
    inline static NrLookupProbe* active{};
    GetResourceFn originalGet{};
    QueryFn originalQuery{};
    DWORD thread = GetCurrentThreadId();
    std::set<ID3D11ShaderResourceView*> views;
    std::set<IUnknown*> resources;
    unsigned gets{}, queries{};
    bool armed{};

    static void STDMETHODCALLTYPE get(ID3D11ShaderResourceView* view, ID3D11Resource** resource) {
        auto& probe = *active;
        if (GetCurrentThreadId() == probe.thread && probe.armed && probe.views.count(view))
            ++probe.gets;
        probe.originalGet(view, resource);
    }
    static HRESULT STDMETHODCALLTYPE query(IUnknown* resource, REFIID id, void** output) {
        auto& probe = *active;
        if (GetCurrentThreadId() == probe.thread && probe.armed && id == __uuidof(ID3D11Texture2D) &&
            probe.resources.count(resource))
            ++probe.queries;
        return probe.originalQuery(resource, id, output);
    }
    void attach(bool enabled) {
        const auto operation = enabled ? DetourAttach : DetourDetach;
        checkHr(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        checkHr(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
        checkHr(HRESULT_FROM_WIN32(operation(reinterpret_cast<PVOID*>(&originalGet), get)));
        checkHr(HRESULT_FROM_WIN32(operation(reinterpret_cast<PVOID*>(&originalQuery), query)));
        checkHr(HRESULT_FROM_WIN32(DetourTransactionCommit()));
    }
    void begin(const std::vector<ComPtr<ID3D11ShaderResourceView>>& srvs) {
        require(!srvs.empty(), "lookup measurement requires a warmed NR output ring");
        views.clear();
        resources.clear();
        for (const auto& view : srvs) {
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            views.insert(view.Get());
            resources.insert(resource.Get());
            if (!originalGet) {
                originalGet = reinterpret_cast<GetResourceFn>((*reinterpret_cast<void***>(view.Get()))[7]);
                originalQuery = reinterpret_cast<QueryFn>((*reinterpret_cast<void***>(resource.Get()))[0]);
            }
        }
        if (!active) {
            active = this;
            attach(true);
        }
        gets = queries = 0;
        armed = true;
        ComPtr<ID3D11Resource> resource;
        srvs.front()->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        checkHr(resource.As(&texture));
        armed = false;
        require(gets == 1 && queries == 1, "real COM hooks must observe a positive control");
        gets = queries = 0;
        armed = true;
    }
    void finish(bool postprocess) {
        armed = false;
        const unsigned expected = postprocess ? 0 : 1;
        std::cout << "NR final-resource COM calls: GetResource=" << gets << " textureQI=" << queries
                  << " expected=" << expected << '\n';
        require(gets == expected && queries == expected,
                "NR must extract final FG texture once, or never when postprocessing replaces it");
    }
    ~NrLookupProbe() {
        armed = false;
        if (active == this) {
            try {
                attach(false);
            } catch (...) {
                std::_Exit(2);
            }
            active = nullptr;
        }
    }
};
