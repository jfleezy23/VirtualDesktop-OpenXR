#include "PairCaptureNrEnginePacket.h"
#include "../nr-frame-generation/include/pair_capture_nr_budget.hpp"
#include "../nr-frame-generation/include/pair_capture_nr_host_reservation.h"
#include "../nr-frame-generation/include/pair_capture_nr_source_match.h"
#include "../nr-frame-generation/include/pair_capture_nr_postfx_validate.h"
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool zero(const void* data, size_t bytes) noexcept {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i)
                if (p[i])
                    return false;
            return true;
        }
        bool fail(NrError& error, uint32_t code, uint64_t index = 0) noexcept {
            error.code = code;
            error.index = static_cast<uint32_t>(index);
            return false;
        }
        bool epoch(uint64_t value) noexcept {
            return value && value != UINT64_MAX;
        }
        bool range(uint64_t first, uint64_t count, uint64_t bound) noexcept {
            return first <= bound && count <= bound - first;
        }
        bool separate(const void* data, uint64_t bytes, const NrPacketV1& output) noexcept {
            if (!bytes)
                return true;
            if (!data)
                return false;
            const auto first = reinterpret_cast<uint64_t>(data), target = reinterpret_cast<uint64_t>(&output);
            return first <= UINT64_MAX - bytes && (first + bytes <= target || first >= target + sizeof output);
        }
        template <class T>
        bool inputPointer(const T* value, const NrPacketV1& output) noexcept {
            return value && separate(value, sizeof(T), output);
        }
        template <class T>
        bool span(const EnginePacketSpan<T>& values, uint64_t cap, const NrPacketV1& output) noexcept {
            return values.count <= cap && separate(values.data, values.count * sizeof(T), output);
        }
        bool emptyPlacement(const NrHeaderV1& h) noexcept {
            return !h.roleCount && !h.refCount && !h.groupCount && !h.paramCount && !h.callCount &&
                   !h.descriptorCount && !h.drawCount && !h.aliasCount && !h.writeCount && !h.footprintCount &&
                   !h.constantsUsedBytes && !h.moduleCount && !h.presentRoleMaskLo && !h.presentRoleMaskHi;
        }
        bool inputs(const EnginePacketInput& in, const NrPacketV1& out) noexcept {
            if (!separate(&in, sizeof in, out) || !inputPointer(in.header, out) || !inputPointer(in.identity, out) ||
                !inputPointer(in.budget, out) || !inputPointer(in.pipelineProgram, out) ||
                !inputPointer(in.cohort.request, out) || !inputPointer(in.cohort.ready, out) ||
                !inputPointer(in.cohort.fgBudget, out) || !inputPointer(in.cohort.source, out) ||
                !inputPointer(in.cohort.postFx, out) || !inputPointer(in.cohort.sourceModule, out) ||
                !span(in.refs, 89, out) || !span(in.groups, 3, out) || in.groups.count != 3 ||
                !span(in.modules, 8, out) || in.modules.count < 5 || !span(in.controlCalls, 160, out) ||
                !span(in.aliases, 64, out) || !span(in.sdk, 70, out) || !span(in.roles, 28, out) ||
                !span(in.drawDescriptors, 50, out) || in.drawDescriptors.count > 50 - in.roles.count ||
                !span(in.draws, 8, out))
                return false;
            for (uint64_t i = 0; i < in.modules.count; ++i) {
                const auto& m = in.modules.data[i];
                if (!inputPointer(m.module, out) || (m.actualPin && !separate(m.actualPin, sizeof *m.actualPin, out)))
                    return false;
            }
            for (uint64_t i = 0; i < in.roles.count; ++i) {
                const auto& r = in.roles.data[i];
                if (!inputPointer(r.allocation, out) || !inputPointer(r.metadata, out) || !inputPointer(r.writer, out))
                    return false;
            }
            for (uint64_t i = 0; i < in.sdk.count; ++i)
                if (!inputPointer(in.sdk.data[i].observation, out))
                    return false;
            for (uint64_t i = 0; i < in.draws.count; ++i) {
                const auto& d = in.draws.data[i];
                if (!inputPointer(d.draw, out) || !span(d.shaderSha256, 32, out) || d.shaderSha256.count != 32 ||
                    !span(d.constants, 112, out))
                    return false;
            }
            return true;
        }
        bool requestMatches(const NrPacketV1& p, const EnginePacketCohort& cohort) noexcept {
            const auto& r = *cohort.request;
            const auto& ready = *cohort.ready;
            const auto& x = p.identity;
            return r.valid && epoch(r.attempt) && r.attempt == p.header.attempt && r.mode == p.header.actualNrMode &&
                   epoch(r.minimumSerial) && r.minimumTime && x.hostSerial >= r.minimumSerial &&
                   x.displayTimeBits >= r.minimumTime && !std::memcmp(r.nonce, x.nonce, sizeof r.nonce) &&
                   r.pid == x.pid && r.creation == x.creationFiletime && ready.size == sizeof ready &&
                   ready.version == 1 && ready.status == 1 && ready.observedFlags == 1 && ready.attempt == r.attempt &&
                   ready.mode == r.mode && ready.nonceLo == r.nonce[0] && ready.nonceHi == r.nonce[1] &&
                   ready.pid == r.pid && ready.creationFiletime == r.creation &&
                   ready.sessionNonceLo == x.sessionNonce[0] && ready.sessionNonceHi == x.sessionNonce[1] &&
                   ready.fgContextAddress == x.fgContextAddress &&
                   ready.fgContextLifetimeEpoch == x.fgContextLifetimeEpoch &&
                   ready.fgHistoryGeneration == x.fgHistoryGeneration && zero(ready.reserved, sizeof ready.reserved) &&
                   !std::memcmp(r.hostHash, p.modules[0].sha256Words, 32) &&
                   !std::memcmp(r.fgHash, p.modules[1].sha256Words, 32);
        }
        bool heldProgram(const NrPacketV1& p, uint64_t address, uint64_t lifetime, uint64_t type) noexcept {
            if (!address || !epoch(lifetime))
                return false;
            bool found = false;
            for (uint64_t i = 0; i < p.header.refCount; ++i) {
                const auto& r = p.refs[i];
                if (r.borrowedIUnknownAddress == address && r.interfaceEnum == type && r.groupIndex == 1) {
                    if (r.owningSourceLifetimeEpoch != lifetime || r.observedNonNull != 1)
                        return false;
                    found = true;
                }
            }
            return found;
        }
        bool programShape(const NrPacketV1& p, const PipelineProgramFacts& f) noexcept {
            const bool native = p.header.actualNrMode == NR_NATIVE100;
            return heldProgram(p, f.rootAddress, f.rootLifetime, NR_ROOT12) &&
                   heldProgram(p, f.graphicsAddress, f.graphicsLifetime, NR_PSO12) && !zero(f.rootHash.data(), 32) &&
                   !zero(f.vertexHash.data(), 32) && !zero(f.pixelHash.data(), 32) &&
                   (native ? (!f.computeAddress && !f.computeLifetime && zero(f.computeHash.data(), 32))
                           : (heldProgram(p, f.computeAddress, f.computeLifetime, NR_PSO12) &&
                              !zero(f.computeHash.data(), 32)));
        }
        void writeProgram(NrPacketV1& p, const PipelineProgramFacts& f) noexcept {
            const uint64_t words[6] = {f.rootAddress,
                                       f.rootLifetime,
                                       f.computeAddress,
                                       f.computeLifetime,
                                       f.graphicsAddress,
                                       f.graphicsLifetime};
            std::memcpy(p.constants, words, sizeof words);
            std::memcpy(p.constants + 48, f.rootHash.data(), 32);
            std::memcpy(p.constants + 80, f.computeHash.data(), 32);
            std::memcpy(p.constants + 112, f.vertexHash.data(), 32);
            std::memcpy(p.constants + 144, f.pixelHash.data(), 32);
            p.header.constantsUsedBytes = 176;
        }
        bool programMatches(const NrPacketV1& p, const PipelineProgramFacts& f) noexcept {
            if (p.header.constantsUsedBytes < 176 || !programShape(p, f))
                return false;
            const uint64_t words[6] = {f.rootAddress,
                                       f.rootLifetime,
                                       f.computeAddress,
                                       f.computeLifetime,
                                       f.graphicsAddress,
                                       f.graphicsLifetime};
            if (std::memcmp(p.constants, words, sizeof words) || std::memcmp(p.constants + 48, f.rootHash.data(), 32) ||
                std::memcmp(p.constants + 80, f.computeHash.data(), 32) ||
                std::memcmp(p.constants + 112, f.vertexHash.data(), 32) ||
                std::memcmp(p.constants + 144, f.pixelHash.data(), 32))
                return false;
            for (uint64_t i = 0; i < p.header.drawCount; ++i) {
                const auto& d = p.draws[i];
                if (d.constantsOffset < 176 || d.shaderHashOffsetInConstants < 176)
                    return false;
                if (d.kind <= 2 && (d.rootAddress != f.rootAddress ||
                                    d.psoAddress != (d.kind == 1 ? f.computeAddress : f.graphicsAddress) ||
                                    std::memcmp(p.constants + d.shaderHashOffsetInConstants,
                                                d.kind == 1 ? f.computeHash.data() : f.pixelHash.data(),
                                                32)))
                    return false;
            }
            return true;
        }
        bool submissionCalls(const NrPacketV1& p) noexcept {
            for (const auto kind : {NR_CLOSE, NR_EXECUTE, NR_SIGNAL}) {
                unsigned count = 0;
                for (uint64_t i = 0; i < p.header.callCount; ++i)
                    if (p.calls[i].kind == kind && p.calls[i].fenceAddress == p.groups[1].actualFence)
                        ++count;
                if (count != 1)
                    return false;
            }
            unsigned signals = 0;
            for (uint64_t i = 0; i < p.header.callCount; ++i)
                if (p.calls[i].kind == NR_SIGNAL && p.calls[i].fenceAddress == p.groups[0].actualFence)
                    ++signals;
            return signals == 1;
        }
    } // namespace
    bool validateEnginePacket(const NrPacketV1& p,
                              const EnginePacketCohort& cohort,
                              const PipelineProgramFacts& program,
                              NrError& error) noexcept {
        error = {};
        if (!cohort.request || !cohort.ready || !cohort.fgBudget || !cohort.source || !cohort.postFx ||
            !cohort.sourceModule)
            return fail(error, NR_BAD_BUFFER);
        if (!nr_validate_packet(&p, sizeof p, &error))
            return false;
        if (!nr_capture_host_reference_reservation(&p) || p.header.roleCount > 28 || p.header.drawCount > 8 ||
            p.header.descriptorCount > 50)
            return fail(error, NR_BAD_BUDGET);
        if (!programMatches(p, program))
            return fail(error, NR_BAD_DRAW);
        if (!requestMatches(p, cohort))
            return fail(error, NR_BAD_IDENTITY);
        if (!nr_fg_budget_matches_packet(cohort.fgBudget, &p) ||
            !nr_fg_budget_matches_ready(cohort.fgBudget, cohort.ready))
            return fail(error, NR_BAD_BUDGET);
        if (!nr_validate_source(cohort.source, sizeof *cohort.source, &error) ||
            !nr_capture_source_matches(&p, cohort.source))
            return fail(error, NR_BAD_SOURCE);
        if (!matchSourceModule(p, *cohort.source, *cohort.sourceModule, 4))
            return fail(error, NR_BAD_MODULE);
        if (!nr_validate_postfx(&p, cohort.postFx, sizeof *cohort.postFx))
            return fail(error, NR_BAD_DRAW);
        if (!submissionCalls(p))
            return fail(error, NR_BAD_CALL);
        return true;
    }
    bool assembleEnginePacket(NrPacketV1& out, const EnginePacketInput& in, NrError& error) noexcept {
        if (!separate(&error, sizeof error, out))
            return false;
        error = {};
        if (!zero(&out, sizeof out))
            return fail(error, NR_NONZERO_UNUSED);
        struct ClearOnFailure {
            NrPacketV1& packet;
            bool committed{};
            ~ClearOnFailure() {
                if (!committed)
                    std::memset(&packet, 0, sizeof packet);
            }
        } transaction{out};
        if (!inputs(in, out))
            return fail(error, NR_BAD_RANGE);
        if (!emptyPlacement(*in.header))
            return fail(error, NR_BAD_HEADER);
        out.header = *in.header;
        out.identity = *in.identity;
        out.budget = *in.budget;
        for (uint64_t i = 0; i < in.refs.count; ++i)
            out.refs[out.header.refCount++] = in.refs.data[i];
        for (uint64_t i = 0; i < in.groups.count; ++i)
            out.groups[out.header.groupCount++] = in.groups.data[i];
        for (uint64_t i = 0; i < in.controlCalls.count; ++i) {
            const auto& c = in.controlCalls.data[i];
            if (c.kind <= NR_RELEASE_FEATURE || c.kind == NR_PIN || c.kind == NR_COPY)
                return fail(error, NR_BAD_CALL, i);
            out.calls[out.header.callCount++] = c;
        }
        for (uint64_t i = 0; i < in.aliases.count; ++i) {
            const auto& a = in.aliases.data[i];
            if (a.exportCallIndex >= in.controlCalls.count || a.importCallIndex >= in.controlCalls.count)
                return fail(error, NR_BAD_ALIAS, i);
            out.aliases[out.header.aliasCount++] = a;
        }
        for (uint64_t i = 0; i < in.modules.count; ++i) {
            const auto& m = in.modules.data[i];
            if (!appendModuleObservation(out, *m.module, m.actualPin, m.expectation))
                return fail(error, NR_BAD_MODULE, i);
        }
        for (uint64_t i = 0; i < in.sdk.count; ++i) {
            const auto& sdk = in.sdk.data[i];
            if (!appendSdkObservation(out, *sdk.observation, sdk.binding))
                return fail(error, NR_BAD_CALL, i);
        }
        for (uint64_t i = 0; i < in.roles.count; ++i) {
            const auto& r = in.roles.data[i];
            if (!appendSnapshotRole(out, *r.allocation, *r.metadata, *r.writer))
                return fail(error, NR_BAD_ROLE, i);
        }
        const auto first = out.header.descriptorCount;
        if (first + in.drawDescriptors.count > 50)
            return fail(error, NR_BAD_DESCRIPTOR);
        for (uint64_t i = 0; i < in.drawDescriptors.count; ++i) {
            const auto& d = in.drawDescriptors.data[i];
            if (d.kind == NR_DESC_RESOURCE_DESCRIPTION)
                return fail(error, NR_BAD_DESCRIPTOR, i);
            out.descriptors[out.header.descriptorCount++] = d;
        }
        if (!programShape(out, *in.pipelineProgram))
            return fail(error, NR_BAD_DRAW);
        writeProgram(out, *in.pipelineProgram);
        for (uint64_t i = 0; i < in.draws.count; ++i) {
            const auto& supplied = in.draws.data[i];
            auto d = *supplied.draw;
            if (d.kind < 1 || d.kind > 4 || !range(d.descriptorFirst, d.descriptorCount, in.drawDescriptors.count) ||
                d.constantsBytes != supplied.constants.count || zero(supplied.shaderSha256.data, 32) ||
                !range(out.header.constantsUsedBytes, 32 + supplied.constants.count, sizeof out.constants))
                return fail(error, NR_BAD_DRAW, i);
            if (d.kind <= 2) {
                const auto& f = *in.pipelineProgram;
                const auto* hash = d.kind == 1 ? f.computeHash.data() : f.pixelHash.data();
                if (d.rootAddress != f.rootAddress ||
                    d.psoAddress != (d.kind == 1 ? f.computeAddress : f.graphicsAddress) ||
                    std::memcmp(supplied.shaderSha256.data, hash, 32))
                    return fail(error, NR_BAD_DRAW, i);
            }
            d.descriptorFirst += first;
            d.shaderHashOffsetInConstants = out.header.constantsUsedBytes;
            std::memcpy(out.constants + out.header.constantsUsedBytes, supplied.shaderSha256.data, 32);
            out.header.constantsUsedBytes += 32;
            d.constantsOffset = out.header.constantsUsedBytes;
            if (supplied.constants.count)
                std::memcpy(out.constants + out.header.constantsUsedBytes,
                            supplied.constants.data,
                            static_cast<size_t>(supplied.constants.count));
            out.header.constantsUsedBytes += supplied.constants.count;
            out.draws[out.header.drawCount++] = d;
        }
        if (!validateEnginePacket(out, in.cohort, *in.pipelineProgram, error))
            return false;
        transaction.committed = true;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
