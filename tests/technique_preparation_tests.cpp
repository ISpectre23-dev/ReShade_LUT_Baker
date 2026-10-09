#include "bake_control.hpp"
#include "technique_preparation.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void expect(const bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Model ReShade 6.8 resource creation: enabling queues permutation zero, and
// disabling retains that queue. Enumeration is empty until creation completes.
// Offscreen permutations require the shared constant buffer from permutation zero.
struct runtime_model
{
    bool enabled = false;
    bool compiled = true;
    bool default_created = false;
    bool constant_buffer_created = false;
    bool creation_pending = false;
    bool veto_enable = false;
    bool veto_disable = false;
    bool throw_probe = false;
    unsigned int preset_writes = 0;
    unsigned int default_requests = 0;
    unsigned int offscreen_requests = 0;
    std::vector<bool> state_writes;

    void set_state(const bool value)
    {
        state_writes.push_back(value);
        if (value && !compiled)
            return;
        // ReShade skips the before-change event while loading, so restoring a
        // newly queued default cannot be vetoed by it.
        if (!creation_pending && (value ? veto_enable : veto_disable))
            return;
        enabled = value;
        if (value && !default_created && !creation_pending)
        {
            creation_pending = true;
            ++default_requests;
        }
    }

    void complete_default(const bool success = true)
    {
        creation_pending = false;
        default_created = success;
        constant_buffer_created = success;
        compiled = success;
    }

    bool request_offscreen()
    {
        ++offscreen_requests;
        return !creation_pending && constant_buffer_created;
    }

    lut_baker::technique_preparation prepare()
    {
        return lut_baker::prepare_default_technique(
            [this] { return enabled; },
            [this](const bool value) { set_state(value); },
            [this] {
                if (throw_probe)
                    throw std::runtime_error("probe failed");
                return !creation_pending;
            });
    }
};
}

int main()
{
    using lut_baker::technique_preparation;
    using lut_baker::bake_control;
    using namespace std::chrono_literals;
    const bake_control::clock::time_point start {};

    // Disabled, uninitialized effects need shared resources before offscreen work.
    for (const lut_baker::technique_key key : {
        lut_baker::technique_key { "DPX.fx", "DPX" },
        lut_baker::technique_key { "FakeHDR.fx", "HDR" } })
    {
        runtime_model old_path;
        expect(!old_path.request_offscreen(), "old offscreen-first path has no shared constant buffer");

        runtime_model runtime;
        bake_control control;
        control.start(start);
        const auto initial = runtime.prepare();
        expect(initial == technique_preparation::waiting, "unused disabled effect queues default resources first");
        expect(!runtime.enabled && runtime.state_writes == std::vector<bool> { true, false }, "disabled state restored in the same callback");
        expect(runtime.default_requests == 1 && runtime.offscreen_requests == 0, "no offscreen request while default buffer is absent");
        expect(control.wait_for_initialization(key, 10), "first default initialization wait is allowed");
        expect(control.initializing() && control.waiting_key() == key, "default stage retains exact technique identity");
        bool remains_waiting = true;
        for (unsigned int frame = 0; frame != 10000; ++frame)
            remains_waiting = remains_waiting && !control.can_attempt(10);
        expect(remains_waiting, "presentations alone cannot repeat default preparation");

        runtime.complete_default();
        expect(control.can_attempt(11), "default completion event permits verification");
        expect(runtime.prepare() == technique_preparation::ready, "default is rechecked before offscreen compilation");
        expect(!runtime.enabled && runtime.request_offscreen(), "offscreen request now has its constant buffer without enabling gameplay");
        expect(runtime.default_requests == 1 && runtime.preset_writes == 0, "preparation does not reload or save the preset");
        control.begin_attempt();
        expect(control.wait_for_compilation(key, 11), "same exact technique may subsequently wait for its offscreen permutation");
        expect(!control.initializing() && !control.can_attempt(11) && control.can_attempt(12), "offscreen and default waits are distinct");
        control.begin_attempt();
        expect(!control.wait_for_compilation(key, 12), "offscreen failure remains bounded after default preparation");
        expect(!control.wait_for_initialization(key, 12), "default initialization failure is also bounded");
        expect(control.timed_out(start + 60s), "both stages share the original overall deadline");
    }

    runtime_model active;
    active.enabled = true;
    active.complete_default();
    expect(active.prepare() == technique_preparation::ready && active.state_writes.empty(), "already enabled effect is untouched");

    runtime_model disabled_ready;
    disabled_ready.complete_default();
    expect(disabled_ready.prepare() == technique_preparation::ready && !disabled_ready.enabled,
        "previously initialized disabled effect is ready without another wait");

    runtime_model failed;
    expect(failed.prepare() == technique_preparation::waiting, "allocation failure setup");
    failed.complete_default(false);
    expect(failed.prepare() == technique_preparation::rejected && !failed.enabled && failed.offscreen_requests == 0,
        "failed default allocation/compilation never enters offscreen path");

    runtime_model vetoed;
    vetoed.veto_enable = true;
    expect(vetoed.prepare() == technique_preparation::rejected && !vetoed.enabled && vetoed.default_requests == 0,
        "external enable veto fails safely without rendering or requesting a permutation");

    runtime_model loading_restore;
    loading_restore.veto_disable = true;
    expect(loading_restore.prepare() == technique_preparation::waiting && !loading_restore.enabled,
        "newly queued default restores before the loading queue can render");

    runtime_model restore_vetoed;
    restore_vetoed.complete_default();
    restore_vetoed.veto_disable = true;
    expect(restore_vetoed.prepare() == technique_preparation::restore_failed && restore_vetoed.offscreen_requests == 0,
        "external restoration veto is explicit failure, not fake success");

    runtime_model throwing;
    throwing.throw_probe = true;
    bool caught = false;
    try { (void)throwing.prepare(); }
    catch (const std::runtime_error &) { caught = true; }
    expect(caught && !throwing.enabled, "diagnostic exception does not retain temporary enabled state");

    bool exception_enabled = false;
    unsigned int reads = 0;
    caught = false;
    try
    {
        (void)lut_baker::prepare_default_technique(
            [&] {
                if (++reads == 2)
                    throw std::runtime_error("state probe failed");
                return exception_enabled;
            },
            [&](const bool value) { exception_enabled = value; },
            [] { return true; });
    }
    catch (const std::runtime_error &) { caught = true; }
    expect(caught && !exception_enabled, "guard restores even if the first post-enable probe throws");

    bake_control cancelled;
    cancelled.start(start);
    expect(cancelled.wait_for_initialization({ "DPX.fx", "DPX" }, 40), "preparation cancellation setup");
    cancelled.stop();
    expect(!cancelled.can_attempt(41) && !cancelled.timed_out(start + 100s), "cancelled preparation cannot restart after its reload event");
    cancelled.start(start + 101s);
    expect(cancelled.wait_for_initialization({ "DPX.fx", "DPX" }, 41), "next export gets fresh default-stage history");

    // Two techniques with the same effect and name are tracked separately, so
    // each one gets its own wait.
    bake_control duplicates;
    duplicates.start(start);
    expect(duplicates.wait_for_initialization({ "same.fx", "grade", 0, 2 }, 50), "first duplicate preparation");
    expect(duplicates.wait_for_initialization({ "same.fx", "grade", 1, 2 }, 51), "second duplicate has independent bounded preparation");

    // Prepare the entire selected chain before any offscreen work. Use a
    // deliberately non-alphabetical order and leave an unselected effect alone.
    const std::vector<lut_baker::technique_key> order {
        { "FakeHDR.fx", "HDR" }, { "DPX.fx", "DPX" }
    };
    const lut_baker::technique_selection requested { order.begin(), order.end() };
    const auto requested_before = requested;
    std::vector<runtime_model> chain(order.size());
    runtime_model unselected;
    bake_control chain_control;
    chain_control.start(start);
    std::uint64_t generation = 70;
    unsigned int waits = 0;
    bool all_ready = false;
    for (unsigned int pass = 0; pass != 3 && !all_ready; ++pass)
    {
        expect(chain_control.can_attempt(generation), "chain resumes only with authoritative completion");
        all_ready = true;
        for (std::size_t index = 0; index != chain.size(); ++index)
        {
            const auto result = chain[index].prepare();
            expect(!chain[index].enabled, "chain member restored before moving to the next step");
            if (result == technique_preparation::waiting)
            {
                expect(chain_control.wait_for_initialization(order[index], generation), "each chain member has its own default wait");
                expect(chain[0].offscreen_requests == 0 && chain[1].offscreen_requests == 0,
                    "no partial-chain offscreen request during normal preparation");
                chain[index].complete_default();
                ++generation;
                ++waits;
                all_ready = false;
                break;
            }
            expect(result == technique_preparation::ready, "chain preparation succeeds");
        }
    }
    expect(all_ready && waits == 2, "both unused effects complete preparation before baking");
    std::vector<lut_baker::technique_key> executed;
    if (all_ready)
    {
        for (std::size_t index = 0; index != chain.size(); ++index)
        {
            expect(chain[index].request_offscreen(), "all chain buffers exist for offscreen requests");
            executed.push_back(order[index]);
        }
    }
    expect(executed == order && requested == requested_before, "real relative order and immutable request are preserved");
    expect(unselected.state_writes.empty() && unselected.offscreen_requests == 0,
        "unselected effect is never prepared or rendered");

    if (failures == 0)
        std::cout << "All technique-preparation tests passed.\n";
    return failures == 0 ? 0 : 1;
}
