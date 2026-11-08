#include "PairCaptureNrDrawPacket.h"
#include <cstring>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        bool epoch(uint64_t n) noexcept {
            return n && n != UINT64_MAX;
        }
        bool zero(const void* data, size_t bytes) noexcept {
            const auto* b = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i)
                if (b[i])
                    return false;
            return true;
        }
        template <class T>
        bool span(EnginePacketSpan<T> s, uint64_t cap) noexcept {
            return s.count <= cap && (!s.count || s.data);
        }
        bool held(const EnginePacketInput& f, uint64_t address, uint64_t life, uint64_t type, uint64_t group) noexcept {
            if (!address || !epoch(life))
                return false;
            bool found = false;
            for (uint64_t i = 0; i < f.refs.count; ++i) {
                const auto& r = f.refs.data[i];
                if (r.borrowedIUnknownAddress == address && r.interfaceEnum == type && r.groupIndex == group) {
                    if (r.owningSourceLifetimeEpoch != life || r.observedNonNull != 1)
                        return false;
                    found = true;
                }
            }
            return found;
        }
        bool heapHeld(const EnginePacketInput& f, uint64_t address) noexcept {
            uint64_t life = 0;
            for (uint64_t i = 0; i < f.refs.count; ++i) {
                const auto& r = f.refs.data[i];
                if (r.borrowedIUnknownAddress == address && r.interfaceEnum == NR_HEAP12 && r.groupIndex == 1) {
                    if (!address || !epoch(r.owningSourceLifetimeEpoch) || r.observedNonNull != 1 ||
                        (life && life != r.owningSourceLifetimeEpoch))
                        return false;
                    life = r.owningSourceLifetimeEpoch;
                }
            }
            return life != 0;
        }
        bool actualWriter(const ResourceWriteFact& w) noexcept {
            return w.resource && epoch(w.lifetime) && epoch(w.sequence) && epoch(w.serial) && w.contextOrList &&
                   w.thread && w.qpc && (w.api == 11 || w.api == 12) &&
                   (w.api == 12 ? epoch(w.recordingEpoch) : !w.recordingEpoch) &&
                   (w.kind == NR_COPY || w.kind == NR_DISPATCH || w.kind == NR_DRAW || w.kind == NR_EVALUATE) &&
                   (w.kind != NR_EVALUATE || (w.api == 12 && epoch(w.actualCallSequence)));
        }
        bool aliasValid(const EnginePacketInput& f, const NrAliasV1& a) noexcept {
            if ((a.apiKind != NR_TEXTURE11_TO_TEXTURE11 && a.apiKind != NR_TEXTURE11_TO_RESOURCE12) ||
                a.observedMask != 3 || a.reserved || !a.sourceAddress || !a.targetAddress ||
                a.sourceAddress == a.targetAddress || !epoch(a.sourceLifetime) || !epoch(a.targetLifetime) ||
                !epoch(a.sourceWriteEpoch) || !epoch(a.targetWriteEpoch) || !a.exportedHandleObserved ||
                a.exportCallIndex >= f.controlCalls.count || a.importCallIndex >= f.controlCalls.count)
                return false;
            const auto& ex = f.controlCalls.data[a.exportCallIndex];
            const auto& im = f.controlCalls.data[a.importCallIndex];
            return ex.kind == NR_EXPORT_FENCE && im.kind == NR_IMPORT_FENCE && ex.beginObserved && ex.endObserved &&
                   im.beginObserved && im.endObserved && ex.resultType == NR_HRESULT && im.resultType == NR_HRESULT &&
                   ex.resultBits <= INT32_MAX && im.resultBits <= INT32_MAX && ex.classifiedSuccess == 1 &&
                   im.classifiedSuccess == 1 && ex.fenceAddress == a.sourceAddress &&
                   im.fenceAddress == a.targetAddress && ex.deviceAddress == a.sourceDevice &&
                   im.deviceAddress == a.targetDevice && ex.aux.auxiliaryValue == a.exportedHandleObserved &&
                   im.aux.auxiliaryValue == a.exportedHandleObserved;
        }
        struct Endpoint {
            uint64_t address, life, write;
        };
        bool same(Endpoint a, Endpoint b) noexcept {
            return a.address == b.address && a.life == b.life && a.write == b.write;
        }
        bool hop(const NrAliasV1& a, Endpoint from, Endpoint& to) noexcept {
            const Endpoint source{a.sourceAddress, a.sourceLifetime, a.sourceWriteEpoch},
                target{a.targetAddress, a.targetLifetime, a.targetWriteEpoch};
            if (same(from, source)) {
                to = target;
                return true;
            }
            if (same(from, target)) {
                to = source;
                return true;
            }
            return false;
        }
        bool related(const EnginePacketInput& f, Endpoint a, Endpoint b) noexcept {
            if (same(a, b))
                return true;
            for (uint64_t i = 0; i < f.aliases.count; ++i) {
                Endpoint next{};
                const auto& x = f.aliases.data[i];
                if (!aliasValid(f, x) || !hop(x, a, next))
                    continue;
                if (same(next, b))
                    return true;
                for (uint64_t j = 0; j < f.aliases.count; ++j) {
                    Endpoint end{};
                    if (j != i && aliasValid(f, f.aliases.data[j]) && hop(f.aliases.data[j], next, end) && same(end, b))
                        return true;
                }
            }
            return false;
        }
        const EnginePacketRoleInput* role(const EnginePacketInput& f, uint64_t kind, uint64_t eye) noexcept {
            const EnginePacketRoleInput* found = nullptr;
            for (uint64_t i = 0; i < f.roles.count; ++i) {
                const auto& r = f.roles.data[i];
                if (!r.allocation || !r.metadata || !r.writer)
                    return nullptr;
                if (r.allocation->plan.role != kind || r.allocation->plan.eye != eye)
                    continue;
                if (found)
                    return nullptr;
                const auto& m = *r.metadata;
                const auto& a = *r.allocation;
                const auto& s = a.sourceDescription;
                const uint64_t group = kind <= 2 ? 0 : kind <= 12 ? 1 : 2;
                if (a.plan.group != group || !m.geometryObserved || m.hostSerial != f.identity->hostSerial ||
                    m.sourceOccurrence != f.identity->sourceOccurrence || !m.rectWidth || !m.rectHeight ||
                    !m.cropWidth || !m.cropHeight || m.selectedArraySlice >= a.plan.subresources ||
                    !actualWriter(*r.writer) || !s.resourceAddress || !epoch(s.resourceLifetimeEpoch) ||
                    !epoch(s.sourceWriteEpoch) ||
                    !held(f,
                          s.resourceAddress,
                          s.resourceLifetimeEpoch,
                          group == 1 ? NR_RESOURCE12 : NR_TEXTURE11,
                          group) ||
                    !related(f,
                             {r.writer->resource, r.writer->lifetime, r.writer->sequence},
                             {s.resourceAddress, s.resourceLifetimeEpoch, s.sourceWriteEpoch}))
                    return nullptr;
                found = &r;
            }
            return found;
        }
        bool resource(const NrDescriptorV1& v, const EnginePacketRoleInput* r) noexcept {
            return r && v.resourceAddress == r->allocation->sourceDescription.resourceAddress &&
                   v.resourceLifetimeEpoch == r->allocation->sourceDescription.resourceLifetimeEpoch;
        }
        bool consumed(const EnginePacketInput& f,
                      const NrDescriptorV1& v,
                      const ResourceWriteFact& w,
                      const EnginePacketRoleInput* r) noexcept {
            if (!resource(v, r) || !actualWriter(w))
                return false;
            const auto& expected = *r->writer;
            const auto& s = r->allocation->sourceDescription;
            // A proven storage association maps endpoints/epoch namespaces, never the
            // original writer's command, serial, thread or CPU observation time.
            return w.api == expected.api && w.kind == expected.kind && w.serial == expected.serial &&
                   w.contextOrList == expected.contextOrList && w.sourceAddress == expected.sourceAddress &&
                   w.recordingEpoch == expected.recordingEpoch && w.actualCallSequence == expected.actualCallSequence &&
                   w.thread == expected.thread && w.qpc == expected.qpc &&
                   related(f,
                           {w.resource, w.lifetime, w.sequence},
                           {s.resourceAddress, s.resourceLifetimeEpoch, s.sourceWriteEpoch});
        }
        bool creationSame(NrDescriptorV1 a, NrDescriptorV1 b) noexcept {
            a.bindEpoch = b.bindEpoch = 0;
            a.sourceWriteEpoch = b.sourceWriteEpoch = 0;
            a.observedMask = b.observedMask = 0;
            return !std::memcmp(&a, &b, sizeof a);
        }
        bool program(const EnginePacketInput& f) noexcept {
            const auto& p = *f.pipelineProgram;
            const bool native = f.header->actualNrMode == NR_NATIVE100;
            return held(f, p.rootAddress, p.rootLifetime, NR_ROOT12, 1) &&
                   held(f, p.graphicsAddress, p.graphicsLifetime, NR_PSO12, 1) && !zero(p.rootHash.data(), 32) &&
                   !zero(p.vertexHash.data(), 32) && !zero(p.pixelHash.data(), 32) &&
                   (native ? (!p.computeAddress && !p.computeLifetime && zero(p.computeHash.data(), 32))
                           : (held(f, p.computeAddress, p.computeLifetime, NR_PSO12, 1) &&
                              !zero(p.computeHash.data(), 32)));
        }
        bool pipeline(const EnginePacketInput& f, const PipelineDrawFacts& r, uint64_t kind, uint64_t eye) noexcept {
            const auto& d = r.draw;
            const auto& g = f.groups.data[1];
            const auto& p = *f.pipelineProgram;
            const auto& rec = r.recording;
            const bool native = f.header->actualNrMode == NR_NATIVE100;
            if (!r.bound || !r.recorded || !r.restored || r.serial != f.identity->hostSerial ||
                !epoch(r.commandObservation) || d.kind != kind || d.eye != eye || d.listOrContext != g.actualList ||
                d.recordingEpoch != g.recordingEpoch || !epoch(d.bindEpoch) || !epoch(d.pipelineLifetime) ||
                !epoch(d.pipelineGeneration) || !epoch(d.batchEpoch) ||
                d.psoAddress != (kind == 1 ? p.computeAddress : p.graphicsAddress) || d.rootAddress != p.rootAddress ||
                d.pipelineLifetime != f.identity->pipelineLifetimeEpoch ||
                d.pipelineGeneration != f.identity->pipelineGenerationEpoch ||
                d.batchEpoch != f.identity->batchObjectEpoch || d.constantsBytes != 80 ||
                std::memcmp(r.shaderHash.data(), kind == 1 ? p.computeHash.data() : p.pixelHash.data(), 32) ||
                reinterpret_cast<uint64_t>(rec.device) != f.identity->nrDeviceAddress ||
                reinterpret_cast<uint64_t>(rec.queue) != g.actualQueueOrContext ||
                reinterpret_cast<uint64_t>(rec.fence) != g.actualFence ||
                reinterpret_cast<uint64_t>(rec.list) != g.actualList ||
                reinterpret_cast<uint64_t>(rec.allocator) != g.actualAllocator || rec.facts.observed != 31 ||
                rec.facts.nrRecordingEpoch != d.recordingEpoch ||
                reinterpret_cast<uint64_t>(rec.facts.originatingCommandContext) != f.identity->nrContextAddress ||
                rec.facts.nrCommandContextLifetimeEpoch != f.identity->nrContextLifetimeEpoch ||
                !epoch(rec.facts.nrListObjectEpoch) || !epoch(rec.facts.nrAllocatorObjectEpoch) ||
                rec.facts.nrListObjectEpoch != f.identity->listObjectEpoch ||
                rec.facts.nrAllocatorObjectEpoch != f.identity->allocatorObjectEpoch ||
                r.shaderReadMask !=
                    pipelineShaderReadMask(static_cast<uint32_t>(kind), native, r.descriptors[1].resourceAddress != 0))
                return false;
            for (unsigned i = 0; i < 7; ++i) {
                const auto& v = r.descriptors[i];
                if (v.kind != (i < 4   ? NR_DESC_SRV
                               : i < 6 ? NR_DESC_UAV
                                       : NR_DESC_RTV) ||
                    v.descriptionKind != (i < 4   ? NR_D12_SRV
                                          : i < 6 ? NR_D12_UAV
                                                  : NR_D12_RTV) ||
                    !v.cpuHandle || v.objectAddress != v.cpuHandle || !epoch(v.objectLifetimeEpoch) ||
                    !epoch(v.writeEpoch) || !heapHeld(f, v.heapAddress) ||
                    (i < 6 ? (v.heapSlot != eye * 6 + i || !v.descriptorIncrement || v.bindEpoch != d.bindEpoch ||
                              v.descriptorIncrement > (UINT64_MAX - d.rootTable0) / 5 ||
                              v.gpuHandle != d.rootTable0 + i * v.descriptorIncrement)
                           : (v.heapSlot || v.descriptorIncrement || v.gpuHandle)))
                    return false;
                if (i < 6 && i &&
                    (v.heapAddress != r.descriptors[0].heapAddress ||
                     v.descriptorIncrement != r.descriptors[0].descriptorIncrement ||
                     v.cpuHandle != r.descriptors[0].cpuHandle + i * v.descriptorIncrement))
                    return false;
            }
            const auto* colour = role(f, kind == 1 ? NR_MODEL_COLOUR : NR_PRE_RESOLVE_COLOUR, eye);
            if (!consumed(f, r.descriptors[0], r.sourceAtBind[0], colour))
                return false;
            if (kind == 1) {
                if ((r.shaderReadMask & 2) &&
                    !consumed(f, r.descriptors[1], r.sourceAtBind[1], role(f, NR_MODEL_DEPTH, eye)))
                    return false;
                if (!d.dispatchX || !d.dispatchY || d.dispatchZ != 1 || d.vertexCount || d.instanceCount ||
                    r.targetCpu || r.outputCount != (r.descriptors[5].resourceAddress ? 2u : 1u))
                    return false;
                for (unsigned i = 0; i < r.outputCount; ++i) {
                    const auto* output = role(f, i ? NR_PROXY_DEPTH : NR_PROXY, eye);
                    const auto& w = r.outputs[i];
                    if (!resource(r.descriptors[4 + i], output) || !actualWriter(w) || w.api != 12 ||
                        !pipelineOutputFresh(w, r, 4 + i) || std::memcmp(&w, output->writer, sizeof w))
                        return false;
                }
            } else {
                if (!native && !consumed(f, r.descriptors[2], r.sourceAtBind[2], role(f, NR_EXACT_PROXY, eye)))
                    return false;
                if (!consumed(f,
                              r.descriptors[3],
                              r.sourceAtBind[3],
                              role(f, native ? NR_NATIVE_ANSWER : NR_SPATIAL_ANSWER, native ? 2 : eye)) ||
                    r.outputCount != 1 || r.targetCpu != r.descriptors[6].cpuHandle || !r.targetCpu ||
                    !actualWriter(r.outputs[0]) || r.outputs[0].api != 12 || !pipelineOutputFresh(r.outputs[0], r, 6) ||
                    !resource(r.descriptors[6], role(f, NR_POST_RESOLVE, 2)) || d.vertexCount != 3 ||
                    d.instanceCount != 1 || d.startVertex || d.startInstance)
                    return false;
            }
            return d.resourceWriteEpoch == r.outputs[0].sequence;
        }
        void append(DrawPacketProjection& out,
                    const NrDrawV1& d,
                    const uint8_t* hash,
                    const uint8_t* constants,
                    uint64_t first,
                    uint64_t count,
                    uint64_t module) noexcept {
            const auto i = out.drawCount++;
            out.draws[i] = d;
            auto& row = out.draws[i];
            row.descriptorFirst = first;
            row.descriptorCount = count;
            row.shaderModuleIndex = module;
            row.constantsOffset = row.shaderHashOffsetInConstants = 0;
            row.observedMask = 15;
            std::memcpy(out.hashes[i].data(), hash, 32);
            std::memcpy(out.constants[i].data(), constants, static_cast<size_t>(d.constantsBytes));
            out.inputs[i] = {&row, {out.hashes[i].data(), 32}, {out.constants[i].data(), d.constantsBytes}};
        }
        bool postFx(const EnginePacketInput& f, const PostFxRecord& r, uint64_t kind, uint64_t eye) noexcept {
            if (!postFxRecordComplete(r) || r.serial != f.identity->hostSerial || r.draw.kind != kind ||
                r.draw.eye != eye || r.upscaling != !!f.identity->settings.upscaling ||
                r.sharpening != !!f.identity->settings.sharpening ||
                r.draw.listOrContext != f.groups.data[2].actualQueueOrContext ||
                !held(f, r.draw.psoAddress, r.shaderLifetime, NR_SHADER11, 2) ||
                !held(f, r.draw.rootAddress, r.bufferLifetime, NR_BUFFER11, 2))
                return false;
            for (unsigned i = 0; i < r.descriptorCount; ++i) {
                const auto& v = r.descriptors[i];
                const auto binding = i == 0 ? 2u : i == 1 ? 3u : 4u;
                if (v.bindEpoch != r.bindingEpochs[binding] ||
                    !held(f, v.objectAddress, v.objectLifetimeEpoch, i == 2 ? NR_SAMPLER11 : NR_VIEW11, 2))
                    return false;
            }
            const auto* output = role(f, kind == 3 ? NR_POST_EASU : NR_POST_CAS, eye);
            if (!resource(r.descriptors[1], output) ||
                r.draw.resourceWriteEpoch != output->allocation->sourceDescription.sourceWriteEpoch ||
                output->writer->api != 11 || output->writer->kind != NR_DISPATCH ||
                output->writer->serial != r.serial || output->writer->contextOrList != r.draw.listOrContext ||
                output->writer->qpc > r.dispatchQpc)
                return false;
            if (kind == 4 && f.identity->settings.upscaling) {
                const auto* input = role(f, NR_POST_EASU, eye);
                if (!resource(r.descriptors[0], input) ||
                    r.descriptors[0].sourceWriteEpoch != input->allocation->sourceDescription.sourceWriteEpoch)
                    return false;
            } else {
                const auto* input = role(f, NR_POST_RESOLVE, 2);
                if (!input || !related(f,
                                       {r.descriptors[0].resourceAddress,
                                        r.descriptors[0].resourceLifetimeEpoch,
                                        r.descriptors[0].sourceWriteEpoch},
                                       {input->allocation->sourceDescription.resourceAddress,
                                        input->allocation->sourceDescription.resourceLifetimeEpoch,
                                        input->allocation->sourceDescription.sourceWriteEpoch}))
                    return false;
            }
            return true;
        }
    } // namespace
    bool projectDrawPacket(DrawPacketProjection& out, const DrawPacketInput& in, NrError& error) noexcept {
        out = {};
        error = {};
        struct Transaction {
            DrawPacketProjection& out;
            bool done{};
            ~Transaction() {
                if (!done)
                    out = {};
            }
        } transaction{out};
        auto fail = [&](uint64_t index) {
            error.code = NR_BAD_DRAW;
            error.index = static_cast<uint32_t>(index);
            return false;
        };
        if (!in.frame || !in.frame->header || !in.frame->identity || !in.frame->pipelineProgram ||
            !span(in.pipeline, 4) || in.pipeline.count != 4 || !span(in.postFx, 4) || in.postFx.count != 4)
            return fail(0);
        const auto& f = *in.frame;
        const bool native = f.header->actualNrMode == NR_NATIVE100;
        if ((!native && f.header->actualNrMode != NR_SPATIAL85) || !span(f.refs, 89) || !span(f.groups, 3) ||
            f.groups.count != 3 || !span(f.roles, 28) || !span(f.modules, 8) ||
            in.shaderModuleIndex >= f.modules.count || !span(f.aliases, 64) || !span(f.controlCalls, 160) ||
            !program(f))
            return fail(0);
        const auto& m = f.modules.data[in.shaderModuleIndex];
        if (!m.module || m.module->moduleKind != 1 || m.expectation.kind != 1 ||
            m.module->loadLifetimeEpoch != m.expectation.loadLifetimeEpoch ||
            std::memcmp(m.module->sha256Words, m.expectation.expectedSha256.data(), 32))
            return fail(0);
        for (unsigned i = 0; i < 4; ++i) {
            const auto& r = in.pipeline.data[i];
            if (native && i < 2) {
                if (r.bound || r.recorded || r.restored || r.serial || r.draw.kind)
                    return fail(i);
                continue;
            }
            if (!pipeline(f, r, i / 2 + 1, i % 2))
                return fail(i);
            for (unsigned j = 0; j < i; ++j)
                if (in.pipeline.data[j].recorded && in.pipeline.data[j].commandObservation == r.commandObservation)
                    return fail(i);
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& end = in.pipeline.data[2 + eye];
            for (unsigned slot = 0; slot < 6; ++slot) {
                auto v = end.descriptors[slot];
                if (!native && !creationSame(v, in.pipeline.data[eye].descriptors[slot]))
                    return fail(eye);
                if (v.resourceAddress) {
                    const uint64_t kind = slot == 0                ? NR_PRE_RESOLVE_COLOUR
                                          : slot == 1              ? NR_MODEL_DEPTH
                                          : slot == 2 || slot == 4 ? NR_PROXY
                                          : slot == 3              ? (native ? NR_NATIVE_ANSWER : NR_SPATIAL_ANSWER)
                                                                   : NR_PROXY_DEPTH;
                    const auto* r = role(f, kind, kind == NR_NATIVE_ANSWER ? 2 : eye);
                    if (!resource(v, r))
                        return fail(slot);
                    v.sourceWriteEpoch = r->allocation->sourceDescription.sourceWriteEpoch;
                } else if (v.resourceLifetimeEpoch)
                    return fail(slot);
                v.observedMask = 15;
                out.descriptors[eye ? 7 + slot : slot] = v;
            }
        }
        const auto& left = in.pipeline.data[2];
        const auto& right = in.pipeline.data[3];
        const auto* target = role(f, NR_POST_RESOLVE, 2);
        if (!target || !creationSame(left.descriptors[6], right.descriptors[6]) ||
            left.commandObservation >= right.commandObservation ||
            left.outputs[0].sequence >= right.outputs[0].sequence ||
            right.targetWriteBefore != left.outputs[0].sequence ||
            std::memcmp(&right.outputs[0], target->writer, sizeof(ResourceWriteFact)))
            return fail(6);
        out.descriptors[6] = right.descriptors[6];
        out.descriptors[6].sourceWriteEpoch = right.outputs[0].sequence;
        out.descriptors[6].bindEpoch = right.draw.bindEpoch;
        out.descriptors[6].observedMask = 15;
        out.descriptorCount = 13;
        for (unsigned i = native ? 2 : 0; i < 4; ++i) {
            const auto& r = in.pipeline.data[i];
            append(out,
                   r.draw,
                   r.shaderHash.data(),
                   r.constants.data(),
                   i < 2 ? (i ? 7 : 0) : (i == 2 ? 0 : 6),
                   i < 2 ? 6 : 7,
                   in.shaderModuleIndex);
        }
        for (unsigned kind = 3; kind <= 4; ++kind) {
            const bool enabled = kind == 3 ? !!f.identity->settings.upscaling : !!f.identity->settings.sharpening;
            const auto& l = in.postFx.data[(kind - 3) * 2];
            const auto& r = in.postFx.data[(kind - 3) * 2 + 1];
            if (!enabled) {
                if (l.serial || r.serial || l.draw.kind || r.draw.kind)
                    return fail(kind);
                continue;
            }
            if (!postFx(f, l, kind, 0) || !postFx(f, r, kind, 1))
                return fail(kind);
            const auto first = out.descriptorCount;
            if (kind == 3) {
                if (!creationSame(l.descriptors[0], r.descriptors[0]) ||
                    l.descriptors[0].sourceWriteEpoch != r.descriptors[0].sourceWriteEpoch ||
                    !creationSame(l.descriptors[2], r.descriptors[2]))
                    return fail(kind);
                out.descriptors[first] = l.descriptors[1];
                out.descriptors[first + 1] = l.descriptors[0];
                out.descriptors[first + 2] = l.descriptors[2];
                out.descriptors[first + 3] = r.descriptors[1];
                out.descriptorCount += 4;
            } else {
                out.descriptors[first] = l.descriptors[0];
                out.descriptors[first + 1] = l.descriptors[1];
                out.descriptors[first + 2] = r.descriptors[0];
                out.descriptors[first + 3] = r.descriptors[1];
                out.descriptorCount += 4;
            }
            append(
                out, l.draw, l.shaderHash.data(), l.constants.data(), first, kind == 3 ? 3 : 2, in.shaderModuleIndex);
            append(out,
                   r.draw,
                   r.shaderHash.data(),
                   r.constants.data(),
                   first + (kind == 3 ? 1 : 2),
                   kind == 3 ? 3 : 2,
                   in.shaderModuleIndex);
        }
        transaction.done = true;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
