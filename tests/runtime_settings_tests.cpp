#include "bake_control.hpp"
#include "runtime_settings.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
int failures = 0;
void expect(const bool value, const char *message)
{
    if (!value)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
std::uint32_t bits(const float value)
{
    std::uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}
lut_baker::runtime_settings live_settings()
{
    using lut_baker::uniform_value_type;
    lut_baker::runtime_settings value;
    value.preset = "PresetA.ini";
    value.techniques = { { { "B.fx", "Grade" }, true }, { { "A.fx", "Grade" }, false }, { { "Other.fx", "Unselected" }, true } };
    value.uniforms = {
        { "A.fx", "Strength", uniform_value_type::real, 1, 1, 0, { bits(0.73f) } },
        { "B.fx", "Strength", uniform_value_type::real, 1, 3, 0, { bits(1.27f), bits(-0.0f), 0x7fc12345u } },
        { "Other.fx", "Toggle", uniform_value_type::boolean, 1, 2, 0, { 1, 0 } },
        { "Other.fx", "Signed", uniform_value_type::sint, 1, 1, 0, { static_cast<std::uint32_t>(-17) } },
        { "Other.fx", "Unsigned", uniform_value_type::uint, 1, 1, 0, { 0xfffffffdu } },
        { "Other.fx", "MatrixArray", uniform_value_type::real, 2, 3, 2,
            { bits(1), bits(2), bits(3), bits(4), bits(5), bits(6), bits(7), bits(8), bits(9), bits(10), bits(11), bits(12) } },
        { "Other.fx", "Half", uniform_value_type::real16, 1, 1, 0, { bits(0.125f) } },
        { "Other.fx", "SmallInt", uniform_value_type::sint16, 1, 1, 0, { 3 } },
        { "Other.fx", "SmallUint", uniform_value_type::uint16, 1, 1, 0, { 5 } }
    };
    return value;
}

// Models load_current_preset resetting the whole runtime after compilation.
// This is a settings API model, not a synthetic grading shader or GPU test.
struct runtime_model
{
    lut_baker::runtime_settings live = live_settings();
    bool loading = false;
    bool loading_after_enable = false;
    bool reject_uniform = false;
    bool reject_enable = false;
    bool reject_order = false;
    unsigned int writes = 0;
    unsigned int saves = 0;
    unsigned int dynamic_frame = 100;
    std::uint64_t generation = 0;
    std::uint64_t bindings_generation = 0;
    std::string bindings_preset;
    std::function<void()> on_uniform_write;
    std::vector<std::pair<std::string, std::string>> uniforms;
    std::vector<lut_baker::technique_key> techniques;

    lut_baker::settings_result capture(lut_baker::runtime_settings &value, std::string &error)
    {
        error.clear();
        if (loading)
            return lut_baker::settings_result::waiting;
        value = live;
        bindings_generation = generation;
        bindings_preset = live.preset;
        uniforms.clear();
        techniques.clear();
        for (const auto &uniform : live.uniforms)
            uniforms.emplace_back(uniform.effect, uniform.name);
        for (const auto &technique : live.techniques)
            techniques.push_back(technique.key);
        return lut_baker::settings_result::ready;
    }
    void set_uniform(const std::size_t index, const lut_baker::uniform_setting &value)
    {
        check_bindings();
        ++writes;
        for (auto &uniform : live.uniforms)
            if (uniform.effect == uniforms.at(index).first && uniform.name == uniforms.at(index).second)
            {
                if (!(reject_uniform && uniform.effect == "A.fx"))
                    uniform.words = value.words;
                if (on_uniform_write)
                {
                    const auto callback = std::move(on_uniform_write);
                    callback();
                }
                return;
            }
    }
    void set_technique(const std::size_t index, const bool enabled)
    {
        check_bindings();
        ++writes;
        for (auto &technique : live.techniques)
            if (technique.key == techniques.at(index))
            {
                if (!(reject_enable && enabled && technique.key.effect == "B.fx"))
                    technique.enabled = enabled;
                if (enabled && loading_after_enable)
                    loading = true;
                return;
            }
    }
    void reorder(const std::vector<std::size_t> &order)
    {
        check_bindings();
        ++writes;
        if (reject_order)
            return;
        std::vector<lut_baker::technique_setting> sorted;
        for (const auto index : order)
            for (const auto &entry : live.techniques)
                if (entry.key == techniques.at(index))
                    sorted.push_back(entry);
        live.techniques = std::move(sorted);
    }
    void check_bindings() const
    {
        if (generation != bindings_generation || live.preset != bindings_preset)
            throw std::runtime_error("context changed during settings recovery");
    }
    void apply_saved_preset()
    {
        for (auto &uniform : live.uniforms)
            std::fill(uniform.words.begin(), uniform.words.end(), 0u);
        for (auto &technique : live.techniques)
            technique.enabled = !technique.enabled;
        std::reverse(live.uniforms.begin(), live.uniforms.end());
        std::reverse(live.techniques.begin(), live.techniques.end());
        ++dynamic_frame;
    }
};

bool same_settings(const lut_baker::runtime_settings &left, const lut_baker::runtime_settings &right)
{
    lut_baker::settings_restore_plan plan;
    std::string error;
    return lut_baker::plan_settings_restore(left, right, plan, error) && plan.empty();
}
}

int main()
{
    using lut_baker::settings_result;
    const auto saved = live_settings();
    std::string error;
    expect(lut_baker::validate_runtime_settings(saved, error), "all public API value types, vectors and matrix arrays are capturable");
    expect(lut_baker::uniform_component_count(saved.uniforms[5]) == 12, "matrix array has logical components, not padded buffer bytes");

    runtime_model runtime;
    runtime.apply_saved_preset();
    expect(!same_settings(saved, runtime.live), "preset reload reproduces lost unsaved parameters, states and order");
    expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::ready, "unsaved settings restored after new compilation");
    expect(same_settings(saved, runtime.live), "exact parameters and non-alphabetical order restored across the entire runtime");
    expect(runtime.live.uniforms.front().effect == "Other.fx", "uniform enumeration order can change; matching uses exact identity and shape");
    expect(runtime.dynamic_frame == 101 && runtime.saves == 0, "dynamic frame state and preset file are untouched");
    const auto writes = runtime.writes;
    expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::ready && runtime.writes == writes,
        "cached/no-change path does not write settings or reset enabled techniques");

    lut_baker::settings_backup backup;
    expect(backup.capture(saved), "candidate snapshot captured before compilation request");
    backup.arm(7);
    expect(backup.pending() && !backup.can_restore(7) && !backup.can_restore(6), "ready catalog alone cannot discard backup before compilation starts");
    auto different = saved;
    different.uniforms.front().words = { bits(0.12f) };
    expect(!backup.capture(different) && same_settings(backup.saved(), saved), "another request cannot replace a pending backup");
    expect(backup.can_restore(8), "completed reload allows recovery");
    backup.disarm();
    expect(!backup.pending() && !backup.can_restore(99), "a render event disarms a cached technique without restoring stale settings later");
    backup.clear();

    using namespace std::chrono_literals;
    const lut_baker::bake_control::clock::time_point start {};
    for (const bool timeout : { false, true })
    {
        lut_baker::bake_control control;
        control.start(start);
        expect(backup.capture(saved), "late recovery snapshot captured");
        backup.arm(20);
        expect(control.wait_for_compilation({ "A.fx", "Grade" }, 20), "late recovery compilation wait");
        if (timeout)
            expect(control.timed_out(start + 60s), "timeout recovery setup");
        control.stop(); // Both cancellation and failure leave the backup intact.
        expect(backup.pending() && !control.can_attempt(21), "stopping bake does not discard queued-compilation backup or resume export");
        runtime.apply_saved_preset();
        expect(backup.can_restore(21) && lut_baker::restore_runtime_settings(backup.saved(), runtime, error) == settings_result::ready,
            "late compilation restores settings after cancellation/timeout");
        backup.clear();
        expect(!control.can_attempt(22) && same_settings(saved, runtime.live), "late recovery cannot restart cancelled/timed-out bake");
    }

    // Each selected effect can require its own offscreen compilation. Preserve
    // all loaded effects across every cycle, including unselected parameters.
    for (unsigned int cycle = 0; cycle != 3; ++cycle)
    {
        const auto candidate = runtime.live;
        expect(backup.capture(candidate), "next compilation cycle captures current live settings");
        backup.arm(30 + cycle);
        runtime.apply_saved_preset();
        expect(backup.can_restore(31 + cycle) && lut_baker::restore_runtime_settings(backup.saved(), runtime, error) == settings_result::ready,
            "multiple compilation cycles restore settings before continuing bake");
        backup.clear();
    }
    expect(same_settings(saved, runtime.live), "unselected effects and queued grading survive all compilation cycles");

    auto changed_context = saved;
    changed_context.preset = "PresetB.ini";
    runtime.live = changed_context;
    runtime.writes = 0;
    expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::error && runtime.writes == 0,
        "explicit preset switch never applies an old backup to the new preset");

    for (unsigned int change = 0; change != 5; ++change)
    {
        runtime.live = saved;
        if (change == 0) runtime.live.uniforms.front().columns = 2;
        if (change == 1) runtime.live.uniforms.front().type = lut_baker::uniform_value_type::uint;
        if (change == 2) runtime.live.uniforms.front().name = "DifferentParameter";
        if (change == 3) runtime.live.techniques.front().key.effect = "DifferentEffect.fx";
        if (change == 4) runtime.live.uniforms.pop_back();
        runtime.writes = 0;
        expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::error && runtime.writes == 0,
            "catalog/type/shape change rejected before any restoration write");
    }

    auto duplicate = saved;
    duplicate.uniforms.push_back(duplicate.uniforms.front());
    expect(!lut_baker::validate_runtime_settings(duplicate, error), "duplicate parameter identity cannot be guessed by name");
    duplicate = saved;
    duplicate.techniques.push_back(duplicate.techniques.front());
    expect(!lut_baker::validate_runtime_settings(duplicate, error), "duplicate technique identity cannot be guessed by occurrence");

    for (unsigned int blocked = 0; blocked != 3; ++blocked)
    {
        runtime = runtime_model {};
        runtime.apply_saved_preset();
        runtime.reject_uniform = blocked == 0;
        runtime.reject_enable = blocked == 1;
        runtime.reject_order = blocked == 2;
        expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::error,
            "uniform, failed-shader enable and order vetoes are verified failures, never fake success");
        expect(!error.empty() && runtime.saves == 0, "restoration failure is diagnosed without saving preset");
    }

    runtime = runtime_model {};
    runtime.apply_saved_preset();
    runtime.loading_after_enable = true;
    expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::waiting,
        "restoration queues resource creation safely if required");
    runtime.loading = false;
    runtime.loading_after_enable = false;
    expect(lut_baker::restore_runtime_settings(saved, runtime, error) == settings_result::ready && same_settings(saved, runtime.live),
        "deferred verification succeeds after resource creation completes");

    // A setter triggers other add-ons' callbacks. Simulate one changing the
    // context and clearing recovery while its immutable snapshot is in use.
    for (const bool preset_change : { false, true })
    {
        runtime = runtime_model {};
        runtime.apply_saved_preset();
        expect(backup.capture(saved), "reentrant recovery snapshot captured");
        backup.arm(90);
        const auto retained = backup.retain();
        runtime.on_uniform_write = [&] {
            backup.clear();
            if (preset_change)
                runtime.live.preset = "NewPreset.ini";
            else
                ++runtime.generation;
            runtime.apply_saved_preset();
        };
        bool stopped = false;
        try { (void)lut_baker::restore_runtime_settings(*retained, runtime, error); }
        catch (const std::runtime_error &) { stopped = true; }
        expect(stopped && runtime.writes == 1 && !backup.pending(), "reentrant preset/catalog change blocks all subsequent writes");
        expect(same_settings(*retained, saved), "discarded owner backup stays alive during callback unwinding");
        expect(std::all_of(runtime.live.uniforms.begin(), runtime.live.uniforms.end(), [](const auto &uniform) {
            return std::all_of(uniform.words.begin(), uniform.words.end(), [](const auto word) { return word == 0; });
        }), "no old parameter values applied after the context switch");
    }

    auto invalid = saved.uniforms.front();
    invalid.rows = std::numeric_limits<std::uint32_t>::max();
    invalid.columns = std::numeric_limits<std::uint32_t>::max();
    invalid.array_length = std::numeric_limits<std::uint32_t>::max();
    expect(lut_baker::uniform_component_count(invalid) == 0, "large dimensions cannot overflow or allocate beyond backup limit");
    invalid.rows = 0;
    invalid.columns = 1;
    invalid.array_length = 0;
    expect(lut_baker::uniform_component_count(invalid) == 0, "zero-dimensional parameter is rejected");

    if (failures == 0)
        std::cout << "All runtime-settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
