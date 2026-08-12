// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// Renderer.cpp -- implementation of the public C ABI declared in
// Renderer.h.
//
// Every exported function is a thin shell: validate arguments, then run
// the real work inside guard(), which turns any exception into a result
// code and a thread-local message. Nothing throws across the boundary.
//
// Handle ownership:
//   mb3d_context  owns the Vulkan device, the formula registry and the
//                 Lua states created from it.
//   mb3d_job      owns its FrameBuffer and the worker thread rendering
//                 into it. Layer pointers handed out stay valid until
//                 mb3d_job_release, which joins first.
//   mb3d_scene    owns parsed settings and, for .m3a, the timeline.
//   mb3d_mesh     owns vertex and index storage.

#include "mb3d/CpuRaymarcher.hpp"
#include "mb3d/LuaEngine.hpp"
#include "mb3d/MarchingCubes.hpp"
#include "mb3d/VulkanContext.hpp"
#include "mb3d/LegacyImport.h"
#include "mb3d/M4DFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

// ---------------------------------------------------------------------
// ABI layout assertions. A managed struct in NativeStructs.cs mirrors
// each of these; if a field is added without updating both sides, the
// build breaks here rather than corrupting memory at runtime.
// ---------------------------------------------------------------------

static_assert(sizeof(mb3d_precision_profile_t) == 112, "precision profile ABI changed");
static_assert(sizeof(mb3d_provenance_t) == 432, "provenance ABI changed");
static_assert(sizeof(mb3d_tiling_settings_t) == 32, "tiling ABI changed");
static_assert(sizeof(mb3d_camera_transform_t) == 176, "camera ABI changed");
static_assert(sizeof(mb3d_formula_slot_t) == 224, "formula slot ABI changed");
static_assert(sizeof(mb3d_light_channel_t) == 136, "light channel ABI changed");
static_assert(sizeof(mb3d_lighting_settings_t) == 1072, "lighting ABI changed");
static_assert(sizeof(mb3d_coloring_settings_t) == 1104, "coloring ABI changed");
static_assert(sizeof(mb3d_calculation_settings_t) == 112, "calculation ABI changed");
static_assert(sizeof(mb3d_render_settings_t) == 4016, "render settings ABI changed");
static_assert(sizeof(mb3d_image_view_t) == 40, "image view ABI changed");
static_assert(sizeof(mb3d_progress_t) == 56, "progress ABI changed");
static_assert(sizeof(mb3d_mesh_settings_t) == 76, "mesh settings ABI changed");
static_assert(sizeof(mb3d_mesh_stats_t) == 72, "mesh stats ABI changed");
static_assert(sizeof(mb3d_keyframe_t) == 488, "keyframe ABI changed");
static_assert(sizeof(mb3d_import_report_t) == 56, "import report ABI changed");

// Offsets matter as much as sizes: a struct can keep its total size while
// a field shifts, and the managed mirror would then read the wrong bytes
// with no size mismatch to catch it. NativeStructs.cs asserts the same
// numbers from the other side.
static_assert(offsetof(mb3d_render_settings_t, profile) == 48, "profile moved");
static_assert(offsetof(mb3d_render_settings_t, tiling) == 160, "tiling moved");
static_assert(offsetof(mb3d_render_settings_t, camera) == 192, "camera moved");
static_assert(offsetof(mb3d_render_settings_t, calculation) == 368, "calculation moved");
static_assert(offsetof(mb3d_render_settings_t, coloring) == 480, "coloring moved");
static_assert(offsetof(mb3d_render_settings_t, lighting) == 1584, "lighting moved");
static_assert(offsetof(mb3d_render_settings_t, formulas) == 2656, "formulas moved");
static_assert(offsetof(mb3d_camera_transform_t, stereo_mode) == 168, "stereo_mode moved");
static_assert(offsetof(mb3d_formula_slot_t, params) == 140, "formula params moved");
static_assert(offsetof(mb3d_provenance_t, profile_id) == 332, "provenance shifted");
static_assert(offsetof(mb3d_provenance_t, import_unix_time) == 400, "provenance shifted");

static_assert(std::is_standard_layout_v<mb3d_render_settings_t>,
              "settings must stay standard-layout for C# interop");
static_assert(std::is_trivially_copyable_v<mb3d_render_settings_t>,
              "settings must be memcpy-able across the boundary");

namespace mb3d {

// ---------------------------------------------------------------------
// Error state
// ---------------------------------------------------------------------

namespace {
thread_local std::string t_last_error;
}

void set_last_error(std::string message) noexcept {
    try {
        t_last_error = std::move(message);
    } catch (...) {
        // A failure to record an error is not worth propagating.
    }
}

const char* last_error() noexcept { return t_last_error.c_str(); }

// ---------------------------------------------------------------------
// Handle types
// ---------------------------------------------------------------------

struct Context {
    VulkanContext   vulkan;
    FormulaRegistry registry;
    mb3d_backend_t  backend{MB3D_BACKEND_CPU};
    mb3d_log_fn     log_fn{nullptr};
    void*           log_user{nullptr};
    std::mutex      mutex;

    void log(int level, const std::string& message) {
        if (log_fn) log_fn(level, message.c_str(), log_user);
    }
};

struct Job {
    mb3d_render_settings_t      settings{};
    std::unique_ptr<FrameBuffer> frame;
    ProgressReporter            progress;
    std::thread                 worker;
    std::atomic<bool>           finished{false};
    mb3d_result_t               result{MB3D_OK};
    std::string                 error;

    ~Job() {
        if (worker.joinable()) worker.join();
    }
};

// A scene carries TWO parameter sets. `active` is what renders;
// `legacy` is the as-imported set, preserved byte-for-byte so a
// precision-mode switch is non-destructive and reversible. Migration
// writes `active` and must never touch `legacy`.
struct Scene {
    mb3d_render_settings_t       active{};
    mb3d_render_settings_t       legacy{};
    bool                         has_legacy{false};
    mb3d_provenance_t            provenance{};
    std::vector<mb3d_keyframe_t> keyframes;
    std::string                  name;
    std::string                  notes;
    std::vector<m4d::OpaqueChunk> unknown_chunks;
};

struct MeshHandle {
    Mesh  mesh;
    float scale_to_mm{1.0f};
};

struct LuaHandle {
    std::unique_ptr<LuaEngine> engine;
};

// Declared in ExrExporter.cpp.
void write_exr(const FrameBuffer& fb, const std::string& path, bool multilayer);

namespace {

Context* as_context(mb3d_context handle) {
    require(handle != nullptr, MB3D_ERR_INVALID_ARG, "null context");
    return reinterpret_cast<Context*>(handle);
}

Job* as_job(mb3d_job handle) {
    require(handle != nullptr, MB3D_ERR_INVALID_ARG, "null job");
    return reinterpret_cast<Job*>(handle);
}

Scene* as_scene(mb3d_scene handle) {
    require(handle != nullptr, MB3D_ERR_INVALID_ARG, "null scene");
    return reinterpret_cast<Scene*>(handle);
}

MeshHandle* as_mesh(mb3d_mesh handle) {
    require(handle != nullptr, MB3D_ERR_INVALID_ARG, "null mesh");
    return reinterpret_cast<MeshHandle*>(handle);
}

LuaHandle* as_lua(mb3d_lua handle) {
    require(handle != nullptr, MB3D_ERR_INVALID_ARG, "null Lua state");
    return reinterpret_cast<LuaHandle*>(handle);
}

void validate_settings(const mb3d_render_settings_t& s) {
    require(s.abi_version == MB3D_ABI_VERSION, MB3D_ERR_ABI_MISMATCH,
            "settings.abi_version is " + std::to_string(s.abi_version) +
            ", this build expects " + std::to_string(MB3D_ABI_VERSION));
    require(s.width > 0 && s.height > 0, MB3D_ERR_INVALID_ARG,
            "render dimensions must be positive");
    // 512 megapixels is well past any real poster render and catches
    // the uninitialised-struct case before it tries to allocate.
    require(static_cast<std::int64_t>(s.width) * s.height <= (512ll << 20),
            MB3D_ERR_INVALID_ARG, "render dimensions are implausibly large");
}

std::string file_extension(const std::string& path) {
    const auto dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string{} : path.substr(dot);
}

}  // namespace
}  // namespace mb3d

using namespace mb3d;

// =====================================================================
// Version and diagnostics
// =====================================================================

extern "C" {

// The stable symbol every host checks on load. Named without the mb3d_
// prefix because it is the one function a host must be able to find
// before it knows anything else about this library.
MB3D_API int32_t MB3D_CALL Core_GetABIVersion(void) { return MB3D_ABI_VERSION; }

MB3D_API int32_t MB3D_CALL mb3d_abi_version(void) { return MB3D_ABI_VERSION; }

MB3D_API int32_t MB3D_CALL mb3d_m4d_format_version(void) { return MB3D_M4D_FORMAT_VERSION; }

MB3D_API const char* MB3D_CALL mb3d_version_string(void) {
    static const std::string version =
        std::to_string(MB3D_VERSION_MAJOR) + "." +
        std::to_string(MB3D_VERSION_MINOR) + "." +
        std::to_string(MB3D_VERSION_PATCH);
    return version.c_str();
}

MB3D_API const char* MB3D_CALL mb3d_last_error(void) { return last_error(); }

// =====================================================================
// Context
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_context_create(int32_t preferred, mb3d_context* out_ctx) {
    return guard([&] {
        require(out_ctx != nullptr, MB3D_ERR_INVALID_ARG, "out_ctx is null");
        *out_ctx = nullptr;

        auto context = std::make_unique<Context>();

        const auto requested = static_cast<mb3d_backend_t>(preferred);
        if (requested == MB3D_BACKEND_CPU) {
            context->backend = MB3D_BACKEND_CPU;
        } else if (context->vulkan.available()) {
            context->backend = MB3D_BACKEND_VULKAN;
        } else {
            // An explicit Vulkan request is an error when there is no
            // device; AUTO silently degrades, which is what a desktop
            // user wants.
            require(requested != MB3D_BACKEND_VULKAN, MB3D_ERR_NO_DEVICE,
                    "Vulkan backend requested but unavailable: " + context->vulkan.init_error());
            context->backend = MB3D_BACKEND_CPU;
        }

        *out_ctx = reinterpret_cast<mb3d_context>(context.release());
        return MB3D_OK;
    });
}

MB3D_API void MB3D_CALL mb3d_context_destroy(mb3d_context ctx) {
    delete reinterpret_cast<Context*>(ctx);
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_context_set_log(mb3d_context ctx, mb3d_log_fn fn,
                                                      void* user) {
    return guard([&] {
        Context* context = as_context(ctx);
        std::scoped_lock lock(context->mutex);
        context->log_fn = fn;
        context->log_user = user;
        return MB3D_OK;
    });
}

MB3D_API int32_t MB3D_CALL mb3d_context_backend(mb3d_context ctx) {
    if (ctx == nullptr) return MB3D_BACKEND_CPU;
    return reinterpret_cast<Context*>(ctx)->backend;
}

MB3D_API uint32_t MB3D_CALL mb3d_context_precision_support(mb3d_context ctx) {
    if (ctx == nullptr) return 0;
    Context* context = reinterpret_cast<Context*>(ctx);

    // fp32 and the CPU fp64 path are always available. Device fp64 is
    // not: several mobile and older integrated parts lack
    // shaderFloat64 entirely, and the UI needs to know before it offers
    // a deep-zoom tier it cannot deliver.
    std::uint32_t mask = (1u << MB3D_PRECISION_FP32) | (1u << MB3D_PRECISION_FP64);
    // Double-double is built on fp64 pairs, so it rides on the same
    // requirement.
    if (context->backend == MB3D_BACKEND_CPU || context->vulkan.available()) {
        mask |= (1u << MB3D_PRECISION_DD);
    }
    return mask;
}

MB3D_API const char* MB3D_CALL mb3d_context_device_name(mb3d_context ctx) {
    if (ctx == nullptr) return "";
    return reinterpret_cast<Context*>(ctx)->vulkan.device_name().c_str();
}

// =====================================================================
// Defaults
// =====================================================================

MB3D_API void MB3D_CALL mb3d_render_settings_default(mb3d_render_settings_t* out) {
    if (out == nullptr) return;
    *out = mb3d_render_settings_t{};

    out->abi_version = MB3D_ABI_VERSION;
    out->width = 1280;
    out->height = 720;
    out->supersample = 1;
    out->layer_mask = MB3D_LAYER_RGBA | MB3D_LAYER_DEPTH | MB3D_LAYER_NORMAL;
    out->backend = MB3D_BACKEND_AUTO;
    out->thread_count = 0;
    out->hybrid_master_mode = MB3D_HYBRID_SEQUENTIAL;
    out->seed = 1;

    // Defaults are the "modern" profile: tight per-slot analytic DE and
    // sampling free to be re-tuned. A scene imported from a legacy file
    // overrides this with the profile chosen at import time.
    if (const mb3d_precision_profile_t* profile = find_profile(MB3D_PROFILE_MODERN)) {
        out->profile = *profile;
    }

    // Tiling is always on. 128x128 with a 16px overlap keeps post passes
    // seam-free, and the budget defaults to 0 so the engine derives it
    // from physical RAM rather than guessing a number that is wrong on
    // both a laptop and a workstation.
    out->tiling.tile_width = 128;
    out->tiling.tile_height = 128;
    out->tiling.tile_overlap = 16;
    out->tiling.max_resident_tiles = 0;
    out->tiling.memory_budget_mib = 0;
    out->tiling.spill_to_disk = 0;

    // Camera: three units back along -Z looking at the origin, which
    // frames a power-8 bulb with a little margin.
    out->camera.position[0] = 0.0;
    out->camera.position[1] = 0.0;
    out->camera.position[2] = -3.0;
    out->camera.up[1] = 1.0;
    out->camera.distance = 3.0;
    out->camera.zoom = 1.0;
    out->camera.fov_degrees = 45.0;
    out->camera.near_clip = 0.0;
    out->camera.far_clip = 20.0;
    out->camera.dof_focal_plane = 3.0;

    out->calculation.max_iterations = 24;
    out->calculation.escape_radius = 4.0f;
    out->calculation.raystep_multiplier = 1.0f;
    out->calculation.stepwidth_limiter = 0.5f;
    out->calculation.de_stop_criterion = 1e-5f;
    out->calculation.de_max_steps = 300;
    out->calculation.binary_search_steps = 6;
    out->calculation.normal_epsilon = 1e-5f;
    out->calculation.smooth_normals = 1;
    out->calculation.normals_on_de = 1;
    out->calculation.de_scale = 1.0f;
    out->calculation.bound_sphere_enabled = 1;
    out->calculation.bound_sphere_radius = 2.5f;
    out->calculation.bound_box_min[0] = -2.0f;
    out->calculation.bound_box_min[1] = -2.0f;
    out->calculation.bound_box_min[2] = -2.0f;
    out->calculation.bound_box_max[0] = 2.0f;
    out->calculation.bound_box_max[1] = 2.0f;
    out->calculation.bound_box_max[2] = 2.0f;

    // Default gradient: a smooth cool-to-warm ramp. Not a rainbow --
    // rainbows band badly on iteration counts.
    for (int i = 0; i < MB3D_GRADIENT_ENTRIES; ++i) {
        const double t = static_cast<double>(i) / (MB3D_GRADIENT_ENTRIES - 1);
        const auto r = static_cast<std::uint32_t>((0.15 + 0.80 * t) * 255.0);
        const auto g = static_cast<std::uint32_t>((0.20 + 0.55 * std::sin(t * 3.14159)) * 255.0);
        const auto b = static_cast<std::uint32_t>((0.55 - 0.45 * t) * 255.0);
        out->coloring.gradient[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    out->coloring.color_source = MB3D_COLOR_ITERATION;
    out->coloring.trap_kind = MB3D_TRAP_NONE;
    out->coloring.trap_dimensions = 3;
    out->coloring.trap_size = 1.0f;
    out->coloring.trap_influence = 1.0f;
    out->coloring.trap_normal[1] = 1.0f;
    out->coloring.color_speed = 1.0f;
    out->coloring.color_cycle = 1.0f;
    out->coloring.smooth_iteration = 1;

    // One key light plus a dim fill; everything else off.
    out->lighting.channels[0].kind = MB3D_LIGHT_POSITIONAL;
    out->lighting.channels[0].enabled = 1;
    out->lighting.channels[0].color[0] = 1.0f;
    out->lighting.channels[0].color[1] = 0.97f;
    out->lighting.channels[0].color[2] = 0.92f;
    out->lighting.channels[0].intensity = 1.0f;
    out->lighting.channels[0].position[0] = -4.0f;
    out->lighting.channels[0].position[1] = 4.0f;
    out->lighting.channels[0].position[2] = -4.0f;
    out->lighting.channels[0].diffuse = 1.0f;
    out->lighting.channels[0].specular = 0.35f;
    out->lighting.channels[0].specular_exponent = 32.0f;
    out->lighting.channels[0].casts_shadow = 1;

    out->lighting.channels[1].kind = MB3D_LIGHT_DIRECTIONAL;
    out->lighting.channels[1].enabled = 1;
    out->lighting.channels[1].color[0] = 0.45f;
    out->lighting.channels[1].color[1] = 0.55f;
    out->lighting.channels[1].color[2] = 0.75f;
    out->lighting.channels[1].intensity = 0.4f;
    out->lighting.channels[1].position[0] = 0.4f;
    out->lighting.channels[1].position[1] = -1.0f;
    out->lighting.channels[1].position[2] = 0.3f;
    out->lighting.channels[1].diffuse = 1.0f;

    out->lighting.ambient_color_a[0] = 0.10f;
    out->lighting.ambient_color_a[1] = 0.11f;
    out->lighting.ambient_color_a[2] = 0.14f;
    out->lighting.ambient_color_b[0] = 0.04f;
    out->lighting.ambient_color_b[1] = 0.05f;
    out->lighting.ambient_color_b[2] = 0.08f;
    out->lighting.ambient_falloff = 1.0f;
    out->lighting.fog_start = 3.0f;
    out->lighting.fog_end = 12.0f;
    out->lighting.gamma = 2.2f;
    out->lighting.contrast = 1.0f;
    out->lighting.saturation = 1.0f;
    out->lighting.exposure = 0.0f;
    out->lighting.background_mode = 1;
    out->lighting.background_b[0] = 0.05f;
    out->lighting.background_b[1] = 0.06f;
    out->lighting.background_b[2] = 0.09f;
    out->lighting.ssao_enabled = 1;
    out->lighting.ssao_radius = 0.05f;
    out->lighting.ssao_intensity = 0.7f;
    out->lighting.ssao_samples = 5;
    out->lighting.hard_shadows = 1;
    out->lighting.refraction_index = 1.45f;
    out->lighting.shadow_bias = 1e-4f;

    // Slot 0: a power-8 bulb. Everything else disabled.
    std::snprintf(out->formulas[0].name, MB3D_MAX_NAME, "Bulb3D");
    out->formulas[0].kind = MB3D_FORMULA_BULB;
    out->formulas[0].formula_class = MB3D_CLASS_3D;
    out->formulas[0].enabled = 1;
    out->formulas[0].iteration_weight = 1;
    out->formulas[0].stop_iteration = -1;
    out->formulas[0].repeat_from_slot = -1;
    out->formulas[0].analytic_de = 1;
    out->formulas[0].params[0] = 8.0f;
    for (int i = 1; i < MB3D_FORMULA_SLOTS; ++i) {
        out->formulas[i].stop_iteration = -1;
        out->formulas[i].repeat_from_slot = -1;
        out->formulas[i].iteration_weight = 1;
        out->formulas[i].analytic_de = 1;
    }
}

// =====================================================================
// Rendering
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_render_begin(mb3d_context ctx,
                                                   const mb3d_render_settings_t* settings,
                                                   mb3d_job* out_job) {
    return guard([&] {
        Context* context = as_context(ctx);
        require(settings != nullptr, MB3D_ERR_INVALID_ARG, "settings is null");
        require(out_job != nullptr, MB3D_ERR_INVALID_ARG, "out_job is null");
        validate_settings(*settings);
        *out_job = nullptr;

        auto job = std::make_unique<Job>();
        job->settings = *settings;
        // Always keep an RGBA layer: the UI needs something to show even
        // when the caller only asked for data passes.
        job->settings.layer_mask |= MB3D_LAYER_RGBA;
        job->frame = std::make_unique<FrameBuffer>(settings->width, settings->height,
                                                   job->settings.layer_mask);

        Job* raw = job.get();
        const FormulaRegistry& registry = context->registry;

        raw->worker = std::thread([raw, &registry, context] {
            try {
                const CpuRaymarcher marcher(registry, raw->settings);
                marcher.render(*raw->frame, raw->progress);
                raw->result = MB3D_OK;
            } catch (const Error& e) {
                raw->result = e.code();
                raw->error = e.what();
                raw->progress.finish(MB3D_JOB_FAILED);
                context->log(3, std::string("render failed: ") + e.what());
            } catch (const std::exception& e) {
                raw->result = MB3D_ERR_UNKNOWN;
                raw->error = e.what();
                raw->progress.finish(MB3D_JOB_FAILED);
                context->log(3, std::string("render failed: ") + e.what());
            }
            raw->finished.store(true, std::memory_order_release);
        });

        *out_job = reinterpret_cast<mb3d_job>(job.release());
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_job_set_callbacks(mb3d_job job, mb3d_progress_fn progress,
                                                        mb3d_cancel_fn cancel, void* user) {
    return guard([&] {
        as_job(job)->progress.configure(progress, cancel, user);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_job_wait(mb3d_job job, int32_t timeout_ms) {
    return guard([&] {
        Job* j = as_job(job);

        if (timeout_ms < 0) {
            if (j->worker.joinable()) j->worker.join();
            require(j->result == MB3D_OK, j->result,
                    j->error.empty() ? "render failed" : j->error);
            return MB3D_OK;
        }

        // Poll rather than condition-variable: the wait is a courtesy
        // for hosts that cannot block, and 1 ms granularity is far finer
        // than any render this engine produces.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (!j->finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return MB3D_ERR_NOT_READY;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (j->worker.joinable()) j->worker.join();
        require(j->result == MB3D_OK, j->result,
                j->error.empty() ? "render failed" : j->error);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_job_cancel(mb3d_job job) {
    return guard([&] {
        Job* j = as_job(job);
        // The reporter's cancel hook is polled by every tile; setting a
        // permanently-true callback is the simplest way to stop it.
        j->progress.configure(nullptr, [](void*) -> int32_t { return 1; }, nullptr);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_job_progress(mb3d_job job, mb3d_progress_t* out) {
    return guard([&] {
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        *out = as_job(job)->progress.snapshot();
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_job_layer(mb3d_job job, uint32_t layer,
                                                mb3d_image_view_t* out_view) {
    return guard([&] {
        Job* j = as_job(job);
        require(out_view != nullptr, MB3D_ERR_INVALID_ARG, "out_view is null");

        Layer* l = j->frame->layer(static_cast<mb3d_layer_t>(layer));
        require(l != nullptr, MB3D_ERR_INVALID_ARG,
                "layer " + std::to_string(layer) + " was not rendered");

        *out_view = mb3d_image_view_t{};
        out_view->data = l->bytes.data();
        out_view->size_bytes = l->bytes.size();
        out_view->width = l->width;
        out_view->height = l->height;
        out_view->stride_bytes = l->stride;
        out_view->format = l->format;
        out_view->layer = layer;
        return MB3D_OK;
    });
}

MB3D_API void MB3D_CALL mb3d_job_release(mb3d_job job) {
    // The destructor joins the worker, so any pointer previously handed
    // out through mb3d_job_layer stops being written before it is freed.
    delete reinterpret_cast<Job*>(job);
}

// =====================================================================
// Post passes
// =====================================================================

namespace {

// Rebuilds normals from the depth buffer rather than the DE field.
// Cheaper than re-marching, and it is what the "recompute normals from
// Z-buffer" tool does.
void recompute_normals_from_depth(FrameBuffer& fb, float strength) {
    Layer* depth = fb.layer(MB3D_LAYER_DEPTH);
    Layer* normal = fb.layer(MB3D_LAYER_NORMAL);
    require(depth && normal, MB3D_ERR_INVALID_ARG,
            "normal recomputation needs both DEPTH and NORMAL layers");

    const int w = fb.width(), h = fb.height();
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const float zc = depth->row<float>(y)[x];
            if (!std::isfinite(zc)) continue;

            const float zl = depth->row<float>(y)[x - 1];
            const float zr = depth->row<float>(y)[x + 1];
            const float zu = depth->row<float>(y - 1)[x];
            const float zd = depth->row<float>(y + 1)[x];
            if (!std::isfinite(zl) || !std::isfinite(zr) ||
                !std::isfinite(zu) || !std::isfinite(zd)) continue;

            const Vec3 n = normalize(Vec3{(zl - zr) * strength, (zu - zd) * strength, 2.0});
            float* px = normal->row<float>(y) + x * 3;
            px[0] = static_cast<float>(n.x);
            px[1] = static_cast<float>(n.y);
            px[2] = static_cast<float>(n.z);
        }
    }
}

// Separable Gaussian on the RGBA layer, radius scaled per pixel by how
// far its depth is from the focal plane.
void depth_of_field(FrameBuffer& fb, float focal_plane, float strength) {
    Layer* rgba = fb.layer(MB3D_LAYER_RGBA);
    Layer* depth = fb.layer(MB3D_LAYER_DEPTH);
    require(rgba && depth, MB3D_ERR_INVALID_ARG, "DOF needs both RGBA and DEPTH layers");
    if (strength <= 0.0f) return;

    const int w = fb.width(), h = fb.height();
    std::vector<std::uint32_t> source(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::copy_n(rgba->row<std::uint32_t>(y), w, source.begin() + static_cast<std::size_t>(y) * w);
    }

    const int max_radius = std::clamp(static_cast<int>(strength * 16.0f), 1, 32);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float z = depth->row<float>(y)[x];
            if (!std::isfinite(z)) continue;

            const float coc = std::min(std::fabs(z - focal_plane) * strength, 1.0f);
            const int radius = static_cast<int>(coc * max_radius);
            if (radius < 1) continue;

            double sum[4] = {0, 0, 0, 0};
            double weight_total = 0.0;
            const double sigma = std::max(radius / 2.0, 0.5);
            const double denom = 2.0 * sigma * sigma;

            for (int dy = -radius; dy <= radius; ++dy) {
                const int sy = std::clamp(y + dy, 0, h - 1);
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sx = std::clamp(x + dx, 0, w - 1);
                    const double weight = std::exp(-(dx * dx + dy * dy) / denom);
                    const std::uint32_t p = source[static_cast<std::size_t>(sy) * w + sx];
                    sum[0] += weight * ((p >> 24) & 0xFF);
                    sum[1] += weight * ((p >> 16) & 0xFF);
                    sum[2] += weight * ((p >> 8) & 0xFF);
                    sum[3] += weight * (p & 0xFF);
                    weight_total += weight;
                }
            }
            if (weight_total <= 0.0) continue;
            auto to8 = [&](double v) {
                return static_cast<std::uint32_t>(std::clamp(v / weight_total, 0.0, 255.0));
            };
            rgba->row<std::uint32_t>(y)[x] =
                (to8(sum[0]) << 24) | (to8(sum[1]) << 16) | (to8(sum[2]) << 8) | to8(sum[3]);
        }
    }
}

// Multiplies the RGBA layer by an existing single-channel layer.
void modulate_rgba(FrameBuffer& fb, mb3d_layer_t source_layer, float intensity) {
    Layer* rgba = fb.layer(MB3D_LAYER_RGBA);
    Layer* source = fb.layer(source_layer);
    require(rgba && source, MB3D_ERR_INVALID_ARG, "post pass is missing a required layer");

    for (int y = 0; y < fb.height(); ++y) {
        std::uint32_t* row = rgba->row<std::uint32_t>(y);
        const float* src = source->row<float>(y);
        for (int x = 0; x < fb.width(); ++x) {
            const float factor = std::clamp(1.0f - (1.0f - src[x]) * intensity, 0.0f, 1.0f);
            const std::uint32_t p = row[x];
            auto scale = [factor](std::uint32_t channel) {
                return static_cast<std::uint32_t>(std::clamp(channel * factor, 0.0f, 255.0f));
            };
            row[x] = (p & 0xFF000000u) |
                     (scale((p >> 16) & 0xFF) << 16) |
                     (scale((p >> 8) & 0xFF) << 8) |
                     scale(p & 0xFF);
        }
    }
}

}  // namespace

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_recompute_normals(mb3d_job job, float strength) {
    return guard([&] {
        Job* j = as_job(job);
        recompute_normals_from_depth(*j->frame, strength > 0.0f ? strength : 1.0f);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_ssao(mb3d_job job, float radius, float intensity,
                                                int32_t samples) {
    return guard([&] {
        Job* j = as_job(job);
        require(j->frame->has(MB3D_LAYER_SSAO), MB3D_ERR_INVALID_ARG,
                "job was rendered without the SSAO layer");
        (void)radius;
        (void)samples;
        modulate_rgba(*j->frame, MB3D_LAYER_SSAO, intensity);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_hard_shadows(mb3d_job job, float bias) {
    return guard([&] {
        Job* j = as_job(job);
        require(j->frame->has(MB3D_LAYER_SHADOW), MB3D_ERR_INVALID_ARG,
                "job was rendered without the SHADOW layer");
        (void)bias;
        modulate_rgba(*j->frame, MB3D_LAYER_SHADOW, 1.0f);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_depth_of_field(mb3d_job job, float focal_plane,
                                                          float strength) {
    return guard([&] {
        Job* j = as_job(job);
        depth_of_field(*j->frame, focal_plane, strength);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_composite(mb3d_job job) {
    return guard([&] {
        Job* j = as_job(job);
        const auto& L = j->settings.lighting;
        if (j->frame->has(MB3D_LAYER_SSAO) && L.ssao_enabled) {
            modulate_rgba(*j->frame, MB3D_LAYER_SSAO, L.ssao_intensity);
        }
        if (j->frame->has(MB3D_LAYER_SHADOW) && L.hard_shadows) {
            modulate_rgba(*j->frame, MB3D_LAYER_SHADOW, 1.0f);
        }
        if (j->settings.camera.dof_strength > 0.0) {
            depth_of_field(*j->frame,
                           static_cast<float>(j->settings.camera.dof_focal_plane),
                           static_cast<float>(j->settings.camera.dof_strength));
        }
        return MB3D_OK;
    });
}

// =====================================================================
// Scenes and file IO
// =====================================================================

MB3D_API int32_t MB3D_CALL mb3d_identify_file(const char* path) {
    if (path == nullptr) return MB3D_FILE_UNKNOWN;
    try {
        return legacy::identify(path);
    } catch (...) {
        return MB3D_FILE_UNKNOWN;
    }
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_create(mb3d_scene* out_scene) {
    return guard([&] {
        require(out_scene != nullptr, MB3D_ERR_INVALID_ARG, "out_scene is null");
        auto scene = std::make_unique<Scene>();
        mb3d_render_settings_default(&scene->active);
        *out_scene = reinterpret_cast<mb3d_scene>(scene.release());
        return MB3D_OK;
    });
}

MB3D_API void MB3D_CALL mb3d_scene_destroy(mb3d_scene scene) {
    delete reinterpret_cast<Scene*>(scene);
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_settings(mb3d_scene scene,
                                                     mb3d_render_settings_t* out) {
    return guard([&] {
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        *out = as_scene(scene)->active;
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_set_settings(mb3d_scene scene,
                                                         const mb3d_render_settings_t* in) {
    return guard([&] {
        require(in != nullptr, MB3D_ERR_INVALID_ARG, "in is null");
        validate_settings(*in);
        // Only the active set is writable. The legacy set is immutable
        // for the life of the scene -- that is what "reversible" means.
        as_scene(scene)->active = *in;
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_legacy_settings(mb3d_scene scene,
                                                            mb3d_render_settings_t* out) {
    return guard([&] {
        Scene* s = as_scene(scene);
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        require(s->has_legacy, MB3D_ERR_INVALID_ARG,
                "this scene was not imported and has no legacy parameter set");
        *out = s->legacy;
        return MB3D_OK;
    });
}

MB3D_API int32_t MB3D_CALL mb3d_scene_has_legacy_settings(mb3d_scene scene) {
    if (scene == nullptr) return 0;
    return reinterpret_cast<Scene*>(scene)->has_legacy ? 1 : 0;
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_provenance(mb3d_scene scene,
                                                       mb3d_provenance_t* out) {
    return guard([&] {
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        *out = as_scene(scene)->provenance;
        return MB3D_OK;
    });
}

// =====================================================================
// Native .m4d container -- the only format this engine writes
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_read(const char* path, mb3d_scene* out_scene) {
    return guard([&] {
        require(path != nullptr && out_scene != nullptr, MB3D_ERR_INVALID_ARG, "null argument");
        *out_scene = nullptr;

        m4d::Document document = m4d::read(path);

        auto scene = std::make_unique<Scene>();
        scene->active = document.active;
        if (document.legacy.has_value()) {
            scene->legacy = *document.legacy;
            scene->has_legacy = true;
        }
        scene->provenance = document.provenance;
        scene->keyframes = std::move(document.keyframes);
        scene->name = std::move(document.name);
        scene->notes = std::move(document.notes);
        // Chunks written by a newer build ride along untouched so a
        // round-trip through this build does not destroy them.
        scene->unknown_chunks = std::move(document.unknown_chunks);

        *out_scene = reinterpret_cast<mb3d_scene>(scene.release());
        return MB3D_OK;
    });
}

namespace {

m4d::Document document_from_scene(const Scene& scene) {
    m4d::Document document;
    document.active = scene.active;
    if (scene.has_legacy) document.legacy = scene.legacy;
    document.provenance = scene.provenance;
    document.keyframes = scene.keyframes;
    document.name = scene.name;
    document.notes = scene.notes;
    document.unknown_chunks = scene.unknown_chunks;
    return document;
}

}  // namespace

MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_write(const char* path, mb3d_scene scene) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        m4d::write(path, document_from_scene(*as_scene(scene)));
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_write_with_image(const char* path,
                                                           mb3d_scene scene, mb3d_job job) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        Scene* s = as_scene(scene);
        Job* j = as_job(job);

        m4d::Document document = document_from_scene(*s);
        // The job owns its frame buffer, so the document borrows it for
        // the duration of the write and releases it before returning.
        document.image.reset(j->frame.get());
        m4d::write(path, document);
        (void)document.image.release();
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_read_image(const char* path, mb3d_job* out_job) {
    return guard([&] {
        require(path != nullptr && out_job != nullptr, MB3D_ERR_INVALID_ARG, "null argument");
        *out_job = nullptr;

        m4d::Document document = m4d::read(path);
        require(document.image != nullptr, MB3D_ERR_PARSE,
                "'" + std::string(path) + "' carries no image payload");

        auto job = std::make_unique<Job>();
        job->settings = document.active;
        job->frame = std::move(document.image);
        job->finished.store(true, std::memory_order_release);

        *out_job = reinterpret_cast<mb3d_job>(job.release());
        return MB3D_OK;
    });
}

// =====================================================================
// Legacy import -- read only
// =====================================================================

namespace {

// Shared tail for both import entry points.
mb3d_result_t finish_import(legacy::ImportResult&& imported, mb3d_scene* out_scene,
                            mb3d_import_report_t* out_report) {
    auto scene = std::make_unique<Scene>();
    scene->active = imported.settings;
    // The as-imported values are frozen here. Nothing downstream may
    // overwrite them -- migration writes `active` only.
    scene->legacy = imported.settings;
    scene->has_legacy = true;
    scene->provenance = imported.provenance;
    scene->keyframes = std::move(imported.keyframes);

    if (out_report) *out_report = imported.report;
    *out_scene = reinterpret_cast<mb3d_scene>(scene.release());
    return MB3D_OK;
}

}  // namespace

MB3D_API mb3d_result_t MB3D_CALL mb3d_import_legacy(const char* path, const char* profile_id,
                                                    mb3d_scene* out_scene,
                                                    mb3d_import_report_t* out_report) {
    return guard([&] {
        require(path != nullptr && out_scene != nullptr, MB3D_ERR_INVALID_ARG, "null argument");
        // The profile is required, not defaulted: importing under the
        // wrong semantics yields an image that differs from the author's
        // in ways nobody can then explain.
        require(profile_id != nullptr && *profile_id != '\0', MB3D_ERR_INVALID_ARG,
                "a precision profile id is required for import");
        *out_scene = nullptr;

        const mb3d_file_kind_t kind = legacy::identify(path);
        require(kind != MB3D_FILE_M4D, MB3D_ERR_INVALID_ARG,
                "'.m4d' is the native format -- use mb3d_m4d_read");

        const std::vector<std::byte> data = legacy::read_file(path);
        legacy::ImportResult imported =
            legacy::import_any(data, kind, profile_id,
                               std::filesystem::path(path).filename().string());

        return finish_import(std::move(imported), out_scene, out_report);
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_import_legacy_memory(const void* data, size_t size,
                                                           int32_t kind,
                                                           const char* profile_id,
                                                           mb3d_scene* out_scene,
                                                           mb3d_import_report_t* out_report) {
    return guard([&] {
        require(data != nullptr && size > 0, MB3D_ERR_INVALID_ARG, "empty buffer");
        require(out_scene != nullptr, MB3D_ERR_INVALID_ARG, "out_scene is null");
        require(profile_id != nullptr && *profile_id != '\0', MB3D_ERR_INVALID_ARG,
                "a precision profile id is required for import");
        *out_scene = nullptr;

        const std::span<const std::byte> bytes(static_cast<const std::byte*>(data), size);
        legacy::ImportResult imported =
            legacy::import_any(bytes, static_cast<mb3d_file_kind_t>(kind), profile_id, {});

        return finish_import(std::move(imported), out_scene, out_report);
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_m3i_extract_parameters(const char* path, void* buffer,
                                                             size_t buffer_size,
                                                             size_t* out_needed) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");

        const std::vector<std::byte> data = legacy::read_file(path);
        const std::vector<std::byte> parameters = legacy::extract_embedded_parameters(data);

        if (out_needed) *out_needed = parameters.size();

        // Query mode: caller passes buffer=NULL to size the allocation.
        if (buffer == nullptr || buffer_size == 0) return MB3D_OK;

        require(buffer_size >= parameters.size(), MB3D_ERR_INVALID_ARG,
                "buffer too small; call with buffer=NULL to query the size");
        std::memcpy(buffer, parameters.data(), parameters.size());
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_inspect_legacy_formula(const char* path,
                                                             char* out_name,
                                                             size_t name_capacity,
                                                             int32_t* out_class,
                                                             int32_t* out_resolved) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");

        const mb3d_file_kind_t kind = legacy::identify(path);
        require(kind == MB3D_FILE_M3F || kind == MB3D_FILE_D3F || kind == MB3D_FILE_DSO,
                MB3D_ERR_INVALID_ARG, "not a formula file");

        const std::vector<std::byte> data = legacy::read_file(path);
        // Metadata only. The machine-code body is located and reported,
        // never mapped, relocated, or called.
        const legacy::FormulaMetadata meta = legacy::parse_formula_metadata(data, kind);

        if (out_class) *out_class = meta.formula_class;

        const auto resolved = legacy::resolve_formula_name(meta.name);
        if (out_resolved) *out_resolved = resolved.has_value() ? 1 : 0;

        if (out_name != nullptr && name_capacity > 0) {
            const std::string& text = resolved.has_value() ? *resolved : meta.name;
            const std::size_t n = std::min(text.size(), name_capacity - 1);
            std::memcpy(out_name, text.data(), n);
            out_name[n] = '\0';
        }
        return MB3D_OK;
    });
}

// =====================================================================
// Profiles and migration
// =====================================================================

MB3D_API int32_t MB3D_CALL mb3d_profile_count(void) {
    return static_cast<int32_t>(profile_count());
}

MB3D_API const char* MB3D_CALL mb3d_profile_id_at(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= profile_count()) return "";
    return profile_at(static_cast<std::size_t>(index)).id;
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_precision_profile_by_id(const char* id,
                                                              mb3d_precision_profile_t* out) {
    return guard([&] {
        require(id != nullptr && out != nullptr, MB3D_ERR_INVALID_ARG, "null argument");

        const mb3d_precision_profile_t* profile = find_profile(id);
        // An unknown id is an error rather than a silent fallback to
        // "modern": quietly substituting a different set of semantics is
        // how a user ends up unable to explain why their scene changed.
        require(profile != nullptr, MB3D_ERR_INVALID_ARG,
                "unknown precision profile '" + std::string(id) + "'");

        *out = *profile;
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_migrate_parameters(
    const mb3d_render_settings_t* legacy_settings,
    int32_t target_width, int32_t target_height,
    const char* target_profile_id,
    mb3d_render_settings_t* out_migrated) {
    return guard([&] {
        require(legacy_settings != nullptr && out_migrated != nullptr,
                MB3D_ERR_INVALID_ARG, "null argument");
        require(target_profile_id != nullptr, MB3D_ERR_INVALID_ARG,
                "target_profile_id is null");

        const mb3d_precision_profile_t* profile = find_profile(target_profile_id);
        require(profile != nullptr, MB3D_ERR_INVALID_ARG,
                "unknown precision profile '" + std::string(target_profile_id) + "'");

        migrate_parameters(*legacy_settings, target_width, target_height, *profile,
                           *out_migrated);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_rescale_for_resolution(
    mb3d_render_settings_t* settings, int32_t target_width, int32_t target_height,
    int32_t lock_to_legacy_look) {
    return guard([&] {
        require(settings != nullptr, MB3D_ERR_INVALID_ARG, "settings is null");
        require(target_width > 0 && target_height > 0, MB3D_ERR_INVALID_ARG,
                "target dimensions must be positive");
        rescale_for_resolution(*settings, target_width, target_height,
                               lock_to_legacy_look != 0);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_migration_preview(
    mb3d_context ctx,
    const mb3d_render_settings_t* legacy_settings,
    const mb3d_render_settings_t* migrated,
    int32_t preview_size,
    mb3d_job* out_legacy_job, mb3d_job* out_migrated_job) {
    return guard([&] {
        require(legacy_settings != nullptr && migrated != nullptr, MB3D_ERR_INVALID_ARG,
                "null argument");
        require(out_legacy_job != nullptr && out_migrated_job != nullptr,
                MB3D_ERR_INVALID_ARG, "null output job");

        // A small side-by-side is the only honest way to answer "is this
        // better or merely different" on a fractal -- nobody can judge it
        // from memory, and no changelog substitutes for seeing both.
        const int size = preview_size > 0 ? std::min(preview_size, 1024) : 400;

        auto shrink = [size](mb3d_render_settings_t s) {
            const double aspect = s.height > 0
                ? static_cast<double>(s.width) / s.height : 1.0;
            s.width = size;
            s.height = std::max(1, static_cast<int>(std::lround(size / aspect)));
            s.supersample = 1;
            s.layer_mask = MB3D_LAYER_RGBA;
            return s;
        };

        const mb3d_render_settings_t a = shrink(*legacy_settings);
        const mb3d_render_settings_t b = shrink(*migrated);

        const mb3d_result_t first = mb3d_render_begin(ctx, &a, out_legacy_job);
        if (first != MB3D_OK) return first;

        const mb3d_result_t second = mb3d_render_begin(ctx, &b, out_migrated_job);
        if (second != MB3D_OK) {
            mb3d_job_release(*out_legacy_job);
            *out_legacy_job = nullptr;
            return second;
        }
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_write_exr(const char* path, mb3d_job job,
                                                int32_t multilayer) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        write_exr(*as_job(job)->frame, path, multilayer != 0);
        return MB3D_OK;
    });
}

// =====================================================================
// Mesh extraction and export
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_extract(mb3d_context ctx,
                                                   const mb3d_render_settings_t* settings,
                                                   const mb3d_mesh_settings_t* mesh_settings,
                                                   mb3d_progress_fn progress, void* user,
                                                   mb3d_mesh* out_mesh) {
    return guard([&] {
        Context* context = as_context(ctx);
        require(settings != nullptr && mesh_settings != nullptr && out_mesh != nullptr,
                MB3D_ERR_INVALID_ARG, "null argument");
        validate_settings(*settings);
        *out_mesh = nullptr;

        ProgressReporter reporter;
        reporter.configure(progress, nullptr, user);

        IsoSurfaceExtractor extractor(context->vulkan, context->registry, *settings,
                                      *mesh_settings);
        auto handle = std::make_unique<MeshHandle>();
        handle->mesh = extractor.extract(reporter);
        handle->scale_to_mm = mesh_settings->scale_to_mm > 0.0f
            ? mesh_settings->scale_to_mm : 1.0f;

        *out_mesh = reinterpret_cast<mb3d_mesh>(handle.release());
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_stats(mb3d_mesh mesh, mb3d_mesh_stats_t* out) {
    return guard([&] {
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        MeshHandle* handle = as_mesh(mesh);
        *out = handle->mesh.stats(handle->scale_to_mm);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_stl(mb3d_mesh mesh, const char* path,
                                                     int32_t binary) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        MeshHandle* handle = as_mesh(mesh);
        if (binary) {
            write_stl_binary(handle->mesh, path, handle->scale_to_mm);
        } else {
            write_stl_ascii(handle->mesh, path, handle->scale_to_mm);
        }
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_step(mb3d_mesh mesh, const char* path) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        MeshHandle* handle = as_mesh(mesh);
        write_step(handle->mesh, path, handle->scale_to_mm);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_obj(mb3d_mesh mesh, const char* path) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        MeshHandle* handle = as_mesh(mesh);
        write_obj(handle->mesh, path, handle->scale_to_mm);
        return MB3D_OK;
    });
}

MB3D_API void MB3D_CALL mb3d_mesh_destroy(mb3d_mesh mesh) {
    delete reinterpret_cast<MeshHandle*>(mesh);
}

// =====================================================================
// Lua
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_create(mb3d_context ctx, mb3d_lua* out_lua) {
    return guard([&] {
        Context* context = as_context(ctx);
        require(out_lua != nullptr, MB3D_ERR_INVALID_ARG, "out_lua is null");
        *out_lua = nullptr;

        auto handle = std::make_unique<LuaHandle>();
        handle->engine = std::make_unique<LuaEngine>(context->registry);
        *out_lua = reinterpret_cast<mb3d_lua>(handle.release());
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_run_file(mb3d_lua lua, const char* path) {
    return guard([&] {
        require(path != nullptr, MB3D_ERR_INVALID_ARG, "path is null");
        as_lua(lua)->engine->run_file(path);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_run_string(mb3d_lua lua, const char* source) {
    return guard([&] {
        require(source != nullptr, MB3D_ERR_INVALID_ARG, "source is null");
        as_lua(lua)->engine->run_string(source);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_bind_settings(mb3d_lua lua,
                                                        mb3d_render_settings_t* settings) {
    return guard([&] {
        as_lua(lua)->engine->bind_settings(settings);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_call_frame(mb3d_lua lua, const char* fn_name,
                                                     int32_t frame, double time) {
    return guard([&] {
        require(fn_name != nullptr, MB3D_ERR_INVALID_ARG, "fn_name is null");
        as_lua(lua)->engine->call_frame(fn_name, frame, time);
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_push_fft(mb3d_lua lua, const float* bins,
                                                   int32_t count) {
    return guard([&] {
        as_lua(lua)->engine->push_fft(bins, count);
        return MB3D_OK;
    });
}

MB3D_API void MB3D_CALL mb3d_lua_destroy(mb3d_lua lua) {
    delete reinterpret_cast<LuaHandle*>(lua);
}

// =====================================================================
// Animation
// =====================================================================

MB3D_API int32_t MB3D_CALL mb3d_animation_keyframe_count(mb3d_scene scene) {
    if (scene == nullptr) return 0;
    return static_cast<int32_t>(reinterpret_cast<Scene*>(scene)->keyframes.size());
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_animation_keyframe_at(mb3d_scene scene, int32_t index,
                                                            mb3d_keyframe_t* out) {
    return guard([&] {
        Scene* s = as_scene(scene);
        require(out != nullptr, MB3D_ERR_INVALID_ARG, "out is null");
        require(index >= 0 && static_cast<std::size_t>(index) < s->keyframes.size(),
                MB3D_ERR_INVALID_ARG, "keyframe index out of range");
        *out = s->keyframes[static_cast<std::size_t>(index)];
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_animation_evaluate(mb3d_scene scene, int32_t frame,
                                                         mb3d_render_settings_t* settings) {
    return guard([&] {
        Scene* s = as_scene(scene);
        require(settings != nullptr, MB3D_ERR_INVALID_ARG, "settings is null");
        require(!s->keyframes.empty(), MB3D_ERR_INVALID_ARG, "scene carries no animation");
        evaluate_keyframes(s->keyframes, frame, *settings);
        return MB3D_OK;
    });
}

// =====================================================================
// Utility
// =====================================================================

MB3D_API mb3d_result_t MB3D_CALL mb3d_sample_de(mb3d_context ctx,
                                                const mb3d_render_settings_t* settings,
                                                const double point[3], double* out_de) {
    return guard([&] {
        Context* context = as_context(ctx);
        require(settings != nullptr && point != nullptr && out_de != nullptr,
                MB3D_ERR_INVALID_ARG, "null argument");

        const CpuRaymarcher marcher(context->registry, *settings);
        *out_de = marcher.de(Vec3(point));
        return MB3D_OK;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_trace_ray(mb3d_context ctx,
                                                const mb3d_render_settings_t* settings,
                                                const double origin[3],
                                                const double direction[3],
                                                double* out_distance, double out_normal[3]) {
    return guard([&] {
        Context* context = as_context(ctx);
        require(settings != nullptr && origin != nullptr && direction != nullptr,
                MB3D_ERR_INVALID_ARG, "null argument");

        const CpuRaymarcher marcher(context->registry, *settings);
        const Ray ray{Vec3(origin), normalize(Vec3(direction))};
        const CpuRaymarcher::Hit hit = marcher.march(ray);

        if (out_distance) *out_distance = hit.hit ? hit.distance : -1.0;
        if (out_normal) {
            out_normal[0] = hit.normal.x;
            out_normal[1] = hit.normal.y;
            out_normal[2] = hit.normal.z;
        }
        return hit.hit ? MB3D_OK : MB3D_ERR_NOT_READY;
    });
}

MB3D_API mb3d_result_t MB3D_CALL mb3d_mutate(const mb3d_render_settings_t* base,
                                             mb3d_render_settings_t* out_array, int32_t count,
                                             int32_t seed, float strength) {
    return guard([&] {
        require(base != nullptr && out_array != nullptr && count > 0, MB3D_ERR_INVALID_ARG,
                "null or empty output array");
        validate_settings(*base);

        std::mt19937 rng(static_cast<std::uint32_t>(seed));
        std::normal_distribution<float> jitter(0.0f, std::max(strength, 1e-4f));
        std::uniform_int_distribution<int> slot_pick(0, MB3D_FORMULA_SLOTS - 1);
        std::uniform_int_distribution<int> param_pick(0, MB3D_FORMULA_PARAMS - 1);

        for (int i = 0; i < count; ++i) {
            mb3d_render_settings_t variant = *base;
            variant.seed = seed + i;

            // Perturb the parameters of enabled slots multiplicatively,
            // so a parameter of 8.0 and one of 0.05 mutate by
            // proportionate amounts rather than the same absolute step.
            for (int slot = 0; slot < MB3D_FORMULA_SLOTS; ++slot) {
                if (!variant.formulas[slot].enabled) continue;
                for (int p = 0; p < MB3D_FORMULA_PARAMS; ++p) {
                    const float value = variant.formulas[slot].params[p];
                    if (value == 0.0f) continue;
                    variant.formulas[slot].params[p] = value * (1.0f + jitter(rng));
                }
            }

            // Occasionally toggle an extra slot on, which is where the
            // genuinely novel hybrids come from.
            if (strength > 0.4f) {
                const int slot = slot_pick(rng);
                variant.formulas[slot].enabled = 1;
                if (variant.formulas[slot].params[0] == 0.0f) {
                    variant.formulas[slot].params[0] = 2.0f + jitter(rng);
                }
                variant.formulas[slot].params[param_pick(rng)] += jitter(rng);
            }

            out_array[i] = variant;
        }
        return MB3D_OK;
    });
}

}  // extern "C"
