// Included only by the test friend. The oracle does not call the production phase helper.
inline static int64_t phasePreviousTime{};
inline static unsigned phaseStretched{}, phaseNominal{};

static bool phaseCheckRequest(const VDXRFG_SubmitInfoV3* info) {
    require(info && requestedFrameGeneration == 4, "4X capture fixture requires a real four-slot request");
    const auto& reservation = runtime().m_fgBegunReservation;
    require(info->display_time_ns == reservation.currentTarget, "public B timestamp reaches the module unchanged");
    if (!phasePreviousTime || info->target_time_ns[3] != info->display_time_ns)
        return false; // Real-only prime; it still establishes the next accepted A endpoint.
    const auto interval = info->display_time_ns - phasePreviousTime;
    const bool stretched = interval > 4 * info->display_period_ns + 1000;
    if (!stretched)
        return false;
    require(interval > 0 && interval < 1000000000, "bounded stretched source interval reaches real Submit");
    for (unsigned slot = 0; slot < 3; ++slot) {
        const auto target = reservation.firstTarget + slot * reservation.period;
        const auto offset = target - phasePreviousTime;
        require(info->target_time_ns[slot] == target,
                "actual module Submit receives the original native S1/S2/S3 targets from public EndFrame");
        require(std::abs(static_cast<double>(info->fractions[slot]) - static_cast<double>(offset) / interval) < 1e-7,
                "actual module fraction matches its integer synthesis sample");
    }
    std::cout << "PHASE_CAPTURE_REQUEST serial=" << info->serial << " previous=" << phasePreviousTime
              << " current=" << info->display_time_ns << " interval=" << interval << " S1=" << info->target_time_ns[0]
              << " S2=" << info->target_time_ns[1] << " S3=" << info->target_time_ns[2] << '\n';
    return true;
}

static void phaseAccepted(const VDXRFG_SubmitInfoV3* info, const VDXRFG_TicketV1* ticket, bool stretched) {
    const auto previousTime = phasePreviousTime;
    phasePreviousTime = info->display_time_ns; // The real module accepted B even if a later test assertion fails.
    if (!ticket->prime && ticket->role_mask == 11) {
        require(info->target_time_ns[3] == info->display_time_ns,
                "every accepted nonprime four-slot capture retains the real B endpoint");
        const auto interval = info->display_time_ns - previousTime;
        require(interval > 0, "every accepted nonprime capture advances the source clock");
        const auto& reservation = runtime().m_fgBegunReservation;
        for (unsigned slot = 0; slot < 3; ++slot) {
            require(info->target_time_ns[slot] == reservation.firstTarget + slot * reservation.period,
                    "every accepted synthetic sample uses its original native target");
            const auto offset = info->target_time_ns[slot] - previousTime;
            require(offset > 0 && offset < interval &&
                        std::abs(static_cast<double>(info->fractions[slot]) - static_cast<double>(offset) / interval) <
                            1e-7,
                    "every accepted synthetic fraction matches its sample inside real A/B endpoints");
        }
        if (stretched)
            ++phaseStretched;
        else if (std::abs(interval - 4 * info->display_period_ns) <= 4)
            ++phaseNominal;
    }
}
