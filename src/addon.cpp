#include <imgui.h>
#include <reshade.hpp>

#include "cube_lut.hpp"
#include "rise_tex.hpp"
#include "technique_catalog.hpp"
#include "version.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using namespace reshade::api;
using lut_baker::technique_key;
using lut_baker::technique_selection;

constexpr std::chrono::seconds permutation_timeout { 60 };
constexpr std::chrono::seconds gpu_submission_timeout { 30 };
constexpr std::uint64_t gpu_wait_timeout_ns = 30'000'000'000ull;

struct technique_entry
{
    technique_key key;
    effect_technique handle {};
    bool enabled = false;
};

enum class operation_phase
{
    ready,
    queued,
    compiling,
    waiting_gpu,
    reading,
    writing,
    success,
    error
};

struct buffer_snapshot
{
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    format color_format = format::unknown;
    color_space presentation_color_space = color_space::unknown;
};

struct gpu_resources
{
    device *owner = nullptr;
    resource identity {};
    resource target {};
    resource readback {};
    resource_view target_rtv {};
    fence completion_fence {};
    std::uint64_t fence_value = 0;
    std::uint32_t lattice_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    format pixel_format = format::unknown;
    bool fp16_fallback = false;
    bool work_in_flight = false;
    bool synchronization_failed = false;
};

struct export_job
{
    lut_baker::export_request request;
    std::size_t technique_count = 0;
    bool identity = false;
    std::vector<lut_baker::float4> samples;
    lut_baker::cube_metadata metadata;
    std::string initial_warning;
};

struct export_result
{
    bool success = false;
    bool identity = false;
    std::filesystem::path output;
    std::size_t technique_count = 0;
    lut_baker::output_format format = lut_baker::output_format::cube;
    std::uint32_t lattice_size = 0;
    lut_baker::rise_export_metrics rise_metrics;
    std::vector<std::string> verified_techniques;
    std::string error;
    std::string warning;
};

struct runtime_state
{
    explicit runtime_state(effect_runtime *value) : runtime(value) {}

    std::recursive_mutex mutex;
    effect_runtime *runtime = nullptr;
    bool destroyed = false;
    bool catalog_dirty = true;
    std::vector<technique_entry> techniques;
    technique_selection selected;

    std::array<char, 160> output_filename {};
    std::array<char, 96> technique_filter {};
    lut_baker::export_preferences preferences;

    operation_phase phase = operation_phase::ready;
    std::string status = "Ready";
    std::string detail;
    std::string warning;
    std::filesystem::path last_output;
    lut_baker::output_format last_format = lut_baker::output_format::cube;
    std::uint32_t last_lattice_size = 0;
    std::size_t last_technique_count = 0;
    double last_duration_seconds = 0.0;
    lut_baker::error_metrics identity_metrics {};
    bool identity_metrics_valid = false;
    lut_baker::rise_export_metrics rise_metrics;
    bool rise_metrics_valid = false;

    bool export_pending = false;
    technique_selection requested;
    // Immutable until the current bake AND its writer have finished.
    lut_baker::export_request request;
    std::chrono::steady_clock::time_point request_started {};
    std::uint32_t attempts = 0;

    bool capture_execution_events = false;
    command_list *capture_command_list = nullptr;
    resource_view capture_rtv {};
    std::vector<effect_technique> expected_execution;
    std::size_t execution_index = 0;
    bool execution_mismatch = false;

    bool submission_pending = false;
    bool submission_has_result = false;
    std::uint64_t submission_fence_value = 0;
    std::chrono::steady_clock::time_point submission_started {};
    std::vector<technique_key> submitted_techniques;

    bool writer_pending = false;
    std::future<export_result> writer_future;

    buffer_snapshot source_buffer;
    gpu_resources gpu;
};

std::mutex s_states_mutex;
std::unordered_map<effect_runtime *, std::shared_ptr<runtime_state>> s_states;

std::shared_ptr<runtime_state> find_state(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(s_states_mutex);
    const auto iterator = s_states.find(runtime);
    return iterator != s_states.end() ? iterator->second : nullptr;
}

void log_message(const reshade::log::level level, const std::string &message)
{
    const std::string complete = "[ReShade LUT Baker] " + message;
    reshade::log::message(level, complete.c_str());
}

std::string get_runtime_string(
    effect_runtime *runtime,
    const effect_technique technique,
    void (effect_runtime::*getter)(effect_technique, char *, std::size_t *) const)
{
    std::size_t size = 0;
    (runtime->*getter)(technique, nullptr, &size);
    if (size == 0)
        return {};

    std::string result(size, '\0');
    (runtime->*getter)(technique, result.data(), &size);
    if (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
}

std::string technique_label(const technique_key &key)
{
    std::string label = key.effect + " :: " + key.name;
    if (key.occurrence_count > 1)
        label += " [instance " + std::to_string(key.occurrence + 1) + " of " + std::to_string(key.occurrence_count) + ']';
    return label;
}

std::string graphics_api_name(const device_api api)
{
    switch (api)
    {
    case device_api::d3d9: return "Direct3D 9";
    case device_api::d3d10: return "Direct3D 10";
    case device_api::d3d11: return "Direct3D 11";
    case device_api::d3d12: return "Direct3D 12";
    case device_api::opengl: return "OpenGL";
    case device_api::vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

std::string color_space_name(const color_space value)
{
    switch (value)
    {
    case color_space::srgb: return "sRGB";
    case color_space::scrgb: return "scRGB linear";
    case color_space::hdr10_pq: return "HDR10 PQ";
    case color_space::hdr10_hlg: return "HDR10 HLG";
    default: return "Unknown";
    }
}

std::string format_name(const format value)
{
    switch (value)
    {
    case format::r8g8b8a8_unorm: return "RGBA8 UNORM";
    case format::r8g8b8a8_unorm_srgb: return "RGBA8 sRGB";
    case format::b8g8r8a8_unorm: return "BGRA8 UNORM";
    case format::b8g8r8a8_unorm_srgb: return "BGRA8 sRGB";
    case format::r10g10b10a2_unorm: return "RGB10A2 UNORM";
    case format::r11g11b10_float: return "R11G11B10 FLOAT";
    case format::r16g16b16a16_float: return "RGBA16F";
    case format::r32g32b32a32_float: return "RGBA32F";
    default:
        return "format " + std::to_string(static_cast<std::uint32_t>(value));
    }
}

std::filesystem::path reshade_base_path()
{
    std::size_t size = 0;
    reshade::get_reshade_base_path(nullptr, &size);
    if (size == 0)
        return std::filesystem::current_path();

    std::string value(size, '\0');
    reshade::get_reshade_base_path(value.data(), &size);
    if (!value.empty() && value.back() == '\0')
        value.pop_back();
    return std::filesystem::u8path(value);
}

std::filesystem::path output_directory()
{
    return reshade_base_path() / "LUT_Bakes";
}

bool refresh_catalog(runtime_state &state)
{
    std::vector<technique_entry> refreshed;
    state.runtime->enumerate_techniques(nullptr, [&refreshed](effect_runtime *runtime, const effect_technique technique) {
        technique_entry entry;
        entry.key.effect = get_runtime_string(runtime, technique, &effect_runtime::get_technique_effect_name);
        entry.key.name = get_runtime_string(runtime, technique, &effect_runtime::get_technique_name);
        entry.handle = technique;
        entry.enabled = runtime->get_technique_state(technique);
        refreshed.push_back(std::move(entry));
    });

    std::unordered_map<technique_key, std::uint32_t, lut_baker::technique_key_hash> occurrence_counts;
    for (const technique_entry &entry : refreshed)
        ++occurrence_counts[entry.key];
    std::unordered_map<technique_key, std::uint32_t, lut_baker::technique_key_hash> next_occurrence;
    for (technique_entry &entry : refreshed)
    {
        entry.key.occurrence_count = occurrence_counts[entry.key];
        entry.key.occurrence = next_occurrence[entry.key]++;
    }

    std::vector<technique_key> refreshed_keys;
    refreshed_keys.reserve(refreshed.size());
    for (const technique_entry &entry : refreshed)
        refreshed_keys.push_back(entry.key);

    lut_baker::catalog_reconciliation reconciliation = lut_baker::reconcile_catalog_selection(
        refreshed_keys,
        state.selected,
        state.export_pending && !state.requested.empty());
    if (!reconciliation.accepted)
        return false;

    // This is the editable selection for future exports. Never reconcile
    // 'requested', which is the immutable snapshot of an active bake.
    state.selected = std::move(reconciliation.selected);
    state.techniques = std::move(refreshed);
    state.catalog_dirty = false;
    return true;
}

bool release_gpu_resources(gpu_resources &gpu, const bool teardown_after_runtime_idle = false)
{
    if (gpu.owner != nullptr)
    {
        if (gpu.synchronization_failed && !teardown_after_runtime_idle)
        {
            log_message(reshade::log::level::error, "GPU synchronization failed; resource handles remain quarantined until effect-runtime teardown.");
            return false;
        }
        if (gpu.work_in_flight && !teardown_after_runtime_idle &&
            (gpu.completion_fence == 0 || !gpu.owner->wait(gpu.completion_fence, gpu.fence_value, gpu_wait_timeout_ns)))
        {
            log_message(reshade::log::level::error, "GPU work did not finish during resource release; resource handles remain quarantined.");
            return false;
        }
        if (gpu.target_rtv != 0)
            gpu.owner->destroy_resource_view(gpu.target_rtv);
        if (gpu.identity != 0)
            gpu.owner->destroy_resource(gpu.identity);
        if (gpu.target != 0)
            gpu.owner->destroy_resource(gpu.target);
        if (gpu.readback != 0)
            gpu.owner->destroy_resource(gpu.readback);
        if (gpu.completion_fence != 0)
            gpu.owner->destroy_fence(gpu.completion_fence);
    }
    gpu = {};
    return true;
}

bool create_resources_for_format(
    runtime_state &state,
    const std::uint32_t size,
    const std::uint32_t width,
    const std::uint32_t height,
    const format pixel_format,
    const bool fp16_fallback,
    std::string &error)
{
    device *const device = state.runtime->get_device();
    const resource_usage target_usage = resource_usage::render_target | resource_usage::copy_source | resource_usage::copy_dest;
    if (!device->check_format_support(pixel_format, target_usage))
        return false;

    gpu_resources candidate;
    candidate.owner = device;
    candidate.lattice_size = size;
    candidate.width = width;
    candidate.height = height;
    candidate.pixel_format = pixel_format;
    candidate.fp16_fallback = fp16_fallback;

    const std::vector<lut_baker::float4> identity = lut_baker::make_identity_lattice(size, width, height);
    if (identity.empty())
    {
        error = "Unable to generate the identity lattice.";
        return false;
    }

    subresource_data initial_data {};
    std::vector<std::uint16_t> half_identity;
    if (pixel_format == format::r32g32b32a32_float)
    {
        initial_data.data = const_cast<lut_baker::float4 *>(identity.data());
        initial_data.row_pitch = width * static_cast<std::uint32_t>(sizeof(lut_baker::float4));
    }
    else
    {
        half_identity.resize(identity.size() * 4);
        for (std::size_t index = 0; index < identity.size(); ++index)
        {
            half_identity[index * 4 + 0] = lut_baker::float_to_half(identity[index].r);
            half_identity[index * 4 + 1] = lut_baker::float_to_half(identity[index].g);
            half_identity[index * 4 + 2] = lut_baker::float_to_half(identity[index].b);
            half_identity[index * 4 + 3] = lut_baker::float_to_half(1.0f);
        }
        initial_data.data = half_identity.data();
        initial_data.row_pitch = width * 4u * static_cast<std::uint32_t>(sizeof(std::uint16_t));
    }
    initial_data.slice_pitch = initial_data.row_pitch * height;

    const resource_desc identity_desc(width, height, 1, 1, pixel_format, 1, memory_heap::default_, resource_usage::copy_source);
    const resource_desc target_desc(width, height, 1, 1, pixel_format, 1, memory_heap::default_, target_usage);
    const resource_desc readback_desc(width, height, 1, 1, pixel_format, 1, memory_heap::readback, resource_usage::copy_dest);

    if (!device->create_resource(identity_desc, &initial_data, resource_usage::copy_source, &candidate.identity) ||
        !device->create_resource(target_desc, nullptr, resource_usage::render_target, &candidate.target) ||
        !device->create_resource_view(candidate.target, resource_usage::render_target, resource_view_desc(pixel_format), &candidate.target_rtv) ||
        !device->create_resource(readback_desc, nullptr, resource_usage::copy_dest, &candidate.readback))
    {
        (void)release_gpu_resources(candidate);
        return false;
    }

    if (!device->create_fence(0, fence_flags::none, &candidate.completion_fence))
    {
        error = "The renderer cannot create the GPU completion fence required for safe asynchronous readback.";
        (void)release_gpu_resources(candidate);
        return false;
    }

    device->set_resource_name(candidate.identity, "LUT Baker identity lattice");
    device->set_resource_name(candidate.target, "LUT Baker FP target");
    device->set_resource_name(candidate.readback, "LUT Baker readback");
    device->set_resource_view_name(candidate.target_rtv, "LUT Baker FP target RTV");

    state.gpu = std::exchange(candidate, {});
    return true;
}

bool submit_gpu_work(
    runtime_state &state,
    command_queue *queue,
    const bool has_result,
    std::vector<technique_key> techniques,
    std::string &error)
{
    queue->flush_immediate_command_list();
    ++state.gpu.fence_value;
    if (!queue->signal(state.gpu.completion_fence, state.gpu.fence_value))
    {
        state.gpu.work_in_flight = true;
        state.gpu.synchronization_failed = true;
        error = "The GPU completion fence could not be signaled. The bake target will not be reused; reset the device or restart the game before trying again.";
        return false;
    }

    state.gpu.work_in_flight = true;
    state.submission_pending = true;
    state.submission_has_result = has_result;
    state.submission_fence_value = state.gpu.fence_value;
    state.submission_started = std::chrono::steady_clock::now();
    state.submitted_techniques = std::move(techniques);
    return true;
}

bool ensure_gpu_resources(runtime_state &state, std::string &error)
{
    if (state.gpu.work_in_flight)
    {
        error = state.gpu.synchronization_failed
            ? "The previous GPU submission could not be synchronized. Reset the graphics device or restart the game."
            : "Previous GPU work is still pending.";
        return false;
    }

    const auto layout = lut_baker::choose_lattice_layout(state.request.lattice_size);
    if (layout.first == 0 || layout.second == 0)
    {
        error = "The selected LUT size cannot be represented as a 2D lattice texture.";
        return false;
    }

    if (state.gpu.owner == state.runtime->get_device() &&
        state.gpu.lattice_size == state.request.lattice_size &&
        state.gpu.width == layout.first && state.gpu.height == layout.second &&
        state.gpu.target != 0 && state.gpu.readback != 0 && state.gpu.target_rtv != 0)
        return true;

    if (!release_gpu_resources(state.gpu))
    {
        error = "Previous GPU resources could not be released safely; reset the graphics device or restart the game.";
        return false;
    }
    if (create_resources_for_format(state, state.request.lattice_size, layout.first, layout.second, format::r32g32b32a32_float, false, error))
        return true;

    if (create_resources_for_format(state, state.request.lattice_size, layout.first, layout.second, format::r16g16b16a16_float, true, error))
    {
        state.warning = "RGBA32F render targets are unavailable. This bake uses RGBA16F and has lower precision.";
        log_message(reshade::log::level::warning, state.warning);
        return true;
    }

    if (error.empty())
        error = "The renderer cannot allocate an RGBA32F or RGBA16F render target with copy support.";
    return false;
}

void set_failure(runtime_state &state, const std::string &message)
{
    state.export_pending = false;
    state.capture_execution_events = false;
    state.phase = operation_phase::error;
    state.status = "Export failed";
    state.detail = message;
    state.last_duration_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.request_started).count();
    log_message(reshade::log::level::error, message);
}

bool resolve_requested_techniques(
    runtime_state &state,
    std::vector<technique_entry> &ordered,
    std::string &missing)
{
    ordered.clear();
    technique_selection found;
    for (const technique_entry &entry : state.techniques)
    {
        if (lut_baker::selection_contains_exact(state.requested, entry.key))
        {
            ordered.push_back(entry);
            found.insert(entry.key);
        }
    }

    if (found.size() == state.requested.size())
        return true;

    for (const technique_key &key : state.requested)
    {
        if (found.find(key) == found.end())
        {
            if (!missing.empty())
                missing += ", ";
            missing += technique_label(key);
        }
    }
    return false;
}

bool readback_samples(runtime_state &state, std::vector<lut_baker::float4> &samples, std::string &error)
{
    device *const device = state.gpu.owner;
    subresource_data mapped {};
    if (!device->map_texture_region(state.gpu.readback, 0, nullptr, map_access::read_only, &mapped))
    {
        error = "GPU readback mapping failed.";
        return false;
    }

    const std::uint32_t bytes_per_pixel = state.gpu.pixel_format == format::r32g32b32a32_float ? 16u : 8u;
    if (mapped.data == nullptr || mapped.row_pitch < state.gpu.width * bytes_per_pixel)
    {
        device->unmap_texture_region(state.gpu.readback, 0);
        error = "GPU readback returned an invalid row pitch.";
        return false;
    }

    const std::size_t sample_count = static_cast<std::size_t>(state.request.lattice_size) * state.request.lattice_size * state.request.lattice_size;
    samples.resize(sample_count);
    std::size_t output_index = 0;
    const auto *const base = static_cast<const std::uint8_t *>(mapped.data);

    for (std::uint32_t y = 0; y < state.gpu.height && output_index < sample_count; ++y)
    {
        const std::uint8_t *const row = base + static_cast<std::size_t>(mapped.row_pitch) * y;
        for (std::uint32_t x = 0; x < state.gpu.width && output_index < sample_count; ++x, ++output_index)
        {
            if (state.gpu.pixel_format == format::r32g32b32a32_float)
            {
                std::memcpy(&samples[output_index], row + static_cast<std::size_t>(x) * 16u, sizeof(lut_baker::float4));
            }
            else
            {
                std::uint16_t channels[4] {};
                std::memcpy(channels, row + static_cast<std::size_t>(x) * 8u, sizeof(channels));
                samples[output_index] = {
                    lut_baker::half_to_float(channels[0]),
                    lut_baker::half_to_float(channels[1]),
                    lut_baker::half_to_float(channels[2]),
                    lut_baker::half_to_float(channels[3])
                };
            }
        }
    }

    device->unmap_texture_region(state.gpu.readback, 0);
    if (output_index != sample_count)
    {
        error = "GPU readback did not contain every lattice sample.";
        return false;
    }
    return true;
}

std::string source_buffer_description(const runtime_state &state)
{
    if (!state.source_buffer.valid)
        return "Unavailable";

    std::ostringstream stream;
    stream << state.source_buffer.width << 'x' << state.source_buffer.height << ' '
           << format_name(state.source_buffer.color_format) << " ("
           << format_bit_depth(state.source_buffer.color_format) << " bpc), "
           << color_space_name(state.source_buffer.presentation_color_space);
    return stream.str();
}

std::string bake_buffer_description(const runtime_state &state)
{
    std::ostringstream stream;
    stream << state.gpu.width << 'x' << state.gpu.height << ' '
           << format_name(state.gpu.pixel_format)
           << ", BUFFER_COLOR_SPACE=0 (unknown), linear FP RTV";
    return stream.str();
}

export_result execute_export_job(export_job job) noexcept
{
    export_result result;
    result.identity = job.identity;
    result.technique_count = job.technique_count;
    result.format = job.request.format;
    result.lattice_size = job.request.lattice_size;
    result.warning = std::move(job.initial_warning);

    try
    {
        std::string error;
        if (job.request.format == lut_baker::output_format::rise_tex)
            result.verified_techniques = std::move(job.metadata.techniques);
        // Directory/name were snapshotted at queue time. Collision checks and
        // serialization are CPU-only; never access the runtime from here.
        result.output = lut_baker::make_unique_output_path(job.request.directory, job.request.filename, job.request.format);
        if (result.output.empty())
        {
            result.error = "Unable to choose a non-existing output filename.";
            return result;
        }
        const bool written = job.request.format == lut_baker::output_format::cube
            ? lut_baker::write_cube_atomic(result.output, job.request.lattice_size, job.samples, job.metadata, false, error)
            : lut_baker::write_rise_tex_atomic(result.output, job.request.lattice_size, job.samples, job.request.range, result.rise_metrics, error);
        if (!written)
        {
            result.error = std::string(lut_baker::output_format_name(job.request.format)) + " file write failed: " + error;
            return result;
        }
        if (job.request.format == lut_baker::output_format::rise_tex && result.rise_metrics.clipped_components != 0)
        {
            if (!result.warning.empty())
                result.warning += " ";
            std::ostringstream clipping;
            clipping << std::setprecision(9) << "Explicit Clamp to 0-1 clipped " << result.rise_metrics.clipped_components
                     << " RGB component(s) in " << result.rise_metrics.clipped_samples << " sample(s). Original range: ["
                     << result.rise_metrics.source_minimum << ", " << result.rise_metrics.source_maximum << "].";
            result.warning += clipping.str();
        }
        result.success = true;
    }
    catch (const std::exception &exception)
    {
        result.error = std::string("Unexpected file export failure: ") + exception.what();
    }
    catch (...)
    {
        result.error = "Unexpected file export failure.";
    }
    return result;
}

void start_export_writer(runtime_state &state, const std::vector<technique_key> &ordered)
{
    std::vector<lut_baker::float4> samples;
    std::string error;
    if (!readback_samples(state, samples, error))
    {
        set_failure(state, error);
        return;
    }

    state.identity_metrics_valid = ordered.empty();
    if (state.identity_metrics_valid)
    {
        state.identity_metrics = lut_baker::measure_identity_error(samples, state.request.lattice_size);
        const double tolerance = state.gpu.fp16_fallback ? 5.0e-4 : 1.0e-6;
        if (!std::isfinite(state.identity_metrics.maximum_absolute) || state.identity_metrics.maximum_absolute > tolerance)
        {
            std::ostringstream message;
            message << "GPU identity validation failed: maximum absolute RGB error "
                    << std::setprecision(9) << state.identity_metrics.maximum_absolute
                    << " exceeds " << tolerance << ". No LUT was written.";
            set_failure(state, message.str());
            return;
        }
    }

    lut_baker::cube_metadata metadata;
    metadata.title = std::filesystem::u8path(state.request.filename).stem().u8string();
    metadata.exporter_version = LUT_BAKER_VERSION_STRING;
    metadata.reshade_api = "20 (ReShade 6.8.0 minimum)";
    metadata.graphics_api = graphics_api_name(state.runtime->get_device()->get_api());
    metadata.source_buffer = source_buffer_description(state);
    metadata.bake_buffer = bake_buffer_description(state);
    for (const technique_key &key : ordered)
        metadata.techniques.push_back(technique_label(key));
    metadata.warnings.push_back("The offscreen FP permutation uses bake dimensions/format and color space unknown; BUFFER_* conditional shaders can differ from gameplay.");
    metadata.warnings.push_back("The FP target has no separate sRGB SRV/RTV; SRGBTexture and SRGBWriteEnabled read/write semantics behave linearly and may differ from gameplay.");
    metadata.warnings.push_back("Direct subset rendering emits begin/finish effect events once per technique; other add-ons reacting to those events can alter the bake.");
    metadata.warnings.push_back("Spatial, temporal, depth, random, dither and neighbor-dependent effects cannot be represented faithfully by a 3D LUT.");
    if (state.gpu.fp16_fallback)
        metadata.warnings.push_back("RGBA32F was unavailable, so the GPU lattice and readback used RGBA16F.");

    export_job job;
    job.request = state.request;
    job.technique_count = ordered.size();
    job.identity = ordered.empty();
    job.samples = std::move(samples);
    job.metadata = std::move(metadata);
    if (state.gpu.fp16_fallback)
        job.initial_warning = "RGBA32F was unavailable. This export used RGBA16F and has lower precision.";

    try
    {
        state.writer_future = std::async(std::launch::async, execute_export_job, std::move(job));
        state.writer_pending = true;
        state.phase = operation_phase::writing;
        state.status = std::string("Writing ") + lut_baker::output_format_name(state.request.format);
        state.detail = "The GPU result is valid. File serialization is running on a background worker.";
    }
    catch (const std::exception &exception)
    {
        set_failure(state, std::string("Unable to start the LUT writer: ") + exception.what());
    }
}

bool poll_export_writer(runtime_state &state)
{
    if (!state.writer_pending)
        return false;

    if (state.writer_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        state.phase = operation_phase::writing;
        state.status = std::string("Writing ") + lut_baker::output_format_name(state.request.format);
        return true;
    }

    export_result result;
    try
    {
        result = state.writer_future.get();
    }
    catch (const std::exception &exception)
    {
        result.error = std::string("LUT writer worker failed: ") + exception.what();
    }
    catch (...)
    {
        result.error = "LUT writer worker failed unexpectedly.";
    }
    state.writer_pending = false;

    if (!result.success)
    {
        set_failure(state, result.error.empty() ? "LUT writer failed without an error message." : result.error);
        return true;
    }

    state.export_pending = false;
    state.phase = operation_phase::success;
    state.status = result.identity ? "Identity LUT exported and verified" : "LUT exported";
    state.last_output = result.output;
    state.last_format = result.format;
    state.last_lattice_size = result.lattice_size;
    state.last_technique_count = result.technique_count;
    state.warning = std::move(result.warning);
    state.rise_metrics_valid = result.format == lut_baker::output_format::rise_tex;
    state.rise_metrics = result.rise_metrics;
    state.last_duration_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.request_started).count();

    std::ostringstream detail;
    detail << result.output.u8string() << " | " << lut_baker::output_format_name(result.format)
           << " " << result.lattice_size << "^3 | " << result.technique_count << " technique(s) | "
           << std::fixed << std::setprecision(3) << state.last_duration_seconds << " s";
    state.detail = detail.str();
    if (!state.warning.empty())
        log_message(reshade::log::level::warning, state.warning);
    log_message(reshade::log::level::info, "Exported " + result.output.u8string());
    if (state.rise_metrics_valid)
    {
        const auto &metrics = state.rise_metrics;
        std::ostringstream message;
        message << std::setprecision(9) << "Rise TEX: RGB source range [" << metrics.source_minimum << ", " << metrics.source_maximum
                << "]; range policy " << (state.request.range == lut_baker::range_policy::clamp ? "explicit clamp" : "reject")
                << "; clipped " << metrics.clipped_components << " components in " << metrics.clipped_samples << " samples"
                << "; quantization max/mean/RMS " << metrics.quantization.maximum_absolute << "/"
                << metrics.quantization.mean_absolute << "/" << metrics.quantization.rms;
        log_message(reshade::log::level::info, message.str());
        for (std::size_t index = 0; index < result.verified_techniques.size(); ++index)
            log_message(reshade::log::level::info, "Rise technique " + std::to_string(index + 1) + ": " + result.verified_techniques[index]);
    }
    return true;
}

void process_bake(runtime_state &state, command_queue *present_queue)
{
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if (state.destroyed)
        return;

    if (poll_export_writer(state))
        return;

    if (state.submission_pending)
    {
        if (state.gpu.owner->get_completed_fence_value(state.gpu.completion_fence) < state.submission_fence_value)
        {
            if (state.export_pending && std::chrono::steady_clock::now() - state.submission_started > gpu_submission_timeout)
                set_failure(state, "Timed out while polling submitted GPU work. No LUT was written; resources will remain retained until the fence completes or the device is reset.");
            else if (state.export_pending)
            {
                state.phase = operation_phase::waiting_gpu;
                state.status = state.submission_has_result ? "Waiting for FP readback" : "Waiting for permutation attempt";
            }
            return;
        }

        state.submission_pending = false;
        state.gpu.work_in_flight = false;
        const bool has_result = state.submission_has_result;
        state.submission_has_result = false;
        std::vector<technique_key> completed_techniques = std::move(state.submitted_techniques);
        state.submitted_techniques.clear();

        if (!state.export_pending)
            return;

        if (has_result)
        {
            state.phase = operation_phase::reading;
            state.status = "Reading FP result and writing CUBE";
            start_export_writer(state, completed_techniques);
            return;
        }

        state.phase = operation_phase::compiling;
        std::ostringstream status;
        status << "Offscreen permutation retry ready (attempt " << state.attempts << ')';
        state.status = status.str();
        state.detail = "The next presented frame will retry every selected technique from a fresh identity lattice.";
        return;
    }

    if (!state.export_pending)
        return;

    if (std::chrono::steady_clock::now() - state.request_started > permutation_timeout)
    {
        set_failure(state, "Timed out waiting for every selected technique to compile and execute on the offscreen FP permutation. Check ReShade.log for shader errors.");
        return;
    }

    command_queue *const queue = state.runtime->get_command_queue();
    if (queue == nullptr || queue != present_queue)
    {
        set_failure(state, "The effect runtime graphics queue does not match the presentation queue. This renderer configuration is not supported safely.");
        return;
    }

    if (state.catalog_dirty && !refresh_catalog(state))
    {
        state.phase = operation_phase::compiling;
        state.status = "Waiting for ReShade effect reload";
        return;
    }

    std::vector<technique_entry> ordered;
    std::string missing;
    if (!resolve_requested_techniques(state, ordered, missing))
    {
        set_failure(state, "A selected technique disappeared during the bake: " + missing);
        return;
    }

    std::string error;
    if (!ensure_gpu_resources(state, error))
    {
        set_failure(state, error);
        return;
    }

    command_list *const command_list = queue->get_immediate_command_list();
    if (command_list == nullptr)
    {
        set_failure(state, "The runtime graphics queue has no immediate graphics command list.");
        return;
    }

    command_list->barrier(state.gpu.target, resource_usage::render_target, resource_usage::copy_dest);
    command_list->copy_texture_region(state.gpu.identity, 0, nullptr, state.gpu.target, 0, nullptr);
    command_list->barrier(state.gpu.target, resource_usage::copy_dest, resource_usage::render_target);

    state.capture_execution_events = true;
    state.capture_command_list = command_list;
    state.capture_rtv = state.gpu.target_rtv;
    state.expected_execution.clear();
    for (const technique_entry &entry : ordered)
        state.expected_execution.push_back(entry.handle);
    state.execution_index = 0;
    state.execution_mismatch = false;

    for (const technique_entry &entry : ordered)
        state.runtime->render_technique(entry.handle, command_list, state.gpu.target_rtv, state.gpu.target_rtv);

    state.capture_execution_events = false;
    ++state.attempts;

    if (state.execution_mismatch || state.execution_index != state.expected_execution.size())
    {
        if (!submit_gpu_work(state, queue, false, {}, error))
        {
            set_failure(state, error);
            return;
        }
        state.phase = operation_phase::waiting_gpu;
        std::ostringstream status;
        status << "Compiling offscreen permutation (attempt " << state.attempts << ')';
        state.status = status.str();
        state.detail = "No file will be exported until every selected technique emits the expected render event in exact order.";
        return;
    }

    command_list->barrier(state.gpu.target, resource_usage::render_target, resource_usage::copy_source);
    command_list->copy_texture_region(state.gpu.target, 0, nullptr, state.gpu.readback, 0, nullptr);
    command_list->barrier(state.gpu.target, resource_usage::copy_source, resource_usage::render_target);

    std::vector<technique_key> completed_techniques;
    completed_techniques.reserve(ordered.size());
    for (const technique_entry &entry : ordered)
        completed_techniques.push_back(entry.key);
    if (!submit_gpu_work(state, queue, true, std::move(completed_techniques), error))
    {
        set_failure(state, error);
        return;
    }
    state.phase = operation_phase::waiting_gpu;
    state.status = "Waiting for FP readback";
    state.detail = "GPU work was submitted asynchronously; the CUBE will be written after its completion fence is observed.";
}

void begin_export(runtime_state &state)
{
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if (state.export_pending || state.submission_pending || state.writer_pending)
        return;

    if (state.catalog_dirty)
        refresh_catalog(state);

    lut_baker::export_request request;
    std::string error;
    if (!lut_baker::snapshot_export_request(state.preferences, state.output_filename.data(), output_directory(), request, error))
    {
        state.phase = operation_phase::error;
        state.status = "Invalid output filename";
        state.detail = error;
        return;
    }

    state.requested = state.selected;

    state.request = std::move(request);
    state.export_pending = true;
    state.phase = operation_phase::queued;
    state.status = state.requested.empty() ? "Identity bake queued" : "Bake queued";
    state.detail = "The bake will start after the next successful presentation.";
    state.warning.clear();
    state.last_output.clear();
    state.identity_metrics_valid = false;
    state.rise_metrics_valid = false;
    state.request_started = std::chrono::steady_clock::now();
    state.attempts = 0;
}

void update_source_snapshot(runtime_state &state, swapchain *swapchain)
{
    resource back_buffer = swapchain->get_current_back_buffer();
    if (back_buffer == 0)
        return;

    const resource_desc description = state.runtime->get_device()->get_resource_desc(back_buffer);
    state.source_buffer.valid = true;
    state.source_buffer.width = description.texture.width;
    state.source_buffer.height = description.texture.height;
    state.source_buffer.color_format = description.texture.format;
    state.source_buffer.presentation_color_space = swapchain->get_color_space();
}

void on_finish_present(command_queue *queue, swapchain *swapchain)
{
    std::vector<std::shared_ptr<runtime_state>> states;
    {
        std::lock_guard<std::mutex> lock(s_states_mutex);
        states.reserve(s_states.size());
        for (const auto &pair : s_states)
            states.push_back(pair.second);
    }

    const resource swapchain_back_buffer = swapchain->get_current_back_buffer();
    for (const std::shared_ptr<runtime_state> &state : states)
    {
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        if (state->destroyed || state->runtime->get_device() != swapchain->get_device() ||
            state->runtime->get_current_back_buffer() != swapchain_back_buffer)
            continue;

        update_source_snapshot(*state, swapchain);
        if (state->export_pending || state->submission_pending || state->writer_pending)
            process_bake(*state, queue);
    }
}

void on_render_technique(
    effect_runtime *runtime,
    const effect_technique technique,
    command_list *command_list,
    const resource_view rtv,
    resource_view)
{
    const std::shared_ptr<runtime_state> state = find_state(runtime);
    if (state == nullptr)
        return;

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    if (!state->capture_execution_events)
        return;

    if (command_list != state->capture_command_list || rtv != state->capture_rtv ||
        state->execution_index >= state->expected_execution.size() ||
        technique != state->expected_execution[state->execution_index])
    {
        state->execution_mismatch = true;
        return;
    }

    ++state->execution_index;
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    auto state = std::make_shared<runtime_state>(runtime);
    std::lock_guard<std::mutex> lock(s_states_mutex);
    s_states[runtime] = std::move(state);
    log_message(reshade::log::level::info, "Initialized for an effect runtime.");
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::shared_ptr<runtime_state> state;
    {
        std::lock_guard<std::mutex> lock(s_states_mutex);
        const auto iterator = s_states.find(runtime);
        if (iterator == s_states.end())
            return;
        state = iterator->second;
        s_states.erase(iterator);
    }

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->destroyed = true;
    state->export_pending = false;
    state->capture_execution_events = false;
    // ReShade invokes this callback from runtime reset after it has idled the
    // graphics queue, so even quarantined handles are safe to destroy here.
    (void)release_gpu_resources(state->gpu, true);
    log_message(reshade::log::level::info, "Released effect runtime resources.");
}

void on_reloaded_effects(effect_runtime *runtime)
{
    const std::shared_ptr<runtime_state> state = find_state(runtime);
    if (state == nullptr)
        return;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->catalog_dirty = true;
}

bool on_set_technique_state(effect_runtime *runtime, effect_technique, bool)
{
    const std::shared_ptr<runtime_state> state = find_state(runtime);
    if (state == nullptr)
        return false;

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    // This is a cancellable before-change event. Another add-on can still veto
    // the requested state, so query authoritative states on the next frame.
    state->catalog_dirty = true;
    return false;
}

bool on_reorder_techniques(effect_runtime *runtime, std::size_t, effect_technique *)
{
    const std::shared_ptr<runtime_state> state = find_state(runtime);
    if (state != nullptr)
    {
        std::lock_guard<std::recursive_mutex> lock(state->mutex);
        state->catalog_dirty = true;
    }
    return false;
}

// Semantic colors stay fixed so success/warning/error read the same in every
// ReShade theme; everything else is taken from the active ImGui style.
const ImVec4 color_success(0.40f, 0.85f, 0.50f, 1.0f);
const ImVec4 color_warning(1.00f, 0.72f, 0.25f, 1.0f);
const ImVec4 color_error(1.00f, 0.42f, 0.36f, 1.0f);
const ImVec4 color_busy(0.26f, 0.48f, 0.80f, 1.0f);

bool is_busy(const runtime_state &state)
{
    return state.export_pending || state.submission_pending || state.writer_pending;
}

std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string lattice_label(const std::uint32_t size)
{
    return std::to_string(size) + 'x' + std::to_string(size) + 'x' + std::to_string(size);
}

// Small filled circle aligned with the current text line.
void status_dot(const ImVec4 &color, const char *tooltip = nullptr)
{
    const float size = ImGui::GetFontSize() * 0.5f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float line_height = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(size, line_height));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + size * 0.5f, cursor.y + line_height * 0.5f), size * 0.5f, ImGui::ColorConvertFloat4ToU32(color));
    if (tooltip != nullptr)
        ImGui::SetItemTooltip("%s", tooltip);
}

void disabled_wrapped(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void wrapped_bullet(const char *text)
{
    ImGui::Bullet();
    ImGui::TextWrapped("%s", text);
}

// Left-aligned label column shared by every field so the form lines up.
float field_label_width()
{
    return ImGui::CalcTextSize("File name").x + ImGui::GetFontSize() * 1.5f;
}

void field_label(const char *label)
{
    const float start = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(start + field_label_width());
}

void skip_field_label()
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + field_label_width());
}

void help_marker(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void open_in_explorer(const std::filesystem::path &path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void draw_header(const runtime_state &state)
{
    ImGui::TextDisabled("Game buffer");
    ImGui::SameLine();
    if (state.source_buffer.valid)
        ImGui::TextUnformatted(source_buffer_description(state).c_str());
    else
        ImGui::TextDisabled("waiting for presentation");

    if (ImGui::CollapsingHeader("How it works and limitations"))
    {
        ImGui::Indent();
        wrapped_bullet("Bakes the combined RGB transform of the selected techniques through an offscreen floating-point lattice. Technique states in ReShade are never changed.");
        ImGui::PushStyleColor(ImGuiCol_Text, color_warning);
        wrapped_bullet("A 3D LUT cannot represent spatial, temporal, depth, random, dither or neighbor-dependent processing.");
        ImGui::PopStyleColor();
        wrapped_bullet("The offscreen permutation uses the bake dimensions/format, BUFFER_COLOR_SPACE=0 and no separate sRGB SRV/RTV, so SRGBTexture/SRGBWriteEnabled behave linearly. BUFFER_*-conditional or sRGB-semantic techniques may differ from gameplay, especially in HDR.");
        wrapped_bullet("Other add-ons reacting to effect begin/finish events can change subset rendering.");
        wrapped_bullet("With nothing selected, the bake runs the GPU identity validation test instead.");
        ImGui::Unindent();
    }
}

void draw_technique_list(runtime_state &state)
{
    ImGui::SeparatorText("Techniques");

    const ImGuiStyle &style = ImGui::GetStyle();
    const float buttons_width =
        ImGui::CalcTextSize("Use enabled").x + ImGui::CalcTextSize("Clear").x + ImGui::CalcTextSize("Refresh").x +
        style.FramePadding.x * 6.0f + style.ItemSpacing.x * 3.0f;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - buttons_width, ImGui::GetFontSize() * 8.0f));
    ImGui::InputTextWithHint("##technique_filter", "Filter by effect or technique", state.technique_filter.data(), state.technique_filter.size());
    ImGui::SameLine();
    if (ImGui::Button("Use enabled"))
        state.selected = lut_baker::select_currently_enabled(state.techniques);
    ImGui::SetItemTooltip("Select exactly the techniques currently enabled in ReShade.");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        state.selected.clear();
    ImGui::SetItemTooltip("Deselect every technique.");
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        state.catalog_dirty = true;
        refresh_catalog(state);
    }
    ImGui::SetItemTooltip("Re-read the technique list from ReShade.");

    const std::string filter = to_lower(state.technique_filter.data());
    const float row_height = ImGui::GetFrameHeight();
    const std::size_t visible_rows = std::clamp<std::size_t>(state.techniques.size(), 4, 12);
    const float table_height = row_height * static_cast<float>(visible_rows + 1) + style.CellPadding.y * 2.0f * static_cast<float>(visible_rows + 1);

    const ImGuiTableFlags table_flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    std::size_t shown = 0;
    if (ImGui::BeginTable("##techniques", 5, table_flags, ImVec2(0.0f, table_height)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##bake", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Technique", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (std::size_t index = 0; index < state.techniques.size(); ++index)
        {
            const technique_entry &entry = state.techniques[index];
            if (!filter.empty() && to_lower(technique_label(entry.key)).find(filter) == std::string::npos)
                continue;
            ++shown;

            bool checked = state.selected.find(entry.key) != state.selected.end();
            bool toggled = false;
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

            // The whole row is a click target; the checkbox overlaps it.
            ImGui::TableNextColumn();
            char number[16];
            std::snprintf(number, sizeof(number), "%03zu##row", index + 1);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Selectable(number, checked, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, row_height)))
                toggled = true;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##selected", &checked))
                toggled = !toggled;

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(entry.key.effect.c_str());

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(entry.key.name.c_str());
            if (entry.key.occurrence_count > 1)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%u/%u", entry.key.occurrence + 1, entry.key.occurrence_count);
                ImGui::SetItemTooltip("Instance %u of %u with the same effect and technique name.", entry.key.occurrence + 1, entry.key.occurrence_count);
            }

            ImGui::TableNextColumn();
            status_dot(entry.enabled ? color_success : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                entry.enabled ? "Currently enabled in ReShade" : "Currently disabled in ReShade");

            if (toggled)
            {
                if (state.selected.find(entry.key) != state.selected.end())
                    state.selected.erase(entry.key);
                else
                    state.selected.insert(entry.key);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (state.techniques.empty())
        ImGui::TextDisabled("No techniques available yet. Reload ReShade effects, then press Refresh.");
    else if (shown == 0)
        ImGui::TextDisabled("No technique matches the filter.");
    else
    {
        ImGui::TextDisabled("%zu of %zu selected, baked in ReShade order", state.selected.size(), state.techniques.size());
        if (!filter.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu shown)", shown);
        }
    }
}

// Add new targets here; the Format dropdown is generated from this list.
struct output_format_option
{
    lut_baker::output_format format;
    const char *name;
    const char *short_name; // Used on the bake button.
    const char *extension;
    const char *description;
};

const output_format_option format_options[] = {
    { lut_baker::output_format::cube, "CUBE", "CUBE", ".cube", "Standard 3D LUT. 16, 32 or 64 points per axis, full float precision. Loadable by ReShadeLUTPreview.fx." },
    { lut_baker::output_format::rise_tex, "Monster Hunter Rise", "Rise TEX", ".tex.28", "Native Rise TEX28 LUT. Always 32x32x32 with 8 bits per RGB channel." },
};

const output_format_option &find_format_option(const lut_baker::output_format format)
{
    for (const output_format_option &option : format_options)
        if (option.format == format)
            return option;
    return format_options[0];
}

void set_output_format(runtime_state &state, const lut_baker::output_format next)
{
    if (next == state.preferences.format)
        return;
    const std::string filename = lut_baker::filename_for_format(state.output_filename.data(), state.preferences.format, next);
    if (filename.size() < state.output_filename.size())
    {
        state.output_filename.fill('\0');
        std::copy(filename.begin(), filename.end(), state.output_filename.begin());
    }
    state.preferences.format = next;
}

void draw_output_settings(runtime_state &state)
{
    ImGui::SeparatorText("Output");

    field_label("Format");
    const output_format_option *const current = &find_format_option(state.preferences.format);
    const std::string preview = std::string(current->name) + "  (" + current->extension + ')';
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##output_format", preview.c_str()))
    {
        for (const output_format_option &option : format_options)
        {
            const bool selected = option.format == state.preferences.format;
            if (ImGui::Selectable(option.name, selected))
                set_output_format(state, option.format);
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::SetItemTooltip("%s", option.description);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(option.extension).x);
            ImGui::TextDisabled("%s", option.extension);
        }
        ImGui::EndCombo();
    }

    field_label("LUT size");
    if (state.preferences.format == lut_baker::output_format::cube)
    {
        static const std::uint32_t sizes[] = { 16u, 32u, 64u };
        for (const std::uint32_t size : sizes)
        {
            if (size != sizes[0])
                ImGui::SameLine();
            const std::string label = lattice_label(size);
            if (ImGui::RadioButton(label.c_str(), state.preferences.cube_size == size))
                state.preferences.cube_size = size;
        }
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("32x32x32, 8-bit");
        ImGui::SameLine();
        ImGui::TextDisabled("(fixed)");
        ImGui::SetItemTooltip("Monster Hunter Rise LUTs are always 32x32x32 with 8 bits per RGB channel.");

        field_label("Range");
        bool clamp = state.preferences.rise_range == lut_baker::range_policy::clamp;
        if (ImGui::Checkbox("Clamp to 0-1", &clamp))
            state.preferences.rise_range = clamp ? lut_baker::range_policy::clamp : lut_baker::range_policy::reject;
        help_marker("Rise TEX stores normalized RGB only and no gamma conversion is applied. By default, an export whose RGB leaves 0-1 is rejected. "
            "With clipping, values below 0 or above 1 are discarded and the result reports the original range and affected component/sample counts.");

        skip_field_label();
        disabled_wrapped("TEX cannot be loaded by ReShadeLUTPreview.fx. Verify native loading and the look in-game.");
    }

    field_label("File name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##output_filename", "automatic timestamp", state.output_filename.data(), state.output_filename.size());

    skip_field_label();
    std::string normalized, error;
    if (state.output_filename[0] == '\0')
        ImGui::TextDisabled("Saves as ReShade_LUT_YYYYMMDD_HHMMSS%s", lut_baker::output_extension(state.preferences.format));
    else if (lut_baker::validate_output_filename(state.output_filename.data(), normalized, error, state.preferences.format))
        ImGui::TextDisabled("Saves as %s", normalized.c_str());
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color_error);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }

    const std::filesystem::path directory = output_directory();
    std::error_code exists_error;
    const bool directory_exists = std::filesystem::is_directory(directory, exists_error);
    field_label("Folder");
    ImGui::BeginDisabled(!directory_exists);
    if (ImGui::Button("Open"))
        open_in_explorer(directory);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(directory_exists ? "Open the output folder." : "The folder is created on the first export.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", directory.u8string().c_str());
}

void draw_export_button(runtime_state &state)
{
    const lut_baker::output_format format = state.preferences.format;
    const std::uint32_t size = lut_baker::effective_lattice_size(state.preferences);
    const std::string target = std::string(find_format_option(format).short_name) + ' ' + lattice_label(size);

    std::string label;
    if (state.selected.empty())
        label = "Export identity LUT  (GPU validation)  -  " + target;
    else
        label = "Bake " + std::to_string(state.selected.size()) + (state.selected.size() == 1 ? " technique" : " techniques") + "  -  " + target;
    label += "##export";

    ImGui::Spacing();
    const ImGuiStyle &style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f)))
        begin_export(state);
    ImGui::PopStyleColor(2);
    if (state.selected.empty())
        ImGui::SetItemTooltip("Nothing is selected, so this writes an identity LUT and verifies the GPU round trip.");
}

void draw_progress(const runtime_state &state)
{
    ImGui::Spacing();
    // A negative fraction draws ImGui's indeterminate animation: the number of
    // compile retries and the GPU latency are not known in advance.
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color_busy);
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f), state.status.c_str());
    ImGui::PopStyleColor();
}

void metric_row(const char *label, const char *format, ...)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

void draw_result(runtime_state &state)
{
    if (state.phase == operation_phase::ready)
        return;

    ImGui::SeparatorText("Result");
    if (is_busy(state))
    {
        if (!state.detail.empty())
            ImGui::TextWrapped("%s", state.detail.c_str());
    }
    else if (state.phase == operation_phase::error)
    {
        ImGui::TextColored(color_error, "%s", state.status.c_str());
        if (!state.detail.empty())
            ImGui::TextWrapped("%s", state.detail.c_str());
    }
    else if (state.phase == operation_phase::success)
    {
        ImGui::TextColored(color_success, "%s", state.status.c_str());
        if (ImGui::BeginTable("##result", 2, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, field_label_width() - ImGui::GetStyle().CellPadding.x * 2.0f);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            metric_row("File", "%s", state.last_output.filename().u8string().c_str());
            metric_row("Format", "%s %s", lut_baker::output_format_name(state.last_format), lattice_label(state.last_lattice_size).c_str());
            if (state.last_technique_count == 0)
                metric_row("Techniques", "none (identity)");
            else
                metric_row("Techniques", "%zu", state.last_technique_count);
            metric_row("Time", "%.3f s", state.last_duration_seconds);
            ImGui::EndTable();
        }
    }

    if (!state.warning.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color_warning);
        ImGui::TextWrapped("%s", state.warning.c_str());
        ImGui::PopStyleColor();
    }

    if (state.identity_metrics_valid || state.rise_metrics_valid)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNode("Validation metrics"))
        {
            if (ImGui::BeginTable("##metrics", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                if (state.identity_metrics_valid)
                {
                    const auto &metrics = state.identity_metrics;
                    metric_row("Identity error max/mean/RMS", "%.9g / %.9g / %.9g", metrics.maximum_absolute, metrics.mean_absolute, metrics.rms);
                }
                if (state.rise_metrics_valid)
                {
                    const auto &metrics = state.rise_metrics;
                    metric_row("Source RGB range", "[%.9g, %.9g]", metrics.source_minimum, metrics.source_maximum);
                    metric_row("Clipped components/samples", "%zu / %zu", metrics.clipped_components, metrics.clipped_samples);
                    metric_row("8-bit quantization max/mean/RMS", "%.9g / %.9g / %.9g",
                        metrics.quantization.maximum_absolute, metrics.quantization.mean_absolute, metrics.quantization.rms);
                }
                ImGui::EndTable();
            }
            if (state.rise_metrics_valid)
                disabled_wrapped("Quantization error is measured after the range policy, separately from GPU identity error.");
            ImGui::TreePop();
        }
    }

    if (!state.last_output.empty() && !is_busy(state))
    {
        if (ImGui::Button("Open output folder"))
            open_in_explorer(state.last_output.parent_path());
        ImGui::SameLine();
        if (ImGui::Button("Copy file name"))
            ImGui::SetClipboardText(state.last_output.filename().u8string().c_str());
        ImGui::SetItemTooltip(state.last_format == lut_baker::output_format::cube
            ? "Copy the file name to paste into ReShadeLUTPreview.fx."
            : "Copy the file name to the clipboard.");
    }
}

void draw_overlay(effect_runtime *runtime)
{
    const std::shared_ptr<runtime_state> state = find_state(runtime);
    if (state == nullptr)
    {
        ImGui::TextDisabled("No active ReShade effect runtime is available.");
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    if (state->catalog_dirty && !state->export_pending)
        refresh_catalog(*state);

    draw_header(*state);

    const bool busy = is_busy(*state);
    ImGui::BeginDisabled(busy);
    draw_technique_list(*state);
    draw_output_settings(*state);
    ImGui::EndDisabled();

    if (busy)
        draw_progress(*state);
    else
        draw_export_button(*state);

    draw_result(*state);
}

void register_callbacks()
{
    reshade::register_overlay(nullptr, draw_overlay);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::register_event<reshade::addon_event::reshade_set_technique_state>(on_set_technique_state);
    reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
    reshade::register_event<reshade::addon_event::reshade_reorder_techniques>(on_reorder_techniques);
}

void unregister_callbacks()
{
    reshade::unregister_event<reshade::addon_event::reshade_reorder_techniques>(on_reorder_techniques);
    reshade::unregister_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
    reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(on_set_technique_state);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_overlay(nullptr, draw_overlay);
}
}

extern "C" __declspec(dllexport) const char *NAME = "ReShade LUT Baker";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Bakes selected ReShade techniques in floating point and exports CUBE or native Monster Hunter Rise TEX LUTs.";

BOOL APIENTRY DllMain(HMODULE module, const DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module))
            return FALSE;
        register_callbacks();
        break;
    case DLL_PROCESS_DETACH:
        unregister_callbacks();
        reshade::unregister_addon(module);
        break;
    default:
        break;
    }
    return TRUE;
}
