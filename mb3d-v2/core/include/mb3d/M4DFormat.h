// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// M4DFormat.h -- the native .m4d container.
//
// .m4d is the only format MB3D-V2 writes. Everything else is import-only.
//
// LAYOUT (all little-endian, all offsets absolute from file start):
//
//   0        M4dHeader                     -- 128 bytes, magic "MB4D"
//   ...      chunk payloads, in any order
//   ...      chunk directory: chunk_count x M4dChunkEntry
//
// The directory sits at the end so a writer can stream payloads without
// knowing final offsets up front. The header points at it.
//
// WHY CHUNKS. The format has to carry two full parameter sets plus an
// optional multi-gigabyte image payload, and has to stay readable by a
// build that predates chunk types added later. A tagged chunk directory
// gives both: a reader takes the chunks it knows, records the rest as
// opaque, and round-trips them untouched.
//
// VERSIONING. `format_version` is checked exactly, not compared. A file
// from a newer build is refused with MB3D_ERR_FORMAT_VERSION rather than
// guessed at -- silently misreading a scene is worse than not opening
// it. Backward compatibility is handled by keeping old chunk types
// readable forever, never by reinterpreting an unknown version.
//
// DUAL PARAMETER SETS. CHUNK_SETTINGS_ACTIVE is what renders.
// CHUNK_SETTINGS_LEGACY is the as-imported set, preserved byte-for-byte
// so a precision-mode switch is reversible. Migration writes the former
// and never touches the latter.

#pragma once

#include "mb3d/Internal.hpp"

#include <optional>

namespace mb3d::m4d {

// ---------------------------------------------------------------------
// On-disk structures
// ---------------------------------------------------------------------

#pragma pack(push, 1)

// Chunk type tags. Four-character codes so a hex dump is readable.
enum ChunkType : std::uint32_t {
    CHUNK_SETTINGS_ACTIVE = 0x54534341u,  // "ACST" -- active settings
    CHUNK_SETTINGS_LEGACY = 0x54534C4Cu,  // "LLST" -- as-imported settings
    CHUNK_PROVENANCE      = 0x564F5250u,  // "PROV"
    CHUNK_PROFILE         = 0x464F5250u,  // "PROF" -- resolved profile
    CHUNK_KEYFRAMES       = 0x5346454Bu,  // "KEFS"
    CHUNK_GRADIENT        = 0x44415247u,  // "GRAD"
    CHUNK_IMAGE_LAYER     = 0x5259414Cu,  // "LAYR"
    CHUNK_SCENE_NAME      = 0x454D414Eu,  // "NAME"
    CHUNK_NOTES           = 0x53544F4Eu,  // "NOTS"
};

struct M4dHeader {
    char          magic[4];          // "MB4D"
    std::uint32_t format_version;    // MB3D_M4D_FORMAT_VERSION
    std::uint32_t writer_abi;        // MB3D_ABI_VERSION of the writer
    std::uint32_t flags;

    std::uint64_t directory_offset;
    std::uint32_t chunk_count;
    std::uint32_t header_size;       // sizeof(M4dHeader); lets a future
                                     // reader skip an extended header

    // Denormalised so a file browser can show dimensions without walking
    // the directory. Authoritative values live in the settings chunk.
    std::uint32_t image_width;
    std::uint32_t image_height;

    std::uint64_t file_size;         // for truncation detection
    std::uint64_t content_hash;      // FNV-1a over every chunk payload

    std::uint32_t reserved[18];
};

struct M4dChunkEntry {
    std::uint32_t type;              // ChunkType
    std::uint32_t index;             // discriminator for repeated types
    std::uint64_t offset;
    std::uint64_t size_stored;
    std::uint64_t size_raw;          // == size_stored when uncompressed
    std::uint32_t compression;       // 0 raw, 1 deflate
    std::uint32_t checksum;          // FNV-1a over the stored bytes
};

#pragma pack(pop)

static_assert(sizeof(M4dHeader) == 128, "M4dHeader must stay 128 bytes");
static_assert(sizeof(M4dChunkEntry) == 40, "M4dChunkEntry must stay 40 bytes");

constexpr std::uint32_t kFlagHasImage        = 1u << 0;
constexpr std::uint32_t kFlagHasLegacyParams = 1u << 1;
constexpr std::uint32_t kFlagHasAnimation    = 1u << 2;
constexpr std::uint32_t kFlagMigrated        = 1u << 3;

// ---------------------------------------------------------------------
// In-memory document
// ---------------------------------------------------------------------

// An unrecognised chunk, kept verbatim so a round-trip through an older
// build does not destroy data written by a newer one.
struct OpaqueChunk {
    std::uint32_t type{};
    std::uint32_t index{};
    std::vector<std::byte> bytes;
};

struct Document {
    mb3d_render_settings_t   active{};
    // Present only for imported scenes. Never modified after import.
    std::optional<mb3d_render_settings_t> legacy;
    mb3d_provenance_t        provenance{};
    std::vector<mb3d_keyframe_t> keyframes;
    std::string              name;
    std::string              notes;

    // Optional rendered payload.
    std::unique_ptr<FrameBuffer> image;

    std::vector<OpaqueChunk> unknown_chunks;

    bool has_legacy() const { return legacy.has_value(); }
};

// ---------------------------------------------------------------------
// Reader / writer
// ---------------------------------------------------------------------

// Both throw mb3d::Error on failure. Every read is bounds-checked
// against the declared file size before any allocation is made.
Document read(const std::string& path);
void     write(const std::string& path, const Document& document);

// Reads header and directory only -- no payloads. Used by the file
// browser and by drag-and-drop.
struct Summary {
    std::uint32_t format_version{};
    std::uint32_t writer_abi{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool          has_image{};
    bool          has_legacy_params{};
    bool          has_animation{};
    bool          migrated{};
    std::uint32_t chunk_count{};
};

Summary read_summary(const std::string& path);

// FNV-1a, used for both the per-chunk checksum and the whole-content
// hash. Not cryptographic -- it detects truncation and bit rot, which is
// all that is claimed for it.
std::uint32_t fnv1a32(std::span<const std::byte> bytes, std::uint32_t seed = 0x811C9DC5u);
std::uint64_t fnv1a64(std::span<const std::byte> bytes, std::uint64_t seed = 0xCBF29CE484222325ull);

}  // namespace mb3d::m4d
