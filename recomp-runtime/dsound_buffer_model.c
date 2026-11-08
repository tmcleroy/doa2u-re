#include "dsound_buffer_model.h"

#include <stddef.h>

/* Longer than this between two looks at a voice is a host stall, not playing time. */
#define RECOMP_DSOUND_STALL_MS 100u

static int buffer_configured(const RecompDsoundBufferModel *model)
{
    return model != NULL && model->size_bytes != 0u &&
        model->sample_rate != 0u && model->block_align != 0u &&
        model->size_bytes % model->block_align == 0u &&
        model->loop_start_bytes < model->loop_end_bytes &&
        model->loop_end_bytes <= model->size_bytes &&
        model->loop_end_bytes % model->block_align == 0u &&
        model->loop_start_bytes % model->block_align == 0u;
}

uint32_t recomp_dsound_buffer_configure(
    RecompDsoundBufferModel *model, uint32_t size_bytes,
    uint32_t sample_rate, uint32_t block_align, uint64_t now_ms)
{
    if (model == NULL || size_bytes == 0u || sample_rate == 0u ||
        block_align == 0u || size_bytes % block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    *model = (RecompDsoundBufferModel){
        .size_bytes = size_bytes,
        .sample_rate = sample_rate,
        .original_sample_rate = sample_rate,
        .block_align = block_align,
        .last_ms = now_ms,
        .loop_end_bytes = size_bytes,
    };
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_cursor(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return 0u;
    }
    if ((model->play_flags & RECOMP_DSOUND_PLAY_LOOPING) &&
        model->cursor_bytes >= model->loop_end_bytes) {
        model->cursor_bytes = model->loop_start_bytes +
            (model->cursor_bytes - model->loop_end_bytes) %
            (model->loop_end_bytes - model->loop_start_bytes);
    }
    if (!model->playing || now_ms <= model->last_ms) {
        return model->cursor_bytes;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    /* A host stall (the game thread held past a frame: a compositor or overlay keeping the window's
       picture, a slow disk) stops the voice where it is. On the Xbox the threads that refill a
       streaming ring run on through such a time; here they cannot, so moving the cursor on would
       play the ring's old contents (music repeating a phrase) until they catch up. */
    if (elapsed > RECOMP_DSOUND_STALL_MS) {
        model->last_ms = now_ms;
        return model->cursor_bytes;
    }
    uint64_t seconds = elapsed / 1000u;
    uint64_t fraction =
        (elapsed % 1000u) * model->sample_rate + model->frame_remainder;
    uint64_t frames = ((model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)
        ? model->loop_end_bytes : model->size_bytes) / model->block_align;
    uint64_t position = model->cursor_bytes / model->block_align;
    model->last_ms = now_ms;
    model->frame_remainder = (uint32_t)(fraction % 1000u);
    uint64_t remaining = frames - position;
    if (seconds <= remaining / model->sample_rate &&
        seconds * model->sample_rate + fraction / 1000u < remaining) {
        model->cursor_bytes = (uint32_t)((position + seconds * model->sample_rate +
            fraction / 1000u) * model->block_align);
        return model->cursor_bytes;
    }
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        model->playing = 0u;
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
        return 0u;
    }
    uint64_t loop_start = model->loop_start_bytes / model->block_align;
    uint64_t loop_frames = frames - loop_start;
    /* Subtract the first passage to the end before wrapping within the tail.
       Reduce before multiplying so even a uint64_t elapsed time cannot overflow. */
    uint64_t advance =
        ((seconds % loop_frames) * model->sample_rate) % loop_frames + fraction / 1000u;
    model->cursor_bytes = (uint32_t)((loop_start +
        (advance + loop_frames - remaining % loop_frames) % loop_frames) *
        model->block_align);
    return model->cursor_bytes;
}

uint32_t recomp_dsound_buffer_consume(
    RecompDsoundBufferModel *model, uint64_t now_ms, uint32_t *offset_bytes)
{
    if (!buffer_configured(model) || offset_bytes == NULL ||
        !model->playing || now_ms <= model->last_ms) {
        return 0u;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    uint32_t position = recomp_dsound_buffer_cursor(model, model->last_ms);
    uint32_t remainder = model->frame_remainder;
    recomp_dsound_buffer_cursor(model, now_ms);
    /* (a stall: the cursor stayed where it was, so nothing was played) */
    if (elapsed > RECOMP_DSOUND_STALL_MS) {
        return 0u;
    }
    uint64_t frames = (elapsed * model->sample_rate + remainder) / 1000u;
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        uint32_t remaining = (model->size_bytes - position) / model->block_align;
        if (frames > remaining) {
            frames = remaining;
        }
    }
    if (frames > UINT32_MAX / model->block_align) {
        return 0u;
    }
    uint32_t consumed_bytes = (uint32_t)frames * model->block_align;
    /* A full-buffer looping ring that is fed incrementally via partial Lock()
       writes (like CRI RNA's 65,536-byte streaming ring buffer) must not wrap
       around and replay stale PCM if the feeder underruns or is waiting to stop
       the voice. Static looping sounds (never partially locked multiple times)
       keep looping indefinitely. */
    if ((model->play_flags & RECOMP_DSOUND_PLAY_LOOPING) &&
        model->stream_locks >= 2u) {
        if (consumed_bytes > model->stream_remaining_bytes) {
            consumed_bytes = model->stream_remaining_bytes;
            model->stream_remaining_bytes = 0u;
        } else {
            model->stream_remaining_bytes -= consumed_bytes;
        }
    }
    *offset_bytes = position;
    return consumed_bytes;
}

void recomp_dsound_buffer_lock(
    RecompDsoundBufferModel *model, uint32_t lock_bytes)
{
    if (!buffer_configured(model) || lock_bytes == 0u) {
        return;
    }
    if (lock_bytes < model->size_bytes &&
        model->loop_start_bytes == 0u &&
        model->loop_end_bytes == model->size_bytes) {
        if (model->stream_locks < 2u) {
            ++model->stream_locks;
        }
        if (model->stream_locks == 1u) {
            model->stream_remaining_bytes = model->size_bytes;
        } else if (lock_bytes >= model->size_bytes - model->stream_remaining_bytes) {
            model->stream_remaining_bytes = model->size_bytes;
        } else {
            model->stream_remaining_bytes += lock_bytes;
        }
    } else {
        model->stream_locks = 0u;
        model->stream_remaining_bytes = 0u;
    }
}

uint32_t recomp_dsound_buffer_play(
    RecompDsoundBufferModel *model, uint32_t flags, uint64_t now_ms)
{
    if (!buffer_configured(model) ||
        (flags & ~(RECOMP_DSOUND_PLAY_LOOPING | RECOMP_DSOUND_PLAY_FROMSTART))) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    if (!model->playing || (flags & RECOMP_DSOUND_PLAY_FROMSTART)) {
        model->stream_locks = 0u;
        model->stream_remaining_bytes = 0u;
    }
    if (flags & RECOMP_DSOUND_PLAY_FROMSTART) {
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
    }
    if ((flags & RECOMP_DSOUND_PLAY_LOOPING) &&
        model->cursor_bytes >= model->loop_end_bytes) {
        model->cursor_bytes = model->loop_start_bytes;
    }
    model->play_flags = flags;
    model->playing = 1u;
    if (now_ms > model->last_ms) {
        model->last_ms = now_ms;
    }
    return RECOMP_DSOUND_OK;
}

void recomp_dsound_buffer_stop(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (model != NULL) {
        recomp_dsound_buffer_cursor(model, now_ms);
        model->playing = 0u;
        model->stream_locks = 0u;
        model->stream_remaining_bytes = 0u;
    }
}

uint32_t recomp_dsound_buffer_set_position(
    RecompDsoundBufferModel *model, uint32_t position_bytes, uint64_t now_ms)
{
    if (!buffer_configured(model) || position_bytes >= model->size_bytes ||
        position_bytes % model->block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->cursor_bytes = position_bytes;
    model->frame_remainder = 0u;
    model->stream_locks = 0u;
    model->stream_remaining_bytes = 0u;
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_set_frequency(
    RecompDsoundBufferModel *model, uint32_t sample_rate, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    if (sample_rate == 0u) {
        sample_rate = model->original_sample_rate;
    }
    if (sample_rate == 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->sample_rate = sample_rate;
    return RECOMP_DSOUND_OK;
}
