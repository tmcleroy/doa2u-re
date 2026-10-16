/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mix_model.h"
#include <math.h>
#include <string.h>

void recomp_dsound_mix_default(RecompDsoundMixModel *model)
{
    *model = (RecompDsoundMixModel){.count = 2u, .bins = {0u, 1u}};
}

bool recomp_dsound_mix_set(RecompDsoundMixModel *model, const RecompDsoundMixPair *pairs,
    uint32_t count, bool replace)
{
    if (!model || count > 8u || (count && !pairs)) return false;
    RecompDsoundMixModel next = *model;
    if (replace) next.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (pairs[i].bin >= 32u || pairs[i].volume < -10000 || pairs[i].volume > 0) return false;
        for (uint32_t j = 0; j < i; ++j) if (pairs[j].bin == pairs[i].bin) return false;
        if (replace) next.bins[i] = pairs[i].bin;
        next.volumes[pairs[i].bin] = pairs[i].volume;
    }
    *model = next;
    return true;
}

bool recomp_dsound_mix_matrices(const RecompDsoundMixModel *model, uint32_t channels,
    const uint32_t headroom[32], float dry[4], float wet[4])
{
    if (!model || model->count > 8u || (channels != 1u && channels != 2u)) return false;
    memset(dry, 0, 4u * sizeof *dry);
    memset(wet, 0, 4u * sizeof *wet);
    for (uint32_t i = 0; i < model->count; ++i) {
        uint32_t bin = model->bins[i];
        if (bin >= 32u) return false;
        /* A fully attenuated send is silent wherever it is routed. */
        if (model->volumes[bin] == -10000) continue;
        /* Crosstalk and FX sends need their own native signal services. */
        if (bin > 5u && bin != 10u) return false;
        float gain = powf(10.0f, model->volumes[bin] / 2000.0f);
        gain = ldexpf(gain, -(int)(headroom[bin] & 7u));
        if (bin == 10u) {
            wet[0] += gain;
            wet[channels == 1u ? 1u : 3u] += gain;
        } else if (bin <= 1u) dry[bin * channels + (channels == 2u ? bin : 0u)] += gain;
        else if (bin != 3u) {
            /* shortcut: fixed -3 dB stereo fold of center/rear with LFE dropped;
               add speaker-config-aware routing when a surround host output exists. */
            uint32_t in = i % channels; /* hardware feeds stereo channels to alternating slots */
            if (bin != 5u) dry[in] += gain * 0.70710678f;
            if (bin != 4u) dry[channels + in] += gain * 0.70710678f;
        }
    }
    return true;
}
