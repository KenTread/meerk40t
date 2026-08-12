// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.

#pragma once

#include "omf/Internal.hpp"

namespace omf {

// Multi-threaded CPU renderer. Also the reference implementation the
// Vulkan compute path is validated against (see core/tests).
class CpuRaymarcher {
public:
    struct Hit {
        bool           hit{false};
        double         distance{0.0};
        Vec3           position{};
        Vec3           normal{};
        int            steps{0};
        IterationState state{};
    };

    struct Tile {
        int x0, y0, x1, y1;
    };

    CpuRaymarcher(const FormulaRegistry& registry, const omf_render_settings_t& settings);

    void render(FrameBuffer& fb, ProgressReporter& progress) const;
    void render_tile(FrameBuffer& fb, const Tile& tile, int eye) const;

    // Exposed for the Navigator depth picker, Bulb Tracer and the
    // marching-cubes extractor.
    double de(const Vec3& p) const;
    double de_state(const Vec3& p, IterationState& out) const;
    Hit    march(const Ray& ray) const;
    Vec3   normal_at(const Vec3& p, double hit_distance) const;

    const omf_render_settings_t& settings() const { return settings_; }

private:
    bool   clip_ray(const Ray& ray, double& t_near, double& t_far) const;
    double shadow_ray(const Vec3& origin, const Vec3& direction,
                      double max_distance, double softness) const;
    double ambient_occlusion(const Vec3& p, const Vec3& n, double radius, int samples) const;
    Rgb    shade(const Hit& hit, const Ray& ray) const;
    Rgb    background(const Ray& ray) const;
    Rgb    tonemap(const Rgb& linear) const;

    omf_render_settings_t settings_;
    FormulaPipeline        pipeline_;
};

}  // namespace omf
