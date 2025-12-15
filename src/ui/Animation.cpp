#include "ui/Animation.hpp"
#include <cmath>

namespace ouroboros::ui {

float Animation::progress() const {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - start_time_);

    if (elapsed >= duration_) {
        return 1.0f;
    }

    float linear_progress = static_cast<float>(elapsed.count()) / duration_.count();
    return apply_easing(linear_progress, easing_);
}

float Animation::apply_easing(float t, EasingFunction easing) {
    // Clamp to [0, 1]
    t = std::clamp(t, 0.0f, 1.0f);

    switch (easing) {
        case EasingFunction::Linear:
            return t;

        case EasingFunction::EaseOut:
            // Quadratic deceleration: f(t) = 1 - (1-t)^2
            return 1.0f - std::pow(1.0f - t, 2.0f);

        case EasingFunction::EaseInOut:
            // Smooth S-curve
            if (t < 0.5f) {
                return 2.0f * t * t;
            } else {
                return 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
            }

        case EasingFunction::EaseOutCubic:
            // Cubic deceleration: f(t) = 1 - (1-t)^3
            return 1.0f - std::pow(1.0f - t, 3.0f);

        default:
            return t;
    }
}

}  // namespace ouroboros::ui
