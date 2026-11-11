/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host/host_audio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } \
    else { fprintf(stdout, "PASS: %s\n", msg); } \
} while (0)

int main(void) {
    const u8 be_pcm[] = {
        0x12, 0x34, 0xFE, 0xDC, /* R=0x1234, L=-292 */
        0x80, 0x00, 0x7F, 0xFF, /* R/L extrema */
    };
    const u8 expected_le[] = {
        0xDC, 0xFE, 0x34, 0x12,
        0xFF, 0x7F, 0x00, 0x80,
    };
    u8 out[sizeof be_pcm];
    memset(out, 0, sizeof out);

    CHECK(gcn_host_audio_convert_be_s16(out, be_pcm, sizeof be_pcm) == sizeof be_pcm,
          "conversion reports every complete s16 byte");
    CHECK(memcmp(out, expected_le, sizeof out) == 0,
          "GameCube big-endian R/L becomes native little-endian L/R PCM");
    CHECK(gcn_host_audio_convert_be_s16(out, be_pcm, 7u) == 4u,
          "conversion truncates an incomplete final stereo frame safely");
    CHECK(gcn_host_audio_convert_be_s16(NULL, be_pcm, sizeof be_pcm) == 0u,
          "conversion rejects a null destination");

#ifdef _WIN32
    _putenv_s("GCN_AUDIO", "0");
    _putenv_s("GCN_AUDIO_CAPTURE", "");
#else
    setenv("GCN_AUDIO", "0", 1);
    unsetenv("GCN_AUDIO_CAPTURE");
#endif
    gcn_host_audio_start(32000);
    gcn_host_audio_submit_be_s16(NULL, be_pcm, sizeof be_pcm);
    GcnHostAudioStats complete;
    gcn_host_audio_get_stats(&complete);
    CHECK(complete.emulated_frames == 2 && complete.pcm_hash == 7160345683946154871ull,
          "PCM fingerprint covers actual ordered samples with host output disabled");
    gcn_host_audio_stop();
    gcn_host_audio_start(32000);
    gcn_host_audio_submit_be_s16(NULL, be_pcm, 4);
    gcn_host_audio_submit_be_s16(NULL, be_pcm + 4, 4);
    GcnHostAudioStats split;
    gcn_host_audio_get_stats(&split);
    CHECK(split.pcm_hash == complete.pcm_hash && split.emulated_frames == complete.emulated_frames,
          "PCM fingerprint is independent of submission boundaries");
    gcn_host_audio_stop();

    if (failures) {
        fprintf(stderr, "%d host-audio test(s) failed\n", failures);
        return 1;
    }
    fprintf(stdout, "all host-audio tests passed\n");
    return 0;
}
