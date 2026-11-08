#include "pair_capture_nr_worker.hpp"
#include "pair_capture_nr_control.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <bcrypt.h>
#include <cstdio>
#include <fstream>

namespace vdxrfg::pair_capture::nr {
    bool mappedExtent(const NrRoleV1& r, uint64_t pitch, uint64_t& bytes) noexcept {
        bytes = 0;
        const uint64_t pixelBytes = nr_capture_pixel_bytes(r.format);
        if (!pixelBytes || !r.width || r.width > UINT64_MAX / pixelBytes || !r.height || !r.depthOrArray || !pitch ||
            pitch < r.width * pixelBytes || pitch > r.mapPitchBound || pitch > UINT64_MAX / r.height)
            return false;
        const auto slice = pitch * r.height;
        if (slice > UINT64_MAX / r.depthOrArray)
            return false;
        bytes = slice * r.depthOrArray;
        return bytes <= r.chargedBytes;
    }
    namespace {
        uint64_t fenceRef(const NrPacketV1& p, unsigned group) noexcept {
            for (uint64_t i = 0; i < p.header.refCount; ++i) {
                const auto& r = p.refs[i];
                if (r.groupIndex == group && r.borrowedIUnknownAddress == p.groups[group].actualFence &&
                    r.roleMask == 64)
                    return i;
            }
            return NR_ABSENT;
        }
        template <class Fence>
        bool waitComplete(Fence* fence, uint64_t target, uint64_t& completed, HANDLE& retained) {
            if (!fence || !target || target == UINT64_MAX)
                return false;
            completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX)
                return false;
            if (completed >= target)
                return true;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event)
                return false;
            if (FAILED(fence->SetEventOnCompletion(target, event))) {
                CloseHandle(event);
                return false;
            }
            const DWORD result = WaitForSingleObject(event, 15000);
            completed = fence->GetCompletedValue();
            if (result != WAIT_OBJECT_0 || completed == UINT64_MAX || completed < target) {
                retained = event;
                return false;
            }
            CloseHandle(event);
            return true;
        }
        bool persist(const std::filesystem::path& directory, const void* data, PayloadRecord& record) {
            if (!data || !record.bytes || record.bytes > ULONG_MAX)
                return false;
            if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                           nullptr,
                           0,
                           reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                           static_cast<ULONG>(record.bytes),
                           record.sha256.data(),
                           32) < 0)
                return false;
            std::ofstream file(directory / record.filename.data(), std::ios::binary | std::ios::trunc);
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(record.bytes));
            file.close();
            return file.good();
        }
    } // namespace
    bool writeHostFrame(Receiver& receiver,
                        unsigned frame,
                        const std::filesystem::path& output,
                        NrPacketV1& final,
                        std::array<PayloadRecord, 64>& records,
                        uint64_t& written,
                        HANDLE retainedEvents[3]) noexcept {
        try {
            const auto* owned = receiver.beginRead(frame);
            if (!owned)
                return false;
            final = *owned;
            // Exact host packet remains immutable, including its original pointer
            // values. The disk image is evidence only and never an acquisition input.
            std::ofstream packet(output / (frame ? "nr-host-B.packet" : "nr-host-A.packet"), std::ios::binary);
            packet.write(reinterpret_cast<const char*>(owned), sizeof(*owned));
            packet.close();
            if (!packet.good())
                return false;
            for (unsigned g = 0; g < 3; ++g) {
                const auto& row = owned->groups[g];
                const auto index = fenceRef(*owned, g);
                uint64_t completed{};
                const bool success =
                    g == 1 ? waitComplete(static_cast<ID3D12Fence*>(receiver.borrow(frame, index, NR_FENCE12)),
                                          row.coveringSignalValue,
                                          completed,
                                          retainedEvents[g])
                           : waitComplete(static_cast<ID3D11Fence*>(receiver.borrow(frame, index, NR_FENCE11)),
                                          row.coveringSignalValue,
                                          completed,
                                          retainedEvents[g]);
                const NrSubmissionKey key{
                    row.actualQueueOrContext, row.actualFence, row.actualList, row.actualAllocator, row.recordingEpoch};
                if (!success || !receiver.completed(frame, g, key, completed))
                    return false;
                final.groups[g].completedObserved = 1;
                final.groups[g].completedValue = completed;
            }
            for (uint64_t i = 0; i < owned->header.roleCount; ++i) {
                const auto& role = owned->roles[i];
                auto& record = records[static_cast<size_t>(i)];
                record.frame = frame;
                record.roleIndex = i;
                const int n = std::snprintf(record.filename.data(),
                                            record.filename.size(),
                                            "nr-%c-role-%llu-eye-%llu.bin",
                                            frame ? 'B' : 'A',
                                            role.role,
                                            role.eye);
                if (n <= 0 || static_cast<size_t>(n) >= record.filename.size())
                    return false;
                if (role.groupIndex == 1) {
                    auto* resource =
                        static_cast<ID3D12Resource*>(receiver.borrow(frame, role.snapshotRefIndex, NR_RESOURCE12));
                    if (!resource || written > owned->budget.privatePayloadReservation ||
                        role.payloadBytes > owned->budget.privatePayloadReservation - written)
                        return false;
                    void* mapped{};
                    const D3D12_RANGE range{0, static_cast<SIZE_T>(role.payloadBytes)};
                    if (FAILED(resource->Map(0, &range, &mapped)))
                        return false;
                    record.bytes = role.payloadBytes;
                    record.rowPitch = owned->footprints[role.footprintFirst].rowPitch;
                    bool okay = false;
                    try {
                        okay = persist(output, mapped, record);
                    } catch (...) {
                        const D3D12_RANGE noWrite{};
                        resource->Unmap(0, &noWrite);
                        throw;
                    }
                    const D3D12_RANGE noWrite{};
                    resource->Unmap(0, &noWrite);
                    if (!okay)
                        return false;
                } else {
                    if (role.subresourceCount != 1 || role.depthOrArray != 1 || !receiver.claimScratch(frame, 1))
                        return false;
                    auto* texture =
                        static_cast<ID3D11Texture2D*>(receiver.borrow(frame, role.snapshotRefIndex, NR_TEXTURE11));
                    IDXGISurface* surface{};
                    if (!texture || FAILED(texture->QueryInterface(IID_PPV_ARGS(&surface))) || !surface) {
                        receiver.returnScratch(frame, 1);
                        return false;
                    }
                    DXGI_MAPPED_RECT mapped{};
                    const HRESULT result = surface->Map(&mapped, DXGI_MAP_READ);
                    if (FAILED(result)) {
                        surface->Release();
                        receiver.returnScratch(frame, 1);
                        return false;
                    }
                    bool okay = mapped.Pitch > 0 &&
                                mappedExtent(role, static_cast<uint64_t>(mapped.Pitch), record.bytes) &&
                                written <= owned->budget.privatePayloadReservation &&
                                record.bytes <= owned->budget.privatePayloadReservation - written;
                    if (okay) {
                        record.rowPitch = static_cast<uint64_t>(mapped.Pitch);
                        try {
                            okay = persist(output, mapped.pBits, record);
                        } catch (...) {
                            surface->Unmap();
                            surface->Release();
                            receiver.returnScratch(frame, 1);
                            throw;
                        }
                    }
                    const HRESULT unmap = surface->Unmap();
                    surface->Release();
                    receiver.returnScratch(frame, 1);
                    if (!okay || FAILED(unmap))
                        return false;
                    auto& footprint = final.footprints[role.footprintFirst];
                    footprint.rowPitch = record.rowPitch;
                    footprint.totalBytes = record.bytes;
                    final.roles[i].payloadBytes = record.bytes;
                }
                written += record.bytes;
            }
            // A caller still has to append actual FG admission/Draw/native output
            // facts, persist all5 FG payloads, validate, then close readers/endWorker.
            return true;
        } catch (...) {
            return false;
        }
    }
} // namespace vdxrfg::pair_capture::nr
