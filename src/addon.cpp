#include <imgui.h>
#include <reshade.hpp>

#include "bake_control.hpp"
#include "cube_lut.hpp"
#include "png_lut.hpp"
#include "rise_tex.hpp"
#include "runtime_settings.hpp"
#include "technique_catalog.hpp"
#include "technique_preparation.hpp"
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
#include <stdexcept>
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
    cancelled,
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
    lut_baker::quantization_metrics quantized_metrics;
    lut_baker::png_layout png_distribution = lut_baker::png_layout::horizontal;
    lut_baker::png_bit_depth png_depth = lut_baker::png_bit_depth::eight;
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
    lut_baker::quantization_metrics quantized_metrics;
    bool quantized_metrics_valid = false;
    lut_baker::png_layout last_png_distribution = lut_baker::png_layout::horizontal;
    lut_baker::png_bit_depth last_png_depth = lut_baker::png_bit_depth::eight;

    bool export_pending = false;
    technique_selection requested;
    // Immutable until the current bake AND its writer have finished.
    lut_baker::export_request request;
    std::chrono::steady_clock::time_point request_started {};
    std::uint32_t attempts = 0;
    lut_baker::bake_control bake_control;
    std::uint64_t reload_generation = 0;
    lut_baker::settings_backup settings_backup;
    std::string request_preset;

    bool capture_execution_events = false;
    command_list *capture_command_list = nullptr;
    resource_view capture_rtv {};
    std::vector<effect_technique> expected_execution;
    std::size_t execution_index = 0;
    bool execution_mismatch = false;
    effect_technique current_execution {};
    bool current_execution_rendered = false;

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

template <typename Handle>
std::string get_runtime_string(
    effect_runtime *runtime,
    const Handle technique,
    void (effect_runtime::*getter)(Handle, char *, std::size_t *) const)
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

std::string current_preset_path(effect_runtime *runtime)
{
    std::size_t size = 0;
    runtime->get_current_preset_path(nullptr, &size);
    if (size == 0)
        return {};
    std::string result(size, '\0');
    runtime->get_current_preset_path(result.data(), &size);
    if (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
}

// This adapter only reads/writes public CPU-side settings. It never saves a
// preset, changes preprocessor definitions or accesses GPU resources.
class runtime_settings_access
{
public:
    runtime_settings_access(effect_runtime *runtime, const std::uint64_t &generation)
        : runtime_(runtime), generation_(generation) {}

    lut_baker::settings_result capture(lut_baker::runtime_settings &settings, std::string &error)
    {
        settings = {};
        uniforms_.clear();
        techniques_.clear();
        error.clear();
        try
        {
            settings.preset = current_preset_path(runtime_);
            bindings_generation_ = generation_;
            bindings_preset_ = settings.preset;
            runtime_->enumerate_techniques(nullptr, [this, &settings](effect_runtime *runtime, const effect_technique technique) {
                lut_baker::technique_setting entry;
                entry.key.effect = get_runtime_string(runtime, technique, &effect_runtime::get_technique_effect_name);
                entry.key.name = get_runtime_string(runtime, technique, &effect_runtime::get_technique_name);
                entry.enabled = runtime->get_technique_state(technique);
                settings.techniques.push_back(std::move(entry));
                techniques_.push_back(technique);
            });
            if (settings.techniques.empty())
                return lut_baker::settings_result::waiting;

            std::size_t words = 0;
            runtime_->enumerate_uniform_variables(nullptr, [this, &settings, &words](effect_runtime *runtime, const effect_uniform_variable variable) {
                // 'source' uniforms are driven by ReShade or another add-on
                // (time, frame count, input, etc.), not user grading settings.
                std::size_t source_size = 0;
                if (runtime->get_annotation_string_from_uniform_variable(variable, "source", nullptr, &source_size) && source_size > 1)
                    return;

                lut_baker::uniform_setting uniform;
                uniform.effect = get_runtime_string(runtime, variable, &effect_runtime::get_uniform_variable_effect_name);
                uniform.name = get_runtime_string(runtime, variable, &effect_runtime::get_uniform_variable_name);
                format type = format::unknown;
                runtime->get_uniform_variable_type(variable, &type, &uniform.rows, &uniform.columns, &uniform.array_length);
                switch (type)
                {
                case format::r32_typeless: uniform.type = lut_baker::uniform_value_type::boolean; break;
                case format::r32_sint: uniform.type = lut_baker::uniform_value_type::sint; break;
                case format::r32_uint: uniform.type = lut_baker::uniform_value_type::uint; break;
                case format::r32_float: uniform.type = lut_baker::uniform_value_type::real; break;
                case format::r16_sint: uniform.type = lut_baker::uniform_value_type::sint16; break;
                case format::r16_uint: uniform.type = lut_baker::uniform_value_type::uint16; break;
                case format::r16_float: uniform.type = lut_baker::uniform_value_type::real16; break;
                default: throw std::runtime_error("A shader parameter has a type that cannot be backed up.");
                }
                const std::size_t count = lut_baker::uniform_component_count(uniform);
                if (count == 0 || count > lut_baker::settings_word_limit - words)
                    throw std::runtime_error("Shader parameters exceed the 64 MiB settings-backup limit or have invalid dimensions.");
                words += count;
                uniform.words.resize(count);
                switch (uniform.type)
                {
                case lut_baker::uniform_value_type::boolean:
                {
                    const auto values = std::make_unique<bool[]>(count);
                    runtime->get_uniform_value_bool(variable, values.get(), count);
                    for (std::size_t index = 0; index < count; ++index)
                        uniform.words[index] = values[index] ? 1u : 0u;
                    break;
                }
                case lut_baker::uniform_value_type::sint:
                case lut_baker::uniform_value_type::sint16:
                {
                    std::vector<std::int32_t> values(count);
                    runtime->get_uniform_value_int(variable, values.data(), count);
                    std::memcpy(uniform.words.data(), values.data(), count * sizeof(std::uint32_t));
                    break;
                }
                case lut_baker::uniform_value_type::uint:
                case lut_baker::uniform_value_type::uint16:
                    runtime->get_uniform_value_uint(variable, uniform.words.data(), count);
                    break;
                case lut_baker::uniform_value_type::real:
                case lut_baker::uniform_value_type::real16:
                {
                    std::vector<float> values(count);
                    runtime->get_uniform_value_float(variable, values.data(), count);
                    std::memcpy(uniform.words.data(), values.data(), count * sizeof(std::uint32_t));
                    break;
                }
                }
                settings.uniforms.push_back(std::move(uniform));
                uniforms_.push_back(variable);
            });
            if (!lut_baker::validate_runtime_settings(settings, error))
                return lut_baker::settings_result::error;
            return lut_baker::settings_result::ready;
        }
        catch (const std::exception &exception)
        {
            error = std::string("Could not capture live shader settings: ") + exception.what();
            return lut_baker::settings_result::error;
        }
    }

    void set_uniform(const std::size_t index, const lut_baker::uniform_setting &uniform)
    {
        check_bindings();
        const auto variable = uniforms_.at(index);
        const auto count = uniform.words.size();
        switch (uniform.type)
        {
        case lut_baker::uniform_value_type::boolean:
        {
            const auto values = std::make_unique<bool[]>(count);
            for (std::size_t component = 0; component < count; ++component)
                values[component] = uniform.words[component] != 0;
            runtime_->set_uniform_value_bool(variable, values.get(), count);
            break;
        }
        case lut_baker::uniform_value_type::sint:
        case lut_baker::uniform_value_type::sint16:
        {
            std::vector<std::int32_t> values(count);
            std::memcpy(values.data(), uniform.words.data(), count * sizeof(std::uint32_t));
            runtime_->set_uniform_value_int(variable, values.data(), count);
            break;
        }
        case lut_baker::uniform_value_type::uint:
        case lut_baker::uniform_value_type::uint16:
            runtime_->set_uniform_value_uint(variable, uniform.words.data(), count);
            break;
        case lut_baker::uniform_value_type::real:
        case lut_baker::uniform_value_type::real16:
        {
            std::vector<float> values(count);
            std::memcpy(values.data(), uniform.words.data(), count * sizeof(std::uint32_t));
            runtime_->set_uniform_value_float(variable, values.data(), count);
            break;
        }
        }
    }
    void set_technique(const std::size_t index, const bool enabled)
    {
        check_bindings();
        runtime_->set_technique_state(techniques_.at(index), enabled);
    }
    void reorder(const std::vector<std::size_t> &indices)
    {
        check_bindings();
        std::vector<effect_technique> order;
        order.reserve(indices.size());
        for (const std::size_t index : indices)
            order.push_back(techniques_.at(index));
        runtime_->reorder_techniques(order.size(), order.data());
    }

private:
    void check_bindings() const
    {
        if (generation_ != bindings_generation_ || current_preset_path(runtime_) != bindings_preset_)
            throw std::runtime_error("The preset or effect list changed during settings recovery. No further values were applied to the new context.");
    }
    effect_runtime *runtime_;
    const std::uint64_t &generation_;
    std::uint64_t bindings_generation_ = 0;
    std::string bindings_preset_;
    std::vector<effect_uniform_variable> uniforms_;
    std::vector<effect_technique> techniques_;
};

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
            ? "The previous bake could not be synchronized with the GPU. Restart the game (or reset the graphics device) before baking again."
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
        state.warning = "RGBA32F (32-bit float) is unavailable on this renderer. This bake uses RGBA16F (16-bit float) and has lower precision.";
        log_message(reshade::log::level::warning, state.warning);
        return true;
    }

    if (error.empty())
        error = "The renderer cannot allocate an RGBA32F or RGBA16F render target with copy support.";
    return false;
}

void set_failure(runtime_state &state, const std::string &message)
{
    if (!state.settings_backup.pending())
        state.settings_backup.clear();
    state.bake_control.stop();
    state.export_pending = false;
    state.capture_execution_events = false;
    state.phase = operation_phase::error;
    state.status = "Export failed";
    state.detail = message;
    state.last_duration_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.request_started).count();
    log_message(reshade::log::level::error, message);
}

bool capture_settings_candidate(runtime_state &state)
{
    if (state.requested.empty())
        return true;
    runtime_settings_access access(state.runtime, state.reload_generation);
    lut_baker::runtime_settings settings;
    std::string error;
    const auto result = access.capture(settings, error);
    if (result == lut_baker::settings_result::waiting)
    {
        state.phase = operation_phase::compiling;
        state.status = "Waiting for live shader settings";
        return false;
    }
    if (result == lut_baker::settings_result::ready && settings.preset != state.request_preset)
    {
        set_failure(state, "The active preset changed before the bake could start. No LUT was written. Start a new export for the current preset.");
        return false;
    }
    if (result == lut_baker::settings_result::error || !state.settings_backup.capture(std::move(settings)))
    {
        set_failure(state, "Live shader settings could not be backed up. No LUT was written. " +
            (error.empty() ? "A previous compilation still needs settings recovery; wait for ReShade to finish." : error));
        return false;
    }
    return true;
}

void restore_settings_when_ready(runtime_state &state)
{
    if (!state.settings_backup.can_restore(state.reload_generation))
        return;
    const auto saved = state.settings_backup.retain();
    runtime_settings_access access(state.runtime, state.reload_generation);
    std::string error;
    lut_baker::settings_result result;
    try
    {
        result = lut_baker::restore_runtime_settings(*saved, access, error);
    }
    catch (const std::exception &exception)
    {
        result = lut_baker::settings_result::error;
        error = exception.what();
    }
    if (!state.settings_backup.pending() || state.settings_backup.retain() != saved)
        return; // A reentrant context-change callback already cancelled recovery.
    if (result == lut_baker::settings_result::waiting)
        return; // Retain the backup if restoring an enabled state queued creation.
    state.settings_backup.clear();
    state.catalog_dirty = true;
    if (result == lut_baker::settings_result::error)
    {
        set_failure(state, "Live settings recovery failed. No LUT was written. " + error +
            " Check ReShade.log and reload any failed shader before retrying; failed effects are not forced to run.");
        return;
    }
    log_message(reshade::log::level::info, "Restored live shader parameters, technique states and order after compilation. The preset was not saved.");
    if (!state.export_pending)
        state.detail += " Live shader parameters, technique states and order have now been restored.";
}

void check_bake_timeout(runtime_state &state)
{
    // Check independently of the submission/reload branches (and from the UI
    // too, if this runtime stops receiving matching finish_present callbacks).
    // A validated CPU writer is intentionally outside the compilation deadline.
    if (state.export_pending && !state.writer_pending &&
        state.bake_control.timed_out(std::chrono::steady_clock::now()))
    {
        std::string message = "Timed out waiting for the offscreen bake. No LUT was written. A selected shader probably failed to compile for the bake: check ReShade's Log tab or ReShade.log.";
        if (state.bake_control.waiting())
            message += " Waiting technique: " + technique_label(state.bake_control.waiting_key()) + '.';
        if (state.settings_backup.pending())
            message += " The settings backup is retained for recovery when ReShade finishes compiling. Another export remains blocked until recovery or an explicit effect reload.";
        set_failure(state, message);
    }
}

void abort_export(runtime_state &state)
{
    if (!lut_baker::can_abort_export(state.export_pending, state.writer_pending))
        return;
    state.bake_control.stop();
    state.export_pending = false;
    state.capture_execution_events = false;
    state.phase = operation_phase::cancelled;
    state.status = "Export cancelled";
    state.detail = "No LUT was written. Shader compilation already queued in ReShade cannot be interrupted and may finish in the background.";
    if (state.submission_pending)
        state.detail += " Submitted GPU work will drain through its fence before another export is allowed.";
    if (state.settings_backup.pending())
        state.detail += " Unsaved settings will be restored when ReShade finishes compiling; another export stays blocked until recovery.";
    else
        state.settings_backup.clear();
    state.last_duration_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.request_started).count();
    // Leave request/requested and all submission/fence/resource state intact.
    log_message(reshade::log::level::info, state.status + ". " + state.detail);
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
        error = "GPU readback failed (could not map the result). No LUT was written.";
        return false;
    }

    const std::uint32_t bytes_per_pixel = state.gpu.pixel_format == format::r32g32b32a32_float ? 16u : 8u;
    if (mapped.data == nullptr || mapped.row_pitch < state.gpu.width * bytes_per_pixel)
    {
        device->unmap_texture_region(state.gpu.readback, 0);
        error = "GPU readback returned an invalid row pitch. No LUT was written.";
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
        error = "GPU readback did not contain every lattice sample. No LUT was written.";
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
    result.png_distribution = job.request.png_distribution;
    result.png_depth = job.request.png_depth;
    result.warning = std::move(job.initial_warning);

    try
    {
        std::string error;
        if (!lut_baker::check_export_space(job.request, error))
        {
            result.error = error;
            return result;
        }
        if (job.request.format != lut_baker::output_format::cube)
            result.verified_techniques = std::move(job.metadata.techniques);
        // Directory/name were snapshotted at queue time. Collision checks and
        // serialization are CPU-only; never access the runtime from here.
        result.output = lut_baker::make_unique_output_path(job.request.directory, job.request.filename, job.request.format);
        if (result.output.empty())
        {
            result.error = "Unable to choose a non-existing output filename.";
            return result;
        }
        bool written = false;
        switch (job.request.format)
        {
        case lut_baker::output_format::cube:
            written = lut_baker::write_cube_atomic(result.output, job.request.lattice_size, job.samples, job.metadata, false, error);
            break;
        case lut_baker::output_format::rise_tex:
            written = lut_baker::write_rise_tex_atomic(result.output, job.request.lattice_size, job.samples, job.request.range, result.quantized_metrics, error);
            break;
        case lut_baker::output_format::png:
            written = lut_baker::write_png_lut_atomic(result.output, job.request.lattice_size, job.samples,
                job.request.png_distribution, job.request.range, job.metadata, result.quantized_metrics, error, job.request.png_depth);
            break;
        }
        if (!written)
        {
            result.error = std::string(lut_baker::output_format_name(job.request.format)) + " file write failed: " + error;
            return result;
        }
        if (job.request.format != lut_baker::output_format::cube && result.quantized_metrics.clipped_components != 0)
        {
            if (!result.warning.empty())
                result.warning += " ";
            std::ostringstream clipping;
            clipping << std::setprecision(9) << "Clamp to 0-1 clipped " << result.quantized_metrics.clipped_components
                     << " RGB value(s) in " << result.quantized_metrics.clipped_samples << " sample(s). Original range: ["
                     << result.quantized_metrics.source_minimum << ", " << result.quantized_metrics.source_maximum << "].";
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
        job.initial_warning = "RGBA32F (32-bit float) was unavailable on this renderer. This export used RGBA16F (16-bit float) and has lower precision.";

    try
    {
        state.writer_future = std::async(std::launch::async, execute_export_job, std::move(job));
        state.writer_pending = true;
        state.phase = operation_phase::writing;
        state.status = std::string("Writing ") + lut_baker::output_format_name(state.request.format) + " file";
        state.detail = "The GPU result is valid. The file is being written in the background.";
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
        state.status = std::string("Writing ") + lut_baker::output_format_name(state.request.format) + " file";
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

    state.bake_control.stop();
    state.export_pending = false;
    state.phase = operation_phase::success;
    state.status = result.identity ? "Identity LUT exported and verified" : "LUT exported";
    state.last_output = result.output;
    state.last_format = result.format;
    state.last_lattice_size = result.lattice_size;
    state.last_technique_count = result.technique_count;
    state.warning = std::move(result.warning);
    state.quantized_metrics_valid = result.format != lut_baker::output_format::cube;
    state.quantized_metrics = result.quantized_metrics;
    state.last_png_distribution = result.png_distribution;
    state.last_png_depth = result.png_depth;
    state.last_duration_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.request_started).count();

    std::ostringstream detail;
    detail << result.output.u8string() << " | " << lut_baker::output_format_name(result.format)
           << " " << result.lattice_size << "^3 | " << result.technique_count << " technique(s) | "
           << std::fixed << std::setprecision(3) << state.last_duration_seconds << " s";
    state.detail = detail.str();
    if (result.format == lut_baker::output_format::png)
        state.detail += std::string(" | ") + lut_baker::png_layout_name(result.png_distribution) + " | " +
            std::to_string(static_cast<unsigned int>(result.png_depth)) + " bits per channel";
    if (!state.warning.empty())
        log_message(reshade::log::level::warning, state.warning);
    log_message(reshade::log::level::info, "Exported " + result.output.u8string());
    if (state.quantized_metrics_valid)
    {
        const auto &metrics = state.quantized_metrics;
        std::ostringstream message;
        message << std::setprecision(9) << lut_baker::output_format_name(result.format) << ": RGB source range [" << metrics.source_minimum << ", " << metrics.source_maximum
                << "]; range policy " << (state.request.range == lut_baker::range_policy::clamp ? "explicit clamp" : "reject")
                << "; clipped " << metrics.clipped_components << " components in " << metrics.clipped_samples << " samples"
                << "; quantization max/mean/RMS " << metrics.quantization.maximum_absolute << "/"
                << metrics.quantization.mean_absolute << "/" << metrics.quantization.rms;
        if (result.format == lut_baker::output_format::png)
            message << "; " << static_cast<unsigned int>(result.png_depth) << " bits per channel";
        log_message(reshade::log::level::info, message.str());
        for (std::size_t index = 0; index < result.verified_techniques.size(); ++index)
            log_message(reshade::log::level::info, std::string(lut_baker::output_format_name(result.format)) + " technique " + std::to_string(index + 1) + ": " + result.verified_techniques[index]);
    }
    return true;
}

// Submit, compilation and fence polling share one stable progress label.
void set_compile_progress(runtime_state &state)
{
    if (state.bake_control.initializing())
    {
        state.status = "Preparing shader resources";
        state.detail = "ReShade is initializing the shader's resources before the floating-point bake. Its original disabled state has already been restored. Technique: " +
            technique_label(state.bake_control.waiting_key());
        return;
    }
    std::ostringstream status;
    status << "Compiling shaders (attempt " << state.attempts << ')';
    state.status = status.str();
    state.detail = "ReShade is compiling the offscreen version of the selected shaders. Nothing is written until every selected technique runs in the expected order.";
    if (state.bake_control.waiting())
        state.detail += " Technique: " + technique_label(state.bake_control.waiting_key());
}

void process_bake(runtime_state &state, command_queue *present_queue)
{
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if (state.destroyed)
        return;

    restore_settings_when_ready(state);

    if (poll_export_writer(state))
        return;

    check_bake_timeout(state);

    if (state.submission_pending)
    {
        if (state.gpu.owner->get_completed_fence_value(state.gpu.completion_fence) < state.submission_fence_value)
        {
            if (state.export_pending && std::chrono::steady_clock::now() - state.submission_started > gpu_submission_timeout)
                set_failure(state, "Timed out while polling submitted GPU work. No LUT was written; resources will remain retained until the fence completes or the device is reset.");
            else if (state.export_pending)
            {
                state.phase = operation_phase::waiting_gpu;
                if (state.submission_has_result)
                    state.status = "Waiting for GPU readback";
                else
                    set_compile_progress(state);
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
            state.status = "Reading GPU result";
            start_export_writer(state, completed_techniques);
            return;
        }

        state.phase = operation_phase::compiling;
        set_compile_progress(state);
        return;
    }

    if (!state.export_pending)
        return;

    if (state.settings_backup.pending())
    {
        state.phase = operation_phase::compiling;
        set_compile_progress(state);
        return;
    }

    if (!state.bake_control.can_attempt(state.reload_generation))
    {
        state.phase = operation_phase::compiling;
        set_compile_progress(state);
        return;
    }

    command_queue *const queue = state.runtime->get_command_queue();
    if (queue == nullptr || queue != present_queue)
    {
        set_failure(state, "The effect runtime graphics queue does not match the presentation queue. This renderer configuration is not supported safely.");
        return;
    }

    // Enumeration is empty while ReShade is loading. Refresh before touching
    // handles on EACH real attempt, including the post-compilation verification.
    if (!refresh_catalog(state))
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

    if (!capture_settings_candidate(state))
        return;

    // Do not request an offscreen permutation before ReShade has created the
    // default permutation's shared resources. This is required even if the
    // shader was parsed/compiled and appears in the catalog while disabled.
    // Check on every attempt: a real effect reload can invalidate earlier work.
    const std::uint64_t preparation_generation = state.reload_generation;
    for (const technique_entry &entry : ordered)
    {
        state.settings_backup.arm(preparation_generation);
        const auto preparation = lut_baker::prepare_default_technique(
            [&state, &entry] { return state.runtime->get_technique_state(entry.handle); },
            [&state, &entry](const bool enabled) { state.runtime->set_technique_state(entry.handle, enabled); },
            [&state] {
                bool available = false;
                state.runtime->enumerate_techniques(nullptr, [&available](effect_runtime *, effect_technique) { available = true; });
                return available;
            });
        if (preparation != lut_baker::technique_preparation::waiting)
            state.settings_backup.disarm();
        if (preparation == lut_baker::technique_preparation::restore_failed)
        {
            set_failure(state, "ReShade did not restore the disabled state of " + technique_label(entry.key) +
                ". No LUT was written. Another add-on may have blocked the change; disable this technique in ReShade and check the other add-ons before retrying.");
            return;
        }
        if (preparation == lut_baker::technique_preparation::rejected)
        {
            set_failure(state, "ReShade could not prepare the resources of " + technique_label(entry.key) +
                ". No LUT was written. Check ReShade.log for a shader/resource error or another add-on blocking initialization, then reload the effect before retrying.");
            return;
        }
        if (preparation == lut_baker::technique_preparation::waiting)
        {
            if (!state.bake_control.wait_for_initialization(entry.key, preparation_generation))
            {
                set_failure(state, "The resources of " + technique_label(entry.key) +
                    " are still unavailable after initialization. No LUT was written and no further retry will be made. Check ReShade.log and reload the effect before retrying.");
                return;
            }
            log_message(reshade::log::level::info, "Waiting for default resource initialization of " + technique_label(entry.key) +
                "; its disabled state was restored before rendering. Offscreen rendering will wait until those resources are ready.");
            state.phase = operation_phase::compiling;
            set_compile_progress(state);
            return;
        }
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

    state.bake_control.begin_attempt();
    const std::uint64_t attempt_reload_generation = state.reload_generation;
    state.capture_execution_events = true;
    state.capture_command_list = command_list;
    state.capture_rtv = state.gpu.target_rtv;
    state.expected_execution.clear();
    for (const technique_entry &entry : ordered)
        state.expected_execution.push_back(entry.handle);
    state.execution_index = 0;
    state.execution_mismatch = false;

    const technique_entry *not_rendered = nullptr;
    for (const technique_entry &entry : ordered)
    {
        const std::size_t before = state.execution_index;
        state.current_execution = entry.handle;
        state.current_execution_rendered = false;
        state.settings_backup.arm(attempt_reload_generation);
        state.runtime->render_technique(entry.handle, command_list, state.gpu.target_rtv, state.gpu.target_rtv);
        if (state.current_execution_rendered)
            state.settings_backup.disarm();
        // Stop at the first missing event. Later techniques must never run on
        // a partial chain or enqueue more compilation while ReShade is loading.
        if (state.execution_mismatch || state.execution_index != before + 1)
        {
            not_rendered = &entry;
            break;
        }
    }

    state.capture_execution_events = false;
    if (!state.settings_backup.pending())
        state.settings_backup.clear();
    ++state.attempts;

    if (state.execution_mismatch || state.execution_index != state.expected_execution.size())
    {
        if (!submit_gpu_work(state, queue, false, {}, error))
        {
            set_failure(state, error);
            return;
        }
        if (state.execution_mismatch || not_rendered == nullptr)
        {
            set_failure(state, "The techniques did not execute in ReShade's order. No LUT was written. Another add-on may be interfering.");
            return;
        }
        if (!state.bake_control.wait_for_compilation(not_rendered->key, attempt_reload_generation))
        {
            set_failure(state, "A selected technique did not execute after ReShade compiled its offscreen version: " +
                technique_label(not_rendered->key) + ". The shader may fail to compile or disable itself for the floating-point target. "
                "No LUT was written and no retry will be made. Check ReShade.log; your preset was not changed.");
            return;
        }
        log_message(reshade::log::level::info, "Waiting for offscreen compilation of " + technique_label(not_rendered->key) +
            "; verification will resume only after ReShade's effects-reloaded event.");
        state.phase = operation_phase::waiting_gpu;
        set_compile_progress(state);
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
    state.status = "Waiting for GPU readback";
    state.detail = "GPU work was submitted asynchronously; the LUT will be written after its completion fence is observed.";
}

void begin_export(runtime_state &state)
{
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if (state.export_pending || state.submission_pending || state.writer_pending || state.settings_backup.pending())
        return;

    if (state.catalog_dirty && !refresh_catalog(state) && !state.selected.empty())
    {
        state.phase = operation_phase::error;
        state.status = "ReShade is still loading effects";
        state.detail = "No bake was queued. Wait for ReShade to finish loading effects, then try again.";
        return;
    }

    bool performance_mode = false; // ReShade defaults to normal mode if absent.
    (void)reshade::get_config_value(state.runtime, "GENERAL", "PerformanceMode", performance_mode);
    if (!state.selected.empty() && performance_mode)
    {
        state.phase = operation_phase::error;
        state.status = "Performance mode is not supported for grading bakes";
        state.detail = "No bake was queued. Performance mode compiles saved parameters as shader constants, so unsaved settings cannot be restored reliably. Turn Performance mode off and let ReShade reload before baking. Identity exports remain available.";
        log_message(reshade::log::level::error, state.detail);
        return;
    }

    lut_baker::export_request request;
    std::string error;
    if (!lut_baker::snapshot_export_request(state.preferences, state.output_filename.data(), output_directory(), request, error))
    {
        state.phase = operation_phase::error;
        state.status = "Invalid export settings";
        state.detail = error;
        log_message(reshade::log::level::error, error);
        return;
    }
    if (!lut_baker::check_export_space(request, error))
    {
        state.phase = operation_phase::error;
        state.status = "Export could not start";
        state.detail = error;
        log_message(reshade::log::level::error, error);
        return;
    }

    state.requested = state.selected;
    state.request_preset = state.requested.empty() ? std::string() : current_preset_path(state.runtime);

    state.request = std::move(request);
    state.export_pending = true;
    state.phase = operation_phase::queued;
    state.status = state.requested.empty() ? "Identity bake queued" : "Bake queued";
    state.detail = "The bake will start after the next successful presentation.";
    state.warning.clear();
    state.last_output.clear();
    state.identity_metrics_valid = false;
    state.quantized_metrics_valid = false;
    state.request_started = std::chrono::steady_clock::now();
    state.attempts = 0;
    state.bake_control.start(state.request_started);
    log_message(reshade::log::level::info, "Queued " + std::string(lut_baker::output_format_name(state.request.format)) +
        " " + std::to_string(state.request.lattice_size) + "^3 bake of " + std::to_string(state.requested.size()) + " technique(s).");
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
        if (state->export_pending || state->submission_pending || state->writer_pending || state->settings_backup.pending())
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

    if (command_list == state->capture_command_list && rtv == state->capture_rtv && technique == state->current_execution)
        state->current_execution_rendered = true;

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
    state->bake_control.stop();
    state->export_pending = false;
    state->capture_execution_events = false;
    state->settings_backup.clear(); // Handles/settings belong to the old device.
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
    ++state->reload_generation;
    state->catalog_dirty = true;
    if (state->settings_backup.pending())
    {
        // ReShade also emits this event immediately after destroying a catalog.
        // Do not apply an old backup to newly replaced effects by name alone.
        bool available = false;
        runtime->enumerate_techniques(nullptr, [&available](effect_runtime *, effect_technique) { available = true; });
        if (!available)
        {
            state->settings_backup.clear();
            set_failure(*state, "The effect list was explicitly reloaded/replaced before settings recovery completed. No LUT was written. The old backup was discarded to avoid applying it to different effects; configure the reloaded effects before retrying.");
            return;
        }
        restore_settings_when_ready(*state);
    }
}

void on_set_current_preset_path(effect_runtime *runtime, const char *path)
{
    const auto state = find_state(runtime);
    if (state == nullptr)
        return;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    const std::string preset = path != nullptr ? path : "";
    const bool active_changed = state->export_pending && !state->writer_pending && !state->request_preset.empty() && preset != state->request_preset;
    const bool recovery_changed = state->settings_backup.pending() && preset != state->settings_backup.saved().preset;
    if (active_changed || recovery_changed)
    {
        state->settings_backup.clear();
        set_failure(*state, "The active preset changed during the bake/settings recovery. No LUT was written. Settings from the previous preset were not applied to the new one; start a new export for the current preset.");
    }
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
    return state.export_pending || state.submission_pending || state.writer_pending || state.settings_backup.pending();
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
    ImGui::SetItemTooltip("Back buffer the game is presenting: resolution, format, bit depth and color space. Reference only; the bake runs on its own offscreen target.");
    ImGui::SameLine();
    if (state.source_buffer.valid)
        ImGui::TextUnformatted(source_buffer_description(state).c_str());
    else
        ImGui::TextDisabled("waiting for presentation");

    if (ImGui::CollapsingHeader("How it works and limitations"))
    {
        ImGui::Indent();
        wrapped_bullet("Bakes the combined color transform of the selected techniques into a single 3D LUT. It runs on an offscreen floating-point target. Disabled techniques are briefly enabled to prepare their resources, then restored before any rendering. The preset is not saved or changed.");
        wrapped_bullet("Keeps unsaved shader parameters, technique states and order in memory while ReShade compiles. It restores and verifies them before continuing the bake. Performance mode must be off for grading exports.");
        ImGui::PushStyleColor(ImGuiCol_Text, color_warning);
        wrapped_bullet("A 3D LUT maps color to color only. Spatial, temporal, depth-based, random or dithered effects (blur, sharpening, bloom, film grain, vignette...) cannot be captured.");
        ImGui::PopStyleColor();
        wrapped_bullet("The offscreen target uses the bake's own size and format, with BUFFER_COLOR_SPACE = 0 and no sRGB conversion (SRGBTexture / SRGBWriteEnabled act as linear). Shaders that depend on those can differ from gameplay, especially in HDR.");
        wrapped_bullet("Other add-ons that hook effect begin/finish events can alter the bake.");
        wrapped_bullet("With nothing selected, the bake exports an identity LUT (no color change) and validates the GPU round trip.");
        ImGui::Unindent();
    }
}

void draw_technique_list(runtime_state &state)
{
    ImGui::SeparatorText("Techniques");

    const ImGuiStyle &style = ImGui::GetStyle();
    const float buttons_width =
        ImGui::CalcTextSize("Select active").x +ImGui::CalcTextSize("Clear").x + ImGui::CalcTextSize("Refresh").x +
        style.FramePadding.x * 6.0f + style.ItemSpacing.x * 3.0f;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - buttons_width, ImGui::GetFontSize() * 8.0f));
    ImGui::InputTextWithHint("##technique_filter", "Filter by effect or technique", state.technique_filter.data(), state.technique_filter.size());
    ImGui::SameLine();
    if (ImGui::Button("Select active"))
        state.selected = lut_baker::select_currently_enabled(state.techniques);
    ImGui::SetItemTooltip("Select exactly the techniques currently enabled in ReShade and deselect the rest.");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        state.selected.clear();
    ImGui::SetItemTooltip("Deselect everything. An empty selection exports an identity LUT.");
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
        ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed);
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
                entry.enabled ? "Enabled in ReShade" : "Disabled in ReShade (can still be baked)");

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

// List common formats first, then game-specific targets. This list builds the dropdown.
struct output_format_option
{
    lut_baker::output_format format;
    const char *name;
    const char *short_name; // Used on the bake button.
    const char *extension;
    bool game_specific;
    const char *description;
};

const output_format_option format_options[] = {
    { lut_baker::output_format::cube, "CUBE", "CUBE", ".cube", false, "A 3D lookup table (.cube). Float values are preserved, including values outside 0-1. Choose 16, 32, 64, 128 or a custom size from 2 to 128." },
    { lut_baker::output_format::png, "PNG", "PNG", ".png", false, "A 3D lookup table stored in a PNG image. Choose 8 or 16 bits per channel, with values in 0-1. Choose Horizontal strip or Square tiles. The reader must support the layout and bit depth." },
    { lut_baker::output_format::rise_tex, "Monster Hunter Rise", "Rise TEX", ".tex.28", true, "Native Monster Hunter Rise LUT (TEX v28). Fixed 32x32x32, 8 bits per channel; values limited to 0-1." },
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
        bool first_group = true;
        bool previous_game_specific = false;
        for (const output_format_option &option : format_options)
        {
            if (first_group || option.game_specific != previous_game_specific)
            {
                ImGui::SeparatorText(option.game_specific ? "Games" : "Common formats");
                first_group = false;
                previous_game_specific = option.game_specific;
            }
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
    else
        ImGui::SetItemTooltip("File format of the exported LUT. Hover a format in the list for its details.");

    const bool is_png = state.preferences.format == lut_baker::output_format::png;
    if (is_png)
    {
        field_label("Layout");
        const bool square = state.preferences.png_distribution == lut_baker::png_layout::square;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##png_layout", lut_baker::png_layout_name(state.preferences.png_distribution)))
        {
            if (ImGui::Selectable("Horizontal strip", !square))
                state.preferences.png_distribution = lut_baker::png_layout::horizontal;
            if (!square)
                ImGui::SetItemDefaultFocus();
            ImGui::SetItemTooltip("One row of slices, for example 4096x64 at size 64.");
            if (ImGui::Selectable("Square tiles", square))
                state.preferences.png_distribution = lut_baker::png_layout::square;
            if (square)
                ImGui::SetItemDefaultFocus();
            ImGui::SetItemTooltip("A square grid of slices, for example 512x512 at size 64. Only sizes 16 and 64. This is not Hald layout.");
            ImGui::EndCombo();
        }
        else
            ImGui::SetItemTooltip("How the LUT slices are arranged in the PNG image. The reader must expect the same layout.");

        field_label("Bit depth");
        const bool sixteen = state.preferences.png_depth == lut_baker::png_bit_depth::sixteen;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##png_bit_depth", sixteen ? "16-bit" : "8-bit"))
        {
            if (ImGui::Selectable("8-bit", !sixteen))
                state.preferences.png_depth = lut_baker::png_bit_depth::eight;
            if (!sixteen)
                ImGui::SetItemDefaultFocus();
            ImGui::SetItemTooltip("Default, for ReShade compatibility. ReShade 6.8.0 loads PNG samples as 8 bits.");
            if (ImGui::Selectable("16-bit", sixteen))
                state.preferences.png_depth = lut_baker::png_bit_depth::sixteen;
            if (sixteen)
                ImGui::SetItemDefaultFocus();
            ImGui::SetItemTooltip("More precision for applications that support 16-bit PNG. ReShade 6.8.0 still reduces it to 8 bits when loading.");
            ImGui::EndCombo();
        }
        else
            ImGui::SetItemTooltip("Bits per RGB channel. These are integer samples, not floating point.");
    }
    field_label("LUT size");
    if (state.preferences.format != lut_baker::output_format::rise_tex)
    {
        static const std::uint32_t sizes[] = { 16u, 32u, 64u, 128u };
        static const char *const tips[] = {
            "Smallest files. Suitable for simple grading.",
            "More samples than 16, with relatively small files.",
            "Recommended for most grading. Good balance between accuracy and file size.",
            "Usually overkill. Eight times as many samples as 64. Larger files and higher memory use."
        };
        for (const std::uint32_t size : sizes)
        {
            if (size != sizes[0])
                ImGui::SameLine();
            const bool unavailable = is_png && state.preferences.png_distribution == lut_baker::png_layout::square && size != 16 && size != 64;
            ImGui::BeginDisabled(unavailable);
            const std::string label = std::to_string(size) + u8"\u00B3";
            const bool selected = is_png ? state.preferences.png_size == size : !state.preferences.cube_custom && state.preferences.cube_size == size;
            if (ImGui::RadioButton(label.c_str(), selected))
            {
                if (is_png)
                    state.preferences.png_size = size;
                else
                {
                    state.preferences.cube_size = size;
                    state.preferences.cube_custom = false;
                }
            }
            ImGui::EndDisabled();
            const std::size_t tip_index = size == 16 ? 0u : size == 32 ? 1u : size == 64 ? 2u : 3u;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (unavailable)
                    ImGui::SetTooltip("Square tiles only supports sizes 16 and 64. Choose one of those, or switch to Horizontal strip.");
                else if (is_png && size == 128)
                    ImGui::SetTooltip("%s\nThe horizontal strip is 16384 pixels wide. The reader must support that width.", tips[tip_index]);
                else
                    ImGui::SetTooltip("%s", tips[tip_index]);
            }
        }
        if (!is_png)
        {
            ImGui::SameLine();
            if (ImGui::RadioButton("Custom", state.preferences.cube_custom))
                state.preferences.cube_custom = true;
            ImGui::SetItemTooltip("Choose an integer from 2 to 128 points per axis. Other applications may have their own size limits.");
            if (state.preferences.cube_custom)
            {
                field_label("Custom size");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::InputInt("##custom_cube_size", &state.preferences.custom_cube_size);
                ImGui::SetItemTooltip("Allowed range: 2 to 128. Values outside the range are rejected before baking.");
            }
        }
        help_marker("Points per color axis. More points can reduce interpolation error, but increase file size and memory use. The maximum is 128.");
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(u8"32\u00B3, 8-bit");
        ImGui::SameLine();
        ImGui::TextDisabled("(fixed)");
        ImGui::SetItemTooltip("Monster Hunter Rise LUTs are always 32x32x32 with 8 bits per RGB channel.");

    }
    if (state.preferences.format != lut_baker::output_format::cube)
    {
        field_label("Range");
        auto &policy = is_png ? state.preferences.png_range : state.preferences.rise_range;
        bool clamp = policy == lut_baker::range_policy::clamp;
        if (ImGui::Checkbox("Clamp to 0-1", &clamp))
            policy = clamp ? lut_baker::range_policy::clamp : lut_baker::range_policy::reject;
        help_marker("This format stores values in 0-1. No gamma conversion is applied.\n"
            "Off: an export with values outside 0-1 is rejected and nothing is written.\n"
            "On: those values are clamped, and the result reports the original range and how many were clipped.");

        skip_field_label();
        disabled_wrapped(is_png ? "Use a PNG-compatible reader with matching layout, size and bit depth." : "Verify the result in-game.");
    }

    std::string settings_error;
    if (!lut_baker::validate_export_preferences(state.preferences, settings_error))
    {
        skip_field_label();
        ImGui::PushStyleColor(ImGuiCol_Text, color_error);
        ImGui::TextWrapped("%s", settings_error.c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        lut_baker::export_request estimate_request;
        estimate_request.format = state.preferences.format;
        estimate_request.lattice_size = lut_baker::effective_lattice_size(state.preferences);
        estimate_request.png_distribution = state.preferences.png_distribution;
        estimate_request.png_depth = state.preferences.png_depth;
        lut_baker::export_estimate estimate;
        if (lut_baker::estimate_export(estimate_request, estimate, settings_error))
        {
            skip_field_label();
            if (is_png)
                ImGui::TextDisabled("%ux%u pixels, %u bits per channel", estimate.image_width, estimate.image_height,
                    static_cast<unsigned int>(state.preferences.png_depth));
            else
                ImGui::TextDisabled("%llu RGB samples", static_cast<unsigned long long>(estimate.samples));
            skip_field_label();
            ImGui::TextDisabled("File budget: %.1f MiB", static_cast<double>(estimate.file_bytes) / (1024 * 1024));
            ImGui::SetItemTooltip("A conservative file budget, not the exact file size. Free space is checked before baking and again before writing, plus a 16 MiB reserve.");
        }
    }

    field_label("File name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##output_filename", "empty = automatic timestamp", state.output_filename.data(), state.output_filename.size());

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
    std::string settings_error;
    const bool valid = lut_baker::validate_export_preferences(state.preferences, settings_error);
    const lut_baker::output_format format = state.preferences.format;
    const std::uint32_t size = lut_baker::effective_lattice_size(state.preferences);
    const std::string target = std::string(find_format_option(format).short_name) + ' ' + lattice_label(size);

    std::string label;
    if (!valid)
        label = "Fix output settings to continue";
    else if (state.selected.empty())
        label = "Export identity LUT  (GPU validation)  -  " + target;
    else
        label = "Bake " + std::to_string(state.selected.size()) + (state.selected.size() == 1 ? " technique" : " techniques") + "  -  " + target;
    label += "##export";

    ImGui::Spacing();
    const ImGuiStyle &style = ImGui::GetStyle();
    ImGui::BeginDisabled(!valid);
    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f)))
        begin_export(state);
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (!valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", settings_error.c_str());
    else if (state.selected.empty())
        ImGui::SetItemTooltip("Nothing is selected: exports an identity LUT (no color change) and verifies that the GPU reproduces it within floating-point tolerance.");
}

void draw_progress(runtime_state &state)
{
    ImGui::Spacing();
    // A negative fraction draws ImGui's indeterminate animation: the number of
    // compile retries and the GPU latency are not known in advance.
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color_busy);
    const char *const progress_label = state.settings_backup.pending() && !state.export_pending
        ? "Waiting for settings recovery" : state.status.c_str();
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f), progress_label);
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(!lut_baker::can_abort_export(state.export_pending, state.writer_pending));
    if (ImGui::Button("Cancel export"))
        abort_export(state);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(state.writer_pending
        ? "The validated result is already being written. Cancellation is available before file writing starts."
        : "Stops the bake without writing a file. Shader compilation and GPU work already submitted still finish in the background.");
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
    if (state.phase == operation_phase::cancelled)
    {
        ImGui::TextColored(color_warning, "%s", state.status.c_str());
        ImGui::TextWrapped("%s", state.detail.c_str());
    }
    else if (is_busy(state))
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
            if (state.last_format == lut_baker::output_format::png)
                metric_row("Layout", "%s, %u bits per channel", lut_baker::png_layout_name(state.last_png_distribution),
                    static_cast<unsigned int>(state.last_png_depth));
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

    if (state.identity_metrics_valid || state.quantized_metrics_valid)
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
                    metric_row("Identity error (max / mean / RMS)","%.9g / %.9g / %.9g", metrics.maximum_absolute, metrics.mean_absolute, metrics.rms);
                }
                if (state.quantized_metrics_valid)
                {
                    const auto &metrics = state.quantized_metrics;
                    metric_row("Source RGB range", "[%.9g, %.9g]", metrics.source_minimum, metrics.source_maximum);
                    metric_row("Clipped values / samples", "%zu / %zu", metrics.clipped_components, metrics.clipped_samples);
                    const unsigned int bits = state.last_format == lut_baker::output_format::png ? static_cast<unsigned int>(state.last_png_depth) : 8u;
                    const std::string label = std::to_string(bits) + "-bit quantization error (max / mean / RMS)";
                    metric_row(label.c_str(),"%.9g / %.9g / %.9g",
                        metrics.quantization.maximum_absolute, metrics.quantization.mean_absolute, metrics.quantization.rms);
                }
                ImGui::EndTable();
            }
            if (state.quantized_metrics_valid)
                disabled_wrapped("Quantization error is measured after the range check (and after clamping, if enabled) and is separate from the GPU identity error.");
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
        ImGui::SetItemTooltip("Copy the file name to the clipboard.");
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
    check_bake_timeout(*state);
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
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
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
    reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_current_preset_path);
    reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_overlay(nullptr, draw_overlay);
}
}

extern "C" __declspec(dllexport) const char *NAME = "ReShade LUT Baker";
extern "C" __declspec(dllexport) const char *AUTHOR = "ISpectre23";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Bakes the color grading of selected ReShade techniques into a 3D CUBE LUT, a compatible PNG image or a supported game's native LUT format.";

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
