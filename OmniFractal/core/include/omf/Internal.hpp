// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// Internal.hpp -- shared C++20 plumbing behind the C ABI.
// Nothing in here is visible to hosts; Renderer.h is the whole contract.

#pragma once

#include "omf/Renderer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace omf {

// ---------------------------------------------------------------------
// Error reporting. The C ABI never throws; every entry point funnels
// through guard() which converts exceptions into result codes and stores
// the message in thread-local storage for omf_last_error().
// ---------------------------------------------------------------------

void set_last_error(std::string message) noexcept;
const char* last_error() noexcept;

class Error : public std::exception {
public:
    Error(omf_result_t code, std::string what)
        : code_(code), what_(std::move(what)) {}
    const char* what() const noexcept override { return what_.c_str(); }
    omf_result_t code() const noexcept { return code_; }

private:
    omf_result_t code_;
    std::string what_;
};

// Wraps a callable so any escaping exception becomes a result code.
template <typename Fn>
omf_result_t guard(Fn&& fn) noexcept {
    try {
        set_last_error({});
        return fn();
    } catch (const Error& e) {
        set_last_error(e.what());
        return e.code();
    } catch (const std::bad_alloc&) {
        set_last_error("out of memory");
        return OMF_ERR_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return OMF_ERR_UNKNOWN;
    } catch (...) {
        set_last_error("unknown error");
        return OMF_ERR_UNKNOWN;
    }
}

[[noreturn]] inline void fail(omf_result_t code, std::string msg) {
    throw Error(code, std::move(msg));
}

inline void require(bool cond, omf_result_t code, std::string msg) {
    if (!cond) fail(code, std::move(msg));
}

// ---------------------------------------------------------------------
// Small vector maths. Deliberately not GLM-typed at this level: the
// raymarcher runs in double precision because deep zooms need it, and
// GLM's double vectors do not vectorise any better than this does.
// ---------------------------------------------------------------------

struct Vec3 {
    double x{}, y{}, z{};

    constexpr Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(const double* p) : x(p[0]), y(p[1]), z(p[2]) {}
    explicit Vec3(const float* p) : x(p[0]), y(p[1]), z(p[2]) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(const Vec3& v);
Vec3 normalize(const Vec3& v);
Vec3 abs(const Vec3& v);
Vec3 min(const Vec3& a, const Vec3& b);
Vec3 max(const Vec3& a, const Vec3& b);
Vec3 rotate_euler(const Vec3& v, double rx, double ry, double rz);   // degrees, XYZ

struct Vec4 {
    double x{}, y{}, z{}, w{};
    Vec3 xyz() const { return {x, y, z}; }
};

// ---------------------------------------------------------------------
// Formula evaluation
// ---------------------------------------------------------------------

// Mutable state threaded through the hybrid iteration loop. `dr` is the
// running derivative estimate used for the distance estimate; `trap`
// accumulates the orbit-trap minimum for coloring.
struct IterationState {
    Vec3   z{};
    Vec3   c{};
    double w{};          // 4th component for 4D formulas
    double cw{};
    double dr{1.0};
    double r{};          // |z| at the end of the last step
    double trap{1e30};
    double trap_sum{};
    int    iteration{};
    int    escaped_at{-1};
};

// A formula is a pure step function: it advances IterationState by one
// iteration using the slot's parameters. Built-ins are plain functions;
// custom formulas from .m3f/.d3f are compiled into the same signature.
using FormulaFn = void (*)(IterationState&, const omf_formula_slot_t&);

struct FormulaDef {
    std::string name;
    omf_formula_kind_t kind{OMF_FORMULA_NONE};
    FormulaFn   fn{nullptr};
    std::string glsl;          // GPU body, injected into raymarch.comp
    std::string source;        // original .m3f/.d3f text, for round-trip
    bool        analytic_de{true};
};

// Registry of built-ins plus anything registered from disk at runtime.
class FormulaRegistry {
public:
    FormulaRegistry();

    const FormulaDef* find(std::string_view name) const;
    void add(FormulaDef def);
    // Parses .m3f / .d3f source into a FormulaDef. Throws Error on a
    // malformed header.
    static FormulaDef parse_formula_source(std::string_view name,
                                           std::string_view source,
                                           omf_formula_kind_t kind);

    std::size_t size() const { return order_.size(); }
    const std::string& name_at(std::size_t i) const { return order_.at(i); }

private:
    std::unordered_map<std::string, FormulaDef> defs_;
    std::vector<std::string> order_;
};

// Resolved per-render formula pipeline: slot definitions paired with the
// callable, so the inner loop never touches a hash map.
struct FormulaPipeline {
    struct Slot {
        const FormulaDef*      def{nullptr};
        omf_formula_slot_t    cfg{};
    };
    std::vector<Slot> slots;
    omf_hybrid_mode_t master{OMF_HYBRID_SEQUENTIAL};

    static FormulaPipeline build(const FormulaRegistry& reg,
                                 const omf_render_settings_t& settings);
};

// The distance estimator. This is *the* hot function: the raymarcher,
// the marching-cubes extractor and omf_sample_de all call it.
double distance_estimate(const FormulaPipeline& pipe,
                         const omf_calculation_settings_t& calc,
                         const omf_coloring_settings_t& color,
                         const Vec3& point,
                         IterationState* out_state = nullptr);

// ---------------------------------------------------------------------
// Precision profiles and migration (PrecisionProfiles.cpp)
// ---------------------------------------------------------------------

// Returns nullptr for an unknown id. Callers must treat that as an error
// rather than falling back to a default profile -- silently substituting
// different evaluation semantics is how a scene changes inexplicably.
const omf_precision_profile_t* find_profile(std::string_view id);
std::size_t profile_count();
const omf_precision_profile_t& profile_at(std::size_t index);

// Re-tunes legacy parameters for a target profile and resolution. Never
// mutates `legacy`; the caller keeps both so the change is reversible.
void migrate_parameters(const omf_render_settings_t& legacy,
                        int target_width, int target_height,
                        const omf_precision_profile_t& target_profile,
                        omf_render_settings_t& out);

// `lock_to_legacy_look` scales DEstop with resolution to preserve the
// original appearance; clearing it lets detail grow with pixel count.
void rescale_for_resolution(omf_render_settings_t& settings,
                            int target_width, int target_height,
                            bool lock_to_legacy_look);

// Interpolates a keyframe timeline into `settings`.
void evaluate_keyframes(const std::vector<omf_keyframe_t>& keyframes, int frame,
                        omf_render_settings_t& settings);

// ---------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------

struct Ray {
    Vec3 origin;
    Vec3 direction;   // normalised
};

class Camera {
public:
    explicit Camera(const omf_camera_transform_t& t, int width, int height,
                    double eye_offset = 0.0);
    // px/py are pixel coordinates including sub-pixel jitter.
    Ray primary_ray(double px, double py) const;

private:
    Vec3   origin_;
    Vec3   forward_, right_, up_;
    double half_w_{}, half_h_{};
    double inv_w_{}, inv_h_{};
    bool   ortho_{false};
    bool   equirect_{false};
};

// ---------------------------------------------------------------------
// Layer storage
// ---------------------------------------------------------------------

struct Layer {
    std::vector<std::byte> bytes;
    int width{}, height{};
    int stride{};
    omf_pixel_format_t format{OMF_PF_BGRA8};
    omf_layer_t which{OMF_LAYER_RGBA};

    template <typename T> std::span<T> as() {
        return {reinterpret_cast<T*>(bytes.data()), bytes.size() / sizeof(T)};
    }
    template <typename T> T* row(int y) {
        return reinterpret_cast<T*>(bytes.data() + static_cast<std::size_t>(y) * stride);
    }
};

int bytes_per_pixel(omf_pixel_format_t f);

class FrameBuffer {
public:
    FrameBuffer(int width, int height, std::uint32_t layer_mask);
    Layer* layer(omf_layer_t which);
    const Layer* layer(omf_layer_t which) const;
    bool has(omf_layer_t which) const { return layer(which) != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_, height_;
    std::vector<std::unique_ptr<Layer>> layers_;
};

// ---------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------

struct Rgb {
    float r{}, g{}, b{};
    Rgb operator+(const Rgb& o) const { return {r + o.r, g + o.g, b + o.b}; }
    Rgb operator*(float s) const { return {r * s, g * s, b * s}; }
    Rgb operator*(const Rgb& o) const { return {r * o.r, g * o.g, b * o.b}; }
    Rgb& operator+=(const Rgb& o) { r += o.r; g += o.g; b += o.b; return *this; }
};

// Samples the 256-entry ramp with linear interpolation and wrapping,
// applying speed / offset / cycle exactly as the legacy editor does.
Rgb sample_gradient(const omf_coloring_settings_t& c, double t);
std::uint32_t pack_bgra(const Rgb& c, float alpha = 1.0f);

// ---------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------

struct Mesh {
    std::vector<float>         positions;   // xyz triples
    std::vector<float>         normals;     // xyz triples, may be empty
    std::vector<std::uint32_t> indices;     // triangle list

    std::size_t vertex_count() const { return positions.size() / 3; }
    std::size_t triangle_count() const { return indices.size() / 3; }

    void compute_normals();
    void weld(float epsilon);
    void laplacian_smooth(int iterations);
    omf_mesh_stats_t stats(float scale_to_mm) const;
};

// ---------------------------------------------------------------------
// Progress plumbing
// ---------------------------------------------------------------------

class ProgressReporter {
public:
    ProgressReporter() = default;
    void configure(omf_progress_fn fn, omf_cancel_fn cancel, void* user);
    void begin(std::int64_t total_units, int tiles_total);
    void advance(std::int64_t units, int tiles = 0);
    void finish(omf_job_state_t state);
    bool cancelled() const;
    omf_progress_t snapshot() const;

private:
    mutable std::atomic<std::int64_t> done_{0};
    std::atomic<std::int64_t> total_{0};
    std::atomic<int> tiles_done_{0};
    std::atomic<int> tiles_total_{0};
    std::atomic<int> state_{OMF_JOB_IDLE};
    mutable std::atomic<bool> cancel_flag_{false};
    std::chrono::steady_clock::time_point start_{};
    omf_progress_fn progress_fn_{nullptr};
    omf_cancel_fn   cancel_fn_{nullptr};
    void*            user_{nullptr};
};

}  // namespace omf
