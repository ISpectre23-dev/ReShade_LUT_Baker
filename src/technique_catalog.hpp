#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace lut_baker
{
struct technique_key
{
    std::string effect;
    std::string name;
    std::uint32_t occurrence = 0;
    std::uint32_t occurrence_count = 1;

    bool operator==(const technique_key &other) const noexcept
    {
        return effect == other.effect && name == other.name && occurrence == other.occurrence;
    }
};

struct technique_key_hash
{
    std::size_t operator()(const technique_key &value) const noexcept
    {
        const std::size_t effect_hash = std::hash<std::string> {}(value.effect);
        const std::size_t name_hash = std::hash<std::string> {}(value.name);
        const std::size_t occurrence_hash = std::hash<std::uint32_t> {}(value.occurrence);
        const std::size_t combined = effect_hash ^ (name_hash + 0x9e3779b9u + (effect_hash << 6) + (effect_hash >> 2));
        return combined ^ (occurrence_hash + 0x9e3779b9u + (combined << 6) + (combined >> 2));
    }
};

using technique_selection = std::unordered_set<technique_key, technique_key_hash>;

struct catalog_reconciliation
{
    bool accepted = false;
    technique_selection selected;
};

[[nodiscard]] inline bool selection_contains_exact(
    const technique_selection &selection,
    const technique_key &key)
{
    const auto match = selection.find(key);
    return match != selection.end() && match->occurrence_count == key.occurrence_count;
}

[[nodiscard]] inline catalog_reconciliation reconcile_catalog_selection(
    const std::vector<technique_key> &refreshed_catalog,
    const technique_selection &previous_selection,
    const bool pending_request_nonempty)
{
    catalog_reconciliation result;

    // ReShade enumeration is temporarily empty while effects are compiling.
    // Keep the previous catalog/selection until an authoritative result exists.
    if (refreshed_catalog.empty() && (!previous_selection.empty() || pending_request_nonempty))
        return result;

    result.accepted = true;
    result.selected.reserve(std::min(refreshed_catalog.size(), previous_selection.size()));
    for (const technique_key &current : refreshed_catalog)
    {
        if (selection_contains_exact(previous_selection, current))
            result.selected.insert(current);
    }
    return result;
}

template <typename TechniqueEntries>
[[nodiscard]] technique_selection select_currently_enabled(const TechniqueEntries &techniques)
{
    technique_selection selected;
    selected.reserve(techniques.size());
    for (const auto &entry : techniques)
    {
        if (entry.enabled)
            selected.insert(entry.key);
    }
    return selected;
}
}
