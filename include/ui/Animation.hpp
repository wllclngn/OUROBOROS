#pragma once

#include "ui/LayoutConstraints.hpp"
#include <chrono>
#include <algorithm>

namespace ouroboros::ui {

/**
 * Easing functions for smooth animations
 */
enum class EasingFunction {
    Linear,         // No easing, constant speed
    EaseOut,        // Starts fast, ends slow (deceleration)
    EaseInOut,      // Slow start and end, fast middle
    EaseOutCubic    // Smooth deceleration curve
};

/**
 * Time-based animation with easing
 */
class Animation {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::milliseconds;

    /**
     * Create animation with specified duration and easing
     */
    Animation(Duration duration, EasingFunction easing = EasingFunction::EaseOut)
        : duration_(duration), easing_(easing) {
        start_time_ = Clock::now();
    }

    /**
     * Get current progress [0.0, 1.0] with easing applied
     * Returns 1.0 when animation is complete
     */
    float progress() const;

    /**
     * Check if animation is complete
     */
    bool is_complete() const {
        return progress() >= 1.0f;
    }

    /**
     * Reset animation to start
     */
    void reset() {
        start_time_ = Clock::now();
    }

    /**
     * Apply easing function to linear progress
     */
    static float apply_easing(float t, EasingFunction easing);

private:
    TimePoint start_time_;
    Duration duration_;
    EasingFunction easing_;
};

/**
 * Animates a layout rectangle from start to target position/size
 */
struct RectAnimation {
    LayoutRect start;
    LayoutRect target;
    Animation animation;

    RectAnimation(const LayoutRect& from, const LayoutRect& to, Animation::Duration duration)
        : start(from), target(to), animation(duration, EasingFunction::EaseOut) {}

    /**
     * Get current interpolated rectangle based on animation progress
     */
    LayoutRect current() const {
        float t = animation.progress();
        LayoutRect result;
        result.x = lerp(start.x, target.x, t);
        result.y = lerp(start.y, target.y, t);
        result.width = lerp(start.width, target.width, t);
        result.height = lerp(start.height, target.height, t);
        return result;
    }

private:
    static int lerp(int a, int b, float t) {
        return static_cast<int>(a + (b - a) * t);
    }
};

}  // namespace ouroboros::ui
