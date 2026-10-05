#pragma once

namespace lut_baker
{
enum class technique_preparation
{
    ready,
    waiting,
    rejected,
    restore_failed
};

// ReShade 6.8 creates the shared constant buffer only for permutation zero.
// A disabled, never-initialized effect must finish that creation before an
// offscreen permutation is requested. Enabling queues it; disabling immediately
// retains the queue without rendering a frame or saving the preset.
// The caller supplies a fresh authoritative handle and checks catalog availability
// because enumeration is empty while ReShade's creation queue is still pending.
template <typename GetState, typename SetState, typename CatalogAvailable>
[[nodiscard]] technique_preparation prepare_default_technique(
    GetState get_state, SetState set_state, CatalogAvailable catalog_available)
{
    if (get_state())
        return technique_preparation::ready;

    // Also restore if a diagnostic/probe throws after enabling. ReShade's API
    // normally does not throw; an external veto is reported, never treated as
    // successful preparation. No state is retained across frames by this guard.
    struct restore_disabled
    {
        SetState &setter;
        bool armed = true;
        ~restore_disabled() noexcept
        {
            if (armed)
            {
                try { setter(false); }
                catch (...) {} // Preserve an already propagating exception.
            }
        }
    } restore { set_state };

    set_state(true);
    const bool accepted = get_state();
    set_state(false);
    restore.armed = false;
    if (get_state())
        return technique_preparation::restore_failed;
    if (!accepted)
        return technique_preparation::rejected;

    return catalog_available() ? technique_preparation::ready : technique_preparation::waiting;
}
}
