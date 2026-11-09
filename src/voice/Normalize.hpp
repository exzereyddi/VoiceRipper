#pragma once
#include <cstdint>
#include <vector>

namespace vr {

// Loudness normalization ported from the live-capture AGC logic in
// voice_fix.cpp's NormalizeClientVoice (same idea Valve's engine voice
// pipeline uses): track a running target gain so quiet talkers get
// boosted towards a fixed peak level without over-amplifying already
// loud segments, with the gain ramped smoothly across each processing
// window to avoid clicks.
void NormalizeLoudness(std::vector<int16_t>& samples, int sampleRate);

// Straight linear gain, clamped to int16 range. Equivalent to voice_fix's
// ApplyScale, usable as a manual post-normalize adjustment.
void ApplyGain(std::vector<int16_t>& samples, float scale);

} // namespace vr
