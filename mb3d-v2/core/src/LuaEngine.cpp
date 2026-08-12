// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// LuaEngine.cpp -- embedded LuaJIT via sol2.
//
// Scripts get a `scene` table that is a live view onto the bound
// mb3d_render_settings_t, not a copy: writing scene.camera.zoom mutates
// the struct the renderer is about to read. That is what makes scripted
// camera flights work without a marshalling step per frame.
//
// The sandbox removes os.execute, io.popen and package loading. Scripts
// are user content, and a .m3p shared on a forum may well arrive with a
// companion .lua -- there is no reason for either to spawn processes.

#include "mb3d/LuaEngine.hpp"

#include <algorithm>
#include <cmath>

#if defined(MB3D_WITH_LUA)
#  include <sol/sol.hpp>
#endif

namespace mb3d {

#if defined(MB3D_WITH_LUA)

struct LuaEngine::Impl {
    sol::state lua;
    mb3d_render_settings_t* bound{nullptr};
    std::vector<float> fft;
};

namespace {

// Exposes a fixed-size float array as a 1-based Lua table proxy so
// scripts can write scene.formula[1].params[1] naturally.
template <typename T, std::size_t N>
sol::table array_proxy(sol::state_view lua, T (&array)[N]) {
    sol::table proxy = lua.create_table();
    sol::table meta = lua.create_table();

    meta.set_function("__index", [&array](sol::table, int index) -> double {
        if (index < 1 || index > static_cast<int>(N)) return 0.0;
        return static_cast<double>(array[index - 1]);
    });
    meta.set_function("__newindex", [&array](sol::table, int index, double value) {
        if (index < 1 || index > static_cast<int>(N)) return;
        array[index - 1] = static_cast<T>(value);
    });
    meta.set_function("__len", [](sol::table) { return static_cast<int>(N); });

    proxy[sol::metatable_key] = meta;
    return proxy;
}

}  // namespace

LuaEngine::LuaEngine(FormulaRegistry& registry) : registry_(registry), impl_(new Impl) {
    sol::state& lua = impl_->lua;
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                       sol::lib::table, sol::lib::bit32, sol::lib::jit);

    // Sandbox. Scripts are untrusted -- a .m4d shared on a forum may
    // well arrive with a companion .lua, and there is no reason for
    // either to touch the filesystem or spawn a process.
    lua["os"] = sol::nil;
    lua["io"] = sol::nil;
    lua["package"] = sol::nil;
    lua["require"] = sol::nil;
    lua["dofile"] = sol::nil;
    lua["loadfile"] = sol::nil;
    lua["load"] = sol::nil;
    lua["loadstring"] = sol::nil;

    // ffi is the important one. LuaJIT's FFI grants arbitrary dlopen and
    // raw read/write over the host process's address space, which makes
    // any downloaded script equivalent to a native executable. Removing
    // the other libraries and leaving this would be security theatre.
    lua["ffi"] = sol::nil;
    // jit.util exposes bytecode internals that can be used to reach the
    // same places by a longer route.
    lua.script("if jit and jit.util then jit.util = nil end");

    bind_types();
}

LuaEngine::~LuaEngine() = default;

void LuaEngine::bind_types() {
    sol::state& lua = impl_->lua;

    lua.new_usertype<mb3d_camera_transform_t>(
        "Camera",
        "distance",    &mb3d_camera_transform_t::distance,
        "zoom",        &mb3d_camera_transform_t::zoom,
        "fov",         &mb3d_camera_transform_t::fov_degrees,
        "near_clip",   &mb3d_camera_transform_t::near_clip,
        "far_clip",    &mb3d_camera_transform_t::far_clip,
        "dof_focal_plane", &mb3d_camera_transform_t::dof_focal_plane,
        "dof_strength",    &mb3d_camera_transform_t::dof_strength,
        "stereo_ipd",  &mb3d_camera_transform_t::stereo_ipd,
        "stereo_mode", &mb3d_camera_transform_t::stereo_mode,
        "projection",  &mb3d_camera_transform_t::projection,
        // Vectors are exposed as get/set pairs rather than tables so a
        // script cannot leave a half-assigned position behind.
        "get_position", [](mb3d_camera_transform_t& c) {
            return std::make_tuple(c.position[0], c.position[1], c.position[2]);
        },
        "set_position", [](mb3d_camera_transform_t& c, double x, double y, double z) {
            c.position[0] = x; c.position[1] = y; c.position[2] = z;
        },
        "get_target", [](mb3d_camera_transform_t& c) {
            return std::make_tuple(c.target[0], c.target[1], c.target[2]);
        },
        "set_target", [](mb3d_camera_transform_t& c, double x, double y, double z) {
            c.target[0] = x; c.target[1] = y; c.target[2] = z;
        },
        "get_rotation", [](mb3d_camera_transform_t& c) {
            return std::make_tuple(c.rotation[0], c.rotation[1], c.rotation[2]);
        },
        "set_rotation", [](mb3d_camera_transform_t& c, double x, double y, double z) {
            c.rotation[0] = x; c.rotation[1] = y; c.rotation[2] = z;
        });

    lua.new_usertype<mb3d_formula_slot_t>(
        "FormulaSlot",
        "enabled",          &mb3d_formula_slot_t::enabled,
        "kind",             &mb3d_formula_slot_t::kind,
        "hybrid_mode",      &mb3d_formula_slot_t::hybrid_mode,
        "iteration_weight", &mb3d_formula_slot_t::iteration_weight,
        "start_iteration",  &mb3d_formula_slot_t::start_iteration,
        "stop_iteration",   &mb3d_formula_slot_t::stop_iteration,
        "julia_enabled",    &mb3d_formula_slot_t::julia_enabled,
        "name", sol::property(
            [](mb3d_formula_slot_t& s) {
                return std::string(s.name, ::strnlen(s.name, MB3D_MAX_NAME));
            },
            [](mb3d_formula_slot_t& s, const std::string& v) {
                const std::size_t n = std::min(v.size(), sizeof(s.name) - 1);
                std::memcpy(s.name, v.data(), n);
                s.name[n] = '\0';
            }),
        "get_param", [](mb3d_formula_slot_t& s, int i) -> double {
            return (i >= 1 && i <= MB3D_FORMULA_PARAMS) ? s.params[i - 1] : 0.0;
        },
        "set_param", [](mb3d_formula_slot_t& s, int i, double v) {
            if (i >= 1 && i <= MB3D_FORMULA_PARAMS) s.params[i - 1] = static_cast<float>(v);
        });

    lua.new_usertype<mb3d_calculation_settings_t>(
        "Calculation",
        "max_iterations",      &mb3d_calculation_settings_t::max_iterations,
        "escape_radius",       &mb3d_calculation_settings_t::escape_radius,
        "raystep_multiplier",  &mb3d_calculation_settings_t::raystep_multiplier,
        "stepwidth_limiter",   &mb3d_calculation_settings_t::stepwidth_limiter,
        "de_stop",             &mb3d_calculation_settings_t::de_stop_criterion,
        "de_max_steps",        &mb3d_calculation_settings_t::de_max_steps,
        "binary_search_steps", &mb3d_calculation_settings_t::binary_search_steps,
        "de_scale",            &mb3d_calculation_settings_t::de_scale);

    lua.new_usertype<mb3d_coloring_settings_t>(
        "Coloring",
        "trap_kind",     &mb3d_coloring_settings_t::trap_kind,
        "trap_size",     &mb3d_coloring_settings_t::trap_size,
        "trap_influence",&mb3d_coloring_settings_t::trap_influence,
        "color_speed",   &mb3d_coloring_settings_t::color_speed,
        "color_offset",  &mb3d_coloring_settings_t::color_offset,
        "color_cycle",   &mb3d_coloring_settings_t::color_cycle,
        "get_gradient", [](mb3d_coloring_settings_t& c, int i) -> std::uint32_t {
            return (i >= 1 && i <= MB3D_GRADIENT_ENTRIES) ? c.gradient[i - 1] : 0u;
        },
        "set_gradient", [](mb3d_coloring_settings_t& c, int i, std::uint32_t v) {
            if (i >= 1 && i <= MB3D_GRADIENT_ENTRIES) c.gradient[i - 1] = v;
        });

    lua.new_usertype<mb3d_light_channel_t>(
        "LightChannel",
        "kind",      &mb3d_light_channel_t::kind,
        "enabled",   &mb3d_light_channel_t::enabled,
        "intensity", &mb3d_light_channel_t::intensity,
        "falloff",   &mb3d_light_channel_t::falloff,
        "specular",  &mb3d_light_channel_t::specular,
        "diffuse",   &mb3d_light_channel_t::diffuse,
        "set_color", [](mb3d_light_channel_t& l, double r, double g, double b) {
            l.color[0] = static_cast<float>(r);
            l.color[1] = static_cast<float>(g);
            l.color[2] = static_cast<float>(b);
        },
        "set_position", [](mb3d_light_channel_t& l, double x, double y, double z) {
            l.position[0] = static_cast<float>(x);
            l.position[1] = static_cast<float>(y);
            l.position[2] = static_cast<float>(z);
        });

    // Audio-reactive helpers. `fft` holds the most recent magnitude
    // spectrum pushed by the host; band() averages a normalised slice of
    // it, which is what a script actually wants ("how loud is the bass")
    // rather than raw bins.
    sol::table audio = lua.create_named_table("audio");
    audio.set_function("bins", [this] { return static_cast<int>(impl_->fft.size()); });
    audio.set_function("bin", [this](int i) -> double {
        const auto index = static_cast<std::size_t>(i - 1);
        return index < impl_->fft.size() ? impl_->fft[index] : 0.0;
    });
    audio.set_function("band", [this](double low, double high) -> double {
        if (impl_->fft.empty()) return 0.0;
        const double n = static_cast<double>(impl_->fft.size());
        auto lo = static_cast<std::size_t>(std::clamp(low, 0.0, 1.0) * n);
        auto hi = static_cast<std::size_t>(std::clamp(high, 0.0, 1.0) * n);
        if (hi <= lo) hi = lo + 1;
        hi = std::min(hi, impl_->fft.size());
        double sum = 0.0;
        for (std::size_t i = lo; i < hi; ++i) sum += impl_->fft[i];
        return sum / static_cast<double>(hi - lo);
    });

    // Formula registration from script -- lets a plugin ship a formula
    // without a separate .m3f file.
    lua.set_function("register_formula",
        [this](const std::string& name, const std::string& source) {
            registry_.add(FormulaRegistry::parse_formula_source(name, source,
                                                                MB3D_FORMULA_CUSTOM));
        });

    sol::table log = lua.create_named_table("mb3d");
    log.set_function("version", [] {
        return std::to_string(MB3D_VERSION_MAJOR) + "." + std::to_string(MB3D_VERSION_MINOR);
    });
}

void LuaEngine::bind_settings(mb3d_render_settings_t* settings) {
    require(settings != nullptr, MB3D_ERR_INVALID_ARG, "cannot bind a null settings pointer");
    impl_->bound = settings;

    sol::state& lua = impl_->lua;
    sol::table scene = lua.create_named_table("scene");

    scene["camera"]      = std::ref(settings->camera);
    scene["calculation"] = std::ref(settings->calculation);
    scene["coloring"]    = std::ref(settings->coloring);

    sol::table formulas = lua.create_table();
    for (int i = 0; i < MB3D_FORMULA_SLOTS; ++i) {
        formulas[i + 1] = std::ref(settings->formulas[i]);
    }
    scene["formula"] = formulas;

    sol::table lights = lua.create_table();
    for (int i = 0; i < MB3D_LIGHT_CHANNELS; ++i) {
        lights[i + 1] = std::ref(settings->lighting.channels[i]);
    }
    scene["light"] = lights;

    scene["width"] = sol::property(
        [settings] { return settings->width; },
        [settings](int v) { settings->width = v; });
    scene["height"] = sol::property(
        [settings] { return settings->height; },
        [settings](int v) { settings->height = v; });
    scene["seed"] = sol::property(
        [settings] { return settings->seed; },
        [settings](int v) { settings->seed = v; });
}

void LuaEngine::run_string(std::string_view source) {
    const sol::protected_function_result result =
        impl_->lua.safe_script(source, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        fail(MB3D_ERR_LUA, std::string("Lua error: ") + err.what());
    }
}

void LuaEngine::run_file(const std::string& path) {
    const sol::protected_function_result result =
        impl_->lua.safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        fail(MB3D_ERR_LUA, std::string("Lua error in '") + path + "': " + err.what());
    }
}

void LuaEngine::call_frame(const std::string& fn_name, int frame, double time) {
    sol::protected_function fn = impl_->lua[fn_name];
    require(fn.valid(), MB3D_ERR_LUA, "no such Lua function: " + fn_name);

    const sol::protected_function_result result = fn(frame, time);
    if (!result.valid()) {
        const sol::error err = result;
        fail(MB3D_ERR_LUA, std::string("Lua error in ") + fn_name + "(): " + err.what());
    }
}

void LuaEngine::push_fft(const float* bins, int count) {
    if (bins == nullptr || count <= 0) {
        impl_->fft.clear();
        return;
    }
    impl_->fft.assign(bins, bins + count);
}

#else  // !MB3D_WITH_LUA

struct LuaEngine::Impl {};

LuaEngine::LuaEngine(FormulaRegistry& registry) : registry_(registry), impl_(new Impl) {
    fail(MB3D_ERR_UNSUPPORTED,
         "scripting requires LuaJIT; rebuild with -DMB3D_WITH_LUA=ON");
}

LuaEngine::~LuaEngine() = default;
void LuaEngine::bind_types() {}
void LuaEngine::bind_settings(mb3d_render_settings_t*) {}
void LuaEngine::run_string(std::string_view) {}
void LuaEngine::run_file(const std::string&) {}
void LuaEngine::call_frame(const std::string&, int, double) {}
void LuaEngine::push_fft(const float*, int) {}

#endif  // MB3D_WITH_LUA

}  // namespace mb3d
