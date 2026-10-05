#include "runtime_settings.hpp"

#include <algorithm>
#include <unordered_map>

namespace lut_baker
{
namespace
{
std::string identity(const std::string &effect, const std::string &name)
{
    return effect + '\0' + name;
}
std::string label(const std::string &effect, const std::string &name)
{
    return effect + " :: " + name;
}
bool same_layout(const uniform_setting &left, const uniform_setting &right) noexcept
{
    return left.type == right.type && left.rows == right.rows && left.columns == right.columns && left.array_length == right.array_length;
}
}

std::size_t uniform_component_count(const uniform_setting &uniform) noexcept
{
    const std::uint64_t rows = uniform.rows;
    const std::uint64_t columns = uniform.columns;
    const std::uint64_t elements = std::max(1u, uniform.array_length);
    if (rows == 0 || columns == 0 || rows > settings_word_limit / columns || rows * columns > settings_word_limit / elements)
        return 0;
    return static_cast<std::size_t>(rows * columns * elements);
}

bool validate_runtime_settings(const runtime_settings &settings, std::string &error)
{
    error.clear();
    if (settings.preset.empty() || settings.techniques.empty())
    {
        error = "The current preset or technique list is unavailable.";
        return false;
    }
    std::unordered_map<std::string, std::size_t> uniforms;
    std::size_t total = 0;
    for (const uniform_setting &uniform : settings.uniforms)
    {
        const auto count = uniform_component_count(uniform);
        if (uniform.effect.empty() || uniform.name.empty() || count == 0 || count != uniform.words.size() ||
            static_cast<unsigned int>(uniform.type) > static_cast<unsigned int>(uniform_value_type::real16) ||
            count > settings_word_limit - total)
        {
            error = "Shader parameter data is invalid or exceeds the 64 MiB settings-backup limit: " + label(uniform.effect, uniform.name) + '.';
            return false;
        }
        total += count;
        if (!uniforms.emplace(identity(uniform.effect, uniform.name), uniforms.size()).second)
        {
            error = "A shader parameter has an ambiguous duplicate identity: " + label(uniform.effect, uniform.name) + ". Remove duplicate effect files/search paths before baking.";
            return false;
        }
    }
    std::unordered_map<std::string, std::size_t> techniques;
    for (const technique_setting &technique : settings.techniques)
    {
        const auto &key = technique.key;
        if (key.effect.empty() || key.name.empty() || key.occurrence != 0 || key.occurrence_count != 1 ||
            !techniques.emplace(identity(key.effect, key.name), techniques.size()).second)
        {
            error = "A technique has an invalid or ambiguous duplicate identity: " + label(key.effect, key.name) + ". Remove duplicate effect files/search paths before baking.";
            return false;
        }
    }
    return true;
}

bool plan_settings_restore(const runtime_settings &saved, const runtime_settings &current,
    settings_restore_plan &plan, std::string &error)
{
    plan = {};
    if (!validate_runtime_settings(saved, error) || !validate_runtime_settings(current, error))
        return false;
    if (saved.preset != current.preset)
    {
        error = "The active preset changed. Settings from the previous preset will not be reapplied.";
        return false;
    }
    if (saved.uniforms.size() != current.uniforms.size() || saved.techniques.size() != current.techniques.size())
    {
        error = "The effect list changed while settings recovery was pending. Old settings cannot be matched safely.";
        return false;
    }
    std::unordered_map<std::string, std::size_t> uniforms;
    for (std::size_t index = 0; index < current.uniforms.size(); ++index)
        uniforms.emplace(identity(current.uniforms[index].effect, current.uniforms[index].name), index);
    for (std::size_t index = 0; index < saved.uniforms.size(); ++index)
    {
        const auto &uniform = saved.uniforms[index];
        const auto found = uniforms.find(identity(uniform.effect, uniform.name));
        if (found == uniforms.end() || !same_layout(uniform, current.uniforms[found->second]))
        {
            error = "A shader parameter disappeared or changed type/dimensions: " + label(uniform.effect, uniform.name) + ". Old values will not be assigned to a different parameter.";
            return false;
        }
        if (uniform.words != current.uniforms[found->second].words)
            plan.uniforms.emplace_back(index, found->second);
    }
    std::unordered_map<std::string, std::size_t> techniques;
    for (std::size_t index = 0; index < current.techniques.size(); ++index)
        techniques.emplace(identity(current.techniques[index].key.effect, current.techniques[index].key.name), index);
    for (std::size_t index = 0; index < saved.techniques.size(); ++index)
    {
        const auto &technique = saved.techniques[index];
        const auto found = techniques.find(identity(technique.key.effect, technique.key.name));
        if (found == techniques.end())
        {
            error = "A technique disappeared during settings recovery: " + label(technique.key.effect, technique.key.name) + '.';
            return false;
        }
        plan.order.push_back(found->second);
        plan.reorder = plan.reorder || found->second != index;
        if (technique.enabled != current.techniques[found->second].enabled)
            plan.techniques.emplace_back(found->second, technique.enabled);
    }
    return true;
}

std::string describe_unrestored_setting(const runtime_settings &saved,
    const runtime_settings &current, const settings_restore_plan &remaining)
{
    if (!remaining.uniforms.empty())
    {
        const auto &uniform = saved.uniforms[remaining.uniforms.front().first];
        return "ReShade did not restore shader parameter " + label(uniform.effect, uniform.name) + ". Another add-on may have blocked the change.";
    }
    if (!remaining.techniques.empty())
    {
        const auto &key = current.techniques[remaining.techniques.front().first].key;
        return "ReShade did not restore the enabled state of " + label(key.effect, key.name) + ". A shader may have failed to compile, or another add-on may have blocked the change.";
    }
    return "ReShade did not restore the technique order. Another add-on may have blocked the change.";
}
}
