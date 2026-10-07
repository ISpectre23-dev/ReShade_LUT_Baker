#include "bake_control.hpp"
#include "export_settings.hpp"

#include <chrono>
#include <iostream>

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
}

int main()
{
    using namespace std::chrono_literals;
    using lut_baker::bake_control;
    const bake_control::clock::time_point start {};
    const lut_baker::technique_key first { "first.fx", "grading" };
    const lut_baker::technique_key second { "second.fx", "grading" };
    const lut_baker::technique_key duplicate { "first.fx", "grading", 1, 2 };
    bake_control control;

    expect(!control.can_attempt(0) && !control.timed_out(start + 100s), "idle bake never renders or times out");
    control.start(start);
    expect(control.can_attempt(7), "first attempt can render without a reload event");
    control.begin_attempt();
    expect(control.wait_for_compilation(first, 7), "first missing event waits for compilation");
    expect(control.waiting() && control.waiting_key() == first, "waiting technique retained for diagnostics");
    // Reproduces the reported thousands-of-attempts loop at high frame rates.
    bool no_frame_retries = true;
    for (int frame = 0; frame < 10000; ++frame)
        no_frame_retries = no_frame_retries && !control.can_attempt(7);
    expect(no_frame_retries, "10000 presentations cannot re-enqueue a loading/failed permutation");
    expect(!control.can_attempt(6), "an earlier reload cannot unblock the current compilation");
    expect(control.can_attempt(8), "a new completed reload enables one verification attempt");
    control.begin_attempt();
    expect(!control.wait_for_compilation(first, 8), "same missing technique after reload fails, not another compile loop");
    control.stop();
    expect(!control.can_attempt(100) && !control.timed_out(start + 100s), "failure is terminal despite later reload events");

    // Multiple selected techniques may each require one initialization cycle.
    control.start(start);
    control.begin_attempt();
    expect(control.wait_for_compilation(first, 20), "new export resets failed-permutation history");
    expect(control.can_attempt(21), "first selected technique completed loading");
    control.begin_attempt();
    expect(control.wait_for_compilation(second, 21), "same technique name in another effect has independent identity");
    expect(!control.can_attempt(21) && control.can_attempt(22), "next technique requires its own completed reload");
    control.begin_attempt();
    expect(control.wait_for_compilation(duplicate, 22), "duplicate occurrence has independent identity");
    expect(control.can_attempt(23), "final compilation enables final full-chain attempt");

    // The deadline must not restart after fence completion, reload or retries.
    expect(!control.timed_out(start + 59s), "slow valid compilation is allowed within deadline");
    expect(control.timed_out(start + 60s), "deadline applies exactly at 60 seconds");
    control.begin_attempt();
    expect(control.timed_out(start + 61s), "another attempt cannot reset the deadline");
    control.stop();
    expect(!control.can_attempt(24), "timeout stops subsequent attempts");

    control.start(start);
    expect(control.wait_for_compilation(first, 30), "compile cancellation setup");
    control.stop();
    expect(!control.can_attempt(31), "cancelled compile stays cancelled after reload completion");
    expect(!control.timed_out(start + 100s), "timeout cannot replace cancelled status");
    control.start(start + 101s);
    expect(control.can_attempt(31) && !control.timed_out(start + 102s), "next export starts cleanly after cancellation");

    expect(lut_baker::can_abort_export(true, false), "queued/compiling/GPU-wait requests can be aborted");
    expect(!lut_baker::can_abort_export(true, true), "already started atomic writer cannot be aborted");
    expect(!lut_baker::can_abort_export(false, false), "idle/completed/cancelled request cannot be aborted again");
    expect(!lut_baker::can_abort_export(false, true), "writer remains outside cancellation even at teardown");

    const lut_baker::export_preferences defaults;
    expect(defaults.format == lut_baker::output_format::cube, "factory output format is CUBE");
    expect(lut_baker::effective_lattice_size(defaults) == 64, "factory CUBE lattice is 64 cubed");
    if (failures == 0)
        std::cout << "All bake-control tests passed.\n";
    return failures == 0 ? 0 : 1;
}
