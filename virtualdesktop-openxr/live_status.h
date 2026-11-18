#pragma once

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace virtualdesktop_openxr::live_status {
    constexpr uint64_t Magic = 0x3153544154534456ULL;
    constexpr uint64_t Unknown = UINT64_MAX;
    using Payload = std::array<uint64_t, 31>;

    struct Configuration {
        uint64_t fgMode{}, backend{}, depthMode{1}, graphicsApi{}, refreshMilliHz{};
    };

    enum class Route : uint64_t { ordinary, off, priming, unavailable, ineligible, busy, reprime };

    // Values used by the frame owner. Counters survive per-frame resets.
    struct Producer {
        uint64_t baseFrames{}, nrPairs{}, nrEnabled{Unknown}, coverage{Unknown}, processing{Unknown}, style{Unknown},
            intensity{Unknown}, tone{Unknown}, structure{Unknown}, skin{Unknown};
        uint64_t context{}, mode{}, backend{Unknown}, depth{Unknown}, guides{};
        uint64_t prepares{}, consumes{}, prepareResult{Unknown}, captureResult{Unknown};
        Route route{};
        uint64_t serial{};
        Payload payload() const noexcept {
            return {baseFrames, nrPairs,   nrEnabled,     coverage,      processing,
                    style,      intensity, tone,          structure,     skin,
                    context,    mode,      backend,       depth,         guides,
                    prepares,   consumes,  prepareResult, captureResult, static_cast<uint64_t>(route),
                    serial};
        }
    };

    // Successful End handoffs for a group may select the same role more than
    // once. Groups retire in order; the actual app serial disambiguates reuse
    // of native chains and ticket banks. A disabled layer has no image identity.
    struct Presenter {
        uint64_t handoffs{}, distinct{}, synthetic{}, repeats{}, identified{}, serial{}, role{Unknown};
        unsigned seenRoles{};
        void accepted(uint64_t sourceSerial, uint64_t selectedRole) noexcept {
            ++handoffs;
            if (sourceSerial && selectedRole < 4) {
                if (serial != sourceSerial)
                    seenRoles = 0;
                const auto bit = 1u << selectedRole;
                if (seenRoles & bit)
                    ++repeats;
                else
                    ++distinct;
                seenRoles |= bit;
                synthetic += selectedRole != 2;
                identified = 1;
            } else {
                identified = 0;
                seenRoles = 0;
            }
            serial = sourceSerial;
            role = identified ? selectedRole : Unknown;
        }
        Payload payload() const noexcept {
            return {handoffs, distinct, synthetic, repeats, identified, serial, role};
        }
    };

    // Windows x64 external-memory protocol. Each payload has one serialized
    // writer. Interlocked operations are full barriers; the external reader
    // copies bytes between equal, nonzero, even sequence observations.
    struct alignas(64) Channel {
        volatile LONG64 sequence{};
        volatile uint64_t words[31]{};
    };
    struct Wire {
        Channel session, producer, presenter;
    };
    static_assert(sizeof(Channel) == 256 && alignof(Channel) == 64);
    static_assert(sizeof(Wire) == 768 && offsetof(Wire, producer) == 256 && offsetof(Wire, presenter) == 512);

    class Publisher {
      public:
        Publisher() noexcept = default;
        Publisher(const Publisher&) = delete;
        Publisher& operator=(const Publisher&) = delete;
        ~Publisher() noexcept {
            stop();
        }

#if defined(_WIN64)
        // Session lifecycle owner only, before producer/presenter start.
        // An unavailable status channel never changes runtime operation.
        bool start(const Configuration& configuration) noexcept {
            stop();
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
                return false;
            const auto pid = GetCurrentProcessId();
            const uint64_t creation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            wchar_t name[128]{};
            if (_snwprintf_s(name, _TRUNCATE, L"Local\\VDXR.LiveStatus.v1.%lu.%016llx", pid, creation) < 0)
                return false;
            m_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Wire), name);
            if (!m_section)
                return false;
            const bool reused = GetLastError() == ERROR_ALREADY_EXISTS;
            m_wire = static_cast<Wire*>(MapViewOfFile(m_section, FILE_MAP_WRITE, 0, 0, sizeof(Wire)));
            if (!m_wire || (reused && !validEndedSection(pid, creation))) {
                close();
                return false;
            }
            // A DLL-local counter restarts when OpenXR unloads and reloads
            // this runtime inside the same process. QPC spans both lifetimes.
            LARGE_INTEGER ticks{};
            const uint64_t previous = reused ? m_wire->session.words[5] : 0;
            if (!QueryPerformanceCounter(&ticks) || ticks.QuadPart <= 0 || previous == UINT64_MAX) {
                close();
                return false;
            }
            m_configuration = {};
            m_configuration[0] = Magic;
            m_configuration[1] = 1;
            m_configuration[2] = sizeof(Wire);
            m_configuration[3] = pid;
            m_configuration[4] = creation;
            m_configuration[5] =
                static_cast<uint64_t>(ticks.QuadPart) > previous ? static_cast<uint64_t>(ticks.QuadPart) : previous + 1;
            m_configuration[6] = 1;
            setConfiguration(configuration);

            // Keep session odd across counter initialization, including when a
            // reader retains the previous session's kernel section. Sequences
            // keep advancing, rather than restarting and creating an ABA.
            InterlockedIncrement64(&m_wire->session.sequence);
            publish(m_wire->producer, Producer{}.payload());
            publish(m_wire->presenter, Presenter{}.payload());
            copyPayload(m_wire->session, m_configuration);
            InterlockedIncrement64(&m_wire->session.sequence);
            return true;
        }

        bool available() const noexcept {
            return m_wire != nullptr;
        }
        void publishProducer(const Payload& data) noexcept {
            if (m_wire)
                publish(m_wire->producer, data);
        }
        void publishPresenter(const Payload& data) noexcept {
            if (m_wire)
                publish(m_wire->presenter, data);
        }
        // EndSession can invalidate a session before a final native End returns.
        // The mapped pointer stays valid until the session cleanup owner joins.
        void markEnded() noexcept {
            if (m_wire && m_configuration[6] == 1) {
                m_configuration[6] = 2;
                publish(m_wire->session, m_configuration);
            }
        }
        void stop() noexcept {
            markEnded();
            close();
        }

      private:
        bool validEndedSection(DWORD pid, uint64_t creation) const noexcept {
            const auto& s = m_wire->session;
            return !(s.sequence & 1) && s.sequence != 0 && s.words[0] == Magic && s.words[1] == 1 &&
                   s.words[2] == sizeof(Wire) && s.words[3] == pid && s.words[4] == creation && s.words[6] == 2 &&
                   !(m_wire->producer.sequence & 1) && !(m_wire->presenter.sequence & 1);
        }
        void setConfiguration(const Configuration& configuration) noexcept {
            m_configuration[7] = configuration.fgMode;
            m_configuration[8] = configuration.backend;
            m_configuration[9] = configuration.depthMode;
            m_configuration[10] = configuration.graphicsApi;
            m_configuration[11] = configuration.refreshMilliHz;
        }
        static void copyPayload(Channel& channel, const Payload& data) noexcept {
            for (size_t i = 0; i < data.size(); ++i)
                channel.words[i] = data[i];
        }
        static void publish(Channel& channel, const Payload& data) noexcept {
            InterlockedIncrement64(&channel.sequence);
            copyPayload(channel, data);
            InterlockedIncrement64(&channel.sequence);
        }
        void close() noexcept {
            if (m_wire)
                UnmapViewOfFile(m_wire);
            if (m_section)
                CloseHandle(m_section);
            m_wire = nullptr;
            m_section = nullptr;
        }
        Wire* m_wire{};
        HANDLE m_section{};
        Payload m_configuration{};
#else
        // Retain the existing 32-bit runtime build; live Controls is x64.
        bool start(const Configuration&) noexcept {
            return false;
        }
        bool available() const noexcept {
            return false;
        }
        void publishProducer(const Payload&) noexcept {
        }
        void publishPresenter(const Payload&) noexcept {
        }
        void markEnded() noexcept {
        }
        void stop() noexcept {
        }
#endif
    };
} // namespace virtualdesktop_openxr::live_status
