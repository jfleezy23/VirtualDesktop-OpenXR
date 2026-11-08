#pragma once
#include <cstddef>
#include <cstdint>
namespace vdxrfg::pair_capture {
    // Bounded ordinary-texture read subset used by current t0/t1/t7/t8. The
    // implementation statically checks these values against the installed SDK.
    constexpr uint32_t readPixel = 0x80, readNonPixel = 0x40, readCopyDest = 0x400, readCopySource = 0x800;
    constexpr uint32_t readable = readPixel | readNonPixel | readCopySource;
    struct ReadCopyPlan {
        bool valid{}, transition{}, implicit{};
        uint32_t before{};
    };
    constexpr bool distinctGuidePair(uintptr_t a, uintptr_t b) noexcept {
        return a && b && a != b;
    }
    struct TextureReadState {
        uint32_t value{}, decayedReadMask{};
        bool known{}, implicit{}, invalid{};
        uintptr_t commands{};
        struct ReadUse {
            uintptr_t list{};
            uint32_t mask{};
        };
        ReadUse reads[8]{};
        constexpr ReadUse* use(uintptr_t list) noexcept {
            for (auto& read : reads)
                if (read.list == list)
                    return &read;
            for (auto& read : reads)
                if (!read.list) {
                    read.list = list;
                    return &read;
                }
            return nullptr;
        }
        constexpr bool transition(
            uintptr_t list, uint32_t before, uint32_t after, uint32_t flags, bool full, bool enabled = true) noexcept {
            if (!enabled)
                return false;
            if (invalid || !list || flags || !full || (known && value != before && !(implicit && before == 0))) {
                known = false;
                invalid = true;
                return false;
            }
            value = after;
            known = true;
            implicit = false;
            commands = list;
            for (auto& read : reads)
                read = {};
            return true;
        }
        constexpr bool drawRead(uintptr_t list, bool promotable) noexcept {
            if (!known || invalid || !list)
                return false;
            if (value == 0 || implicit) {
                if (!promotable)
                    return false;
                auto* read = use(list);
                if (!read) {
                    invalid = true;
                    return false;
                }
                read->mask |= readPixel;
                value = read->mask;
                implicit = true;
            } else if ((value & ~readable) || !(value & readPixel))
                return false;
            commands = list;
            return true;
        }
        constexpr ReadCopyPlan copyRead(uintptr_t list) noexcept {
            if (!known || invalid || !list || !value || (value & ~readable))
                return {};
            const uint32_t before = value;
            commands = list;
            if (implicit) {
                auto* read = use(list);
                if (!read || !(read->mask & readPixel))
                    return {};
                read->mask |= readCopySource;
                value = read->mask;
                return {true, false, true, before};
            }
            return {true, (value & readCopySource) == 0, false, before};
        }
        // This predicts state at GPU completion of one actual ExecuteCommandLists
        // batch, not at its CPU return. Never call separately for lists in one batch.
        // Subsequent work on the same queue executes after this boundary; an actual
        // selected full fence separately proves GPU completion for readback/retire.
        constexpr bool executeBatch(const uintptr_t* lists, size_t count) noexcept {
            if (!lists || !count || !known || invalid)
                return false;
            decayedReadMask = 0;
            for (auto& read : reads)
                for (size_t i = 0; i < count; ++i)
                    if (read.list && read.list == lists[i]) {
                        decayedReadMask |= read.mask;
                        read = {};
                        break;
                    }
            if (decayedReadMask) {
                value = 0;
                implicit = false;
                commands = 0;
                for (const auto& read : reads)
                    if (read.list) {
                        value = read.mask;
                        implicit = true;
                        commands = read.list;
                        break;
                    }
                return true;
            }
            for (size_t i = 0; i < count; ++i)
                if (commands == lists[i])
                    commands = 0;
            return false;
        }
    };
} // namespace vdxrfg::pair_capture
