// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// PrecisionProfiles.cpp -- named evaluation-semantics profiles and the
// legacy-to-modern parameter migration.
//
// THE CORE CLAIM THIS FILE ENCODES
//
// Bit-exact reproduction of the originating application is not
// achievable and is not attempted. That application evaluated in 80-bit
// x87, which rounds to a 64-bit significand at every operation. Vulkan
// offers fp32 and fp64 and nothing between. In a chaotic iterated map a
// last-significand-bit difference is amplified to a visible surface
// change within a few hundred iterations.
//
// Computing *wider* does not fix this. IEEE binary128 rounds at 113 bits
// -- a different dynamical system from x87's 80-bit, not a superset of
// it. Both converge toward a true mathematical attractor that neither
// reaches. There is no "exact" here; there is only more bits against an
// infinite process that is always truncated.
//
// What we can reproduce is *semantics*, and semantics is where almost
// all of the visible difference actually lives. Ranked:
//
//   1. DE tightness and method selection. One non-analytic slot in the
//      chain downgraded the whole chain to the numeric 4-point estimate,
//      which is the dominant source of overstepping and therefore of the
//      soft, blobby character of much legacy imagery.
//   2. Sampling: raystep multiplier, stepwidth limiter, first-step
//      random, raystep-sub-DEstop.
//   3. Iteration cap truncation -- detail ends at max-iterations long
//      before it ends at the rounding boundary.
//   4. Arithmetic precision. A distant fourth, until deep zoom, where it
//      abruptly becomes first.
//
// So legacy profiles lock 1-3 and run plain fp64 on the GPU at full
// speed. No 80-bit CPU path is built: MSVC aliases long double to
// double, so it would force a toolchain change for no visual return at
// the zoom depths where the canonical parameter sets actually live.

#include "mb3d/Internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace mb3d {
namespace {

void set_id(mb3d_precision_profile_t& profile, const char* id) {
    std::memset(profile.id, 0, sizeof(profile.id));
    std::strncpy(profile.id, id, sizeof(profile.id) - 1);
}

// The modern profile: tight analytic DE wherever a slot supports it,
// per-slot DE contribution instead of a whole-chain downgrade, and a
// probe epsilon sized for fp64 rather than for x87.
mb3d_precision_profile_t make_modern() {
    mb3d_precision_profile_t p{};
    set_id(p, MB3D_PROFILE_MODERN);
    p.tier = MB3D_PRECISION_FP64;
    p.de_method = MB3D_DE_AUTO;
    p.downgrade_chain_on_mixed_de = 0;
    p.probe_epsilon = 1.0e-6f;
    p.bailout_compare = MB3D_BAILOUT_R2_GT;
    p.slot_order_policy = 0;
    p.hybrid_arithmetic = 1;
    p.rng_stream = 1;
    p.lock_sampling = 0;
    p.lock_iteration_cap = 0;
    // Reference pixel width for DEstop, expressed as radians of FOV per
    // pixel at the profile's nominal 1920-wide, 45-degree framing.
    p.destop_pixel_reference =
        static_cast<float>((45.0 * std::numbers::pi / 180.0) / 1920.0);
    return p;
}

// 1.99.12. The widely-circulated parameter sets target this behaviour.
mb3d_precision_profile_t make_legacy_19912() {
    mb3d_precision_profile_t p{};
    set_id(p, MB3D_PROFILE_LEGACY_19912);
    p.tier = MB3D_PRECISION_FP64;
    // Never promote to analytic: matching the numeric estimate's
    // overstepping is the whole point of the profile.
    p.de_method = MB3D_DE_AUTO;
    p.downgrade_chain_on_mixed_de = 1;
    // Larger probe epsilon. The original 4-point probe was sized against
    // an 80-bit significand and a coarser screen; shrinking it here would
    // sharpen the surface and stop matching.
    p.probe_epsilon = 1.0e-4f;
    p.bailout_compare = MB3D_BAILOUT_R2_GT;
    p.slot_order_policy = 1;          // strict slot order, repeat-from-here
    p.hybrid_arithmetic = 0;
    p.rng_stream = 0;
    p.lock_sampling = 1;
    p.lock_iteration_cap = 1;
    p.destop_pixel_reference =
        static_cast<float>((45.0 * std::numbers::pi / 180.0) / 800.0);
    return p;
}

// 1.99.35. Rendering behaviour changed between releases, and the
// originating project's own documentation warns that older parameter and
// image files may not reproduce. A single "legacy" mode would guarantee
// a permanent stream of "doesn't match, I made this in 1.99.35" reports,
// so profiles are versioned per release.
mb3d_precision_profile_t make_legacy_19935() {
    mb3d_precision_profile_t p = make_legacy_19912();
    set_id(p, MB3D_PROFILE_LEGACY_19935);
    // Later releases tightened the estimate on all-analytic chains while
    // keeping the downgrade for mixed ones.
    p.probe_epsilon = 5.0e-5f;
    p.hybrid_arithmetic = 1;
    p.rng_stream = 1;
    p.destop_pixel_reference =
        static_cast<float>((45.0 * std::numbers::pi / 180.0) / 1280.0);
    return p;
}

const std::vector<mb3d_precision_profile_t>& registry() {
    static const std::vector<mb3d_precision_profile_t> profiles = {
        make_modern(), make_legacy_19912(), make_legacy_19935(),
    };
    return profiles;
}

// Radians of field of view subtended by one pixel. DEstop is defined
// relative to this, which is exactly why re-rendering at a different
// resolution without rescaling changes the image.
double pixel_angular_width(const mb3d_render_settings_t& settings, int width) {
    const double fov = settings.camera.fov_degrees > 0.0 ? settings.camera.fov_degrees : 45.0;
    const int w = width > 0 ? width : (settings.width > 0 ? settings.width : 1920);
    return (fov * std::numbers::pi / 180.0) / static_cast<double>(w);
}

// True when every enabled slot supplies an analytic derivative, i.e.
// when the chain is not forced onto the numeric estimate.
bool chain_is_analytic(const mb3d_render_settings_t& settings) {
    bool any_enabled = false;
    for (const mb3d_formula_slot_t& slot : settings.formulas) {
        if (!slot.enabled) continue;
        any_enabled = true;
        if (!slot.analytic_de) return false;
    }
    return any_enabled;
}

}  // namespace

const mb3d_precision_profile_t* find_profile(std::string_view id) {
    for (const mb3d_precision_profile_t& profile : registry()) {
        if (id == profile.id) return &profile;
    }
    return nullptr;
}

std::size_t profile_count() { return registry().size(); }

const mb3d_precision_profile_t& profile_at(std::size_t index) {
    return registry().at(index);
}

// ---------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------

void migrate_parameters(const mb3d_render_settings_t& legacy,
                        int target_width, int target_height,
                        const mb3d_precision_profile_t& target_profile,
                        mb3d_render_settings_t& out) {
    out = legacy;
    out.profile = target_profile;

    if (target_width > 0) out.width = target_width;
    if (target_height > 0) out.height = target_height;

    // Migrating INTO a locked profile is a no-op beyond the resolution
    // change. That is the point of the lock: someone who chose
    // legacy-1.99.12 wants the artefacts, not our improvements.
    if (target_profile.lock_sampling && target_profile.lock_iteration_cap) {
        return;
    }

    const bool source_analytic = chain_is_analytic(legacy);
    const bool target_analytic =
        source_analytic && !target_profile.downgrade_chain_on_mixed_de;

    // -- 1. raystep multiplier -------------------------------------
    //
    // An artist dropped this to 0.1 to fight overstepping under the
    // numeric estimate. Under a tight analytic DE those extra steps are
    // pure waste: same image, ten times the cost. So the multiplier is
    // raised in proportion to how much the estimate actually tightened.
    if (!target_profile.lock_sampling) {
        double tightening = 1.0;
        if (target_analytic && !legacy.profile.downgrade_chain_on_mixed_de) {
            tightening = 1.0;                 // already analytic; nothing gained
        } else if (target_analytic) {
            // Numeric-to-analytic. The numeric probe underestimates by
            // roughly the probe epsilon ratio, and that is the slack the
            // artist was compensating for by hand.
            const double eps_ratio =
                static_cast<double>(legacy.profile.probe_epsilon > 0.0f
                                        ? legacy.profile.probe_epsilon : 1e-4f) /
                std::max(1e-9, static_cast<double>(target_profile.probe_epsilon));
            tightening = std::clamp(std::sqrt(eps_ratio), 1.0, 8.0);
        }

        const double raised =
            static_cast<double>(legacy.calculation.raystep_multiplier) * tightening;
        // Never exceed 1.0: above that the marcher can step through a
        // surface even with a correct estimate, which is a different
        // failure from the one we are fixing.
        out.calculation.raystep_multiplier =
            static_cast<float>(std::clamp(raised, 0.05, 1.0));
    }

    // -- 2. DEstop rescaling ---------------------------------------
    //
    // DEstop is defined relative to pixel size -- a DEstop of 1 stops
    // roughly one pixel short of the set. Re-rendering a 1920x1080
    // parameter set at 16K without rescaling changes detail level,
    // overstepping behaviour and antialiasing character. That may well
    // be better output, but it is not fidelity, and the user did not ask
    // for it silently.
    if (!target_profile.lock_sampling) {
        const double source_pixel =
            legacy.profile.destop_pixel_reference > 0.0f
                ? static_cast<double>(legacy.profile.destop_pixel_reference)
                : pixel_angular_width(legacy, legacy.width);
        const double target_pixel = pixel_angular_width(out, out.width);

        if (source_pixel > 0.0 && target_pixel > 0.0) {
            const double scale = target_pixel / source_pixel;
            out.calculation.de_stop_criterion = static_cast<float>(
                std::clamp(static_cast<double>(legacy.calculation.de_stop_criterion) * scale,
                           1e-9, 1e-1));
        }
    }

    // -- 3. iteration cap ------------------------------------------
    //
    // The cap was set where detail stopped mattering at the artist's
    // working resolution. Carried forward unchanged onto a much larger
    // frame it starves structure that is finally resolvable, and the
    // user concludes the upgrade is broken. Detail scales with the log
    // of the pixel count, not linearly -- each doubling of linear
    // resolution resolves roughly one more iteration's worth of
    // structure.
    if (!target_profile.lock_iteration_cap && legacy.width > 0) {
        const double linear_ratio =
            static_cast<double>(out.width) / static_cast<double>(legacy.width);
        if (linear_ratio > 1.0) {
            const double extra = std::log2(linear_ratio) * 4.0;
            const int raised = legacy.calculation.max_iterations +
                               static_cast<int>(std::lround(extra));
            out.calculation.max_iterations = std::clamp(raised, 1, 100000);
        }
    }

    // The DE step budget has to grow with the iteration cap, or the
    // marcher runs out of steps before it reaches the newly available
    // detail and the whole migration shows up as missing geometry.
    if (!target_profile.lock_sampling &&
        out.calculation.max_iterations > legacy.calculation.max_iterations) {
        const double growth = static_cast<double>(out.calculation.max_iterations) /
                              std::max(1, legacy.calculation.max_iterations);
        out.calculation.de_max_steps = std::clamp(
            static_cast<int>(std::lround(legacy.calculation.de_max_steps * growth)),
            1, 100000);
    }
}

// Pure resolution change, with an explicit choice about what "the same
// image, bigger" means. Locked mode scales DEstop with resolution so the
// original look survives; unlocked mode lets detail increase. These are
// genuinely different intents and the host must label them plainly
// rather than picking one silently.
void rescale_for_resolution(mb3d_render_settings_t& settings,
                            int target_width, int target_height,
                            bool lock_to_legacy_look) {
    if (target_width <= 0 || target_height <= 0) return;
    if (settings.width <= 0) return;

    const double source_pixel = pixel_angular_width(settings, settings.width);

    const int previous_width = settings.width;
    settings.width = target_width;
    settings.height = target_height;

    if (!lock_to_legacy_look) return;

    const double target_pixel = pixel_angular_width(settings, target_width);
    if (source_pixel <= 0.0 || target_pixel <= 0.0) return;

    settings.calculation.de_stop_criterion = static_cast<float>(
        std::clamp(static_cast<double>(settings.calculation.de_stop_criterion) *
                       (target_pixel / source_pixel),
                   1e-9, 1e-1));
    (void)previous_width;
}

// ---------------------------------------------------------------------
// Keyframe evaluation
// ---------------------------------------------------------------------

namespace {

// Catmull-Rom through four control points. Used for camera position so
// a flight path stays smooth through keyframes rather than kinking at
// each one.
double catmull_rom(double p0, double p1, double p2, double p3, double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

}  // namespace

void evaluate_keyframes(const std::vector<mb3d_keyframe_t>& keyframes, int frame,
                        mb3d_render_settings_t& settings) {
    if (keyframes.empty()) return;

    if (keyframes.size() == 1) {
        settings.camera = keyframes.front().camera;
        return;
    }

    std::size_t index = 0;
    while (index + 1 < keyframes.size() && keyframes[index + 1].frame <= frame) ++index;

    const std::size_t last = keyframes.size() - 1;
    if (index >= last) {
        settings.camera = keyframes[last].camera;
        return;
    }

    const mb3d_keyframe_t& k1 = keyframes[index];
    const mb3d_keyframe_t& k2 = keyframes[index + 1];
    const mb3d_keyframe_t& k0 = keyframes[index > 0 ? index - 1 : index];
    const mb3d_keyframe_t& k3 = keyframes[index + 2 <= last ? index + 2 : last];

    const int span = std::max(1, k2.frame - k1.frame);
    const double t = std::clamp(static_cast<double>(frame - k1.frame) / span, 0.0, 1.0);

    const int mode = k1.interpolation;
    auto blend = [&](double a0, double a1, double a2, double a3) {
        switch (mode) {
            case 0:  return a1;                              // step
            case 1:  return a1 + (a2 - a1) * t;              // linear
            default: return catmull_rom(a0, a1, a2, a3, t);  // spline
        }
    };

    mb3d_camera_transform_t camera = k1.camera;
    for (int i = 0; i < 3; ++i) {
        camera.position[i] = blend(k0.camera.position[i], k1.camera.position[i],
                                   k2.camera.position[i], k3.camera.position[i]);
        camera.target[i]   = blend(k0.camera.target[i], k1.camera.target[i],
                                   k2.camera.target[i], k3.camera.target[i]);
        camera.rotation[i] = blend(k0.camera.rotation[i], k1.camera.rotation[i],
                                   k2.camera.rotation[i], k3.camera.rotation[i]);
    }
    camera.distance = blend(k0.camera.distance, k1.camera.distance,
                            k2.camera.distance, k3.camera.distance);

    // Zoom interpolates logarithmically. A linear ramp on a zoom factor
    // reads as an accelerating lurch rather than a steady push in, which
    // is the single most common complaint about naive fractal flights.
    const double log_zoom = blend(std::log(std::max(k0.camera.zoom, 1e-12)),
                                  std::log(std::max(k1.camera.zoom, 1e-12)),
                                  std::log(std::max(k2.camera.zoom, 1e-12)),
                                  std::log(std::max(k3.camera.zoom, 1e-12)));
    camera.zoom = std::exp(log_zoom);
    settings.camera = camera;

    for (int slot = 0; slot < MB3D_FORMULA_SLOTS; ++slot) {
        for (int p = 0; p < MB3D_FORMULA_PARAMS; ++p) {
            settings.formulas[slot].params[p] = static_cast<float>(
                blend(k0.formula_params[slot][p], k1.formula_params[slot][p],
                      k2.formula_params[slot][p], k3.formula_params[slot][p]));
        }
    }
}

}  // namespace mb3d
