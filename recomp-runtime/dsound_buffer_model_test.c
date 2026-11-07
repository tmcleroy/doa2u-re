#include "dsound_buffer_model.h"

#include <stddef.h>
#include <stdio.h>

static int expect_u32(
    const char *field,
    uint32_t actual,
    uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "DirectSound buffer model: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

int main(void)
{
    int passed = 1;
    {
        RecompDsoundBufferModel clock;
        recomp_dsound_buffer_configure(&clock, 65536u, 44100u, 4u, 0u);
        recomp_dsound_buffer_play(&clock, 1u, 0u);
        for (uint64_t ms = 1u; ms <= 1000u; ++ms) {
            recomp_dsound_buffer_cursor(&clock, ms);
        }
        passed &= expect_u32("fractional PCM ring cursor", clock.cursor_bytes, 45328u);
        recomp_dsound_buffer_stop(&clock, 1000u);
        recomp_dsound_buffer_play(&clock, 1u, 10000u);
        passed &= expect_u32("paused time excluded",
            recomp_dsound_buffer_cursor(&clock, 10100u), 62968u);
    }

    {
        RecompDsoundBufferModel output;
        uint32_t offset = 0u;
        recomp_dsound_buffer_configure(&output, 400u, 1000u, 4u, 0u);
        recomp_dsound_buffer_play(&output, 1u, 0u);
        passed &= expect_u32("PCM consumed bytes",
            recomp_dsound_buffer_consume(&output, 90u, &offset), 360u);
        passed &= expect_u32("PCM initial offset", offset, 0u);
        passed &= expect_u32("PCM wrap bytes",
            recomp_dsound_buffer_consume(&output, 110u, &offset), 80u);
        passed &= expect_u32("PCM wrap offset", offset, 360u);
        passed &= expect_u32("PCM wrapped cursor", output.cursor_bytes, 40u);
        passed &= expect_u32("PCM stalled output discarded",
            recomp_dsound_buffer_consume(&output, 500u, &offset), 0u);
        /* A stall holds the voice: the cursor stays where it was. */
        passed &= expect_u32("PCM stalled cursor held", output.cursor_bytes, 40u);
        recomp_dsound_buffer_play(&output, 2u, 500u);
        recomp_dsound_buffer_consume(&output, 590u, &offset);
        passed &= expect_u32("PCM one-shot tail",
            recomp_dsound_buffer_consume(&output, 610u, &offset), 40u);
        passed &= expect_u32("PCM tail offset", offset, 360u);
        passed &= expect_u32("PCM one-shot stops", output.playing, 0u);
        passed &= expect_u32("PCM stopped output",
            recomp_dsound_buffer_consume(&output, 620u, &offset), 0u);
        recomp_dsound_buffer_configure(&output, 176400u, 44100u, 4u, 0u);
        recomp_dsound_buffer_play(&output, 1u, 0u);
        uint32_t bytes = 0u;
        for (uint64_t ms = 1u; ms <= 1000u; ++ms) {
            bytes += recomp_dsound_buffer_consume(&output, ms, &offset);
        }
        passed &= expect_u32("PCM fractional output", bytes, 176400u);

        /* A short tail can wrap several times in one <=100 ms submission. */
        recomp_dsound_buffer_configure(&output, 80u, 1000u, 2u, 0u);
        output.loop_start_bytes = 48u;
        recomp_dsound_buffer_play(&output, RECOMP_DSOUND_PLAY_LOOPING, 0u);
        passed &= expect_u32("tail intro bytes",
            recomp_dsound_buffer_consume(&output, 30u, &offset), 60u);
        passed &= expect_u32("tail intro offset", offset, 0u);
        passed &= expect_u32("tail multiple wrap bytes",
            recomp_dsound_buffer_consume(&output, 130u, &offset), 200u);
        passed &= expect_u32("tail multiple wrap offset", offset, 60u);
        passed &= expect_u32("tail multiple wrap cursor", output.cursor_bytes, 68u);
        recomp_dsound_buffer_play(&output, RECOMP_DSOUND_PLAY_FROMSTART, 130u);
        passed &= expect_u32("tail one-shot intro",
            recomp_dsound_buffer_consume(&output, 160u, &offset), 60u);
        passed &= expect_u32("tail one-shot final bytes",
            recomp_dsound_buffer_consume(&output, 190u, &offset), 20u);
        passed &= expect_u32("tail one-shot stops", output.playing, 0u);

        recomp_dsound_buffer_configure(&output, 80u, 11025u, 2u, 0u);
        output.loop_start_bytes = 48u;
        recomp_dsound_buffer_play(&output, RECOMP_DSOUND_PLAY_LOOPING, 0u);
        bytes = 0u;
        for (uint64_t ms = 1u; ms <= 1000u; ++ms)
            bytes += recomp_dsound_buffer_consume(&output, ms, &offset);
        passed &= expect_u32("tail fractional output", bytes, 22050u);
        passed &= expect_u32("tail fractional cursor", output.cursor_bytes, 66u);
        passed &= expect_u32("tail next fractional cursor",
            recomp_dsound_buffer_cursor(&output, 1001u), 56u);
        passed &= expect_u32("tail fractional remainder", output.frame_remainder, 25u);
        recomp_dsound_buffer_configure(&output, 80u, 11025u, 2u, 0u);
        output.loop_start_bytes = 48u;
        recomp_dsound_buffer_play(&output, RECOMP_DSOUND_PLAY_LOOPING, 0u);
        passed &= expect_u32("tail huge elapsed is a stall",
            recomp_dsound_buffer_cursor(&output, UINT64_MAX), 0u);
        passed &= expect_u32("tail huge elapsed remainder", output.frame_remainder, 0u);
        /* After a stall the ring plays on from where it stopped, not from where the clock went. */
        recomp_dsound_buffer_configure(&output, 400u, 1000u, 4u, 0u);
        recomp_dsound_buffer_play(&output, RECOMP_DSOUND_PLAY_LOOPING, 0u);
        passed &= expect_u32("ring before a stall",
            recomp_dsound_buffer_consume(&output, 50u, &offset), 200u);
        passed &= expect_u32("ring stall plays nothing",
            recomp_dsound_buffer_consume(&output, 1050u, &offset), 0u);
        passed &= expect_u32("ring stall holds the cursor", output.cursor_bytes, 200u);
        passed &= expect_u32("ring after a stall bytes",
            recomp_dsound_buffer_consume(&output, 1060u, &offset), 40u);
        passed &= expect_u32("ring after a stall offset", offset, 200u);
    }

    {
        RecompDsoundBufferModel clock = {0};
        passed &= expect_u32("null configure", recomp_dsound_buffer_configure(
            NULL, 80u, 1000u, 2u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("zero size", recomp_dsound_buffer_configure(
            &clock, 0u, 1000u, 2u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("zero rate", recomp_dsound_buffer_configure(
            &clock, 80u, 0u, 2u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("zero align", recomp_dsound_buffer_configure(
            &clock, 80u, 1000u, 0u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("partial frame", recomp_dsound_buffer_configure(
            &clock, 81u, 1000u, 2u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("unconfigured play", recomp_dsound_buffer_play(
            &clock, 0u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        recomp_dsound_buffer_configure(&clock, 80u, 1000u, 2u, 0u);
        passed &= expect_u32("unknown play flag", recomp_dsound_buffer_play(
            &clock, 4u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("seek past end", recomp_dsound_buffer_set_position(
            &clock, 80u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        passed &= expect_u32("unaligned seek", recomp_dsound_buffer_set_position(
            &clock, 1u, 0u), RECOMP_DSOUND_INVALID_PARAM);
        clock.loop_start_bytes = 20u;
        clock.loop_end_bytes = 60u;
        recomp_dsound_buffer_play(&clock, RECOMP_DSOUND_PLAY_LOOPING, 0u);
        uint32_t offset = 0u;
        passed &= expect_u32("bounded loop consumed", recomp_dsound_buffer_consume(
            &clock, 50u, &offset), 100u);
        passed &= expect_u32("bounded loop offset", offset, 0u);
        passed &= expect_u32("bounded loop cursor", clock.cursor_bytes, 20u);
        passed &= expect_u32("reverse clock", recomp_dsound_buffer_cursor(&clock, 49u), 20u);
        passed &= expect_u32("null consume offset", recomp_dsound_buffer_consume(
            &clock, 51u, NULL), 0u);
        passed &= expect_u32("rate change", recomp_dsound_buffer_set_frequency(
            &clock, 2000u, 50u), RECOMP_DSOUND_OK);
        passed &= expect_u32("rate cursor", recomp_dsound_buffer_cursor(&clock, 55u), 40u);
        passed &= expect_u32("restore rate", recomp_dsound_buffer_set_frequency(
            &clock, 0u, 55u), RECOMP_DSOUND_OK);
        passed &= expect_u32("restored rate", clock.sample_rate, 1000u);
        passed &= expect_u32("seek", recomp_dsound_buffer_set_position(
            &clock, 50u, 55u), RECOMP_DSOUND_OK);
        passed &= expect_u32("seek wrapped", recomp_dsound_buffer_cursor(&clock, 60u), 20u);
    }
    if (passed) puts("PASS DirectSound buffer clocks, loops, consume, seek, frequency and validation");
    return passed ? 0 : 1;
}
