#pragma once

/// @file FixedStepClock.hpp
/// Accumulator for running a fixed-timestep simulation under a variable frame
/// rate. Wall-clock time is fed in by the app; the simulation itself only ever
/// sees the fixed step, which keeps it deterministic.

#include <algorithm>
#include <cstdint>

namespace crowdsim::core {

class FixedStepClock {
public:
    /// @param stepHz       simulation rate (e.g. 30 or 60)
    /// @param maxStepsPerAdvance cap on catch-up steps per frame (avoids the spiral of death)
    explicit constexpr FixedStepClock(double stepHz = 30.0, std::uint32_t maxStepsPerAdvance = 8) noexcept
        : step_(1.0 / stepHz), maxSteps_(maxStepsPerAdvance) {}

    /// Feed elapsed wall time (seconds, already scaled by sim speed). Returns how many
    /// fixed steps the caller should run now.
    constexpr std::uint32_t advance(double elapsedSeconds) noexcept {
        accumulator_ += std::max(0.0, elapsedSeconds);
        auto steps = static_cast<std::uint32_t>(accumulator_ / step_);
        if (steps > maxSteps_) {
            steps = maxSteps_;
            accumulator_ = 0.0;  // drop the backlog rather than falling further behind
        } else {
            accumulator_ -= static_cast<double>(steps) * step_;
        }
        return steps;
    }

    /// Interpolation factor in [0, 1) between the previous and latest sim states.
    [[nodiscard]] constexpr float alpha() const noexcept {
        return static_cast<float>(std::clamp(accumulator_ / step_, 0.0, 1.0));
    }

    [[nodiscard]] constexpr double stepSeconds() const noexcept { return step_; }
    constexpr void setStepHz(double hz) noexcept { step_ = 1.0 / hz; }
    constexpr void reset() noexcept { accumulator_ = 0.0; }

private:
    double step_;
    std::uint32_t maxSteps_;
    double accumulator_ = 0.0;
};

}  // namespace crowdsim::core
