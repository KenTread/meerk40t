// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.

#pragma once

#include "mb3d/CpuRaymarcher.hpp"
#include "mb3d/VulkanContext.hpp"

namespace mb3d {

// Iso-surface extraction over the distance-estimator field.
//
// The decomposition is marching *tetrahedra* rather than the classic
// 256-case cube table. Two reasons, both of which matter for the 3D
// printing path:
//   1. Tetrahedra have no ambiguous cases, so the output is manifold by
//      construction -- no case-table variant selection, no cracks.
//   2. A shared edge always produces the identical interpolated vertex
//      from both sides, so welding closes the mesh exactly and the
//      result is watertight.
// The cost is roughly 2x the triangle count of an optimal MC mesh, which
// the decimation-free workflow here can absorb.
class IsoSurfaceExtractor {
public:
    IsoSurfaceExtractor(VulkanContext& vk, const FormulaRegistry& registry,
                        const mb3d_render_settings_t& settings,
                        const mb3d_mesh_settings_t& mesh_settings);

    // Runs the GPU path when a device is available, otherwise the
    // threaded CPU path. Both produce identical topology.
    Mesh extract(ProgressReporter& progress);

private:
    // Evaluates the DE across one z-slab of the sampling grid.
    void sample_slab(std::vector<float>& field, int z0, int z_count) const;
    Mesh extract_cpu(ProgressReporter& progress);
    Mesh extract_gpu(ProgressReporter& progress);

    // Applies the flat base clip used for printable models: everything
    // below base_clip_height is removed and the opening is capped.
    void apply_base_clip(Mesh& mesh) const;

    Vec3 grid_position(int x, int y, int z) const;

    VulkanContext&             vk_;
    const FormulaRegistry&     registry_;
    mb3d_render_settings_t     settings_;
    mb3d_mesh_settings_t       mesh_settings_;
    CpuRaymarcher              marcher_;
    int                        nx_{}, ny_{}, nz_{};
    Vec3                       origin_{};
    Vec3                       cell_{};
};

// Binary STL writer. Emits the 80-byte header, triangle count and
// 50-byte facet records; normals are recomputed per facet so the file is
// valid even when the mesh carries no vertex normals.
void write_stl_binary(const Mesh& mesh, const std::string& path, float scale_to_mm);
void write_stl_ascii(const Mesh& mesh, const std::string& path, float scale_to_mm);
void write_obj(const Mesh& mesh, const std::string& path, float scale_to_mm);

// ISO 10303-21 (STEP AP214) via OpenCASCADE. Throws MB3D_ERR_UNSUPPORTED
// when built without OCCT.
void write_step(const Mesh& mesh, const std::string& path, float scale_to_mm);

}  // namespace mb3d
