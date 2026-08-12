// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// LegacyImport.cpp -- hardened, read-only importers for the Mandelbulb 3D
// file suite.
//
// See LegacyImport.h for the threat model. In short: every byte here is
// untrusted, every read is bounds-checked through ByteReader, every
// length is capped before it allocates, and no machine code is ever
// executed.
//
// FORMAT NOTES
// The parameter-bearing formats (.m3p, .m3l, .m3a) are compressed binary:
// a small plaintext container header identifying the format and writer
// version, followed by a DEFLATE stream. Inflating yields a record
// sequence -- a tag, a length, and a payload -- which is what the
// per-format decoders below walk. Records the reader does not recognise
// are counted into report.unmapped_field_count and skipped by their
// declared length, which is why an unknown record can never desynchronise
// the stream.
//
// The layout knowledge lives entirely in this file. It was derived from
// observing the format for interoperability purposes; no legacy source
// was translated. Where a field's meaning is inferred rather than
// certain, the comment says so, because a future maintainer needs to know
// which constants are load-bearing and which are best-effort.

#include "mb3d/LegacyImport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unordered_map>

#if defined(MB3D_WITH_MINIZ)
#  include <miniz.h>
#endif

namespace mb3d::legacy {
namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void set_fixed(char* dest, std::size_t capacity, std::string_view value) {
    const std::size_t n = std::min(value.size(), capacity - 1);
    std::memset(dest, 0, capacity);
    std::memcpy(dest, value.data(), n);
}

// Record tags in the inflated parameter stream. Values are the format's,
// not ours; they are stable across the legacy releases we import.
enum RecordTag : std::uint16_t {
    REC_END              = 0x0000,
    REC_VERSION          = 0x0001,
    REC_IMAGE_SIZE       = 0x0010,
    REC_CAMERA           = 0x0011,
    REC_CAMERA_ANGLES    = 0x0012,
    REC_ZOOM             = 0x0013,
    REC_FOV              = 0x0014,
    REC_STEREO           = 0x0015,
    REC_DOF              = 0x0016,
    REC_CALC_BASIC       = 0x0020,
    REC_CALC_SAMPLING    = 0x0021,
    REC_CALC_BOUNDS      = 0x0022,
    REC_CALC_NORMALS     = 0x0023,
    REC_FORMULA_SLOT     = 0x0030,   // repeated, index in payload
    REC_HYBRID_MODE      = 0x0031,
    REC_JULIA            = 0x0032,
    REC_LIGHT_CHANNEL    = 0x0040,   // repeated
    REC_LIGHT_GLOBAL     = 0x0041,
    REC_FOG              = 0x0042,
    REC_TONEMAP          = 0x0043,
    REC_BACKGROUND       = 0x0044,
    REC_POST             = 0x0045,
    REC_REFLECTION       = 0x0046,
    REC_COLOR_BASIC      = 0x0050,
    REC_ORBIT_TRAP       = 0x0051,
    REC_GRADIENT         = 0x0052,
    REC_KEYFRAME         = 0x0060,   // repeated, .m3a only
    REC_ANIM_HEADER      = 0x0061,
};

// Every record carries its own length, so the walker can skip anything
// it does not understand without losing sync.
struct Record {
    std::uint16_t tag{};
    std::uint32_t index{};
    std::span<const std::byte> payload;
};

// ---------------------------------------------------------------------
// Container header
// ---------------------------------------------------------------------

struct ContainerHeader {
    mb3d_file_kind_t kind{MB3D_FILE_UNKNOWN};
    VersionMarker    version;
    std::size_t      payload_offset{};
    std::size_t      payload_size{};
    std::size_t      inflated_hint{};
    bool             compressed{true};
};

// Parses "Mandelbulb3Dv18" style ASCII markers into an encoded version.
// The trailing integer is a parameter-set generation, not a release
// number, so it is mapped rather than parsed arithmetically. Anything
// unrecognised yields 0, which the caller surfaces as "unknown origin"
// rather than guessing a profile.
VersionMarker decode_version_marker(std::string_view text) {
    VersionMarker marker;
    marker.text = std::string(text);

    static const std::unordered_map<std::string, std::int32_t> kGenerationToRelease = {
        {"mandelbulb3dv16", 19908},
        {"mandelbulb3dv17", 19910},
        {"mandelbulb3dv18", 19912},
        {"mandelbulb3dv19", 19919},
        {"mandelbulb3dv20", 19935},
    };

    const std::string key = to_lower(text);
    if (const auto it = kGenerationToRelease.find(key); it != kGenerationToRelease.end()) {
        marker.encoded = it->second;
    }
    return marker;
}

// The container prefix is a short ASCII line followed by the compressed
// payload. Reading it does not require inflating anything, which is what
// makes identification and version detection cheap.
ContainerHeader parse_container(std::span<const std::byte> data, mb3d_file_kind_t expected) {
    require(data.size() >= 16, MB3D_ERR_CORRUPT, "file is too small to be a legacy container");

    ByteReader reader(data);

    // Scan a bounded prefix for the marker line terminator. Bounded so a
    // file with no newline cannot drive an unbounded scan.
    constexpr std::size_t kMaxMarkerBytes = 256;
    const std::size_t scan_limit = std::min<std::size_t>(kMaxMarkerBytes, data.size());

    std::size_t marker_end = 0;
    for (std::size_t i = 0; i < scan_limit; ++i) {
        const auto c = static_cast<unsigned char>(data[i]);
        if (c == '\n' || c == '\r' || c == '\0') { marker_end = i; break; }
        // A non-printable byte before any terminator means this is not a
        // marker-prefixed container.
        if (c < 0x20 || c > 0x7E) break;
    }

    ContainerHeader header;
    header.kind = expected;

    if (marker_end > 0) {
        header.version = decode_version_marker(
            std::string(reinterpret_cast<const char*>(data.data()), marker_end));
        reader.seek(marker_end);
        // Consume the terminator sequence.
        while (!reader.exhausted()) {
            const auto c = static_cast<unsigned char>(data[reader.position()]);
            if (c != '\n' && c != '\r' && c != '\0') break;
            reader.skip(1);
        }
    }

    // A 32-bit inflated-size hint precedes the DEFLATE stream. Treated
    // strictly as a hint: it is capped and cross-checked, never trusted
    // to size an allocation on its own.
    if (reader.remaining() >= 4) {
        const std::uint32_t hint = reader.u32();
        header.inflated_hint = hint <= Limits::kMaxDecompressed ? hint : 0;
    }

    header.payload_offset = reader.position();
    header.payload_size = reader.remaining();
    require(header.payload_size > 0, MB3D_ERR_CORRUPT, "legacy container has no payload");
    return header;
}

// ---------------------------------------------------------------------
// Record stream
// ---------------------------------------------------------------------

std::vector<Record> walk_records(std::span<const std::byte> stream, int& unmapped_count) {
    ByteReader reader(stream);
    std::vector<Record> records;

    while (reader.remaining() >= 10) {
        const std::uint16_t tag = reader.u16();
        if (tag == REC_END) break;

        const std::uint32_t index = reader.u32();
        const std::uint32_t length = reader.u32();

        // The length is attacker-controlled; bytes() bounds-checks it
        // against what actually remains, so an inflated length aborts the
        // parse instead of reading past the buffer.
        require(length <= reader.remaining(), MB3D_ERR_CORRUPT,
                "record 0x" + std::to_string(tag) + " declares " +
                std::to_string(length) + " bytes but only " +
                std::to_string(reader.remaining()) + " remain");

        Record record;
        record.tag = tag;
        record.index = index;
        record.payload = reader.bytes(length);
        records.push_back(record);

        require(records.size() <= Limits::kMaxRecords, MB3D_ERR_LIMIT_EXCEEDED,
                "record count exceeds the import limit");
    }

    (void)unmapped_count;
    return records;
}

// ---------------------------------------------------------------------
// Record decoders
// ---------------------------------------------------------------------
//
// Each decoder reads only as much of its payload as it understands and
// ignores any trailing bytes. That is deliberate: a later legacy release
// that appended fields to a record still parses correctly here, and the
// extra bytes are not misread as something else.

void decode_camera(const Record& record, mb3d_camera_transform_t& camera) {
    ByteReader reader(record.payload);
    if (reader.remaining() < 6 * sizeof(double)) return;

    for (double& v : camera.position) v = reader.f64();
    for (double& v : camera.target) v = reader.f64();
    if (reader.remaining() >= sizeof(double)) camera.distance = reader.f64();
    if (reader.remaining() >= 3 * sizeof(double)) {
        for (double& v : camera.up) v = reader.f64();
    }
}

void decode_camera_angles(const Record& record, mb3d_camera_transform_t& camera) {
    ByteReader reader(record.payload);
    if (reader.remaining() < 3 * sizeof(double)) return;
    for (double& v : camera.rotation) v = reader.f64();
}

void decode_calc_basic(const Record& record, mb3d_calculation_settings_t& calc) {
    ByteReader reader(record.payload);
    if (reader.remaining() >= 4) calc.max_iterations = reader.i32();
    if (reader.remaining() >= 4) calc.escape_radius = reader.f32();
    if (reader.remaining() >= 4) calc.de_max_steps = reader.i32();

    // A file can legitimately carry an absurd iteration cap; clamp rather
    // than reject, because the scene is otherwise fine and the user can
    // raise it deliberately afterwards.
    calc.max_iterations = std::clamp(calc.max_iterations, 1, 100000);
    calc.de_max_steps = std::clamp(calc.de_max_steps, 1, 100000);
    if (!(calc.escape_radius > 0.0f) || !std::isfinite(calc.escape_radius)) {
        calc.escape_radius = 4.0f;
    }
}

void decode_calc_sampling(const Record& record, mb3d_calculation_settings_t& calc) {
    ByteReader reader(record.payload);
    if (reader.remaining() >= 4) calc.raystep_multiplier = reader.f32();
    if (reader.remaining() >= 4) calc.stepwidth_limiter = reader.f32();
    if (reader.remaining() >= 4) calc.de_stop_criterion = reader.f32();
    if (reader.remaining() >= 4) calc.vary_destop_on_fov = reader.i32() != 0;
    if (reader.remaining() >= 4) calc.first_step_random = reader.i32() != 0;
    if (reader.remaining() >= 4) calc.raystep_sub_destop = reader.i32() != 0;

    // These three dominate the look. A non-finite or non-positive value
    // would make the marcher stall or never terminate, so they are
    // repaired to the engine default rather than propagated.
    if (!(calc.raystep_multiplier > 0.0f) || !std::isfinite(calc.raystep_multiplier)) {
        calc.raystep_multiplier = 1.0f;
    }
    if (!(calc.stepwidth_limiter > 0.0f) || !std::isfinite(calc.stepwidth_limiter)) {
        calc.stepwidth_limiter = 1.0f;
    }
    if (!(calc.de_stop_criterion > 0.0f) || !std::isfinite(calc.de_stop_criterion)) {
        calc.de_stop_criterion = 1e-5f;
    }
}

void decode_calc_bounds(const Record& record, mb3d_calculation_settings_t& calc) {
    ByteReader reader(record.payload);
    if (reader.remaining() >= 4) calc.bound_sphere_enabled = reader.i32() != 0;
    if (reader.remaining() >= 12) for (float& v : calc.bound_sphere_center) v = reader.f32();
    if (reader.remaining() >= 4) calc.bound_sphere_radius = reader.f32();
    if (reader.remaining() >= 4) calc.bound_box_enabled = reader.i32() != 0;
    if (reader.remaining() >= 12) for (float& v : calc.bound_box_min) v = reader.f32();
    if (reader.remaining() >= 12) for (float& v : calc.bound_box_max) v = reader.f32();
}

void decode_formula_slot(const Record& record, mb3d_render_settings_t& settings,
                         mb3d_import_report_t& report) {
    if (record.index >= MB3D_FORMULA_SLOTS) {
        // Out-of-range slot index: count it and move on rather than
        // writing past the array.
        ++report.unmapped_field_count;
        return;
    }
    mb3d_formula_slot_t& slot = settings.formulas[record.index];

    ByteReader reader(record.payload);

    // Fixed-width name field. Whatever it contains is a name, never a
    // path and never code.
    const std::string legacy_name = reader.fixed_string(
        std::min<std::size_t>(MB3D_MAX_NAME, reader.remaining()));

    if (reader.remaining() >= 4) slot.enabled = reader.i32() != 0;
    if (reader.remaining() >= 4) slot.formula_class = std::clamp(reader.i32(), 0, 3);
    if (reader.remaining() >= 4) slot.iteration_weight = reader.i32();
    if (reader.remaining() >= 4) slot.start_iteration = reader.i32();
    if (reader.remaining() >= 4) slot.stop_iteration = reader.i32();
    if (reader.remaining() >= 4) slot.repeat_from_slot = reader.i32();
    if (reader.remaining() >= 4) slot.r_bailout = reader.f32();
    if (reader.remaining() >= 4) slot.min_iterations = reader.i32();
    if (reader.remaining() >= 4) slot.max_iterations = reader.i32();

    for (float& p : slot.params) {
        if (reader.remaining() < 4) break;
        p = reader.f32();
    }
    for (std::int32_t& p : slot.iparams) {
        if (reader.remaining() < 4) break;
        p = reader.i32();
    }
    for (float& r : slot.rotation) {
        if (reader.remaining() < 4) break;
        r = reader.f32();
    }

    slot.iteration_weight = std::clamp(slot.iteration_weight, 1, 10000);
    slot.repeat_from_slot =
        (slot.repeat_from_slot >= 0 && slot.repeat_from_slot < MB3D_FORMULA_SLOTS)
            ? slot.repeat_from_slot : -1;

    // Name resolution. An unresolved formula disables the slot: rendering
    // a different formula under the right name produces a confidently
    // wrong image, which is worse than an obviously missing one.
    if (const auto resolved = resolve_formula_name(legacy_name)) {
        set_fixed(slot.name, sizeof(slot.name), *resolved);
        slot.kind = MB3D_FORMULA_IMPORTED;
    } else {
        set_fixed(slot.name, sizeof(slot.name), legacy_name);
        slot.enabled = 0;
        slot.kind = MB3D_FORMULA_NONE;
        ++report.unresolved_formula_count;
    }
}

void decode_light_channel(const Record& record, mb3d_lighting_settings_t& lighting,
                          mb3d_import_report_t& report) {
    if (record.index >= MB3D_LIGHT_CHANNELS) {
        ++report.unmapped_field_count;
        return;
    }
    mb3d_light_channel_t& channel = lighting.channels[record.index];

    ByteReader reader(record.payload);
    if (reader.remaining() >= 4) channel.kind = std::clamp(reader.i32(), 0, 3);
    if (reader.remaining() >= 4) channel.enabled = reader.i32() != 0;
    if (reader.remaining() >= 12) for (float& c : channel.color) c = reader.f32();
    if (reader.remaining() >= 4) channel.intensity = reader.f32();
    if (reader.remaining() >= 12) for (float& p : channel.position) p = reader.f32();
    if (reader.remaining() >= 4) channel.falloff = reader.f32();
    if (reader.remaining() >= 4) channel.specular = reader.f32();
    if (reader.remaining() >= 4) channel.specular_exponent = reader.f32();
    if (reader.remaining() >= 4) channel.diffuse = reader.f32();
    if (reader.remaining() >= 4) channel.casts_shadow = reader.i32() != 0;
    if (reader.remaining() >= 4) channel.volumetric = reader.f32();

    // Light-map references are names only. The asset itself is never
    // bundled and never loaded from a path in the file -- the host
    // resolves it against a user-configured directory.
    if (reader.remaining() > 0) {
        const std::string map_name =
            reader.fixed_string(std::min<std::size_t>(MB3D_MAX_NAME, reader.remaining()));
        set_fixed(channel.lightmap, sizeof(channel.lightmap), map_name);
    }
}

void decode_gradient(const Record& record, mb3d_coloring_settings_t& coloring) {
    ByteReader reader(record.payload);
    // Entries beyond 256 are ignored; fewer than 256 leaves the tail at
    // the engine default rather than at black.
    const std::size_t count =
        std::min<std::size_t>(MB3D_GRADIENT_ENTRIES, reader.remaining() / 4);
    for (std::size_t i = 0; i < count; ++i) {
        coloring.gradient[i] = 0xFF000000u | (reader.u32() & 0x00FFFFFFu);
    }
}

void decode_orbit_trap(const Record& record, mb3d_coloring_settings_t& coloring) {
    ByteReader reader(record.payload);
    if (reader.remaining() >= 4) coloring.trap_kind = std::clamp(reader.i32(), 0, 6);
    if (reader.remaining() >= 4) coloring.trap_dimensions = std::clamp(reader.i32(), 1, 3);
    if (reader.remaining() >= 12) for (float& v : coloring.trap_center) v = reader.f32();
    if (reader.remaining() >= 12) for (float& v : coloring.trap_normal) v = reader.f32();
    if (reader.remaining() >= 4) coloring.trap_size = reader.f32();
    if (reader.remaining() >= 4) coloring.trap_influence = reader.f32();
    if (reader.remaining() >= 4) coloring.trap_min_iteration = std::max(0, reader.i32());
}

void decode_keyframe(const Record& record, std::vector<mb3d_keyframe_t>& keyframes) {
    require(keyframes.size() < Limits::kMaxKeyframes, MB3D_ERR_LIMIT_EXCEEDED,
            "keyframe count exceeds the import limit");

    ByteReader reader(record.payload);
    mb3d_keyframe_t key{};
    key.frame = static_cast<std::int32_t>(record.index);

    if (reader.remaining() >= 4) key.interpolation = std::clamp(reader.i32(), 0, 3);
    if (reader.remaining() >= 4) key.tension = reader.f32();
    if (reader.remaining() >= 4) key.bias = reader.f32();
    if (reader.remaining() >= 4) key.continuity = reader.f32();

    if (reader.remaining() >= 9 * sizeof(double)) {
        for (double& v : key.camera.position) v = reader.f64();
        for (double& v : key.camera.target) v = reader.f64();
        for (double& v : key.camera.rotation) v = reader.f64();
    }
    if (reader.remaining() >= sizeof(double)) key.camera.zoom = reader.f64();
    if (reader.remaining() >= sizeof(double)) key.camera.distance = reader.f64();

    for (auto& slot : key.formula_params) {
        for (float& p : slot) {
            if (reader.remaining() < 4) break;
            p = reader.f32();
        }
    }
    keyframes.push_back(key);
}

// Applies decoded records onto a settings struct.
void apply_records(const std::vector<Record>& records, ImportResult& result) {
    mb3d_render_settings_t& settings = result.settings;

    for (const Record& record : records) {
        switch (record.tag) {
            case REC_VERSION:
                break;   // handled from the container header
            case REC_IMAGE_SIZE: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 8) {
                    const std::int32_t w = reader.i32();
                    const std::int32_t h = reader.i32();
                    if (w > 0 && h > 0 &&
                        w <= static_cast<std::int32_t>(Limits::kMaxImageExtent) &&
                        h <= static_cast<std::int32_t>(Limits::kMaxImageExtent)) {
                        settings.width = w;
                        settings.height = h;
                    }
                }
                break;
            }
            case REC_CAMERA:        decode_camera(record, settings.camera); break;
            case REC_CAMERA_ANGLES: decode_camera_angles(record, settings.camera); break;
            case REC_ZOOM: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 8) settings.camera.zoom = reader.f64();
                if (!(settings.camera.zoom > 0.0)) settings.camera.zoom = 1.0;
                break;
            }
            case REC_FOV: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 8) settings.camera.fov_degrees = reader.f64();
                break;
            }
            case REC_STEREO: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 4) {
                    settings.camera.stereo_mode = std::clamp(reader.i32(), 0, 4);
                }
                if (reader.remaining() >= 8) settings.camera.stereo_ipd = reader.f64();
                break;
            }
            case REC_DOF: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 8) settings.camera.dof_focal_plane = reader.f64();
                if (reader.remaining() >= 8) settings.camera.dof_strength = reader.f64();
                break;
            }
            case REC_CALC_BASIC:    decode_calc_basic(record, settings.calculation); break;
            case REC_CALC_SAMPLING: decode_calc_sampling(record, settings.calculation); break;
            case REC_CALC_BOUNDS:   decode_calc_bounds(record, settings.calculation); break;
            case REC_CALC_NORMALS: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 4) {
                    settings.calculation.binary_search_steps =
                        std::clamp(reader.i32(), 0, 64);
                }
                if (reader.remaining() >= 4) {
                    settings.calculation.smooth_normals = reader.i32() != 0;
                }
                if (reader.remaining() >= 4) {
                    settings.calculation.normals_on_de = reader.i32() != 0;
                }
                break;
            }
            case REC_FORMULA_SLOT:
                decode_formula_slot(record, settings, result.report);
                break;
            case REC_HYBRID_MODE: {
                ByteReader reader(record.payload);
                if (reader.remaining() >= 4) {
                    settings.hybrid_master_mode = std::clamp(reader.i32(), 0, 4);
                }
                if (reader.remaining() >= 4) {
                    const std::int32_t op = std::clamp(reader.i32(), 0, 6);
                    for (auto& slot : settings.formulas) slot.decomb_op = op;
                }
                break;
            }
            case REC_LIGHT_CHANNEL:
                decode_light_channel(record, settings.lighting, result.report);
                break;
            case REC_FOG: {
                ByteReader reader(record.payload);
                auto& L = settings.lighting;
                if (reader.remaining() >= 12) for (float& c : L.fog_color_a) c = reader.f32();
                if (reader.remaining() >= 12) for (float& c : L.fog_color_b) c = reader.f32();
                if (reader.remaining() >= 4) L.fog_start = reader.f32();
                if (reader.remaining() >= 4) L.fog_end = reader.f32();
                if (reader.remaining() >= 4) L.fog_density = reader.f32();
                if (reader.remaining() >= 4) L.far_fog_density = reader.f32();
                if (reader.remaining() >= 4) L.iteration_fog_density = reader.f32();
                break;
            }
            case REC_TONEMAP: {
                ByteReader reader(record.payload);
                auto& L = settings.lighting;
                if (reader.remaining() >= 4) L.gamma = reader.f32();
                if (reader.remaining() >= 4) L.contrast = reader.f32();
                if (reader.remaining() >= 4) L.brightness = reader.f32();
                if (reader.remaining() >= 4) L.saturation = reader.f32();
                if (!(L.gamma > 0.0f) || !std::isfinite(L.gamma)) L.gamma = 2.2f;
                break;
            }
            case REC_POST: {
                ByteReader reader(record.payload);
                auto& L = settings.lighting;
                if (reader.remaining() >= 4) L.ssao_enabled = reader.i32() != 0;
                if (reader.remaining() >= 4) L.ssao_radius = reader.f32();
                if (reader.remaining() >= 4) L.ssao_intensity = reader.f32();
                if (reader.remaining() >= 4) L.ssao_samples = std::clamp(reader.i32(), 0, 256);
                if (reader.remaining() >= 4) L.hard_shadows = reader.i32() != 0;
                if (reader.remaining() >= 4) L.smooth_shadows = reader.i32() != 0;
                if (reader.remaining() >= 4) L.deao_enabled = reader.i32() != 0;
                if (reader.remaining() >= 4) L.deao_samples = std::clamp(reader.i32(), 0, 1024);
                if (reader.remaining() >= 4) L.deao_radius = reader.f32();
                break;
            }
            case REC_REFLECTION: {
                ByteReader reader(record.payload);
                auto& L = settings.lighting;
                if (reader.remaining() >= 4) L.reflectivity = reader.f32();
                if (reader.remaining() >= 4) {
                    L.reflection_bounces = std::clamp(reader.i32(), 0, 16);
                }
                if (reader.remaining() >= 4) L.transparency = reader.f32();
                if (reader.remaining() >= 4) L.refraction_index = reader.f32();
                break;
            }
            case REC_COLOR_BASIC: {
                ByteReader reader(record.payload);
                auto& C = settings.coloring;
                if (reader.remaining() >= 4) C.color_source = std::clamp(reader.i32(), 0, 6);
                if (reader.remaining() >= 4) C.color_speed = reader.f32();
                if (reader.remaining() >= 4) C.color_offset = reader.f32();
                if (reader.remaining() >= 4) C.color_cycle = reader.f32();
                if (reader.remaining() >= 4) C.inside_rendering = reader.i32() != 0;
                break;
            }
            case REC_ORBIT_TRAP: decode_orbit_trap(record, settings.coloring); break;
            case REC_GRADIENT:   decode_gradient(record, settings.coloring); break;
            case REC_KEYFRAME:   decode_keyframe(record, result.keyframes); break;
            case REC_ANIM_HEADER: break;

            default:
                // Unknown record: counted so the host can tell the user
                // the import was lossy, then skipped by its own declared
                // length. This is why the walker cannot desynchronise.
                ++result.report.unmapped_field_count;
                break;
        }
    }
}

void finalise(ImportResult& result, mb3d_file_kind_t kind, const ContainerHeader& header,
              std::string_view profile_id, std::string_view source_name,
              std::size_t bytes_read) {
    mb3d_precision_profile_t profile{};
    const mb3d_result_t rc =
        mb3d_precision_profile_by_id(std::string(profile_id).c_str(), &profile);
    require(rc == MB3D_OK, MB3D_ERR_INVALID_ARG,
            "unknown precision profile '" + std::string(profile_id) + "'");
    result.settings.profile = profile;

    result.provenance.source_format = kind;
    result.provenance.source_version = header.version.encoded;
    set_fixed(result.provenance.source_version_text,
              sizeof(result.provenance.source_version_text), header.version.text);
    set_fixed(result.provenance.source_filename,
              sizeof(result.provenance.source_filename), source_name);
    set_fixed(result.provenance.profile_id, sizeof(result.provenance.profile_id), profile_id);
    result.provenance.import_unix_time =
        static_cast<std::int64_t>(std::time(nullptr));
    result.provenance.importer_abi_version = MB3D_ABI_VERSION;
    result.provenance.migrated = 0;
    result.provenance.unmapped_field_count = result.report.unmapped_field_count;

    result.report.source_format = kind;
    result.report.source_version = header.version.encoded;
    result.report.keyframe_count = static_cast<std::int32_t>(result.keyframes.size());
    result.report.has_parameters = 1;
    result.report.bytes_read = static_cast<std::int64_t>(bytes_read);

    std::sort(result.keyframes.begin(), result.keyframes.end(),
              [](const mb3d_keyframe_t& a, const mb3d_keyframe_t& b) {
                  return a.frame < b.frame;
              });
}

// Shared path for the three compressed parameter containers.
ImportResult import_compressed(std::span<const std::byte> data, mb3d_file_kind_t kind,
                               std::string_view profile_id) {
    const ContainerHeader header = parse_container(data, kind);

    const std::span<const std::byte> compressed =
        data.subspan(header.payload_offset, header.payload_size);
    const std::vector<std::byte> inflated = inflate(compressed, header.inflated_hint);

    ImportResult result;
    mb3d_render_settings_default(&result.settings);

    int unmapped = 0;
    const std::vector<Record> records = walk_records(inflated, unmapped);
    apply_records(records, result);

    finalise(result, kind, header, profile_id, {}, data.size());
    return result;
}

// ---------------------------------------------------------------------
// .m3i container
// ---------------------------------------------------------------------

#pragma pack(push, 1)
struct M3iFileHeader {
    char          magic[4];
    std::uint16_t version;
    std::uint16_t layer_count;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t flags;
    std::uint64_t parameter_offset;
    std::uint64_t parameter_size;
    std::uint64_t directory_offset;
};

struct M3iLayerEntry {
    std::uint32_t layer;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t offset;
    std::uint64_t size_stored;
    std::uint64_t size_raw;
};
#pragma pack(pop)

static_assert(sizeof(M3iFileHeader) == 44, "M3iFileHeader layout changed");
static_assert(sizeof(M3iLayerEntry) == 40, "M3iLayerEntry layout changed");

M3iFileHeader read_m3i_header(std::span<const std::byte> data) {
    require(data.size() >= sizeof(M3iFileHeader), MB3D_ERR_CORRUPT,
            "file is too small to be an .m3i container");

    M3iFileHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));

    require(std::memcmp(header.magic, "M3I", 3) == 0, MB3D_ERR_PARSE,
            "not an .m3i container");
    require(header.width > 0 && header.height > 0 &&
            header.width <= Limits::kMaxImageExtent &&
            header.height <= Limits::kMaxImageExtent,
            MB3D_ERR_CORRUPT, "'.m3i' declares implausible dimensions");
    require(header.layer_count <= Limits::kMaxLayers, MB3D_ERR_LIMIT_EXCEEDED,
            "'.m3i' declares too many layers");
    return header;
}

}  // namespace

// ---------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------

std::uint8_t ByteReader::u8() {
    need(1);
    return static_cast<std::uint8_t>(data_[cursor_++]);
}

std::uint16_t ByteReader::u16() {
    need(2);
    const auto lo = static_cast<std::uint16_t>(data_[cursor_]);
    const auto hi = static_cast<std::uint16_t>(data_[cursor_ + 1]);
    cursor_ += 2;
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t ByteReader::u32() {
    need(4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data_[cursor_ + i]) << (i * 8);
    }
    cursor_ += 4;
    return value;
}

std::uint64_t ByteReader::u64() {
    need(8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data_[cursor_ + i]) << (i * 8);
    }
    cursor_ += 8;
    return value;
}

std::int32_t ByteReader::i32() {
    const std::uint32_t raw = u32();
    std::int32_t value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

float ByteReader::f32() {
    const std::uint32_t raw = u32();
    float value;
    std::memcpy(&value, &raw, sizeof(value));
    // A NaN or infinity reaching a render parameter turns into a hung
    // marcher, so it is neutralised at the boundary.
    return std::isfinite(value) ? value : 0.0f;
}

double ByteReader::f64() {
    const std::uint64_t raw = u64();
    double value;
    std::memcpy(&value, &raw, sizeof(value));
    return std::isfinite(value) ? value : 0.0;
}

std::span<const std::byte> ByteReader::bytes(std::size_t count) {
    need(count);
    const auto view = data_.subspan(cursor_, count);
    cursor_ += count;
    return view;
}

std::string ByteReader::fixed_string(std::size_t count) {
    require(count <= Limits::kMaxStringBytes, MB3D_ERR_LIMIT_EXCEEDED,
            "string field exceeds the import limit");
    const auto view = bytes(count);

    // Stop at the first NUL; legacy fixed fields are NUL-padded.
    std::size_t length = 0;
    while (length < view.size() && view[length] != std::byte{0}) ++length;

    std::string text;
    text.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(view[i]);
        // Legacy text is Windows-1252. Keep printable ASCII, drop
        // control bytes, and substitute anything else -- a name is only
        // ever displayed or matched, never executed or used as a path.
        text.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    }
    return text;
}

std::string ByteReader::prefixed_string(std::size_t length_bytes) {
    std::size_t length = 0;
    switch (length_bytes) {
        case 1: length = u8(); break;
        case 2: length = u16(); break;
        case 4: length = u32(); break;
        default: fail(MB3D_ERR_INVALID_ARG, "unsupported string length prefix width");
    }
    require(length <= Limits::kMaxStringBytes, MB3D_ERR_LIMIT_EXCEEDED,
            "string field exceeds the import limit");
    return fixed_string(length);
}

// ---------------------------------------------------------------------
// Decompression
// ---------------------------------------------------------------------

std::vector<std::byte> inflate(std::span<const std::byte> compressed,
                               std::size_t expected_size) {
    require(!compressed.empty(), MB3D_ERR_CORRUPT, "empty compressed payload");

#if !defined(MB3D_WITH_MINIZ)
    (void)expected_size;
    fail(MB3D_ERR_UNSUPPORTED,
         "legacy import requires DEFLATE support; rebuild with -DMB3D_WITH_MINIZ=ON");
#else
    // Two independent caps. The absolute cap bounds worst-case memory;
    // the ratio cap is what actually defeats a bomb, because a bomb is
    // small on disk and enormous inflated.
    const std::size_t ratio_cap =
        compressed.size() > (Limits::kMaxDecompressed / Limits::kMaxCompressionRatio)
            ? Limits::kMaxDecompressed
            : compressed.size() * Limits::kMaxCompressionRatio;
    const std::size_t hard_cap = std::min(Limits::kMaxDecompressed, ratio_cap);

    // The hint is only ever used to size the first allocation, never to
    // bound the loop -- a lying hint costs a realloc, nothing more.
    std::size_t capacity = expected_size > 0 && expected_size <= hard_cap
        ? expected_size
        : std::min<std::size_t>(hard_cap, std::max<std::size_t>(compressed.size() * 4, 4096));

    std::vector<std::byte> output(capacity);

    mz_stream stream{};
    // Try zlib-wrapped first; legacy payloads carry the 2-byte header.
    int status = mz_inflateInit(&stream);
    require(status == MZ_OK, MB3D_ERR_UNKNOWN, "failed to initialise the inflater");

    stream.next_in = reinterpret_cast<const unsigned char*>(compressed.data());
    stream.avail_in = static_cast<unsigned int>(compressed.size());
    stream.next_out = reinterpret_cast<unsigned char*>(output.data());
    stream.avail_out = static_cast<unsigned int>(output.size());

    std::size_t produced = 0;
    for (;;) {
        status = mz_inflate(&stream, MZ_NO_FLUSH);
        produced = stream.total_out;

        if (status == MZ_STREAM_END) break;

        if (status == MZ_OK && stream.avail_out == 0) {
            // Grow, but never past the cap. Hitting the cap is treated as
            // a hostile input, not as a transient shortage.
            require(output.size() < hard_cap, MB3D_ERR_LIMIT_EXCEEDED,
                    "decompressed payload exceeds " + std::to_string(hard_cap) +
                    " bytes; refusing to continue");

            const std::size_t previous = output.size();
            const std::size_t grown = std::min(hard_cap, previous * 2);
            output.resize(grown);
            stream.next_out = reinterpret_cast<unsigned char*>(output.data() + previous);
            stream.avail_out = static_cast<unsigned int>(grown - previous);
            continue;
        }

        if (status == MZ_OK || status == MZ_BUF_ERROR) {
            // No progress and no output space needed means the stream is
            // truncated.
            mz_inflateEnd(&stream);
            fail(MB3D_ERR_CORRUPT, "compressed payload is truncated");
        }

        mz_inflateEnd(&stream);
        fail(MB3D_ERR_CORRUPT,
             std::string("DEFLATE stream is invalid (miniz status ") +
             std::to_string(status) + ")");
    }

    mz_inflateEnd(&stream);
    require(produced > 0, MB3D_ERR_CORRUPT, "decompressed payload is empty");
    output.resize(produced);
    return output;
#endif
}

// ---------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------

mb3d_file_kind_t identify_bytes(std::span<const std::byte> head, std::string_view extension) {
    // Magic first: a renamed file is still what it is, and drag-and-drop
    // sources rename constantly.
    if (head.size() >= 4) {
        const auto* bytes = reinterpret_cast<const char*>(head.data());
        if (std::memcmp(bytes, "MB4D", 4) == 0) return MB3D_FILE_M4D;
        if (std::memcmp(bytes, "M3I", 3) == 0)  return MB3D_FILE_M3I;
        if (std::memcmp(bytes, "M3V", 3) == 0)  return MB3D_FILE_M3V;
    }

    const std::string ext = to_lower(extension);
    if (ext == ".m4d") return MB3D_FILE_M4D;
    if (ext == ".m3p") return MB3D_FILE_M3P;
    if (ext == ".m3i") return MB3D_FILE_M3I;
    if (ext == ".m3a") return MB3D_FILE_M3A;
    if (ext == ".m3l") return MB3D_FILE_M3L;
    if (ext == ".m3c") return MB3D_FILE_M3C;
    if (ext == ".m3v") return MB3D_FILE_M3V;
    if (ext == ".m3f") return MB3D_FILE_M3F;
    if (ext == ".d3f") return MB3D_FILE_D3F;
    if (ext == ".dso") return MB3D_FILE_DSO;

    // Last resort: the ASCII marker line on a compressed container.
    if (head.size() >= 8) {
        const std::string text(reinterpret_cast<const char*>(head.data()),
                               std::min<std::size_t>(head.size(), 64));
        if (to_lower(text).find("mandelbulb3d") != std::string::npos) return MB3D_FILE_M3P;
    }
    return MB3D_FILE_UNKNOWN;
}

mb3d_file_kind_t identify(const std::string& path) {
    std::array<std::byte, 128> head{};
    std::size_t read = 0;

    if (std::ifstream file(path, std::ios::binary); file.good()) {
        file.read(reinterpret_cast<char*>(head.data()),
                  static_cast<std::streamsize>(head.size()));
        read = static_cast<std::size_t>(file.gcount());
    }

    const auto dot = path.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string{} : path.substr(dot);
    return identify_bytes(std::span(head.data(), read), extension);
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), MB3D_ERR_IO, "cannot open '" + path + "'");

    const auto size = static_cast<std::uint64_t>(file.tellg());
    require(size > 0, MB3D_ERR_CORRUPT, "'" + path + "' is empty");
    require(size <= Limits::kMaxFileBytes, MB3D_ERR_LIMIT_EXCEEDED,
            "'" + path + "' is larger than the " +
            std::to_string(Limits::kMaxFileBytes) + " byte import limit");

    std::vector<std::byte> data(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    require(file.good(), MB3D_ERR_IO, "short read on '" + path + "'");
    return data;
}

// ---------------------------------------------------------------------
// Public importers
// ---------------------------------------------------------------------

ImportResult import_m3p(std::span<const std::byte> data, std::string_view profile_id) {
    return import_compressed(data, MB3D_FILE_M3P, profile_id);
}

ImportResult import_m3a(std::span<const std::byte> data, std::string_view profile_id) {
    ImportResult result = import_compressed(data, MB3D_FILE_M3A, profile_id);
    require(!result.keyframes.empty(), MB3D_ERR_PARSE,
            "'.m3a' timeline contains no keyframes");
    return result;
}

ImportResult import_m3l(std::span<const std::byte> data, std::string_view profile_id) {
    return import_compressed(data, MB3D_FILE_M3L, profile_id);
}

std::vector<std::byte> extract_embedded_parameters(std::span<const std::byte> data) {
    const M3iFileHeader header = read_m3i_header(data);

    require(header.parameter_size > 0, MB3D_ERR_PARSE,
            "'.m3i' carries no embedded parameter block");
    require(header.parameter_size <= Limits::kMaxParameterBytes, MB3D_ERR_LIMIT_EXCEEDED,
            "embedded parameter block is implausibly large");
    require(header.parameter_offset <= data.size() &&
            header.parameter_size <= data.size() - header.parameter_offset,
            MB3D_ERR_CORRUPT, "embedded parameter block lies outside the file");

    const auto view = data.subspan(static_cast<std::size_t>(header.parameter_offset),
                                   static_cast<std::size_t>(header.parameter_size));
    return {view.begin(), view.end()};
}

ImportResult import_m3i(std::span<const std::byte> data, std::string_view profile_id) {
    const M3iFileHeader header = read_m3i_header(data);

    // Parameters first: a scene is useful even if the pixel payload is
    // damaged, and this is the path drag-and-drop takes.
    ImportResult result;
    mb3d_render_settings_default(&result.settings);

    if (header.parameter_size > 0) {
        const std::vector<std::byte> parameters = extract_embedded_parameters(data);
        result = import_m3p(parameters, profile_id);
    }

    result.report.image_width = static_cast<std::int32_t>(header.width);
    result.report.image_height = static_cast<std::int32_t>(header.height);
    result.report.layer_count = header.layer_count;

    // Layer directory.
    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(header.layer_count) * sizeof(M3iLayerEntry);
    require(header.directory_offset <= data.size() &&
            directory_bytes <= data.size() - header.directory_offset,
            MB3D_ERR_CORRUPT, "'.m3i' layer directory lies outside the file");

    std::vector<M3iLayerEntry> directory(header.layer_count);
    if (header.layer_count > 0) {
        std::memcpy(directory.data(), data.data() + header.directory_offset,
                    static_cast<std::size_t>(directory_bytes));
    }

    // Validate every entry before allocating anything.
    std::uint32_t layer_mask = 0;
    for (const M3iLayerEntry& entry : directory) {
        require(entry.width == header.width && entry.height == header.height,
                MB3D_ERR_CORRUPT, "'.m3i' layer disagrees with the container dimensions");
        require(entry.offset <= data.size() &&
                entry.size_stored <= data.size() - entry.offset,
                MB3D_ERR_CORRUPT, "'.m3i' layer payload extends past end of file");
        layer_mask |= entry.layer;
    }

    if (layer_mask != 0) {
        result.image = std::make_unique<FrameBuffer>(static_cast<int>(header.width),
                                                     static_cast<int>(header.height),
                                                     layer_mask);
        for (const M3iLayerEntry& entry : directory) {
            Layer* layer = result.image->layer(static_cast<mb3d_layer_t>(entry.layer));
            if (layer == nullptr) continue;

            // Exact match. A short payload would leave the tail of the
            // layer uninitialised, which is an information leak as well
            // as a correctness bug.
            require(entry.size_stored == layer->bytes.size(), MB3D_ERR_CORRUPT,
                    "'.m3i' layer payload does not match its declared geometry");
            std::memcpy(layer->bytes.data(), data.data() + entry.offset,
                        layer->bytes.size());
        }
    }

    result.provenance.source_format = MB3D_FILE_M3I;
    result.report.source_format = MB3D_FILE_M3I;
    return result;
}

ImportResult import_any(std::span<const std::byte> data, mb3d_file_kind_t kind,
                        std::string_view profile_id, std::string_view source_name) {
    ImportResult result;
    switch (kind) {
        case MB3D_FILE_M3P: result = import_m3p(data, profile_id); break;
        case MB3D_FILE_M3I: result = import_m3i(data, profile_id); break;
        case MB3D_FILE_M3A: result = import_m3a(data, profile_id); break;
        case MB3D_FILE_M3L: result = import_m3l(data, profile_id); break;

        case MB3D_FILE_M3F:
        case MB3D_FILE_D3F:
        case MB3D_FILE_DSO:
            fail(MB3D_ERR_UNTRUSTED,
                 "formula files carry executable code and are never loaded as scenes; "
                 "use mb3d_inspect_legacy_formula to read their metadata");

        case MB3D_FILE_M4D:
            fail(MB3D_ERR_INVALID_ARG, "'.m4d' is the native format -- use mb3d_m4d_read");

        default:
            fail(MB3D_ERR_UNSUPPORTED, "unrecognised legacy file kind");
    }

    if (!source_name.empty()) {
        set_fixed(result.provenance.source_filename,
                  sizeof(result.provenance.source_filename), source_name);
    }
    return result;
}

// ---------------------------------------------------------------------
// Formula metadata -- never executed
// ---------------------------------------------------------------------

FormulaMetadata parse_formula_metadata(std::span<const std::byte> data,
                                       mb3d_file_kind_t kind) {
    require(kind == MB3D_FILE_M3F || kind == MB3D_FILE_D3F || kind == MB3D_FILE_DSO,
            MB3D_ERR_INVALID_ARG, "not a formula file kind");
    require(!data.empty(), MB3D_ERR_CORRUPT, "formula file is empty");

    FormulaMetadata meta;

    // The declarative header is plain text `key = value` lines. It is
    // terminated by the start of the binary body. We scan only the text
    // prefix and stop at the first byte that cannot be part of it -- the
    // body is never read beyond noting where it begins.
    constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
    const std::size_t scan_limit = std::min(kMaxHeaderBytes, data.size());

    std::size_t header_end = scan_limit;
    for (std::size_t i = 0; i < scan_limit; ++i) {
        const auto c = static_cast<unsigned char>(data[i]);
        const bool printable = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t';
        if (!printable) { header_end = i; break; }
    }

    const std::string header_text(reinterpret_cast<const char*>(data.data()), header_end);

    // Anything after the text header is a machine-code body. We record
    // its extent for the report and never touch it again. There is no
    // code path in this program that maps, relocates, or calls it.
    if (header_end < data.size()) {
        meta.has_executable_body = true;
        meta.executable_body_offset = header_end;
        meta.executable_body_size = data.size() - header_end;
    }

    auto value_of = [&header_text](std::string_view key) -> std::string {
        const std::string needle = std::string(key) + "=";
        std::size_t search = 0;
        while (search < header_text.size()) {
            const std::size_t line_end = header_text.find('\n', search);
            std::string line = header_text.substr(
                search, line_end == std::string::npos ? std::string::npos : line_end - search);

            // Normalise "key = value" to "key=value" before matching.
            std::string compact;
            compact.reserve(line.size());
            for (char c : line) {
                if (c != ' ' && c != '\t' && c != '\r') compact.push_back(c);
            }
            const std::string lowered = to_lower(compact);
            if (lowered.rfind(needle, 0) == 0) return compact.substr(needle.size());

            if (line_end == std::string::npos) break;
            search = line_end + 1;
        }
        return {};
    };

    meta.name = value_of("name");
    meta.display_name = value_of("caption");
    if (meta.display_name.empty()) meta.display_name = meta.name;

    const std::string class_text = to_lower(value_of("class"));
    if (class_text == "3da")      meta.formula_class = MB3D_CLASS_3DA;
    else if (class_text == "4d")  meta.formula_class = MB3D_CLASS_4D;
    else if (class_text == "4da") meta.formula_class = MB3D_CLASS_4DA;
    else                          meta.formula_class = MB3D_CLASS_3D;

    meta.analytic_de = to_lower(value_of("analyticde")) == "1" ||
                       to_lower(value_of("analyticde")) == "true";

    for (int i = 0; i < MB3D_FORMULA_PARAMS; ++i) {
        const std::string name = value_of("param" + std::to_string(i));
        if (name.empty()) break;
        meta.parameter_names[static_cast<std::size_t>(i)] = name;
        meta.parameter_count = i + 1;
    }

    require(!meta.name.empty(), MB3D_ERR_PARSE,
            "formula file declares no name; refusing to guess");
    return meta;
}

// Maps legacy formula names onto built-in implementations.
//
// The table is intentionally conservative. A name that is not listed
// returns nullopt and the importing slot is disabled, because
// substituting a formula that merely resembles the requested one yields
// an image that looks plausible and is wrong -- far worse for a user
// than an obviously missing slot they can see and report.
std::optional<std::string> resolve_formula_name(std::string_view legacy_name) {
    static const std::unordered_map<std::string, std::string> kKnown = {
        // Bulbs
        {"integerpower3d",   "Bulb"},
        {"fastmandelbulb",   "Bulb"},
        {"bulbpow2",         "Bulb"},
        {"powerxyz",         "Bulb"},
        {"quaternion",       "Quaternion"},
        {"bulb4d",           "Quaternion"},
        // Boxes and folds
        {"amazingbox",       "Mandelbox"},
        {"amazingsurf",      "Mandelbox"},
        {"mandelbox",        "Mandelbox"},
        {"tglad",            "Mandelbox"},
        {"tgladfold",        "Mandelbox"},
        {"aboxmod1",         "Mandelbox"},
        {"aboxmod2",         "Mandelbox"},
        {"menger3",          "MengerFold"},
        {"mengersponge",     "MengerFold"},
        {"kaleidoscopicifs", "KaleidoFold"},
        {"kifs",             "KaleidoFold"},
        // Transforms / DIFs
        {"_rotate",          "Transform"},
        {"_sphericalfold",   "Transform"},
        {"_spherinv",        "Transform"},
        {"_tile",            "Transform"},
        {"_scale",           "Transform"},
        {"_abs",             "Transform"},
        // IFS
        {"sierpinski3",      "IFS"},
        {"octahedron",       "IFS"},
    };

    std::string key;
    key.reserve(legacy_name.size());
    for (char c : legacy_name) {
        // Legacy names vary in spacing and case between releases.
        if (c == ' ' || c == '-') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (const auto it = kKnown.find(key); it != kKnown.end()) return it->second;
    return std::nullopt;
}

}  // namespace mb3d::legacy
