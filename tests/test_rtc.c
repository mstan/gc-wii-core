/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "exi/rtc.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; }       \
    else fprintf(stdout, "PASS: %s\n", msg);                               \
} while (0)

int main(void) {
    GcnRtc rtc = {0};

    CHECK(gcn_rtc_from_unix_local(GCN_RTC_GC_EPOCH_UNIX, 0) == 0u,
          "GC epoch converts to counter zero");
    CHECK(gcn_rtc_from_unix_local(GCN_RTC_GC_EPOCH_UNIX + 3600, -3600) == 0u,
          "local UTC offset is applied exactly once");

    gcn_rtc_set_fixed(&rtc, 1234u);
    CHECK(gcn_rtc_read(&rtc, 10ull * GCN_RTC_CORE_CLOCK_HZ) == 1234u,
          "deterministic fixture remains fixed");

    gcn_rtc_start(&rtc, 100u, 50u);
    CHECK(gcn_rtc_read(&rtc, 50u + GCN_RTC_CORE_CLOCK_HZ - 1u) == 100u,
          "running RTC waits for a complete emulated second");
    CHECK(gcn_rtc_read(&rtc, 50u + GCN_RTC_CORE_CLOCK_HZ) == 101u,
          "running RTC advances after one emulated second");
    CHECK(gcn_rtc_read(&rtc, 50u + 3u * GCN_RTC_CORE_CLOCK_HZ + 17u) == 103u,
          "running RTC advances multiple seconds and retains residue");

    gcn_rtc_write(&rtc, 900u, 50u + 3u * GCN_RTC_CORE_CLOCK_HZ + 17u);
    CHECK(gcn_rtc_read(&rtc, 50u + 4u * GCN_RTC_CORE_CLOCK_HZ + 16u) == 900u,
          "guest write rebases the running RTC");
    CHECK(gcn_rtc_read(&rtc, 50u + 4u * GCN_RTC_CORE_CLOCK_HZ + 17u) == 901u,
          "guest-written RTC continues advancing from emulated time");

    gcn_rtc_start(&rtc, 77u, 1000u);
    CHECK(gcn_rtc_read(&rtc, 999u) == 77u,
          "cycle regression rebases without fabricating elapsed time");

    const u64 clocks[] = { GCN_RTC_CORE_CLOCK_HZ, GCN_RTC_WII_CORE_CLOCK_HZ };
    for (unsigned p = 0; p < 2; ++p) {
        const u64 hz = clocks[p];
        gcn_rtc_start(&rtc, 100u, 17u);
        CHECK(gcn_rtc_read_clock(&rtc, 17u + hz - 1u, hz) == 100u,
              "each clock profile retains a fractional second");
        GcnRtc saved = rtc;
        CHECK(gcn_rtc_read_clock(&rtc, 17u + hz, hz) == 101u,
              "each clock profile advances one real emulated second");
        CHECK(gcn_rtc_read_clock(&rtc, 17u + 3u * hz + 11u, hz) == 103u &&
              rtc.anchor_cycles == 17u + 3u * hz,
              "profile progression preserves subsecond residue");
        CHECK(gcn_rtc_read_clock(&rtc, 17u + 3u * hz + 11u, hz) == 103u,
              "paused/repeated observation does not advance time");
        rtc = saved;
        CHECK(gcn_rtc_read_clock(&rtc, 17u + hz, hz) == 101u,
              "restored counter/anchor/running needs no new snapshot field");
        gcn_rtc_write(&rtc, 900u, 17u + hz + 29u);
        CHECK(gcn_rtc_read_clock(&rtc, 17u + 2u * hz + 28u, hz) == 900u &&
              gcn_rtc_read_clock(&rtc, 17u + 2u * hz + 29u, hz) == 901u,
              "guest write restarts the profile's fractional second");
        gcn_rtc_start(&rtc, 0xffffffffu, 1u);
        CHECK(gcn_rtc_read_clock(&rtc, 1u + 2u * hz, hz) == 1u,
              "both profiles preserve 32-bit hardware counter wrap");
        CHECK(gcn_rtc_read_clock(&rtc, 0u, hz) == 1u && !rtc.anchor_cycles,
              "both profiles rebase a restored older cycle counter");
        gcn_rtc_set_fixed(&rtc, 47u);
        gcn_rtc_write(&rtc, 48u, 100u);
        CHECK(gcn_rtc_read_clock(&rtc, 100u + 100u * hz, hz) == 48u && !rtc.running,
              "profile selection never starts a deterministic fixed fixture");
    }
    gcn_rtc_start(&rtc, 32u, 11u);
    CHECK(gcn_rtc_read_clock(&rtc, UINT64_MAX, 0u) == 32u && rtc.anchor_cycles == 11u,
          "invalid clock refuses without division or state mutation");

    if (failures) {
        fprintf(stderr, "%d RTC test(s) failed\n", failures);
        return 1;
    }
    fprintf(stdout, "all RTC tests passed\n");
    return 0;
}
