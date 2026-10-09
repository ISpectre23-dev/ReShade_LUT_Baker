#pragma once

#include "technique_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lut_baker
{
enum class uniform_value_type { boolean, sint, uint, real, sint16, uint16, real16 };
enum class settings_result { ready, waiting, error };

// Store values from the public API, without constant-buffer padding. Keep
// float values as raw words to preserve signed zero and NaN bit patterns.
struct uniform_setting
{
    std::string effect;
    std::string name;
    uniform_value_type type = uniform_value_type::real;
    std::uint32_t rows = 0;
    std::uint32_t columns = 0;
    std::uint32_t array_length = 0; // ReShade reports zero for non-arrays.
    std::vector<std::uint32_t> words;
};

struct technique_setting
{
    technique_key key;
    bool enabled = false;
};

struct runtime_settings
{
    std::string preset;
    std::vector<uniform_setting> uniforms;
    std::vector<technique_setting> techniques; // Actual runtime order.
};

constexpr std::size_t settings_word_limit = 16u * 1024u * 1024u; // 64 MiB.
[[nodiscard]] std::size_t uniform_component_count(const uniform_setting &uniform) noexcept;
[[nodiscard]] bool validate_runtime_settings(const runtime_settings &settings, std::string &error);

struct settings_restore_plan
{
    // Indices refer to a fresh enumeration, never handles saved before a reload.
    std::vector<std::pair<std::size_t, std::size_t>> uniforms; // Saved, current.
    std::vector<std::pair<std::size_t, bool>> techniques;
    std::vector<std::size_t> order;
    bool reorder = false;
    [[nodiscard]] bool empty() const noexcept { return uniforms.empty() && techniques.empty() && !reorder; }
};

[[nodiscard]] bool plan_settings_restore(const runtime_settings &saved,
    const runtime_settings &current, settings_restore_plan &plan, std::string &error);
[[nodiscard]] std::string describe_unrestored_setting(const runtime_settings &saved,
    const runtime_settings &current, const settings_restore_plan &remaining);

// capture() refreshes the handles used by index-based writes. Check every
// setting before changing any, then read back the result: another add-on may
// reject a write even when the setter returns.
template <typename Access>
[[nodiscard]] settings_result restore_runtime_settings(const runtime_settings &saved, Access &access, std::string &error)
{
    runtime_settings current;
    const auto captured = access.capture(current, error);
    if (captured != settings_result::ready)
        return captured;
    settings_restore_plan plan;
    if (!plan_settings_restore(saved, current, plan, error))
        return settings_result::error;
    if (plan.empty())
        return settings_result::ready;

    for (const auto &write : plan.uniforms)
        access.set_uniform(write.second, saved.uniforms[write.first]);
    if (plan.reorder)
        access.reorder(plan.order);
    // Restore disabled states before enabled ones. A newly enabled technique
    // may queue initialization and make the runtime temporarily unavailable.
    for (const bool enabled : { false, true })
        for (const auto &write : plan.techniques)
            if (write.second == enabled)
                access.set_technique(write.first, enabled);

    runtime_settings verified;
    const auto verification = access.capture(verified, error);
    if (verification != settings_result::ready)
        return verification;
    settings_restore_plan remaining;
    if (!plan_settings_restore(saved, verified, remaining, error))
        return settings_result::error;
    if (!remaining.empty())
    {
        error = describe_unrestored_setting(saved, verified, remaining);
        return settings_result::error;
    }
    return settings_result::ready;
}

// Keep the backup after cancellation or timeout until queued compilation ends.
// The catalog may still be available before ReShade starts processing the queue.
class settings_backup
{
public:
    [[nodiscard]] bool capture(runtime_settings value)
    {
        if (pending_)
            return false;
        saved_ = std::make_shared<const runtime_settings>(std::move(value));
        return true;
    }
    void arm(const std::uint64_t generation) noexcept { pending_ = saved_ != nullptr; generation_ = generation; }
    void disarm() noexcept { pending_ = false; }
    void clear() noexcept { pending_ = false; saved_.reset(); }
    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] bool can_restore(const std::uint64_t generation) const noexcept { return pending_ && generation > generation_; }
    [[nodiscard]] const runtime_settings &saved() const { return *saved_; }
    // A settings setter may trigger a preset/reload callback that clears the
    // backup. Hold a shared reference so data still in use is not freed.
    [[nodiscard]] std::shared_ptr<const runtime_settings> retain() const noexcept { return saved_; }

private:
    std::shared_ptr<const runtime_settings> saved_;
    bool pending_ = false;
    std::uint64_t generation_ = 0;
};
}
