#include "fg_atlas_gpu_helpers.h"

#include <algorithm>

namespace {
    struct Fixture {
        AtlasGpu gpu;
        HMODULE module{};
        VDXRFG_ApiV2 api{};
        void* context{};
        std::array<VDXRFG_TicketV1, 2> live{};
        ComPtr<ID3D12Resource> input;
        std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> outputs;
        VDXRFG_SubmitInfoV1 info{};
        const UINT eyeWidth, imageHeight, multiplier;
        const bool hostOwned, direct;
        AtlasPixels sentinel;

        Fixture(const char* modulePath,
                const std::string& backend,
                UINT factor,
                const std::string& scenario,
                bool ownsReal,
                bool requireDirect)
            : eyeWidth(scenario.find("odd") != std::string::npos ? 513u : 512u),
              imageHeight(scenario.find("odd") != std::string::npos ? 257u : 256u), multiplier(factor),
              hostOwned(ownsReal), direct(requireDirect), sentinel(eyeWidth * 2 * imageHeight * 4) {
            check(backend == "nvidia" || backend == "fidelityfx-full", "known atlas fixture backend");
            check(multiplier == 2 || multiplier == 3, "known atlas fixture multiplier");
            check(scenario == "unorm-full" || scenario == "srgb-full" || scenario == "unorm-crop" ||
                      scenario == "srgb-crop" || scenario == "unorm-odd" || scenario == "srgb-odd" ||
                      scenario == "unorm-odd-crop" || scenario == "srgb-odd-crop",
                  "known atlas pixel scenario");
            module =
                LoadLibraryExA(modulePath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            check(module != nullptr, "load atlas fixture module");
            const auto get = reinterpret_cast<VDXRFG_GetApiV2Fn>(GetProcAddress(module, "VDXRFG_GetApiV2"));
            check(get != nullptr, "atlas fixture V2 export");
            api.struct_size = sizeof(api);
            api.api_version = VDXRFG_API_VERSION_2;
            success(get(&api), "atlas fixture creation table");
            input = gpu.texture(eyeWidth * 2, imageHeight);
            VDXRFG_CreateInfoV2 create{};
            create.struct_size = sizeof(create);
            create.api_version = VDXRFG_API_VERSION_2;
            create.flags = (hostOwned ? VDXRFG_CREATE_HOST_OWNS_REAL : 0u) |
                           (backend == "fidelityfx-full" ? VDXRFG_CREATE_FIDELITYFX_FULL : 0u);
            auto& base = create.base;
            base.struct_size = sizeof(base);
            base.api_version = VDXRFG_API_VERSION;
            base.device = gpu.device.Get();
            base.queue = gpu.queue.Get();
            base.input = input.Get();
            base.width = eyeWidth * 2;
            base.height = imageHeight;
            base.multiplier = multiplier;
            base.format = scenario.find("srgb") != std::string::npos ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                     : DXGI_FORMAT_R8G8B8A8_UNORM;
            for (size_t i = 0; i < sentinel.size(); ++i)
                sentinel[i] = static_cast<uint8_t>((i * 37 + i / 17 + 53) & 255);
            for (UINT bank = 0; bank < 2; ++bank) {
                for (UINT role = 0; role < 3; ++role) {
                    if (role == 1 && multiplier == 2)
                        continue;
                    outputs[bank][role] = gpu.texture(eyeWidth * 2, imageHeight);
                    base.outputs[bank][role] = outputs[bank][role].Get();
                }
            }
            success(api.Create(&create, &context), "create atlas pixel context");
            info.struct_size = sizeof(info);
            info.api_version = VDXRFG_API_VERSION;
            info.input = input.Get();
            info.display_period_ns = 10000000;
            for (UINT eye = 0; eye < 2; ++eye) {
                auto& view = info.views[eye];
                view.orientation[3] = 1;
                view.position[0] = eye ? .032f : -.032f;
                view.fov[0] = view.fov[3] = -.8f;
                view.fov[1] = view.fov[2] = .8f;
                view.rect = {eye * eyeWidth, 0, eyeWidth, imageHeight};
                if (scenario.find("crop") != std::string::npos)
                    view.rect = eye ? VDXRFG_RectV1{eyeWidth + 23, 5, eyeWidth - 41, imageHeight - 17}
                                    : VDXRFG_RectV1{7, 11, eyeWidth - 19, imageHeight - 29};
            }
        }

        void close() {
            for (auto& ticket : live)
                if (ticket.token)
                    retire(ticket.bank);
            if (context) {
                success(api.Destroy(context), "settle atlas fixture context");
                context = nullptr;
            }
            if (module) {
                check(FreeLibrary(module) != 0, "unload settled atlas fixture module");
                module = nullptr;
            }
        }

        ~Fixture() {
            try {
                close();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "atlas fixture cleanup: %s\n", error.what());
            }
        }

        AtlasPixels scene(UINT step) const {
            AtlasPixels result(sentinel.size());
            for (UINT y = 0; y < imageHeight; ++y) {
                for (UINT eye = 0; eye < 2; ++eye) {
                    for (UINT x = 0; x < eyeWidth; ++x) {
                        auto* p = result.data() + ((y * eyeWidth * 2) + eye * eyeWidth + x) * 4;
                        p[0] = static_cast<uint8_t>(17 + (x * 3 + y * 5 + eye * 61) % 113);
                        p[1] = static_cast<uint8_t>(23 + ((x / 7 + y / 11 + eye) % 2) * 49);
                        p[2] = static_cast<uint8_t>(31 + (x + y * 2 + eye * 37) % 97);
                        p[3] = static_cast<uint8_t>(39 + (x * 5 + y * 3 + eye * 71) % 187);
                        const UINT center = eye ? 331 - step : 113 + step;
                        if (x >= center && x < center + 37 && y >= 89 && y < 163) {
                            p[eye ? 0 : 1] = 239;
                            p[2] = static_cast<uint8_t>(eye ? 51 : 197);
                            p[3] = static_cast<uint8_t>(eye ? 213 : 83);
                        }
                    }
                }
            }
            return result;
        }

        void seed() {
            for (auto& bank : outputs)
                for (auto& resource : bank)
                    if (resource)
                        gpu.transfer(resource.Get(), &sentinel);
        }

        VDXRFG_TicketV1 submit(const AtlasPixels& pixels, bool prime) {
            gpu.transfer(input.Get(), &pixels);
            const auto previous = info.display_time_ns;
            ++info.serial;
            info.display_time_ns =
                prime ? 1000000000 + info.serial * 1000000000 : previous + multiplier * info.display_period_ns;
            for (UINT phase = 0; phase < multiplier - 1; ++phase) {
                info.fractions[phase] = static_cast<float>(phase + 1) / multiplier;
                info.target_time_ns[phase] = previous + (phase + 1) * info.display_period_ns;
            }
            VDXRFG_TicketV1 ticket{};
            success(api.Submit(context, &info, &ticket), "submit atlas pixel capture");
            check(ticket.bank < 2 && !live[ticket.bank].token, "atlas capture claims an available bank");
            live[ticket.bank] = ticket;
            check(ticket.prime == static_cast<uint32_t>(prime) && ticket.token && ticket.generation &&
                      ticket.input_consumed_fence && ticket.output_ready_fence &&
                      ticket.input_consumed_value == ticket.token && ticket.output_ready_value == ticket.token &&
                      ticket.role_mask == ((hostOwned ? 0u : 4u) | (prime             ? 0u
                                                                    : multiplier == 3 ? 3u
                                                                                      : 1u)),
                  "atlas capture preserves authoritative role, ownership and fence metadata");
            gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
            check(static_cast<ID3D12Fence*>(ticket.input_consumed_fence)->GetCompletedValue() >=
                      ticket.input_consumed_value,
                  "ready publication follows input consumption");
            check(gpu.transfer(outputs[ticket.bank][2].Get()) == (hostOwned ? sentinel : pixels),
                  "real output is exact or remains host-owned");
            return ticket;
        }

        void retire(UINT bank) {
            const auto ticket = live[bank];
            gpu.wait(static_cast<ID3D12Fence*>(ticket.output_ready_fence), ticket.output_ready_value);
            success(api.Retire(context, ticket.token), "retire atlas pixel ticket");
            live[bank] = {};
        }

        bool inside(UINT x, UINT y) const {
            for (const auto& view : info.views)
                if (x >= view.rect.x && x < view.rect.x + view.rect.width && y >= view.rect.y &&
                    y < view.rect.y + view.rect.height)
                    return true;
            return false;
        }

        AtlasPixels declaredPixels(const AtlasPixels& pixels) const {
            AtlasPixels result;
            for (const auto& view : info.views)
                for (UINT y = view.rect.y; y < view.rect.y + view.rect.height; ++y) {
                    const auto first = pixels.begin() + (y * eyeWidth * 2 + view.rect.x) * 4;
                    result.insert(result.end(), first, first + view.rect.width * 4);
                }
            return result;
        }

        AtlasPixels synthetic(UINT bank, UINT phase) {
            auto pixels = gpu.transfer(outputs[bank][phase].Get());
            if (direct) {
                for (UINT y = 0; y < imageHeight; ++y)
                    for (UINT x = 0; x < eyeWidth * 2; ++x)
                        if (!inside(x, y))
                            for (UINT channel = 0; channel < 4; ++channel) {
                                const size_t index = (y * eyeWidth * 2 + x) * 4 + channel;
                                check(pixels[index] == sentinel[index],
                                      "direct atlas preserves undrawn seeded padding");
                            }
            }
            return pixels;
        }

        void write(const std::filesystem::path& path, const AtlasPixels& pixels) const {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
            check(file.good(), "write declared atlas pixel evidence");
        }
    };

    void capture(Fixture& fixture, const std::filesystem::path& directory) {
        check(!std::filesystem::exists(directory), "use a fresh atlas capture directory");
        std::filesystem::create_directories(directory);
        fixture.seed();
        const auto a = fixture.scene(0), b = fixture.scene(24), c = fixture.scene(48);
        const auto prime = fixture.submit(a, true);
        for (UINT role = 0; role < fixture.multiplier - 1; ++role)
            check(fixture.gpu.transfer(fixture.outputs[prime.bank][role].Get()) == fixture.sentinel,
                  "prime leaves every synthetic atlas untouched");
        const auto pair = fixture.submit(b, false);
        check(pair.bank != prime.bank && pair.previous_serial == prime.current_serial,
              "held prime and pair retain separate atlas banks");
        std::array<AtlasPixels, 2> held;
        for (UINT phase = 0; phase < fixture.multiplier - 1; ++phase) {
            held[phase] = fixture.synthetic(pair.bank, phase);
            const auto roi = fixture.declaredPixels(held[phase]);
            check(roi != fixture.declaredPixels(a) && roi != fixture.declaredPixels(b),
                  "declared synthetic pixels differ from both endpoints");
            fixture.write(directory / ("pair-s" + std::to_string(phase + 1) + ".rgba"), roi);
        }
        check(fixture.api.Reset(fixture.context, 0) == VDXRFG_E_BUSY &&
                  fixture.api.Destroy(fixture.context) == VDXRFG_E_BUSY,
              "retained output banks prevent reset and destruction");
        fixture.retire(prime.bank);
        const auto next = fixture.submit(c, false);
        check(next.bank == prime.bank, "a new pair reuses only the retired bank");
        for (UINT phase = 0; phase < fixture.multiplier - 1; ++phase) {
            check(fixture.gpu.transfer(fixture.outputs[pair.bank][phase].Get()) == held[phase],
                  "new pair cannot alter a retained synthetic bank");
            fixture.write(directory / ("next-s" + std::to_string(phase + 1) + ".rgba"),
                          fixture.declaredPixels(fixture.synthetic(next.bank, phase)));
        }
        fixture.retire(next.bank);
        fixture.retire(pair.bank);
        success(fixture.api.Reset(fixture.context, 0), "reset drained atlas context");
        fixture.seed();
        const auto resetPrime = fixture.submit(a, true);
        check(resetPrime.generation > pair.generation && resetPrime.token > next.token,
              "reset preserves nonreused generation and ticket identity");
        fixture.retire(resetPrime.bank);
        const auto resetPair = fixture.submit(b, false);
        for (UINT phase = 0; phase < fixture.multiplier - 1; ++phase) {
            const auto roi = fixture.declaredPixels(fixture.synthetic(resetPair.bank, phase));
            check(roi == fixture.declaredPixels(held[phase]), "reset and same-bank reuse preserve declared pixels");
            fixture.write(directory / ("reset-s" + std::to_string(phase + 1) + ".rgba"), roi);
        }
        fixture.retire(resetPair.bank);
        fixture.gpu.noErrors();
        std::ofstream metadata(directory / "capture.txt");
        metadata << "eye_width=" << fixture.eyeWidth << "\nheight=" << fixture.imageHeight
                 << "\nmultiplier=" << fixture.multiplier << "\nhost_owned=" << fixture.hostOwned << '\n';
        for (UINT eye = 0; eye < 2; ++eye) {
            const auto& rect = fixture.info.views[eye].rect;
            metadata << "rect" << eye << '=' << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height
                     << '\n';
        }
        check(metadata.good(), "write declared region metadata");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 7 || argc == 8, "module output-directory backend 2|3 scenario host|module [direct] required");
        const std::string owner = argv[6];
        check(owner == "host" || owner == "module", "known real-output owner");
        check(argc != 8 || std::string(argv[7]) == "direct", "known atlas padding contract");
        Fixture fixture(argv[1], argv[3], static_cast<UINT>(std::stoul(argv[4])), argv[5], owner == "host", argc == 8);
        capture(fixture, argv[2]);
        fixture.close();
        std::puts("PASS: declared atlas pixels, padding and retained lifetime");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
