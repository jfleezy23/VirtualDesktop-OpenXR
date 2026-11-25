#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace virtualdesktop_openxr::nr_before_sr {
    enum class Placement { Late, BeforeSrOnly };

    namespace detail {
        constexpr wchar_t foldPath(wchar_t c) noexcept {
            if (c == L'/')
                return L'\\';
            return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        inline bool samePath(std::wstring_view a, std::wstring_view b) noexcept {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (foldPath(a[i]) != foldPath(b[i]))
                    return false;
            }
            return true;
        }
    } // namespace detail

    // Match the application name before hashing the specific supported build.
    inline bool IsHubrisShippingImage(std::wstring_view processImage) noexcept {
        if (processImage.size() < 4)
            return false;
        const auto drive = detail::foldPath(processImage.front());
        if (drive < L'a' || drive > L'z' || processImage[1] != L':' || detail::foldPath(processImage[2]) != L'\\')
            return false;

        auto remaining = processImage.substr(3);
        for (;;) {
            const auto separator = remaining.find_first_of(L"\\/");
            const auto component = remaining.substr(0, separator);
            if (component.empty() || component.back() == L'.' || component.back() == L' ' ||
                component.find_first_of(L":*?\"<>|") != std::wstring_view::npos ||
                component.find(L'\0') != std::wstring_view::npos)
                return false;
            if (separator == std::wstring_view::npos)
                return detail::samePath(component, L"HubrisVR-Win64-Shipping.exe");
            remaining.remove_prefix(separator + 1);
        }
    }

    // The game's explicit startup preference selects compositor NR when native DLSS is off.
    // Missing or unrecognized values leave the qualified native SR route available.
    inline bool DlssExplicitlyDisabled(std::wstring_view value) noexcept {
        const auto start = value.find_first_not_of(L" \t");
        if (start == std::wstring_view::npos)
            return false;
        value = value.substr(start, value.find_last_not_of(L" \t") - start + 1);
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        return detail::samePath(value, L"false");
    }

    class BeforeSrEnablement {
      public:
        void update(bool enabled, bool validProfile) noexcept {
            const bool next = enabled && validProfile;
            m_resetHistory |= next != m_enabled;
            m_enabled = next;
        }

        struct Frame {
            bool enabled{}, resetHistory{};
        };

        Frame beginFrame() noexcept {
            const Frame snapshot{m_enabled, m_resetHistory};
            m_resetHistory = false;
            return snapshot;
        }

      private:
        bool m_enabled{}, m_resetHistory{};
    };

    struct Extent {
        std::uint32_t width{}, height{};
    };
    struct Rect {
        std::uint32_t x{}, y{}, width{}, height{};
    };
    struct TextureRegion {
        std::uintptr_t identity{}, device{};
        std::uint64_t allocationWidth{};
        std::uint32_t allocationHeight{}, arraySize{}, mipLevels{}, samples{};
        bool isTexture2D{};
        Rect active{};
    };

    // Identity values must be comparable native identities (canonical IUnknown for COM objects).
    struct DeviceBinding {
        std::uintptr_t device{}, queue{}, queueDevice{};
        bool d3d12Direct{};
    };
    struct TrackedFeature {
        std::uintptr_t handle{};
        std::uint64_t generation{};
        std::uintptr_t device{};
        std::uint32_t featureId{}, createFlags{0x80000000u};
        Extent maxRender{}, output{};
        bool outputSubrects{};
    };
    struct Evaluation {
        std::uintptr_t handle{};
        std::uint64_t generation{};
        std::uintptr_t list{}, listDevice{}, queue{};
        bool directList{};
        TextureRegion color{}, depth{}, motion{}, output{};
        float mvScaleX{}, mvScaleY{};
    };

    namespace detail {
        // D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION. Keep the descriptor's native 64-bit width until validated.
        constexpr std::uint32_t MaxTextureDimension = 16384;

        constexpr bool validRegion(const TextureRegion& texture, std::uintptr_t device) noexcept {
            const auto& rect = texture.active;
            return texture.identity && texture.device == device && texture.isTexture2D && texture.arraySize == 1 &&
                   texture.mipLevels == 1 && texture.samples == 1 && texture.allocationWidth <= MaxTextureDimension &&
                   texture.allocationHeight <= MaxTextureDimension && rect.width && rect.height &&
                   rect.x <= texture.allocationWidth && rect.width <= texture.allocationWidth - rect.x &&
                   rect.y <= texture.allocationHeight && rect.height <= texture.allocationHeight - rect.y;
        }

        constexpr bool hasExtent(Rect rect, Extent extent) noexcept {
            return rect.width == extent.width && rect.height == extent.height;
        }
    } // namespace detail

    // Checks the layout for one SR evaluate. Queue association, resource formats/states, temporal ownership
    // and submission retirement must also be established by the native producer.
    inline bool CanRunBeforeSr(Placement placement,
                               const DeviceBinding& binding,
                               const TrackedFeature& feature,
                               const Evaluation& call) noexcept {
        if (placement != Placement::BeforeSrOnly || !binding.d3d12Direct || !binding.device || !binding.queue ||
            binding.queueDevice != binding.device || !call.directList || !call.list || call.queue != binding.queue ||
            call.listDevice != binding.device)
            return false;

        // NGX SuperSampling = 1. Supported create flags: HDR, low-res MV, inverted depth, sharpening, auto-exposure.
        // Reserved flags and conventions requiring extra processing (jittered MV or alpha upscaling) pass through.
        constexpr std::uint32_t hdr = 1u << 0;
        constexpr std::uint32_t mvLowRes = 1u << 1;
        constexpr std::uint32_t supportedFlags = hdr | mvLowRes | (1u << 3) | (1u << 5) | (1u << 6);
        if (feature.featureId != 1 || !(feature.createFlags & hdr) || (feature.createFlags & ~supportedFlags) ||
            feature.device != binding.device || !feature.handle || call.handle != feature.handle ||
            !feature.generation || call.generation != feature.generation)
            return false;

        const TextureRegion* views[]{&call.color, &call.depth, &call.motion, &call.output};
        for (std::size_t i = 0; i < 4; ++i) {
            if (!detail::validRegion(*views[i], binding.device))
                return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (views[i]->identity == views[j]->identity)
                    return false;
            }
        }

        const Extent render{call.color.active.width, call.color.active.height};
        const auto& output = call.output.active;
        const auto motion = (feature.createFlags & mvLowRes) ? render : feature.output;
        return render.width <= feature.maxRender.width && render.height <= feature.maxRender.height &&
               feature.maxRender.width <= feature.output.width && feature.maxRender.height <= feature.output.height &&
               detail::hasExtent(output, feature.output) && (feature.outputSubrects || (!output.x && !output.y)) &&
               detail::hasExtent(call.depth.active, render) && detail::hasExtent(call.motion.active, motion) &&
               std::isfinite(call.mvScaleX) && std::isfinite(call.mvScaleY) && call.mvScaleX != 0 && call.mvScaleY != 0;
    }
} // namespace virtualdesktop_openxr::nr_before_sr
