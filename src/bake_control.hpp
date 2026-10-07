#pragma once

#include "technique_catalog.hpp"

#include <chrono>
#include <cstdint>

namespace lut_baker
{
// ReShade's render_technique can enqueue compilation but does not return its
// outcome. A missing render event is NOT a reason to call it every frame: a
// failed permutation can be enqueued again and again, disturbing gameplay
// effects too. Wait for reshade_reloaded_effects, then verify once per key.
// This policy contains no ReShade/GPU calls and is exercised with a fake clock.
class bake_control
{
public:
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds timeout { 60 };

    void start(const clock::time_point now)
    {
        started_ = now;
        active_ = true;
        waiting_ = false;
        initializing_ = false;
        waiting_key_ = {};
        attempted_initialization_.clear();
        attempted_missing_.clear();
    }

    void stop() noexcept { active_ = false; }

    [[nodiscard]] bool timed_out(const clock::time_point now) const noexcept
    {
        return active_ && now - started_ >= timeout;
    }

    [[nodiscard]] bool can_attempt(const std::uint64_t reload_generation) const noexcept
    {
        return active_ && (!waiting_ || reload_generation > waiting_generation_);
    }

    void begin_attempt() noexcept { waiting_ = false; }

    // Returns false if this same exact technique still did not render after
    // its compilation cycle completed. Never guess from an effect name alone.
    [[nodiscard]] bool wait_for_compilation(const technique_key &key, const std::uint64_t reload_generation)
    {
        if (!active_ || selection_contains_exact(attempted_missing_, key))
            return false;
        attempted_missing_.insert(key);
        waiting_key_ = key;
        waiting_generation_ = reload_generation;
        waiting_ = true;
        initializing_ = false;
        return true;
    }

    // Default resources and the offscreen permutation are separate stages.
    // Each exact technique may wait once in each, under the same deadline.
    [[nodiscard]] bool wait_for_initialization(const technique_key &key, const std::uint64_t reload_generation)
    {
        if (!active_ || selection_contains_exact(attempted_initialization_, key))
            return false;
        attempted_initialization_.insert(key);
        waiting_key_ = key;
        waiting_generation_ = reload_generation;
        waiting_ = true;
        initializing_ = true;
        return true;
    }

    [[nodiscard]] bool waiting() const noexcept { return waiting_; }
    [[nodiscard]] bool initializing() const noexcept { return waiting_ && initializing_; }
    [[nodiscard]] const technique_key &waiting_key() const noexcept { return waiting_key_; }

private:
    clock::time_point started_ {};
    bool active_ = false;
    bool waiting_ = false;
    bool initializing_ = false;
    std::uint64_t waiting_generation_ = 0;
    technique_key waiting_key_;
    technique_selection attempted_initialization_;
    technique_selection attempted_missing_;
};

// Cancellation only precedes file serialization. Submitted GPU work must still
// drain through its fence; stopping the request never makes its resources idle.
[[nodiscard]] inline bool can_abort_export(const bool export_pending, const bool writer_pending) noexcept
{
    return export_pending && !writer_pending;
}
}
