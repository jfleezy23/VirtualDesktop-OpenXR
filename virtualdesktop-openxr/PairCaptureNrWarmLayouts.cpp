#include "PairCaptureNrWarmLayouts.h"
#include "PairCaptureNrMetadataBudget.h"
#include "PairCaptureNrPostFx.h"
#include <mutex>
namespace virtualdesktop_openxr::nr_capture {
    namespace {
        struct Census {
            std::mutex mutex;
            std::array<WarmFrameLayout, 8> frames{};
            uint64_t nextDescription{1};
        } census;
        static_assert(sizeof(Census) <= hostMetadataWarm);
        WarmFrameLayout* frame(uint64_t serial) noexcept {
            auto& f = census.frames[serial % census.frames.size()];
            return f.serial == serial ? &f : nullptr;
        }
        void record(uint64_t serial, WarmRoleLayout row) noexcept {
            std::lock_guard lock(census.mutex);
            auto* f = frame(serial);
            if (!f || f->invalid)
                return;
            if (!warmRoleRequired(f->mode, f->upscaling, f->sharpening, row.role) ||
                (warmRoleEyes(row.role) == 1 ? row.eye != 2 : row.eye > 1) || !row.sourceAddress ||
                !census.nextDescription || census.nextDescription == UINT64_MAX) {
                f->invalid = true;
                return;
            }
            row.descriptionObservation = census.nextDescription++;
            for (uint32_t i = 0; i < f->count; ++i)
                if (f->roles[i].role == row.role && f->roles[i].eye == row.eye) {
                    f->invalid = true;
                    return;
                }
            if (f->count == f->roles.size()) {
                f->invalid = true;
                return;
            }
            f->roles[f->count++] = row;
        }
        void layout12(uint64_t serial, uint64_t role, uint64_t eye, ID3D12Resource* resource) noexcept {
            if (!resource)
                return;
            WarmRoleLayout row{};
            row.role = role;
            row.eye = eye;
            row.api = 12;
            row.sourceAddress = reinterpret_cast<uint64_t>(resource);
            row.texture12 = resource->GetDesc(); // Actual caller-owned resource, outside census mutex.
            record(serial, row);
        }
    } // namespace
    void warmLayoutBegin(uint64_t serial, uint64_t mode, bool upscaling, bool sharpening) noexcept {
        if (!hostRequest().valid || !serial || !requiredWarmRoleCount(mode, upscaling, sharpening))
            return;
        std::lock_guard lock(census.mutex);
        auto& f = census.frames[serial % census.frames.size()];
        f = {};
        f.serial = serial;
        f.mode = mode;
        f.upscaling = upscaling;
        f.sharpening = sharpening;
    }
    void warmLayout11(uint64_t serial, uint64_t role, uint64_t eye, ID3D11Texture2D* resource) noexcept {
        if (!hostRequest().valid || !resource)
            return;
        WarmRoleLayout row{};
        row.role = role;
        row.eye = eye;
        row.api = 11;
        row.sourceAddress = reinterpret_cast<uint64_t>(resource);
        resource->GetDesc(&row.texture11); // No observer lock spans COM.
        record(serial, row);
    }
    void warmLayoutGraph12(uint64_t serial,
                           uint64_t mode,
                           uint64_t contextLifetime,
                           const vdxrfg::pair_capture::nr::NrPipelineOwnership& graph,
                           ID3D12Resource* const nativeDepth[2]) noexcept {
        if (!hostRequest().valid || !contextLifetime)
            return;
        {
            std::lock_guard lock(census.mutex);
            auto* f = frame(serial);
            if (!f || f->mode != mode)
                return;
            f->nrContextLifetime = contextLifetime;
            f->pipelineLifetime = graph.nrPipelineLifetimeEpoch;
            f->pipelineGeneration = graph.nrPipelineGenerationEpoch;
        }
        for (uint64_t eye = 0; eye < 2; ++eye) {
            auto* colour = graph.inputs[eye * 2];
            auto* depth = mode == NR_NATIVE100 ? nativeDepth[eye] : graph.inputs[eye * 2 + 1];
            layout12(serial, NR_MODEL_COLOUR, eye, colour);
            layout12(serial, NR_MODEL_DEPTH, eye, depth);
            layout12(serial, NR_POST_EVAL_COLOUR, eye, colour);
            layout12(serial, NR_PRE_RESOLVE_COLOUR, eye, colour);
            if (mode == NR_SPATIAL85) {
                layout12(serial, NR_PROXY, eye, graph.scratch[eye].proxy);
                layout12(serial, NR_PROXY_DEPTH, eye, graph.scratch[eye].depth);
                layout12(serial, NR_SPATIAL_ANSWER, eye, graph.scratch[eye].answer);
                layout12(serial, NR_EXACT_PROXY, eye, graph.scratch[eye].proxy);
            }
        }
        if (mode == NR_NATIVE100)
            layout12(serial, NR_NATIVE_ANSWER, 2, graph.nativeAnswer);
        layout12(serial, NR_POST_RESOLVE, 2, graph.inputs[4]);
    }
    bool warmLayoutFinish(uint64_t serial) noexcept {
        bool complete = false, upscaling = false, sharpening = false;
        {
            std::lock_guard lock(census.mutex);
            auto* f = frame(serial);
            if (f && !f->invalid && f->nrContextLifetime && f->pipelineLifetime && f->pipelineGeneration) {
                complete = f->count == requiredWarmRoleCount(f->mode, f->upscaling, f->sharpening);
                upscaling = f->upscaling;
                sharpening = f->sharpening;
            }
        }
        complete = complete && postFxFrameComplete(serial, upscaling, sharpening);
        {
            std::lock_guard lock(census.mutex);
            auto* f = frame(serial);
            if (!f || f->invalid)
                complete = false;
            else
                f->complete = complete;
        }
        hostWarmLayoutComplete(serial, complete);
        return complete;
    }
    bool warmLayoutCopy(uint64_t serial, WarmFrameLayout& out) noexcept {
        out = {};
        std::lock_guard lock(census.mutex);
        const auto* f = frame(serial);
        if (!f || !f->complete || f->invalid)
            return false;
        out = *f;
        return true;
    }
} // namespace virtualdesktop_openxr::nr_capture
