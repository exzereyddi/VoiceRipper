#include "Normalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vr {

namespace {
constexpr float kIdealGain = 4000.0f; // same target peak voice_fix.cpp uses
constexpr float kMaxScale = 12.0f;
constexpr size_t kWindowSamples = 2048; // ~43ms at 48kHz, small enough to react, big enough to be stable
}

void NormalizeLoudness(std::vector<int16_t>& samples, int sampleRate) {
    if (samples.empty()) return;

    float scale = 1.0f;

    for (size_t winStart = 0; winStart < samples.size(); winStart += kWindowSamples) {
        size_t winEnd = std::min(winStart + kWindowSamples, samples.size());
        size_t n = winEnd - winStart;

        int maxgain = 0;
        for (size_t i = winStart; i < winEnd; i++) {
            int a = std::abs(static_cast<int>(samples[i]));
            if (a > maxgain) maxgain = a;
        }

        if (maxgain == 0) {
            // Silence: leave scale as-is (same behaviour as the live
            // version, which just returns early and keeps prevscale).
            continue;
        }

        float targetScale = kIdealGain / static_cast<float>(maxgain);
        if (targetScale > kMaxScale) targetScale = scale;

        float increment = (targetScale - scale) / static_cast<float>(n);

        // When boosting quiet talkers back towards normal volume (not
        // attenuating), ramp more gently relative to sample rate, same as
        // voice_fix.cpp's `increment *= nSamples / (48000*2)` fudge factor
        // for the "already loud enough" case.
        if (targetScale >= 1.0f) {
            increment *= static_cast<float>(n) / (static_cast<float>(sampleRate) * 2.0f);
        }

        for (size_t i = winStart; i < winEnd; i++) {
            int sample = static_cast<int>(samples[i] * scale);
            if (sample > 32767) sample = 32767;
            else if (sample < -32768) sample = -32768;
            samples[i] = static_cast<int16_t>(sample);
            scale += increment;
        }
    }
}

void ApplyGain(std::vector<int16_t>& samples, float scale) {
    if (scale > 0.99f && scale < 1.01f) return;
    for (auto& s : samples) {
        int v = static_cast<int>(s * scale);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s = static_cast<int16_t>(v);
    }
}

} // namespace vr
