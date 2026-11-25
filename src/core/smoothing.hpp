#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hyprpad {

// Low-pass for pointer motion, built for cheap touchpads whose coordinates
// jitter at light pressure. It filters the position, not the delta, and keeps
// the residual: steady motion comes out 1:1 and the total travel is never
// shortened, so it does not feel like a sensitivity change. Only the
// frame-to-frame wobble is attenuated.
//
// A 3-frame median runs first, because the pad's worst artifact is a single
// frame spike (it also loses and re-acquires the contact, which libinput turns
// into one big jump). Without it a spike looks like fast motion and would drag
// the smoothed position with it.
struct MotionSmoother {
    float    alpha  = 0.32F; // EMA weight, 1 = passthrough
    uint32_t gapMs  = 150;   // silence that means a new touch

    void setStrength(float strength) {
        alpha = 1.0F - std::clamp(strength, 0.0F, 1.0F) * 0.85F;
        reset();
    }

    void reset() {
        m_rawX = m_rawY = m_smoothX = m_smoothY = m_sentX = m_sentY = 0.0F;
        m_prevX = m_prevY = 0.0F;
        m_prev2X = m_prev2Y = 0.0F;
        m_lastMs  = 0;
        m_started = false;
    }

    void apply(float dx, float dy, uint32_t timeMs, float& outX, float& outY) {
        // A new touch must not inherit the residual of the previous one.
        if (!m_started || timeMs - m_lastMs > gapMs)
            reset();
        m_started = true;
        m_lastMs  = timeMs;

        const float filteredX = median3(m_prev2X, m_prevX, dx);
        const float filteredY = median3(m_prev2Y, m_prevY, dy);
        m_prev2X = m_prevX;
        m_prev2Y = m_prevY;
        m_prevX  = dx;
        m_prevY  = dy;

        m_rawX += filteredX;
        m_rawY += filteredY;
        m_smoothX += (m_rawX - m_smoothX) * alpha;
        m_smoothY += (m_rawY - m_smoothY) * alpha;

        outX    = m_smoothX - m_sentX;
        outY    = m_smoothY - m_sentY;
        m_sentX = m_smoothX;
        m_sentY = m_smoothY;
    }

  private:
    static float median3(float a, float b, float c) {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    float    m_rawX = 0, m_rawY = 0;
    float    m_smoothX = 0, m_smoothY = 0;
    float    m_sentX = 0, m_sentY = 0;
    float    m_prevX = 0, m_prevY = 0, m_prev2X = 0, m_prev2Y = 0;
    uint32_t m_lastMs  = 0;
    bool     m_started = false;
};

} // namespace hyprpad
