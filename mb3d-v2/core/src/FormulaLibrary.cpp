// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// FormulaLibrary.cpp -- built-in formula set, the hybrid iteration loop
// and the distance estimator.
//
// Every formula is a one-iteration step function operating on
// IterationState. Keeping them uniform is what makes the 6-slot hybrid
// pipeline work: the loop picks a slot per iteration and calls it, and
// no formula needs to know it is part of a hybrid.
//
// Parameter slots follow the legacy convention so a .m3p written by
// v1.99 maps straight onto params[]:
//   params[0]  power / scale
//   params[1]  minimum radius (boxes) or secondary power
//   params[2]  fixed radius
//   params[3]  folding limit
//   params[4]  offset x     params[5] offset y     params[6] offset z
//   params[7]  z-multiplier / phase
//   params[8]  angle bias
//   params[9]  linear term
//   params[10] auxiliary
//   params[11] auxiliary

#include "mb3d/Internal.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mb3d {
namespace {

constexpr double kDeg2Rad = std::numbers::pi / 180.0;

inline double safe_pow(double base, double exp_) {
    return base <= 0.0 ? 0.0 : std::pow(base, exp_);
}

// -- 3D bulb, the classic z -> z^n + c in spherical coordinates -------
void f_bulb3d(IterationState& s, const mb3d_formula_slot_t& cfg) {
    const double power = cfg.params[0] != 0.0f ? cfg.params[0] : 8.0;
    const double r = length(s.z);
    s.r = r;
    if (r < 1e-12) return;

    // Running derivative for the analytic distance estimate:
    //   dr' = n * r^(n-1) * dr + 1
    s.dr = safe_pow(r, power - 1.0) * power * s.dr + 1.0;

    const double theta = std::acos(std::clamp(s.z.z / r, -1.0, 1.0)) * power
                       + cfg.params[8] * kDeg2Rad;
    const double phi   = std::atan2(s.z.y, s.z.x) * power
                       + cfg.params[7] * kDeg2Rad;
    const double zr    = safe_pow(r, power);

    const double st = std::sin(theta);
    s.z = Vec3{st * std::cos(phi), st * std::sin(phi), std::cos(theta)} * zr;
    s.z += Vec3{cfg.params[4], cfg.params[5], cfg.params[6]};
    s.z += s.c;
}

// -- 4D / quaternion bulb --------------------------------------------
void f_bulb4d(IterationState& s, const mb3d_formula_slot_t& cfg) {
    const double power = cfg.params[0] != 0.0f ? cfg.params[0] : 8.0;
    const double x = s.z.x, y = s.z.y, z = s.z.z, w = s.w;
    const double r = std::sqrt(x * x + y * y + z * z + w * w);
    s.r = r;
    if (r < 1e-12) return;

    s.dr = safe_pow(r, power - 1.0) * power * s.dr + 1.0;

    // Hyperspherical angles; the last angle folds w into the rotation.
    const double t1 = std::acos(std::clamp(z / std::sqrt(x * x + y * y + z * z + 1e-30), -1.0, 1.0));
    const double t2 = std::atan2(y, x);
    const double t3 = std::acos(std::clamp(w / r, -1.0, 1.0));

    const double rp = safe_pow(r, power);
    const double a1 = t1 * power, a2 = t2 * power, a3 = t3 * power;
    const double sa3 = std::sin(a3), sa1 = std::sin(a1);

    s.z = Vec3{rp * sa3 * sa1 * std::cos(a2),
               rp * sa3 * sa1 * std::sin(a2),
               rp * sa3 * std::cos(a1)};
    s.w = rp * std::cos(a3);

    s.z += s.c;
    s.w += s.cw;
}

// -- Mandelbox / Tglad fold ------------------------------------------
void f_mandelbox(IterationState& s, const mb3d_formula_slot_t& cfg) {
    const double scale  = cfg.params[0] != 0.0f ? cfg.params[0] : 2.0;
    const double min_r  = cfg.params[1] != 0.0f ? cfg.params[1] : 0.5;
    const double fixed_r = cfg.params[2] != 0.0f ? cfg.params[2] : 1.0;
    const double limit  = cfg.params[3] != 0.0f ? cfg.params[3] : 1.0;

    // Box fold: reflect anything outside [-limit, limit].
    auto box_fold = [limit](double v) {
        return v > limit ? 2.0 * limit - v : (v < -limit ? -2.0 * limit - v : v);
    };
    s.z = Vec3{box_fold(s.z.x), box_fold(s.z.y), box_fold(s.z.z)};

    // Sphere fold: invert inside min_r, linearly scale between radii.
    const double r2 = dot(s.z, s.z);
    const double min_r2 = min_r * min_r;
    const double fixed_r2 = fixed_r * fixed_r;
    if (r2 < min_r2) {
        const double f = fixed_r2 / min_r2;
        s.z *= f;
        s.dr *= f;
    } else if (r2 < fixed_r2) {
        const double f = fixed_r2 / r2;
        s.z *= f;
        s.dr *= f;
    }

    s.z = s.z * scale + s.c;
    s.dr = s.dr * std::fabs(scale) + 1.0;
    s.r = length(s.z);
}

// -- Kaleidoscopic / menger-style absolute fold -----------------------
void f_fold(IterationState& s, const mb3d_formula_slot_t& cfg) {
    const double scale = cfg.params[0] != 0.0f ? cfg.params[0] : 3.0;
    const Vec3 offset{cfg.params[4] ? cfg.params[4] : 1.0f, cfg.params[5], cfg.params[6]};
    const double angle1 = cfg.params[8] * kDeg2Rad;
    const double angle2 = cfg.params[9] * kDeg2Rad;

    s.z = abs(s.z);
    // Sort the components -- this is the fold that produces the Menger
    // sponge family.
    if (s.z.x < s.z.y) std::swap(s.z.x, s.z.y);
    if (s.z.x < s.z.z) std::swap(s.z.x, s.z.z);
    if (s.z.y < s.z.z) std::swap(s.z.y, s.z.z);

    if (angle1 != 0.0) {
        const double ca = std::cos(angle1), sa = std::sin(angle1);
        s.z = Vec3{ca * s.z.x - sa * s.z.y, sa * s.z.x + ca * s.z.y, s.z.z};
    }

    s.z.z -= 0.5 * offset.z * (scale - 1.0) / scale;
    s.z.z = -std::fabs(-s.z.z);
    s.z.z += 0.5 * offset.z * (scale - 1.0) / scale;

    if (angle2 != 0.0) {
        const double ca = std::cos(angle2), sa = std::sin(angle2);
        s.z = Vec3{ca * s.z.x - sa * s.z.z, s.z.y, sa * s.z.x + ca * s.z.z};
    }

    s.z = s.z * scale - Vec3{offset.x, offset.y, 0.0} * (scale - 1.0);
    s.dr = s.dr * std::fabs(scale);
    s.r = length(s.z);
}

// -- Transform / DIF: rotate, spherical inversion, tile ---------------
void f_transform(IterationState& s, const mb3d_formula_slot_t& cfg) {
    // iparams[0] selects the transform variant; the legacy DIFs are all
    // affine or conformal, so none of them touch dr except the
    // inversion, which scales it.
    switch (cfg.iparams[0]) {
        case 0:  // rotation
            s.z = rotate_euler(s.z, cfg.rotation[0], cfg.rotation[1], cfg.rotation[2]);
            break;
        case 1: {  // spherical inversion about params[4..6] with radius params[0]
            const Vec3 centre{cfg.params[4], cfg.params[5], cfg.params[6]};
            const double radius = cfg.params[0] != 0.0f ? cfg.params[0] : 1.0;
            Vec3 d = s.z - centre;
            const double r2 = std::max(dot(d, d), 1e-18);
            const double f = radius * radius / r2;
            s.z = centre + d * f;
            s.dr *= f;
            break;
        }
        case 2: {  // tile / repeat with period params[4..6]
            const Vec3 period{cfg.params[4] ? cfg.params[4] : 1.0f,
                              cfg.params[5] ? cfg.params[5] : 1.0f,
                              cfg.params[6] ? cfg.params[6] : 1.0f};
            auto rep = [](double v, double p) {
                return p <= 0.0 ? v : v - p * std::round(v / p);
            };
            s.z = Vec3{rep(s.z.x, period.x), rep(s.z.y, period.y), rep(s.z.z, period.z)};
            break;
        }
        case 3:  // absolute value on selected axes
            if (cfg.iparams[1] & 1) s.z.x = std::fabs(s.z.x);
            if (cfg.iparams[1] & 2) s.z.y = std::fabs(s.z.y);
            if (cfg.iparams[1] & 4) s.z.z = std::fabs(s.z.z);
            break;
        case 4:  // uniform scale + translate
            s.z = s.z * (cfg.params[0] != 0.0f ? cfg.params[0] : 1.0)
                + Vec3{cfg.params[4], cfg.params[5], cfg.params[6]};
            s.dr *= std::fabs(cfg.params[0] != 0.0f ? cfg.params[0] : 1.0);
            break;
        default:
            break;
    }
    s.r = length(s.z);
}

// -- Simple IFS-style contraction ------------------------------------
void f_ifs(IterationState& s, const mb3d_formula_slot_t& cfg) {
    const double scale = cfg.params[0] != 0.0f ? cfg.params[0] : 2.0;
    const Vec3 offset{cfg.params[4] ? cfg.params[4] : 1.0f,
                      cfg.params[5] ? cfg.params[5] : 1.0f,
                      cfg.params[6] ? cfg.params[6] : 1.0f};
    s.z = abs(s.z);
    if (s.z.x + s.z.y < 0.0) { const double t = -s.z.y; s.z.y = -s.z.x; s.z.x = t; }
    if (s.z.x + s.z.z < 0.0) { const double t = -s.z.z; s.z.z = -s.z.x; s.z.x = t; }
    if (s.z.y + s.z.z < 0.0) { const double t = -s.z.z; s.z.z = -s.z.y; s.z.y = t; }
    s.z = s.z * scale - offset * (scale - 1.0);
    s.dr *= std::fabs(scale);
    s.r = length(s.z);
}

// GPU counterparts. These strings are injected into raymarch.comp as the
// body of a `case` in the slot dispatch switch, so the CPU and GPU paths
// stay literally the same algorithm.
constexpr const char* kGlslBulb3d = R"(
    float power = (p0 != 0.0) ? p0 : 8.0;
    float r = length(z);
    if (r > 1e-6) {
        dr = pow(r, power - 1.0) * power * dr + 1.0;
        float theta = acos(clamp(z.z / r, -1.0, 1.0)) * power + radians(p8);
        float phi   = atan(z.y, z.x) * power + radians(p7);
        float zr    = pow(r, power);
        z = zr * vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
        z += vec3(p4, p5, p6) + c;
    }
)";

constexpr const char* kGlslMandelbox = R"(
    float scale = (p0 != 0.0) ? p0 : 2.0;
    float minR  = (p1 != 0.0) ? p1 : 0.5;
    float fixR  = (p2 != 0.0) ? p2 : 1.0;
    float lim   = (p3 != 0.0) ? p3 : 1.0;
    z = clamp(z, -lim, lim) * 2.0 - z;
    float r2 = dot(z, z);
    if (r2 < minR * minR)      { float f = (fixR*fixR)/(minR*minR); z *= f; dr *= f; }
    else if (r2 < fixR * fixR) { float f = (fixR*fixR)/r2;          z *= f; dr *= f; }
    z = z * scale + c;
    dr = dr * abs(scale) + 1.0;
)";

}  // namespace

// ---------------------------------------------------------------------
// Vector helpers
// ---------------------------------------------------------------------

double length(const Vec3& v) { return std::sqrt(dot(v, v)); }

Vec3 normalize(const Vec3& v) {
    const double l = length(v);
    return l > 1e-30 ? v / l : Vec3{0, 0, 1};
}

Vec3 abs(const Vec3& v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

Vec3 min(const Vec3& a, const Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3 max(const Vec3& a, const Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

Vec3 rotate_euler(const Vec3& v, double rx, double ry, double rz) {
    if (rx == 0.0 && ry == 0.0 && rz == 0.0) return v;
    const double a = rx * kDeg2Rad, b = ry * kDeg2Rad, g = rz * kDeg2Rad;
    const double ca = std::cos(a), sa = std::sin(a);
    const double cb = std::cos(b), sb = std::sin(b);
    const double cg = std::cos(g), sg = std::sin(g);

    Vec3 r = v;
    r = Vec3{r.x, ca * r.y - sa * r.z, sa * r.y + ca * r.z};   // X
    r = Vec3{cb * r.x + sb * r.z, r.y, -sb * r.x + cb * r.z};  // Y
    r = Vec3{cg * r.x - sg * r.y, sg * r.x + cg * r.y, r.z};   // Z
    return r;
}

// ---------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------

FormulaRegistry::FormulaRegistry() {
    add({"Bulb3D",      MB3D_FORMULA_BULB3D,    &f_bulb3d,    kGlslBulb3d,    {}, true});
    add({"Bulb4D",      MB3D_FORMULA_BULB4D,    &f_bulb4d,    {},             {}, true});
    add({"Mandelbox",   MB3D_FORMULA_BOX,       &f_mandelbox, kGlslMandelbox, {}, true});
    add({"TgladFold",   MB3D_FORMULA_BOX,       &f_mandelbox, kGlslMandelbox, {}, true});
    add({"KaleidoIFS",  MB3D_FORMULA_FOLD,      &f_fold,      {},             {}, true});
    add({"MengerFold",  MB3D_FORMULA_FOLD,      &f_fold,      {},             {}, true});
    add({"Transform",   MB3D_FORMULA_TRANSFORM, &f_transform, {},             {}, true});
    add({"IFS",         MB3D_FORMULA_IFS,       &f_ifs,       {},             {}, true});
}

void FormulaRegistry::add(FormulaDef def) {
    const std::string key = def.name;
    if (!defs_.contains(key)) order_.push_back(key);
    defs_[key] = std::move(def);
}

const FormulaDef* FormulaRegistry::find(std::string_view name) const {
    const auto it = defs_.find(std::string(name));
    return it == defs_.end() ? nullptr : &it->second;
}

// .m3f / .d3f are plain text: a small `key = value` header followed by a
// body between braces. We keep the body verbatim for the GPU path and
// fall back to the nearest built-in for the CPU path until the JIT
// backend is wired up (see docs/FORMULA_JIT.md).
FormulaDef FormulaRegistry::parse_formula_source(std::string_view name,
                                                 std::string_view source,
                                                 mb3d_formula_kind_t kind) {
    FormulaDef def;
    def.name = std::string(name);
    def.kind = kind == MB3D_FORMULA_NONE ? MB3D_FORMULA_CUSTOM : kind;
    def.source = std::string(source);

    const auto open = source.find('{');
    const auto close = source.rfind('}');
    require(open != std::string_view::npos && close != std::string_view::npos && close > open,
            MB3D_ERR_PARSE, "formula source has no { ... } body");
    def.glsl = std::string(source.substr(open + 1, close - open - 1));

    // Header flags we understand today.
    def.analytic_de = source.find("analytic_de = 0") == std::string_view::npos;

    // CPU execution target. Custom formulas run on the GPU; on the CPU
    // fallback they resolve to the closest built-in family so a scene
    // still renders rather than failing outright.
    switch (def.kind) {
        case MB3D_FORMULA_BOX:       def.fn = &f_mandelbox; break;
        case MB3D_FORMULA_FOLD:      def.fn = &f_fold;      break;
        case MB3D_FORMULA_TRANSFORM: def.fn = &f_transform; break;
        case MB3D_FORMULA_IFS:       def.fn = &f_ifs;       break;
        case MB3D_FORMULA_BULB4D:    def.fn = &f_bulb4d;    break;
        default:                     def.fn = &f_bulb3d;    break;
    }
    return def;
}

// ---------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------

FormulaPipeline FormulaPipeline::build(const FormulaRegistry& reg,
                                       const mb3d_render_settings_t& settings) {
    FormulaPipeline pipe;
    pipe.master = static_cast<mb3d_hybrid_mode_t>(settings.hybrid_master_mode);

    for (int i = 0; i < MB3D_FORMULA_SLOTS; ++i) {
        const mb3d_formula_slot_t& cfg = settings.formulas[i];
        if (!cfg.enabled) continue;

        std::string name(cfg.name, ::strnlen(cfg.name, MB3D_MAX_NAME));
        const FormulaDef* def = name.empty() ? nullptr : reg.find(name);
        if (def == nullptr) {
            // Fall back on the declared kind so an unknown name degrades
            // to something renderable instead of a hard failure.
            switch (cfg.kind) {
                case MB3D_FORMULA_BOX:       def = reg.find("Mandelbox"); break;
                case MB3D_FORMULA_FOLD:      def = reg.find("KaleidoIFS"); break;
                case MB3D_FORMULA_TRANSFORM: def = reg.find("Transform"); break;
                case MB3D_FORMULA_IFS:       def = reg.find("IFS"); break;
                case MB3D_FORMULA_BULB4D:    def = reg.find("Bulb4D"); break;
                default:                     def = reg.find("Bulb3D"); break;
            }
        }
        Slot slot;
        slot.def = def;
        slot.cfg = cfg;
        if (slot.cfg.iteration_weight <= 0) slot.cfg.iteration_weight = 1;
        pipe.slots.push_back(slot);
    }

    if (pipe.slots.empty()) {
        Slot slot;
        slot.def = reg.find("Bulb3D");
        mb3d_formula_slot_t cfg{};
        std::snprintf(cfg.name, sizeof(cfg.name), "Bulb3D");
        cfg.kind = MB3D_FORMULA_BULB3D;
        cfg.enabled = 1;
        cfg.iteration_weight = 1;
        cfg.stop_iteration = -1;
        cfg.params[0] = 8.0f;
        slot.cfg = cfg;
        pipe.slots.push_back(slot);
    }
    return pipe;
}

// ---------------------------------------------------------------------
// Orbit traps
// ---------------------------------------------------------------------

namespace {

double orbit_trap_distance(const mb3d_coloring_settings_t& c, const Vec3& z) {
    const Vec3 centre{c.trap_center[0], c.trap_center[1], c.trap_center[2]};
    const Vec3 n = normalize(Vec3{c.trap_normal[0], c.trap_normal[1], c.trap_normal[2]});
    const Vec3 d = z - centre;

    switch (c.trap_kind) {
        case MB3D_TRAP_POINT:
            return length(d);
        case MB3D_TRAP_LINE: {
            const double t = dot(d, n);
            return length(d - n * t);
        }
        case MB3D_TRAP_CROSS: {
            const Vec3 a = abs(d);
            return std::min({std::hypot(a.y, a.z), std::hypot(a.x, a.z), std::hypot(a.x, a.y)});
        }
        case MB3D_TRAP_BOX: {
            const Vec3 a = abs(d) - Vec3{c.trap_size, c.trap_size, c.trap_size};
            return length(max(a, Vec3{0, 0, 0})) + std::min(std::max({a.x, a.y, a.z}), 0.0);
        }
        case MB3D_TRAP_PLANE:
            return std::fabs(dot(d, n));
        case MB3D_TRAP_SPHERE:
            return std::fabs(length(d) - c.trap_size);
        default:
            return 1e30;
    }
}

// Which slot runs at iteration `i`, per the hybrid mode.
inline const FormulaPipeline::Slot* pick_slot(const FormulaPipeline& pipe, int i) {
    const std::size_t n = pipe.slots.size();
    if (n == 1) return &pipe.slots[0];

    switch (pipe.master) {
        case MB3D_HYBRID_ALTERNATE:
            return &pipe.slots[static_cast<std::size_t>(i) % n];

        case MB3D_HYBRID_DECOMB:
            // De-comb: even iterations run slot 0, odd iterations cycle
            // through the rest. This is what removes the striping
            // artefact the legacy UI calls "combing".
            if ((i & 1) == 0) return &pipe.slots[0];
            return &pipe.slots[1 + (static_cast<std::size_t>(i / 2) % (n - 1))];

        case MB3D_HYBRID_SEQUENTIAL:
        case MB3D_HYBRID_INTERPOLATE:
        default: {
            // Weighted round robin: each slot runs iteration_weight
            // iterations before handing over.
            int total = 0;
            for (const auto& s : pipe.slots) total += s.cfg.iteration_weight;
            if (total <= 0) return &pipe.slots[0];
            int pos = i % total;
            for (const auto& s : pipe.slots) {
                if (pos < s.cfg.iteration_weight) return &s;
                pos -= s.cfg.iteration_weight;
            }
            return &pipe.slots.back();
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------
// Distance estimator
// ---------------------------------------------------------------------

double distance_estimate(const FormulaPipeline& pipe,
                         const mb3d_calculation_settings_t& calc,
                         const mb3d_coloring_settings_t& color,
                         const Vec3& point,
                         IterationState* out_state) {
    IterationState s;
    s.z = point;
    s.c = point;
    s.w = 0.0;
    s.cw = 0.0;
    s.dr = 1.0;
    s.r = length(point);
    s.trap = 1e30;

    const double escape = calc.escape_radius > 0.0f ? calc.escape_radius : 4.0f;
    const double escape2 = escape * escape;
    const int max_iter = calc.max_iterations > 0 ? calc.max_iterations : 32;

    for (int i = 0; i < max_iter; ++i) {
        const FormulaPipeline::Slot* slot = pick_slot(pipe, i);
        if (slot == nullptr || slot->def == nullptr || slot->def->fn == nullptr) break;

        const mb3d_formula_slot_t& cfg = slot->cfg;
        if (i < cfg.start_iteration) continue;
        if (cfg.stop_iteration >= 0 && i > cfg.stop_iteration) continue;

        s.iteration = i;

        // Julia mode replaces the additive constant with a fixed seed.
        if (cfg.julia_enabled) {
            s.c = Vec3{cfg.julia_c[0], cfg.julia_c[1], cfg.julia_c[2]};
            s.cw = cfg.julia_c[3];
        }

        if (cfg.rotation[0] != 0.0f || cfg.rotation[1] != 0.0f || cfg.rotation[2] != 0.0f) {
            if (cfg.kind != MB3D_FORMULA_TRANSFORM) {
                s.z = rotate_euler(s.z, cfg.rotation[0], cfg.rotation[1], cfg.rotation[2]);
            }
        }

        if (pipe.master == MB3D_HYBRID_INTERPOLATE && pipe.slots.size() > 1) {
            // Run this slot and the next, then blend the two results.
            IterationState alt = s;
            slot->def->fn(s, cfg);
            const FormulaPipeline::Slot& other =
                pipe.slots[(static_cast<std::size_t>(i) + 1) % pipe.slots.size()];
            if (other.def && other.def->fn) {
                other.def->fn(alt, other.cfg);
                const double t = std::clamp<double>(cfg.interpolate_factor, 0.0, 1.0);
                s.z = s.z * (1.0 - t) + alt.z * t;
                s.dr = s.dr * (1.0 - t) + alt.dr * t;
                s.w = s.w * (1.0 - t) + alt.w * t;
            }
        } else {
            slot->def->fn(s, cfg);
        }

        // Orbit trap accumulation, skipping the early transient.
        if (color.trap_kind != MB3D_TRAP_NONE && i >= color.trap_min_iteration) {
            const double d = orbit_trap_distance(color, s.z);
            s.trap = std::min(s.trap, d);
            s.trap_sum += d;
        }

        const double r2 = dot(s.z, s.z) + s.w * s.w;
        s.r = std::sqrt(r2);
        if (r2 > escape2) {
            s.escaped_at = i;
            break;
        }
    }

    if (out_state) *out_state = s;

    // Analytic (Hubbard-Douady) estimate. `de_scale` is the legacy
    // fudge factor; clamping dr keeps folds from producing a zero DE and
    // stalling the marcher.
    const double dr = std::max(std::fabs(s.dr), 1e-12);
    double de = 0.5 * std::log(std::max(s.r, 1e-12)) * s.r / dr;
    if (calc.de_scale > 0.0f) de *= calc.de_scale;
    if (!std::isfinite(de)) de = 0.0;
    return de;
}

// ---------------------------------------------------------------------
// Gradient sampling
// ---------------------------------------------------------------------

Rgb sample_gradient(const mb3d_coloring_settings_t& c, double t) {
    const double speed = c.color_speed != 0.0f ? c.color_speed : 1.0;
    double u = t * speed + c.color_offset;

    if (c.color_cycle > 0.0f && c.color_cycle != 1.0f) {
        // Cycling curve: reshapes the ramp traversal without changing
        // the endpoints, matching the legacy "cycling" spinner.
        const double frac = u - std::floor(u);
        u = std::floor(u) + std::pow(frac, static_cast<double>(c.color_cycle));
    }

    u -= std::floor(u);                    // wrap to [0,1)
    const double scaled = u * MB3D_GRADIENT_ENTRIES;
    const int i0 = static_cast<int>(scaled) % MB3D_GRADIENT_ENTRIES;
    const int i1 = (i0 + 1) % MB3D_GRADIENT_ENTRIES;
    const float f = static_cast<float>(scaled - std::floor(scaled));

    auto unpack = [](std::uint32_t v) {
        return Rgb{static_cast<float>((v >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(v & 0xFF) / 255.0f};
    };
    const Rgb a = unpack(c.gradient[i0]);
    const Rgb b = unpack(c.gradient[i1]);
    return a * (1.0f - f) + b * f;
}

std::uint32_t pack_bgra(const Rgb& c, float alpha) {
    auto to8 = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (to8(alpha) << 24) | (to8(c.r) << 16) | (to8(c.g) << 8) | to8(c.b);
}

}  // namespace mb3d
