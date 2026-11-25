#include "../virtualdesktop-openxr/NrBeforeSrHistoryBank.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace virtualdesktop_openxr::nr_before_sr;

namespace {
    void require(bool condition, const char* reason) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", reason);
            std::exit(1);
        }
    }

    void
    expect(const StereoHistoryBanks& banks, std::uint64_t epoch, std::size_t bank, bool reset, const char* reason) {
        const auto selected = SelectStereoHistoryBank(epoch, banks);
        require(selected && selected->bank == bank && selected->reset == reset, reason);
    }
} // namespace

int main() {
    StereoHistoryBanks banks{};
    require(!SelectStereoHistoryBank(9, banks), "no available bank passes the pair through");
    banks[0][0] = banks[1][1] = {true, 8};
    require(!SelectStereoHistoryBank(9, banks), "two ready eyes in different banks cannot be combined");

    banks[0][1] = {true, 6};
    expect(banks, 9, 0, true, "unequal prior eye epochs reset both eyes");
    banks[1][0] = {true, 8};
    expect(banks, 9, 1, false, "bank one has immediate stereo history and outranks ready bank zero");
    banks[0][1] = {true, 8};
    expect(banks, 9, 0, false, "ties between continuous banks choose the first");
    require(!SelectStereoHistoryBank(0, banks), "closed epoch does not select a bank");

    banks[0][0].ready = false;
    banks[1][0].lastSuccessfulEpoch = 0;
    expect(banks, 9, 1, true, "one abandoned prior eye resets the available pair");
    banks[1][0].ready = false;
    require(!SelectStereoHistoryBank(9, banks), "partially available banks cannot authorize either eye");
    banks[0][0] = banks[0][1] = banks[1][0] = banks[1][1] = {true, 7};
    expect(banks, 9, 0, true, "a skipped epoch requires reset in the first common ready bank");
    banks[0][0].lastSuccessfulEpoch = banks[0][1].lastSuccessfulEpoch = 0;
    expect(banks, 1, 0, true, "fresh epoch one cannot continue the zero sentinel");

    constexpr auto last = (std::numeric_limits<std::uint64_t>::max)();
    banks[1][0] = banks[1][1] = {true, last - 1};
    expect(banks, last, 1, false, "final epoch can follow its immediate predecessor");
    banks[1][0] = banks[1][1] = {true, last};
    require(!SelectStereoHistoryBank(0, banks), "last successful epoch cannot wrap to zero");
    expect(banks, 1, 0, true, "last successful epoch cannot wrap into a fresh history");

    // Three live application allocators can hold two previous stereo recordings.
    banks = {};
    banks.back()[0] = banks.back()[1] = {true, 0};
    expect(banks, 12, 2, true, "a third bank can take this pair while the first two remain in flight");
    banks.back()[1].ready = false;
    require(!SelectStereoHistoryBank(13, banks), "a busy eye in the last bank must still bypass the whole pair");
    std::puts("PASS: before-SR stereo history bank selection");
}
