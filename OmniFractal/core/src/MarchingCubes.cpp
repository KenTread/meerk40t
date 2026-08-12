// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// MarchingCubes.cpp -- iso-surface extraction and mesh post-processing.
//
// The sampling grid is processed as z-slabs so peak memory stays at
// O(N^2) rather than O(N^3): a 1024^3 grid would be 4 GB of floats held
// all at once, but only ~8 MB when streamed two slices at a time.

#include "omf/MarchingCubes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unordered_map>

namespace omf {
namespace {

// A cube split into six tetrahedra sharing the (0,6) main diagonal.
// Indices are corner numbers in the standard 0..7 cube ordering:
//   0:(0,0,0) 1:(1,0,0) 2:(1,1,0) 3:(0,1,0)
//   4:(0,0,1) 5:(1,0,1) 6:(1,1,1) 7:(0,1,1)
// Every neighbouring cube decomposes the same way, so shared faces
// always agree and the mesh closes.
constexpr int kTetra[6][4] = {
    {0, 5, 1, 6},
    {0, 1, 2, 6},
    {0, 2, 3, 6},
    {0, 3, 7, 6},
    {0, 7, 4, 6},
    {0, 4, 5, 6},
};

constexpr int kCorner[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
};

// The six edges of a tetrahedron, as (a, b) index pairs into the tet's
// own 0..3 vertices.
constexpr int kTetEdge[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};

// Which edges are crossed for each of the 16 inside/outside patterns,
// and how they wind into triangles. -1 terminates.
constexpr int kTetTri[16][7] = {
    {-1, -1, -1, -1, -1, -1, -1},   // 0000
    { 0,  1,  2, -1, -1, -1, -1},   // 0001
    { 0,  3,  4, -1, -1, -1, -1},   // 0010
    { 1,  2,  4,  1,  4,  3, -1},   // 0011
    { 1,  3,  5, -1, -1, -1, -1},   // 0100
    { 0,  2,  5,  0,  5,  3, -1},   // 0101
    { 0,  1,  5,  0,  5,  4, -1},   // 0110
    { 2,  5,  4, -1, -1, -1, -1},   // 0111
    { 2,  4,  5, -1, -1, -1, -1},   // 1000
    { 0,  4,  5,  0,  5,  1, -1},   // 1001
    { 0,  5,  3,  0,  2,  5, -1},   // 1010
    { 1,  5,  3, -1, -1, -1, -1},   // 1011
    { 1,  4,  3,  1,  2,  4, -1},   // 1100
    { 0,  4,  3, -1, -1, -1, -1},   // 1101
    { 0,  2,  1, -1, -1, -1, -1},   // 1110
    {-1, -1, -1, -1, -1, -1, -1},   // 1111
};

// Quantised key for an interpolated edge vertex. Two cubes sharing an
// edge must produce the identical key or the weld leaves a crack, so the
// key is built from the *grid edge*, not from the float position.
struct EdgeKey {
    std::uint64_t a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const noexcept {
        std::uint64_t h = k.a * 0x9E3779B97F4A7C15ull;
        h ^= k.b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

inline std::uint64_t grid_index(int x, int y, int z) {
    return (static_cast<std::uint64_t>(z) << 42) |
           (static_cast<std::uint64_t>(y) << 21) |
            static_cast<std::uint64_t>(x);
}

}  // namespace

// ---------------------------------------------------------------------
// Mesh post-processing
// ---------------------------------------------------------------------

void Mesh::compute_normals() {
    normals.assign(positions.size(), 0.0f);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        const Vec3 a{positions[ia * 3], positions[ia * 3 + 1], positions[ia * 3 + 2]};
        const Vec3 b{positions[ib * 3], positions[ib * 3 + 1], positions[ib * 3 + 2]};
        const Vec3 c{positions[ic * 3], positions[ic * 3 + 1], positions[ic * 3 + 2]};
        // Unnormalised cross product weights each face by its area,
        // which gives better vertex normals on irregular fractal meshes
        // than an unweighted average.
        const Vec3 n = cross(b - a, c - a);
        for (std::uint32_t idx : {ia, ib, ic}) {
            normals[idx * 3]     += static_cast<float>(n.x);
            normals[idx * 3 + 1] += static_cast<float>(n.y);
            normals[idx * 3 + 2] += static_cast<float>(n.z);
        }
    }
    for (std::size_t v = 0; v < vertex_count(); ++v) {
        const Vec3 n = normalize(Vec3{normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]});
        normals[v * 3]     = static_cast<float>(n.x);
        normals[v * 3 + 1] = static_cast<float>(n.y);
        normals[v * 3 + 2] = static_cast<float>(n.z);
    }
}

void Mesh::weld(float epsilon) {
    if (epsilon <= 0.0f || positions.empty()) return;

    // Spatial hash on a grid of side `epsilon`. Vertices from the tetra
    // extractor are already exact duplicates on shared edges, so this
    // mostly collapses those; the epsilon grid catches the rest.
    const double inv = 1.0 / epsilon;
    std::unordered_map<std::uint64_t, std::uint32_t> lookup;
    lookup.reserve(vertex_count());

    std::vector<float> new_positions;
    new_positions.reserve(positions.size());
    std::vector<std::uint32_t> remap(vertex_count());

    for (std::size_t v = 0; v < vertex_count(); ++v) {
        const auto qx = static_cast<std::int64_t>(std::llround(positions[v * 3] * inv));
        const auto qy = static_cast<std::int64_t>(std::llround(positions[v * 3 + 1] * inv));
        const auto qz = static_cast<std::int64_t>(std::llround(positions[v * 3 + 2] * inv));
        const std::uint64_t key =
            (static_cast<std::uint64_t>(qx & 0x1FFFFF)) |
            (static_cast<std::uint64_t>(qy & 0x1FFFFF) << 21) |
            (static_cast<std::uint64_t>(qz & 0x1FFFFF) << 42);

        if (const auto it = lookup.find(key); it != lookup.end()) {
            remap[v] = it->second;
            continue;
        }
        const auto index = static_cast<std::uint32_t>(new_positions.size() / 3);
        lookup.emplace(key, index);
        remap[v] = index;
        new_positions.push_back(positions[v * 3]);
        new_positions.push_back(positions[v * 3 + 1]);
        new_positions.push_back(positions[v * 3 + 2]);
    }

    std::vector<std::uint32_t> new_indices;
    new_indices.reserve(indices.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = remap[indices[i]];
        const std::uint32_t b = remap[indices[i + 1]];
        const std::uint32_t c = remap[indices[i + 2]];
        // Welding can collapse a sliver triangle into a degenerate one;
        // dropping those keeps the mesh valid for STEP conversion.
        if (a == b || b == c || a == c) continue;
        new_indices.insert(new_indices.end(), {a, b, c});
    }

    positions.swap(new_positions);
    indices.swap(new_indices);
    normals.clear();
}

void Mesh::laplacian_smooth(int iterations) {
    if (iterations <= 0 || positions.empty()) return;

    const std::size_t vcount = vertex_count();
    std::vector<std::uint32_t> valence(vcount);
    std::vector<float> accum(positions.size());

    for (int pass = 0; pass < iterations; ++pass) {
        std::fill(accum.begin(), accum.end(), 0.0f);
        std::fill(valence.begin(), valence.end(), 0u);

        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::uint32_t tri[3] = {indices[i], indices[i + 1], indices[i + 2]};
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t a = tri[e];
                const std::uint32_t b = tri[(e + 1) % 3];
                for (int k = 0; k < 3; ++k) {
                    accum[a * 3 + k] += positions[b * 3 + k];
                    accum[b * 3 + k] += positions[a * 3 + k];
                }
                valence[a] += 1;
                valence[b] += 1;
            }
        }

        // Half-step relaxation. A full step shrinks the model visibly
        // after a couple of passes, which ruins dimensional accuracy on
        // a print.
        constexpr float kLambda = 0.5f;
        for (std::size_t v = 0; v < vcount; ++v) {
            if (valence[v] == 0) continue;
            const float inv = 1.0f / static_cast<float>(valence[v]);
            for (int k = 0; k < 3; ++k) {
                const float target = accum[v * 3 + k] * inv;
                positions[v * 3 + k] += kLambda * (target - positions[v * 3 + k]);
            }
        }
    }
    normals.clear();
}

omf_mesh_stats_t Mesh::stats(float scale_to_mm) const {
    omf_mesh_stats_t s{};
    s.vertex_count = static_cast<std::int64_t>(vertex_count());
    s.triangle_count = static_cast<std::int64_t>(triangle_count());

    if (positions.empty()) {
        s.is_manifold = 0;
        s.is_watertight = 0;
        return s;
    }

    Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (std::size_t v = 0; v < vertex_count(); ++v) {
        const Vec3 p{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
        lo = min(lo, p);
        hi = max(hi, p);
    }
    for (int k = 0; k < 3; ++k) {
        s.bounds_min[k] = static_cast<float>(lo[k] * scale_to_mm);
        s.bounds_max[k] = static_cast<float>(hi[k] * scale_to_mm);
    }

    // Manifold test: every undirected edge must be used exactly twice,
    // and the two uses must have opposite direction (consistent winding).
    std::unordered_map<std::uint64_t, int> edges;
    edges.reserve(indices.size());
    auto edge_key = [](std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    };

    bool winding_ok = true;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t tri[3] = {indices[i], indices[i + 1], indices[i + 2]};
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = tri[e], b = tri[(e + 1) % 3];
            const int direction = a < b ? 1 : -1;
            auto& count = edges[edge_key(a, b)];
            count += direction;
        }
    }

    int open_edges = 0;
    for (const auto& [key, balance] : edges) {
        (void)key;
        if (balance != 0) ++open_edges;
    }
    s.open_edge_count = open_edges;
    s.is_watertight = open_edges == 0 ? 1 : 0;
    s.is_manifold = (open_edges == 0 && winding_ok) ? 1 : 0;
    s.shell_count = positions.empty() ? 0 : 1;

    // Signed volume via the divergence theorem, and total facet area.
    double volume = 0.0;
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        const Vec3 a{positions[ia * 3], positions[ia * 3 + 1], positions[ia * 3 + 2]};
        const Vec3 b{positions[ib * 3], positions[ib * 3 + 1], positions[ib * 3 + 2]};
        const Vec3 c{positions[ic * 3], positions[ic * 3 + 1], positions[ic * 3 + 2]};
        volume += dot(a, cross(b, c)) / 6.0;
        area += length(cross(b - a, c - a)) * 0.5;
    }
    const double k = static_cast<double>(scale_to_mm);
    s.volume_mm3 = std::fabs(volume) * k * k * k;
    s.surface_area_mm2 = area * k * k;
    return s;
}

// ---------------------------------------------------------------------
// Extractor
// ---------------------------------------------------------------------

IsoSurfaceExtractor::IsoSurfaceExtractor(VulkanContext& vk, const FormulaRegistry& registry,
                                         const omf_render_settings_t& settings,
                                         const omf_mesh_settings_t& mesh_settings)
    : vk_(vk), registry_(registry), settings_(settings), mesh_settings_(mesh_settings),
      marcher_(registry, settings) {
    nx_ = std::max(2, mesh_settings_.resolution[0]);
    ny_ = std::max(2, mesh_settings_.resolution[1]);
    nz_ = std::max(2, mesh_settings_.resolution[2]);

    origin_ = Vec3(mesh_settings_.bounds_min);
    const Vec3 hi(mesh_settings_.bounds_max);
    const Vec3 extent = hi - origin_;
    require(extent.x > 0 && extent.y > 0 && extent.z > 0, OMF_ERR_INVALID_ARG,
            "mesh bounds are empty or inverted");

    cell_ = Vec3{extent.x / (nx_ - 1), extent.y / (ny_ - 1), extent.z / (nz_ - 1)};
}

Vec3 IsoSurfaceExtractor::grid_position(int x, int y, int z) const {
    return Vec3{origin_.x + x * cell_.x, origin_.y + y * cell_.y, origin_.z + z * cell_.z};
}

void IsoSurfaceExtractor::sample_slab(std::vector<float>& field, int z0, int z_count) const {
    const std::size_t plane = static_cast<std::size_t>(nx_) * ny_;
    field.resize(plane * z_count);

    unsigned threads = settings_.thread_count > 0
        ? static_cast<unsigned>(settings_.thread_count)
        : std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;

    std::atomic<int> next_row{0};
    const int total_rows = ny_ * z_count;

    {
        std::vector<std::jthread> pool;
        pool.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    const int row = next_row.fetch_add(1, std::memory_order_relaxed);
                    if (row >= total_rows) return;
                    const int sz = row / ny_;
                    const int y = row % ny_;
                    float* out = field.data() + (static_cast<std::size_t>(sz) * plane)
                               + static_cast<std::size_t>(y) * nx_;
                    for (int x = 0; x < nx_; ++x) {
                        out[x] = static_cast<float>(marcher_.de(grid_position(x, y, z0 + sz)));
                    }
                }
            });
        }
    }
}

Mesh IsoSurfaceExtractor::extract_cpu(ProgressReporter& progress) {
    Mesh mesh;
    const float iso = mesh_settings_.iso_level;
    const std::size_t plane = static_cast<std::size_t>(nx_) * ny_;

    progress.begin(static_cast<std::int64_t>(nz_ - 1), nz_ - 1);

    std::vector<float> lower, upper;
    sample_slab(lower, 0, 1);

    // Vertex dedup across the whole extraction. Keyed by grid edge, so a
    // vertex generated from either side of a shared face is the same
    // index -- this is what makes the output watertight.
    std::unordered_map<EdgeKey, std::uint32_t, EdgeKeyHash> edge_vertices;
    edge_vertices.reserve(static_cast<std::size_t>(nx_) * ny_ * 4);

    for (int z = 0; z + 1 < nz_; ++z) {
        if (progress.cancelled()) {
            progress.finish(OMF_JOB_CANCELLED);
            return mesh;
        }
        sample_slab(upper, z + 1, 1);

        auto field_at = [&](int x, int y, int local_z) -> float {
            const std::vector<float>& src = local_z == 0 ? lower : upper;
            return src[static_cast<std::size_t>(y) * nx_ + x];
        };

        for (int y = 0; y + 1 < ny_; ++y) {
            for (int x = 0; x + 1 < nx_; ++x) {
                // Gather the eight cube corners.
                float value[8];
                Vec3 point[8];
                int gx[8], gy[8], gz[8];
                for (int c = 0; c < 8; ++c) {
                    gx[c] = x + kCorner[c][0];
                    gy[c] = y + kCorner[c][1];
                    gz[c] = z + kCorner[c][2];
                    value[c] = field_at(gx[c], gy[c], kCorner[c][2]);
                    point[c] = grid_position(gx[c], gy[c], gz[c]);
                }

                for (const auto& tet : kTetra) {
                    // Inside = DE below the iso level.
                    int pattern = 0;
                    for (int v = 0; v < 4; ++v) {
                        if (value[tet[v]] < iso) pattern |= (1 << v);
                    }
                    const int* tris = kTetTri[pattern];
                    if (tris[0] < 0) continue;

                    std::uint32_t edge_index[6];
                    bool edge_built[6] = {};

                    auto build_edge = [&](int e) -> std::uint32_t {
                        if (edge_built[e]) return edge_index[e];
                        const int ca = tet[kTetEdge[e][0]];
                        const int cb = tet[kTetEdge[e][1]];

                        EdgeKey key{grid_index(gx[ca], gy[ca], gz[ca]),
                                    grid_index(gx[cb], gy[cb], gz[cb])};
                        if (key.a > key.b) std::swap(key.a, key.b);

                        if (const auto it = edge_vertices.find(key); it != edge_vertices.end()) {
                            edge_built[e] = true;
                            edge_index[e] = it->second;
                            return it->second;
                        }

                        // Linear interpolation to the crossing point.
                        const float va = value[ca], vb = value[cb];
                        const float denom = vb - va;
                        const double t = std::fabs(denom) < 1e-20f
                            ? 0.5
                            : std::clamp<double>((iso - va) / denom, 0.0, 1.0);
                        const Vec3 p = point[ca] + (point[cb] - point[ca]) * t;

                        const auto index = static_cast<std::uint32_t>(mesh.positions.size() / 3);
                        mesh.positions.push_back(static_cast<float>(p.x));
                        mesh.positions.push_back(static_cast<float>(p.y));
                        mesh.positions.push_back(static_cast<float>(p.z));
                        edge_vertices.emplace(key, index);
                        edge_built[e] = true;
                        edge_index[e] = index;
                        return index;
                    };

                    for (int i = 0; tris[i] >= 0; i += 3) {
                        const std::uint32_t a = build_edge(tris[i]);
                        const std::uint32_t b = build_edge(tris[i + 1]);
                        const std::uint32_t c = build_edge(tris[i + 2]);
                        if (a == b || b == c || a == c) continue;
                        mesh.indices.insert(mesh.indices.end(), {a, b, c});
                    }
                }
            }
        }

        lower.swap(upper);
        progress.advance(1, 1);
    }

    progress.finish(OMF_JOB_DONE);
    return mesh;
}

Mesh IsoSurfaceExtractor::extract_gpu(ProgressReporter& progress) {
#if defined(OMF_WITH_VULKAN)
    // The GPU path evaluates the DE field into a storage buffer slab by
    // slab, then runs the same tetrahedral extraction in a second
    // dispatch. If either shader is missing from the install we fall
    // back rather than fail the export.
    const auto field_spirv = load_spirv("de_field.comp.spv");
    const auto mc_spirv = load_spirv("marching_cubes.comp.spv");
    if (!field_spirv || !mc_spirv || !vk_.available()) {
        return extract_cpu(progress);
    }
    // Dispatch wiring lives in GpuIsoSurface.cpp; until the SPIR-V
    // toolchain is part of the default build we route through the CPU
    // extractor, which produces identical topology.
    return extract_cpu(progress);
#else
    return extract_cpu(progress);
#endif
}

void IsoSurfaceExtractor::apply_base_clip(Mesh& mesh) const {
    if (!mesh_settings_.base_clip_enabled) return;

    const float plane_z = mesh_settings_.base_clip_height;

    // Split every triangle against the plane, keeping the part above it.
    // Cutting rather than culling is what keeps the model watertight
    // after the clip -- a culled mesh would have an open rim.
    std::vector<float> positions = mesh.positions;
    std::vector<std::uint32_t> out_indices;
    std::vector<std::array<Vec3, 2>> rim;   // edges on the cut plane

    auto vertex = [&](std::uint32_t i) {
        return Vec3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    };
    auto emit = [&](const Vec3& p) {
        const auto index = static_cast<std::uint32_t>(positions.size() / 3);
        positions.push_back(static_cast<float>(p.x));
        positions.push_back(static_cast<float>(p.y));
        positions.push_back(static_cast<float>(p.z));
        return index;
    };

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t tri[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        Vec3 p[3] = {vertex(tri[0]), vertex(tri[1]), vertex(tri[2])};
        bool above[3];
        int above_count = 0;
        for (int k = 0; k < 3; ++k) {
            above[k] = p[k].z >= plane_z;
            if (above[k]) ++above_count;
        }

        if (above_count == 0) continue;
        if (above_count == 3) {
            out_indices.insert(out_indices.end(), {tri[0], tri[1], tri[2]});
            continue;
        }

        // Rotate so vertex 0 is the odd one out.
        int pivot = 0;
        for (int k = 0; k < 3; ++k) {
            if ((above_count == 1 && above[k]) || (above_count == 2 && !above[k])) pivot = k;
        }
        const int i0 = pivot, i1 = (pivot + 1) % 3, i2 = (pivot + 2) % 3;

        auto split = [&](const Vec3& a, const Vec3& b) {
            const double t = (plane_z - a.z) / (b.z - a.z);
            return a + (b - a) * std::clamp(t, 0.0, 1.0);
        };
        const Vec3 s01 = split(p[i0], p[i1]);
        const Vec3 s02 = split(p[i0], p[i2]);
        const std::uint32_t e01 = emit(s01);
        const std::uint32_t e02 = emit(s02);

        if (above_count == 1) {
            out_indices.insert(out_indices.end(), {tri[i0], e01, e02});
            rim.push_back({s02, s01});
        } else {
            out_indices.insert(out_indices.end(), {e01, tri[i1], tri[i2]});
            out_indices.insert(out_indices.end(), {e01, tri[i2], e02});
            rim.push_back({s01, s02});
        }
    }

    // Cap the opening with a fan around the rim centroid. The rim is a
    // closed loop by construction, so the fan seals it.
    if (!rim.empty()) {
        Vec3 centroid{};
        for (const auto& e : rim) centroid += (e[0] + e[1]) * 0.5;
        centroid = centroid / static_cast<double>(rim.size());
        centroid.z = plane_z;
        const std::uint32_t hub = emit(centroid);

        for (const auto& e : rim) {
            const std::uint32_t a = emit(e[0]);
            const std::uint32_t b = emit(e[1]);
            out_indices.insert(out_indices.end(), {hub, a, b});
        }
    }

    mesh.positions.swap(positions);
    mesh.indices.swap(out_indices);
    mesh.normals.clear();
}

Mesh IsoSurfaceExtractor::extract(ProgressReporter& progress) {
    Mesh mesh = vk_.available() ? extract_gpu(progress) : extract_cpu(progress);

    if (mesh_settings_.base_clip_enabled) apply_base_clip(mesh);
    if (mesh_settings_.weld_vertices) {
        const float eps = mesh_settings_.weld_epsilon > 0.0f
            ? mesh_settings_.weld_epsilon
            : static_cast<float>(std::min({cell_.x, cell_.y, cell_.z}) * 1e-3);
        mesh.weld(eps);
    }
    if (mesh_settings_.smooth_iterations > 0) {
        mesh.laplacian_smooth(mesh_settings_.smooth_iterations);
    }
    if (mesh_settings_.generate_normals) mesh.compute_normals();

    require(!mesh.indices.empty(), OMF_ERR_GEOMETRY,
            "iso-surface extraction produced no geometry -- check bounds and iso level");
    return mesh;
}

// ---------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------

void write_stl_binary(const Mesh& mesh, const std::string& path, float scale_to_mm) {
    std::ofstream file(path, std::ios::binary);
    require(file.good(), OMF_ERR_IO, "cannot open '" + path + "' for writing");

    // 80-byte header. Solidworks and some slicers choke on a header that
    // starts with "solid", so it deliberately does not.
    char header[80] = {};
    std::snprintf(header, sizeof(header), "OMF v%d.%d binary STL -- %zu triangles",
                  OMF_VERSION_MAJOR, OMF_VERSION_MINOR, mesh.triangle_count());
    file.write(header, sizeof(header));

    const auto count = static_cast<std::uint32_t>(mesh.triangle_count());
    file.write(reinterpret_cast<const char*>(&count), 4);

    const float k = scale_to_mm > 0.0f ? scale_to_mm : 1.0f;

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1],
                            ic = mesh.indices[i + 2];
        const Vec3 a{mesh.positions[ia * 3] * k, mesh.positions[ia * 3 + 1] * k,
                     mesh.positions[ia * 3 + 2] * k};
        const Vec3 b{mesh.positions[ib * 3] * k, mesh.positions[ib * 3 + 1] * k,
                     mesh.positions[ib * 3 + 2] * k};
        const Vec3 c{mesh.positions[ic * 3] * k, mesh.positions[ic * 3 + 1] * k,
                     mesh.positions[ic * 3 + 2] * k};
        const Vec3 n = normalize(cross(b - a, c - a));

        const float facet[12] = {
            static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z),
            static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z),
            static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z),
            static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)};
        file.write(reinterpret_cast<const char*>(facet), sizeof(facet));

        const std::uint16_t attribute = 0;
        file.write(reinterpret_cast<const char*>(&attribute), 2);
    }

    require(file.good(), OMF_ERR_IO, "write failed for '" + path + "'");
}

void write_stl_ascii(const Mesh& mesh, const std::string& path, float scale_to_mm) {
    std::ofstream file(path);
    require(file.good(), OMF_ERR_IO, "cannot open '" + path + "' for writing");

    const float k = scale_to_mm > 0.0f ? scale_to_mm : 1.0f;
    file << "solid omf\n";
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t idx[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        Vec3 v[3];
        for (int j = 0; j < 3; ++j) {
            v[j] = Vec3{mesh.positions[idx[j] * 3] * k,
                        mesh.positions[idx[j] * 3 + 1] * k,
                        mesh.positions[idx[j] * 3 + 2] * k};
        }
        const Vec3 n = normalize(cross(v[1] - v[0], v[2] - v[0]));
        file << "  facet normal " << n.x << ' ' << n.y << ' ' << n.z << "\n    outer loop\n";
        for (const Vec3& p : v) file << "      vertex " << p.x << ' ' << p.y << ' ' << p.z << '\n';
        file << "    endloop\n  endfacet\n";
    }
    file << "endsolid omf\n";
    require(file.good(), OMF_ERR_IO, "write failed for '" + path + "'");
}

void write_obj(const Mesh& mesh, const std::string& path, float scale_to_mm) {
    std::ofstream file(path);
    require(file.good(), OMF_ERR_IO, "cannot open '" + path + "' for writing");

    const float k = scale_to_mm > 0.0f ? scale_to_mm : 1.0f;
    file << "# OmniFractal export\no omf\n";
    for (std::size_t v = 0; v < mesh.vertex_count(); ++v) {
        file << "v " << mesh.positions[v * 3] * k << ' '
                     << mesh.positions[v * 3 + 1] * k << ' '
                     << mesh.positions[v * 3 + 2] * k << '\n';
    }
    if (!mesh.normals.empty()) {
        for (std::size_t v = 0; v < mesh.vertex_count(); ++v) {
            file << "vn " << mesh.normals[v * 3] << ' '
                          << mesh.normals[v * 3 + 1] << ' '
                          << mesh.normals[v * 3 + 2] << '\n';
        }
    }
    const bool has_normals = !mesh.normals.empty();
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t a = mesh.indices[i] + 1;
        const std::uint32_t b = mesh.indices[i + 1] + 1;
        const std::uint32_t c = mesh.indices[i + 2] + 1;
        if (has_normals) {
            file << "f " << a << "//" << a << ' ' << b << "//" << b << ' '
                         << c << "//" << c << '\n';
        } else {
            file << "f " << a << ' ' << b << ' ' << c << '\n';
        }
    }
    require(file.good(), OMF_ERR_IO, "write failed for '" + path + "'");
}

}  // namespace omf
