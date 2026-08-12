// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// CpuRaymarcher.cpp -- multi-threaded CPU reference renderer.
//
// This path is the ground truth. The Vulkan compute shader is written to
// match it; when the two disagree, this one is right. It also carries the
// whole feature set on machines with no usable Vulkan device, which is
// why it implements shading, traps, shadows, SSAO and DOF rather than
// being a stripped-down fallback.
//
// Threading: tiles are handed out from one atomic counter, so scheduling
// stays fair even though per-pixel cost varies by orders of magnitude
// across a fractal boundary.

#include "omf/CpuRaymarcher.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

namespace omf {
namespace {

constexpr double kDeg2Rad = std::numbers::pi / 180.0;

// Cheap deterministic hash -> [0,1). Used for supersample jitter and
// SSAO kernels so a re-render of the same tile is bit-identical, which
// the distributed dispatcher depends on.
inline double hash01(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<double>(x) * (1.0 / 4294967296.0);
}

}  // namespace

// ---------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------

Camera::Camera(const omf_camera_transform_t& t, int width, int height, double eye_offset) {
    origin_ = Vec3(t.position);
    Vec3 target = Vec3(t.target);

    // Legacy scenes carry Euler angles + distance instead of an explicit
    // eye position. Detect that (target == position) and derive the eye.
    Vec3 delta = target - origin_;
    if (length(delta) < 1e-12) {
        const Vec3 base{0.0, 0.0, -1.0};
        Vec3 dir = rotate_euler(base, t.rotation[0], t.rotation[1], t.rotation[2]);
        const double dist = t.distance != 0.0 ? t.distance : 3.0;
        origin_ = target - dir * dist;
        delta = dir * dist;
    }

    forward_ = normalize(delta);
    Vec3 up_hint = Vec3(t.up);
    if (length(up_hint) < 1e-12) up_hint = Vec3{0.0, 1.0, 0.0};
    if (std::fabs(dot(normalize(up_hint), forward_)) > 0.999) up_hint = Vec3{0.0, 0.0, 1.0};

    right_ = normalize(cross(forward_, up_hint));
    up_    = normalize(cross(right_, forward_));

    // Stereo eye separation happens here so both eyes share everything
    // downstream.
    if (eye_offset != 0.0) origin_ += right_ * eye_offset;

    ortho_    = (t.projection == 1) || (t.fov_degrees <= 0.0);
    equirect_ = (t.projection == 2);

    const double aspect = t.aspect > 0.0
        ? t.aspect
        : (height > 0 ? static_cast<double>(width) / static_cast<double>(height) : 1.0);

    const double zoom = t.zoom != 0.0 ? t.zoom : 1.0;
    if (ortho_) {
        half_h_ = 1.0 / zoom;
        half_w_ = half_h_ * aspect;
    } else {
        half_h_ = std::tan(0.5 * t.fov_degrees * kDeg2Rad) / zoom;
        half_w_ = half_h_ * aspect;
    }

    inv_w_ = width > 0 ? 1.0 / width : 1.0;
    inv_h_ = height > 0 ? 1.0 / height : 1.0;
}

Ray Camera::primary_ray(double px, double py) const {
    // NDC in [-1, 1], y flipped so row 0 is the top of the image.
    const double ndc_x = (px * inv_w_) * 2.0 - 1.0;
    const double ndc_y = 1.0 - (py * inv_h_) * 2.0;

    if (equirect_) {
        // 360 x 180 latlong projection for VR / dome output.
        const double lon = ndc_x * std::numbers::pi;
        const double lat = ndc_y * 0.5 * std::numbers::pi;
        const Vec3 d = forward_ * (std::cos(lat) * std::cos(lon))
                     + right_   * (std::cos(lat) * std::sin(lon))
                     + up_      * std::sin(lat);
        return {origin_, normalize(d)};
    }

    if (ortho_) {
        const Vec3 o = origin_ + right_ * (ndc_x * half_w_) + up_ * (ndc_y * half_h_);
        return {o, forward_};
    }

    const Vec3 d = forward_ + right_ * (ndc_x * half_w_) + up_ * (ndc_y * half_h_);
    return {origin_, normalize(d)};
}

// ---------------------------------------------------------------------
// Frame buffer
// ---------------------------------------------------------------------

int bytes_per_pixel(omf_pixel_format_t f) {
    switch (f) {
        case OMF_PF_BGRA8:   return 4;
        case OMF_PF_R32F:    return 4;
        case OMF_PF_RG32F:   return 8;
        case OMF_PF_RGB32F:  return 12;
        case OMF_PF_RGBA32F: return 16;
        case OMF_PF_U32:     return 4;
    }
    return 4;
}

namespace {

omf_pixel_format_t format_for(omf_layer_t layer) {
    switch (layer) {
        case OMF_LAYER_RGBA:    return OMF_PF_BGRA8;
        case OMF_LAYER_RGBA32F: return OMF_PF_RGBA32F;
        case OMF_LAYER_DEPTH:   return OMF_PF_R32F;
        case OMF_LAYER_NORMAL:  return OMF_PF_RGB32F;
        case OMF_LAYER_SSAO:    return OMF_PF_R32F;
        case OMF_LAYER_SHADOW:  return OMF_PF_R32F;
        case OMF_LAYER_MOTION:  return OMF_PF_RG32F;
        case OMF_LAYER_OBJECT:  return OMF_PF_U32;
    }
    return OMF_PF_BGRA8;
}

}  // namespace

FrameBuffer::FrameBuffer(int width, int height, std::uint32_t layer_mask)
    : width_(width), height_(height) {
    require(width > 0 && height > 0, OMF_ERR_INVALID_ARG, "frame buffer has zero extent");

    constexpr omf_layer_t kAll[] = {
        OMF_LAYER_RGBA, OMF_LAYER_RGBA32F, OMF_LAYER_DEPTH, OMF_LAYER_NORMAL,
        OMF_LAYER_SSAO, OMF_LAYER_SHADOW, OMF_LAYER_MOTION, OMF_LAYER_OBJECT};

    for (omf_layer_t which : kAll) {
        if ((layer_mask & static_cast<std::uint32_t>(which)) == 0) continue;
        auto layer = std::make_unique<Layer>();
        layer->which  = which;
        layer->format = format_for(which);
        layer->width  = width;
        layer->height = height;
        layer->stride = width * bytes_per_pixel(layer->format);
        layer->bytes.assign(static_cast<std::size_t>(layer->stride) * height, std::byte{0});
        layers_.push_back(std::move(layer));
    }
}

Layer* FrameBuffer::layer(omf_layer_t which) {
    for (auto& l : layers_) if (l->which == which) return l.get();
    return nullptr;
}

const Layer* FrameBuffer::layer(omf_layer_t which) const {
    for (const auto& l : layers_) if (l->which == which) return l.get();
    return nullptr;
}

// ---------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------

void ProgressReporter::configure(omf_progress_fn fn, omf_cancel_fn cancel, void* user) {
    progress_fn_ = fn;
    cancel_fn_ = cancel;
    user_ = user;
}

void ProgressReporter::begin(std::int64_t total_units, int tiles_total) {
    done_.store(0, std::memory_order_relaxed);
    total_.store(total_units, std::memory_order_relaxed);
    tiles_done_.store(0, std::memory_order_relaxed);
    tiles_total_.store(tiles_total, std::memory_order_relaxed);
    state_.store(OMF_JOB_RUNNING, std::memory_order_release);
    cancel_flag_.store(false, std::memory_order_relaxed);
    start_ = std::chrono::steady_clock::now();
}

void ProgressReporter::advance(std::int64_t units, int tiles) {
    done_.fetch_add(units, std::memory_order_relaxed);
    if (tiles) tiles_done_.fetch_add(tiles, std::memory_order_relaxed);
    if (progress_fn_) {
        const omf_progress_t p = snapshot();
        progress_fn_(&p, user_);
    }
}

void ProgressReporter::finish(omf_job_state_t state) {
    state_.store(state, std::memory_order_release);
    if (progress_fn_) {
        const omf_progress_t p = snapshot();
        progress_fn_(&p, user_);
    }
}

bool ProgressReporter::cancelled() const {
    if (cancel_flag_.load(std::memory_order_relaxed)) return true;
    if (cancel_fn_ && cancel_fn_(user_) != 0) {
        cancel_flag_.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

omf_progress_t ProgressReporter::snapshot() const {
    omf_progress_t p{};
    const std::int64_t done = done_.load(std::memory_order_relaxed);
    const std::int64_t total = total_.load(std::memory_order_relaxed);
    p.state = state_.load(std::memory_order_acquire);
    p.pixels_done = done;
    p.pixels_total = total;
    p.fraction = total > 0 ? static_cast<float>(static_cast<double>(done) / total) : 0.0f;
    p.tiles_done = tiles_done_.load(std::memory_order_relaxed);
    p.tiles_total = tiles_total_.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    p.elapsed_seconds = std::chrono::duration<double>(now - start_).count();
    p.estimated_remaining = p.fraction > 0.001f
        ? p.elapsed_seconds * (1.0 / p.fraction - 1.0)
        : 0.0;
    return p;
}

// ---------------------------------------------------------------------
// Raymarcher
// ---------------------------------------------------------------------

CpuRaymarcher::CpuRaymarcher(const FormulaRegistry& registry,
                             const omf_render_settings_t& settings)
    : settings_(settings),
      pipeline_(FormulaPipeline::build(registry, settings)) {}

double CpuRaymarcher::de(const Vec3& p) const {
    return distance_estimate(pipeline_, settings_.calculation, settings_.coloring, p);
}

double CpuRaymarcher::de_state(const Vec3& p, IterationState& out) const {
    return distance_estimate(pipeline_, settings_.calculation, settings_.coloring, p, &out);
}

// Central-difference gradient. `normal_epsilon` scales with hit distance
// so distant surfaces do not alias into noise.
Vec3 CpuRaymarcher::normal_at(const Vec3& p, double hit_distance) const {
    const double base = settings_.calculation.normal_epsilon > 0.0f
        ? settings_.calculation.normal_epsilon
        : 1e-5;
    const double e = base * std::max(1.0, hit_distance);
    const Vec3 dx{e, 0, 0}, dy{0, e, 0}, dz{0, 0, e};
    return normalize(Vec3{de(p + dx) - de(p - dx),
                          de(p + dy) - de(p - dy),
                          de(p + dz) - de(p - dz)});
}

// Clip the ray against the optional bounding sphere and box before
// marching. On deep zooms this is the difference between a render that
// finishes and one that does not.
bool CpuRaymarcher::clip_ray(const Ray& ray, double& t_near, double& t_far) const {
    const auto& c = settings_.calculation;
    t_near = 0.0;
    t_far = settings_.camera.far_clip > 0.0 ? settings_.camera.far_clip : 100.0;

    if (c.bound_sphere_enabled) {
        const Vec3 centre(c.bound_sphere_center);
        const double radius = c.bound_sphere_radius > 0.0f ? c.bound_sphere_radius : 2.0;
        const Vec3 oc = ray.origin - centre;
        const double b = dot(oc, ray.direction);
        const double cc = dot(oc, oc) - radius * radius;
        const double disc = b * b - cc;
        if (disc < 0.0) return false;
        const double sq = std::sqrt(disc);
        t_near = std::max(t_near, -b - sq);
        t_far  = std::min(t_far,  -b + sq);
        if (t_near > t_far) return false;
    }

    if (c.bound_box_enabled) {
        const Vec3 lo(c.bound_box_min), hi(c.bound_box_max);
        for (int axis = 0; axis < 3; ++axis) {
            const double d = ray.direction[axis];
            if (std::fabs(d) < 1e-12) {
                if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis]) return false;
                continue;
            }
            double t0 = (lo[axis] - ray.origin[axis]) / d;
            double t1 = (hi[axis] - ray.origin[axis]) / d;
            if (t0 > t1) std::swap(t0, t1);
            t_near = std::max(t_near, t0);
            t_far  = std::min(t_far,  t1);
            if (t_near > t_far) return false;
        }
    }

    t_near = std::max(t_near, settings_.camera.near_clip);
    return t_near <= t_far;
}

CpuRaymarcher::Hit CpuRaymarcher::march(const Ray& ray) const {
    const auto& c = settings_.calculation;
    Hit hit;

    double t, t_far;
    if (!clip_ray(ray, t, t_far)) return hit;

    const double step_mul = c.raystep_multiplier > 0.0f ? c.raystep_multiplier : 1.0;
    const double step_max = c.stepwidth_limiter > 0.0f
        ? c.stepwidth_limiter
        : std::numeric_limits<double>::max();
    const double stop = c.de_stop_criterion > 0.0f ? c.de_stop_criterion : 1e-5;
    const int max_steps = c.de_max_steps > 0 ? c.de_max_steps : 300;

    double prev_t = t;
    double prev_d = 0.0;

    for (int i = 0; i < max_steps && t < t_far; ++i) {
        const Vec3 p = ray.origin + ray.direction * t;
        const double d = de(p);
        hit.steps = i + 1;

        // The stop criterion loosens with distance: one pixel subtends
        // more world space further out, so demanding a fixed epsilon
        // everywhere just burns steps for sub-pixel detail.
        const double threshold = stop * std::max(1.0, t);

        if (d < threshold) {
            hit.hit = true;
            hit.distance = t;

            // Binary search refinement between the last outside sample
            // and this inside one. This is what makes the normals smooth
            // enough for SSAO and mesh extraction to be usable.
            if (c.binary_search_steps > 0 && i > 0) {
                double lo = prev_t, hi = t;
                for (int b = 0; b < c.binary_search_steps; ++b) {
                    const double mid = 0.5 * (lo + hi);
                    const double dm = de(ray.origin + ray.direction * mid);
                    if (dm < threshold) hi = mid; else lo = mid;
                }
                hit.distance = hi;
            }

            hit.position = ray.origin + ray.direction * hit.distance;
            hit.normal = normal_at(hit.position, hit.distance);
            de_state(hit.position, hit.state);
            return hit;
        }

        prev_t = t;
        prev_d = d;
        (void)prev_d;

        double advance = d * step_mul;
        advance = std::min(advance, step_max);
        // Never advance by less than the threshold or the marcher stalls
        // in a fold where the DE underestimates badly.
        advance = std::max(advance, threshold * 0.5);
        t += advance;
    }

    hit.distance = t_far;
    return hit;
}

// Hard shadow: march from the surface toward the light and see if
// anything blocks. `softness` widens it into a penumbra using the
// classic min-ratio trick.
double CpuRaymarcher::shadow_ray(const Vec3& origin, const Vec3& direction,
                                 double max_distance, double softness) const {
    const auto& c = settings_.calculation;
    const double stop = c.de_stop_criterion > 0.0f ? c.de_stop_criterion : 1e-5;
    const double bias = std::max(stop * 8.0, 1e-6);

    double t = bias;
    double result = 1.0;
    const int steps = std::max(32, (c.de_max_steps > 0 ? c.de_max_steps : 300) / 3);

    for (int i = 0; i < steps && t < max_distance; ++i) {
        const double d = de(origin + direction * t);
        if (d < stop * std::max(1.0, t)) return 0.0;
        if (softness > 0.0) result = std::min(result, softness * d / t);
        t += std::max(d, bias);
    }
    return std::clamp(result, 0.0, 1.0);
}

// Screen-space-style AO evaluated in world space against the DE field.
// More expensive than a screen-space pass but free of haloing, and it
// works on the CPU path where there is no depth pyramid.
double CpuRaymarcher::ambient_occlusion(const Vec3& p, const Vec3& n,
                                        double radius, int samples) const {
    if (samples <= 0) return 1.0;
    double occlusion = 0.0;
    double weight_sum = 0.0;
    for (int i = 1; i <= samples; ++i) {
        const double h = radius * static_cast<double>(i) / samples;
        const double d = de(p + n * h);
        const double w = 1.0 / (1 << i);
        occlusion += w * (h - d);
        weight_sum += w;
    }
    if (weight_sum <= 0.0) return 1.0;
    return std::clamp(1.0 - 2.0 * occlusion / weight_sum, 0.0, 1.0);
}

Rgb CpuRaymarcher::shade(const Hit& hit, const Ray& ray) const {
    const auto& L = settings_.lighting;
    const auto& C = settings_.coloring;

    if (!hit.hit) return background(ray);

    // Base albedo from the gradient, driven by whichever coloring source
    // the scene selected.
    double t = 0.0;
    if (C.trap_kind != OMF_TRAP_NONE && hit.state.trap < 1e29) {
        t = hit.state.trap * C.trap_influence;
    } else if (C.smooth_iteration && hit.state.escaped_at >= 0) {
        // Continuous iteration count -- removes the banding you get from
        // colouring by the raw integer iteration.
        const double r = std::max(hit.state.r, 1.0000001);
        t = (hit.state.escaped_at + 1.0 - std::log2(std::log(r))) /
            std::max(1, settings_.calculation.max_iterations);
    } else {
        t = static_cast<double>(hit.state.iteration) /
            std::max(1, settings_.calculation.max_iterations);
    }

    if (C.color_by_depth) {
        const double far = settings_.camera.far_clip > 0.0 ? settings_.camera.far_clip : 100.0;
        const double depth_t = std::clamp(hit.distance / far, 0.0, 1.0);
        t = t * (1.0 - C.depth_color_mix) + depth_t * C.depth_color_mix;
    }

    const Rgb albedo = sample_gradient(C, t);

    // Dual-colour ambient: blends near and far ambient by depth, which
    // is what gives legacy renders their sense of aerial perspective.
    const double far = settings_.camera.far_clip > 0.0 ? settings_.camera.far_clip : 100.0;
    const double amb_t = std::clamp(std::pow(hit.distance / far,
                                             std::max(0.01f, L.ambient_falloff)), 0.0, 1.0);
    Rgb ambient{
        static_cast<float>(L.ambient_color_a[0] * (1 - amb_t) + L.ambient_color_b[0] * amb_t),
        static_cast<float>(L.ambient_color_a[1] * (1 - amb_t) + L.ambient_color_b[1] * amb_t),
        static_cast<float>(L.ambient_color_a[2] * (1 - amb_t) + L.ambient_color_b[2] * amb_t)};

    double ao = 1.0;
    if (L.ssao_enabled) {
        ao = ambient_occlusion(hit.position, hit.normal,
                               L.ssao_radius > 0.0f ? L.ssao_radius : 0.05,
                               L.ssao_samples > 0 ? L.ssao_samples : 5);
        ao = 1.0 - (1.0 - ao) * L.ssao_intensity;
    }

    Rgb out = albedo * ambient * static_cast<float>(ao);
    const Vec3 view = -ray.direction;

    for (int i = 0; i < OMF_LIGHT_CHANNELS; ++i) {
        const omf_light_channel_t& ch = L.channels[i];
        if (!ch.enabled || ch.kind == OMF_LIGHT_OFF) continue;

        Vec3 to_light;
        double distance = 1e30;
        double attenuation = 1.0;

        if (ch.kind == OMF_LIGHT_DIRECTIONAL) {
            to_light = normalize(-Vec3(ch.position));
        } else {
            const Vec3 delta = Vec3(ch.position) - hit.position;
            distance = length(delta);
            to_light = distance > 1e-12 ? delta / distance : Vec3{0, 0, 1};
            if (ch.falloff > 0.0f) {
                attenuation = 1.0 / (1.0 + ch.falloff * distance * distance);
            }
        }

        const double ndotl = dot(hit.normal, to_light);
        if (ndotl <= 0.0) continue;

        double visibility = 1.0;
        if (L.hard_shadows && ch.casts_shadow) {
            visibility = shadow_ray(hit.position + hit.normal * std::max(L.shadow_bias, 1e-6),
                                    to_light,
                                    std::min(distance, far),
                                    ch.shadow_softness);
        }
        if (visibility <= 0.0) continue;

        const double diffuse = ndotl * ch.diffuse * attenuation * visibility;

        // Blinn-Phong specular.
        const Vec3 half = normalize(to_light + view);
        const double ndoth = std::max(dot(hit.normal, half), 0.0);
        const double spec = ch.specular > 0.0f
            ? std::pow(ndoth, std::max(1.0f, ch.specular_exponent)) * ch.specular
              * attenuation * visibility
            : 0.0;

        const Rgb light_col{ch.color[0] * ch.intensity,
                            ch.color[1] * ch.intensity,
                            ch.color[2] * ch.intensity};
        out += albedo * light_col * static_cast<float>(diffuse);
        out += light_col * static_cast<float>(spec);
    }

    // Distance fog with its own two-colour ramp.
    if (L.fog_density > 0.0f) {
        const double fog_t = std::clamp(
            (hit.distance - L.fog_start) / std::max(1e-6f, L.fog_end - L.fog_start), 0.0, 1.0);
        const double amount = 1.0 - std::exp(-L.fog_density * fog_t);
        const Rgb fog{
            static_cast<float>(L.fog_color_a[0] * (1 - fog_t) + L.fog_color_b[0] * fog_t),
            static_cast<float>(L.fog_color_a[1] * (1 - fog_t) + L.fog_color_b[1] * fog_t),
            static_cast<float>(L.fog_color_a[2] * (1 - fog_t) + L.fog_color_b[2] * fog_t)};
        out = out * static_cast<float>(1.0 - amount) + fog * static_cast<float>(amount);
    }

    return out;
}

Rgb CpuRaymarcher::background(const Ray& ray) const {
    const auto& L = settings_.lighting;
    if (L.background_mode == 0) {
        return {L.background_a[0], L.background_a[1], L.background_a[2]};
    }
    // Gradient background mapped along the view ray's vertical component.
    const double t = std::clamp(ray.direction.y * 0.5 + 0.5, 0.0, 1.0);
    return {static_cast<float>(L.background_a[0] * (1 - t) + L.background_b[0] * t),
            static_cast<float>(L.background_a[1] * (1 - t) + L.background_b[1] * t),
            static_cast<float>(L.background_a[2] * (1 - t) + L.background_b[2] * t)};
}

Rgb CpuRaymarcher::tonemap(const Rgb& linear) const {
    const auto& L = settings_.lighting;
    Rgb c = linear * std::pow(2.0f, L.exposure);

    // Saturation, then contrast around mid grey, then gamma. Order
    // matters -- this is the sequence the legacy post chain used.
    if (L.saturation != 1.0f) {
        const float luma = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        c = Rgb{luma, luma, luma} * (1.0f - L.saturation) + c * L.saturation;
    }
    if (L.contrast != 1.0f) {
        c = Rgb{(c.r - 0.5f) * L.contrast + 0.5f,
                (c.g - 0.5f) * L.contrast + 0.5f,
                (c.b - 0.5f) * L.contrast + 0.5f};
    }
    c = Rgb{c.r + L.brightness, c.g + L.brightness, c.b + L.brightness};

    const float inv_gamma = 1.0f / (L.gamma > 0.0f ? L.gamma : 2.2f);
    auto encode = [inv_gamma](float v) {
        return std::pow(std::clamp(v, 0.0f, 1.0f), inv_gamma);
    };
    return {encode(c.r), encode(c.g), encode(c.b)};
}

void CpuRaymarcher::render_tile(FrameBuffer& fb, const Tile& tile, int eye) const {
    const int ss = std::clamp(settings_.supersample, 1, 4);
    const double inv_samples = 1.0 / (ss * ss);
    const double eye_offset = settings_.camera.stereo_mode != OMF_STEREO_OFF
        ? (eye == 0 ? -0.5 : 0.5) * settings_.camera.stereo_ipd
        : 0.0;

    const Camera camera(settings_.camera, fb.width(), fb.height(), eye_offset);

    Layer* rgba   = fb.layer(OMF_LAYER_RGBA);
    Layer* rgba32 = fb.layer(OMF_LAYER_RGBA32F);
    Layer* depth  = fb.layer(OMF_LAYER_DEPTH);
    Layer* normal = fb.layer(OMF_LAYER_NORMAL);
    Layer* ssao   = fb.layer(OMF_LAYER_SSAO);
    Layer* shadow = fb.layer(OMF_LAYER_SHADOW);
    Layer* object = fb.layer(OMF_LAYER_OBJECT);

    for (int y = tile.y0; y < tile.y1; ++y) {
        for (int x = tile.x0; x < tile.x1; ++x) {
            Rgb accum{};
            double depth_accum = 0.0;
            Vec3 normal_accum{};
            double ao_accum = 0.0;
            double shadow_accum = 0.0;
            bool any_hit = false;

            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    // Jittered stratified sampling; the hash keeps it
                    // deterministic per pixel and sample index.
                    const std::uint32_t seed =
                        static_cast<std::uint32_t>(x * 73856093) ^
                        static_cast<std::uint32_t>(y * 19349663) ^
                        static_cast<std::uint32_t>((sy * ss + sx) * 83492791) ^
                        static_cast<std::uint32_t>(settings_.seed);
                    const double jx = ss > 1 ? hash01(seed) : 0.5;
                    const double jy = ss > 1 ? hash01(seed ^ 0x9E3779B9u) : 0.5;

                    const Ray ray = camera.primary_ray(x + (sx + jx) / ss,
                                                       y + (sy + jy) / ss);
                    const Hit hit = march(ray);
                    accum += shade(hit, ray);

                    if (hit.hit) {
                        any_hit = true;
                        depth_accum += hit.distance;
                        normal_accum += hit.normal;
                        if (ssao) {
                            ao_accum += ambient_occlusion(
                                hit.position, hit.normal,
                                settings_.lighting.ssao_radius > 0.0f
                                    ? settings_.lighting.ssao_radius : 0.05,
                                settings_.lighting.ssao_samples > 0
                                    ? settings_.lighting.ssao_samples : 5);
                        }
                        if (shadow) {
                            const auto& ch = settings_.lighting.channels[0];
                            const Vec3 to_light = ch.kind == OMF_LIGHT_DIRECTIONAL
                                ? normalize(-Vec3(ch.position))
                                : normalize(Vec3(ch.position) - hit.position);
                            shadow_accum += shadow_ray(
                                hit.position + hit.normal * 1e-4, to_light, 100.0, 0.0);
                        }
                    }
                }
            }

            const Rgb linear = accum * static_cast<float>(inv_samples);
            const Rgb display = tonemap(linear);

            if (rgba)   rgba->row<std::uint32_t>(y)[x] = pack_bgra(display, any_hit ? 1.0f : 0.0f);
            if (rgba32) {
                float* px = rgba32->row<float>(y) + x * 4;
                px[0] = linear.r; px[1] = linear.g; px[2] = linear.b;
                px[3] = any_hit ? 1.0f : 0.0f;
            }
            if (depth) {
                depth->row<float>(y)[x] = any_hit
                    ? static_cast<float>(depth_accum * inv_samples)
                    : std::numeric_limits<float>::infinity();
            }
            if (normal) {
                const Vec3 n = any_hit ? normalize(normal_accum) : Vec3{0, 0, 0};
                float* px = normal->row<float>(y) + x * 3;
                px[0] = static_cast<float>(n.x);
                px[1] = static_cast<float>(n.y);
                px[2] = static_cast<float>(n.z);
            }
            if (ssao)   ssao->row<float>(y)[x] = static_cast<float>(ao_accum * inv_samples);
            if (shadow) shadow->row<float>(y)[x] = static_cast<float>(shadow_accum * inv_samples);
            if (object) object->row<std::uint32_t>(y)[x] = any_hit ? 1u : 0u;
        }
    }
}

void CpuRaymarcher::render(FrameBuffer& fb, ProgressReporter& progress) const {
    // Tiling is a first-class part of the render graph, not an
    // optimisation bolted on later: at ~36 bytes/pixel across the layer
    // stack, a gigapixel frame is ~36 GB resident, so every render goes
    // through the tile path regardless of size.
    const int tile_w = settings_.tiling.tile_width > 0 ? settings_.tiling.tile_width : 128;
    const int tile_h = settings_.tiling.tile_height > 0 ? settings_.tiling.tile_height : 128;

    // Honour the crop rectangle: the region re-render tool and the
    // distributed dispatcher both drive rendering through it.
    int x0 = 0, y0 = 0, x1 = fb.width(), y1 = fb.height();
    if (settings_.crop_w > 0 && settings_.crop_h > 0) {
        x0 = std::clamp(settings_.crop_x, 0, fb.width());
        y0 = std::clamp(settings_.crop_y, 0, fb.height());
        x1 = std::clamp(settings_.crop_x + settings_.crop_w, x0, fb.width());
        y1 = std::clamp(settings_.crop_y + settings_.crop_h, y0, fb.height());
    }

    std::vector<Tile> tiles;
    for (int ty = y0; ty < y1; ty += tile_h) {
        for (int tx = x0; tx < x1; tx += tile_w) {
            tiles.push_back(Tile{tx, ty,
                                 std::min(tx + tile_w, x1),
                                 std::min(ty + tile_h, y1)});
        }
    }

    std::int64_t total_pixels = 0;
    for (const Tile& t : tiles) {
        total_pixels += static_cast<std::int64_t>(t.x1 - t.x0) * (t.y1 - t.y0);
    }
    progress.begin(total_pixels, static_cast<int>(tiles.size()));

    unsigned threads = settings_.thread_count > 0
        ? static_cast<unsigned>(settings_.thread_count)
        : std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    threads = std::min<unsigned>(threads, std::max<std::size_t>(1, tiles.size()));

    std::atomic<std::size_t> next{0};
    std::atomic<bool> aborted{false};

    {
        std::vector<std::jthread> pool;
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) {
            pool.emplace_back([&] {
                for (;;) {
                    const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= tiles.size()) return;
                    if (progress.cancelled()) { aborted.store(true); return; }

                    const Tile& tile = tiles[index];
                    render_tile(fb, tile, 0);
                    progress.advance(
                        static_cast<std::int64_t>(tile.x1 - tile.x0) * (tile.y1 - tile.y0), 1);
                }
            });
        }
    }  // jthreads join here

    progress.finish(aborted.load() ? OMF_JOB_CANCELLED : OMF_JOB_DONE);
}

}  // namespace omf
