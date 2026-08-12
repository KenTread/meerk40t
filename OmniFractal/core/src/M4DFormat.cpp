// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// M4DFormat.cpp -- reader and writer for the native .m4d container.
//
// The reader treats its input as hostile even though .m4d is our own
// format: a file can be truncated by a full disk, corrupted in transit,
// or hand-edited. Every offset is validated against the actual file size
// before a seek, every declared size is checked against a cap before an
// allocation, and every payload is checksummed after the read.

#include "omf/M4DFormat.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace omf::m4d {
namespace {

// Caps. These exist so a corrupt 8-byte size field cannot turn into a
// terabyte allocation. They are far above any legitimate value.
constexpr std::uint64_t kMaxChunkBytes   = 64ull << 30;   // 64 GiB
constexpr std::uint32_t kMaxChunkCount   = 4096;
constexpr std::uint64_t kMaxKeyframes    = 1u << 20;
constexpr std::uint64_t kMaxTextBytes    = 1u << 20;
constexpr std::uint32_t kMaxImageExtent  = 1u << 20;      // 1M px per side

template <typename T>
void write_pod(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

std::span<const std::byte> as_bytes(const void* data, std::size_t size) {
    return {static_cast<const std::byte*>(data), size};
}

omf_pixel_format_t format_for_layer(omf_layer_t layer) {
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

// A chunk staged for writing. Payloads are built in memory first so the
// directory can record exact sizes and checksums.
struct PendingChunk {
    std::uint32_t type{};
    std::uint32_t index{};
    std::vector<std::byte> bytes;
};

template <typename T>
PendingChunk pod_chunk(std::uint32_t type, const T& value, std::uint32_t index = 0) {
    static_assert(std::is_trivially_copyable_v<T>);
    PendingChunk chunk;
    chunk.type = type;
    chunk.index = index;
    chunk.bytes.resize(sizeof(T));
    std::memcpy(chunk.bytes.data(), &value, sizeof(T));
    return chunk;
}

PendingChunk text_chunk(std::uint32_t type, std::string_view text) {
    PendingChunk chunk;
    chunk.type = type;
    chunk.bytes.resize(text.size());
    std::memcpy(chunk.bytes.data(), text.data(), text.size());
    return chunk;
}

// Header prefixed to each CHUNK_IMAGE_LAYER payload.
#pragma pack(push, 1)
struct LayerChunkHeader {
    std::uint32_t layer;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(LayerChunkHeader) == 24, "LayerChunkHeader layout changed");

}  // namespace

// ---------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------

std::uint32_t fnv1a32(std::span<const std::byte> bytes, std::uint32_t seed) {
    std::uint32_t hash = seed;
    for (std::byte b : bytes) {
        hash ^= static_cast<std::uint32_t>(b);
        hash *= 0x01000193u;
    }
    return hash;
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

void write(const std::string& path, const Document& document) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), OMF_ERR_IO, "cannot open '" + path + "' for writing");

    std::vector<PendingChunk> chunks;

    chunks.push_back(pod_chunk(CHUNK_SETTINGS_ACTIVE, document.active));
    chunks.push_back(pod_chunk(CHUNK_PROFILE, document.active.profile));
    chunks.push_back(pod_chunk(CHUNK_PROVENANCE, document.provenance));

    // The legacy set is written verbatim. Migration produces `active`
    // and must never rewrite this, or the switch stops being reversible.
    if (document.legacy.has_value()) {
        chunks.push_back(pod_chunk(CHUNK_SETTINGS_LEGACY, *document.legacy));
    }

    if (!document.keyframes.empty()) {
        require(document.keyframes.size() <= kMaxKeyframes, OMF_ERR_LIMIT_EXCEEDED,
                "too many keyframes to serialise");
        PendingChunk chunk;
        chunk.type = CHUNK_KEYFRAMES;
        chunk.bytes.resize(document.keyframes.size() * sizeof(omf_keyframe_t));
        std::memcpy(chunk.bytes.data(), document.keyframes.data(), chunk.bytes.size());
        chunks.push_back(std::move(chunk));
    }

    if (!document.name.empty()) {
        chunks.push_back(text_chunk(CHUNK_SCENE_NAME, document.name));
    }
    if (!document.notes.empty()) {
        chunks.push_back(text_chunk(CHUNK_NOTES, document.notes));
    }

    // Gradient rides along separately from the settings blob so a host
    // can swap a palette without rewriting the whole scene chunk.
    {
        PendingChunk chunk;
        chunk.type = CHUNK_GRADIENT;
        chunk.bytes.resize(sizeof(document.active.coloring.gradient));
        std::memcpy(chunk.bytes.data(), document.active.coloring.gradient,
                    chunk.bytes.size());
        chunks.push_back(std::move(chunk));
    }

    std::uint32_t width = 0, height = 0;
    if (document.image) {
        width = static_cast<std::uint32_t>(document.image->width());
        height = static_cast<std::uint32_t>(document.image->height());

        constexpr omf_layer_t kOrder[] = {
            OMF_LAYER_RGBA, OMF_LAYER_RGBA32F, OMF_LAYER_DEPTH, OMF_LAYER_NORMAL,
            OMF_LAYER_SSAO, OMF_LAYER_SHADOW, OMF_LAYER_MOTION, OMF_LAYER_OBJECT};

        std::uint32_t layer_index = 0;
        for (omf_layer_t which : kOrder) {
            const Layer* layer = document.image->layer(which);
            if (layer == nullptr) continue;

            LayerChunkHeader header{};
            header.layer  = static_cast<std::uint32_t>(which);
            header.format = static_cast<std::uint32_t>(layer->format);
            header.width  = static_cast<std::uint32_t>(layer->width);
            header.height = static_cast<std::uint32_t>(layer->height);
            header.stride = static_cast<std::uint32_t>(layer->stride);

            PendingChunk chunk;
            chunk.type = CHUNK_IMAGE_LAYER;
            chunk.index = layer_index++;
            chunk.bytes.resize(sizeof(header) + layer->bytes.size());
            std::memcpy(chunk.bytes.data(), &header, sizeof(header));
            std::memcpy(chunk.bytes.data() + sizeof(header), layer->bytes.data(),
                        layer->bytes.size());
            chunks.push_back(std::move(chunk));
        }
    }

    // Preserve chunks written by a newer build.
    for (const OpaqueChunk& unknown : document.unknown_chunks) {
        PendingChunk chunk;
        chunk.type = unknown.type;
        chunk.index = unknown.index;
        chunk.bytes = unknown.bytes;
        chunks.push_back(std::move(chunk));
    }

    require(chunks.size() <= kMaxChunkCount, OMF_ERR_LIMIT_EXCEEDED,
            "chunk count exceeds the container limit");

    M4dHeader header{};
    std::memcpy(header.magic, "MB4D", 4);
    header.format_version = OMF_M4D_FORMAT_VERSION;
    header.writer_abi = OMF_ABI_VERSION;
    header.header_size = sizeof(M4dHeader);
    header.chunk_count = static_cast<std::uint32_t>(chunks.size());
    header.image_width = width;
    header.image_height = height;
    header.flags = 0;
    if (document.image) header.flags |= kFlagHasImage;
    if (document.legacy.has_value()) header.flags |= kFlagHasLegacyParams;
    if (!document.keyframes.empty()) header.flags |= kFlagHasAnimation;
    if (document.provenance.migrated) header.flags |= kFlagMigrated;

    // Reserve the header; it is rewritten once offsets are known.
    write_pod(file, header);

    std::vector<M4dChunkEntry> directory;
    directory.reserve(chunks.size());
    std::uint64_t content_hash = 0xCBF29CE484222325ull;

    for (const PendingChunk& chunk : chunks) {
        M4dChunkEntry entry{};
        entry.type = chunk.type;
        entry.index = chunk.index;
        entry.offset = static_cast<std::uint64_t>(file.tellp());
        entry.size_stored = chunk.bytes.size();
        entry.size_raw = chunk.bytes.size();
        entry.compression = 0;
        entry.checksum = fnv1a32(chunk.bytes);

        content_hash = fnv1a64(chunk.bytes, content_hash);

        file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                   static_cast<std::streamsize>(chunk.bytes.size()));
        require(file.good(), OMF_ERR_IO, "failed writing chunk payload");
        directory.push_back(entry);
    }

    header.directory_offset = static_cast<std::uint64_t>(file.tellp());
    for (const M4dChunkEntry& entry : directory) write_pod(file, entry);

    header.file_size = static_cast<std::uint64_t>(file.tellp());
    header.content_hash = content_hash;

    file.seekp(0);
    write_pod(file, header);
    file.flush();
    require(file.good(), OMF_ERR_IO, "failed finalising '" + path + "'");
}

// ---------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------

namespace {

struct ReadContext {
    std::ifstream file;
    std::uint64_t file_size{};
    M4dHeader     header{};
    std::string   path;
};

ReadContext open_and_read_header(const std::string& path) {
    ReadContext ctx;
    ctx.path = path;
    ctx.file.open(path, std::ios::binary | std::ios::ate);
    require(ctx.file.good(), OMF_ERR_IO, "cannot open '" + path + "'");

    ctx.file_size = static_cast<std::uint64_t>(ctx.file.tellg());
    require(ctx.file_size >= sizeof(M4dHeader), OMF_ERR_CORRUPT,
            "'" + path + "' is too small to be a .m4d file");

    ctx.file.seekg(0);
    ctx.file.read(reinterpret_cast<char*>(&ctx.header), sizeof(M4dHeader));
    require(ctx.file.good(), OMF_ERR_CORRUPT, "truncated .m4d header");

    require(std::memcmp(ctx.header.magic, "MB4D", 4) == 0, OMF_ERR_PARSE,
            "'" + path + "' is not a .m4d container");

    // Exact match, not a range test. A file from a newer build may have
    // reinterpreted a field we would silently misread.
    require(ctx.header.format_version == OMF_M4D_FORMAT_VERSION,
            OMF_ERR_FORMAT_VERSION,
            "'" + path + "' uses .m4d format version " +
            std::to_string(ctx.header.format_version) + "; this build reads version " +
            std::to_string(OMF_M4D_FORMAT_VERSION) + " only");

    require(ctx.header.header_size >= sizeof(M4dHeader), OMF_ERR_CORRUPT,
            "header_size is smaller than the header itself");
    require(ctx.header.chunk_count <= kMaxChunkCount, OMF_ERR_LIMIT_EXCEEDED,
            "chunk count exceeds the container limit");

    // A file that has been truncated after writing still has a valid
    // header; this is what catches it.
    if (ctx.header.file_size != 0) {
        require(ctx.header.file_size <= ctx.file_size, OMF_ERR_CORRUPT,
                "'" + path + "' is truncated: header declares " +
                std::to_string(ctx.header.file_size) + " bytes, found " +
                std::to_string(ctx.file_size));
    }
    return ctx;
}

std::vector<M4dChunkEntry> read_directory(ReadContext& ctx) {
    const std::uint64_t needed =
        static_cast<std::uint64_t>(ctx.header.chunk_count) * sizeof(M4dChunkEntry);

    require(ctx.header.directory_offset <= ctx.file_size &&
            needed <= ctx.file_size - ctx.header.directory_offset,
            OMF_ERR_CORRUPT, "chunk directory lies outside the file");

    std::vector<M4dChunkEntry> directory(ctx.header.chunk_count);
    ctx.file.seekg(static_cast<std::streamoff>(ctx.header.directory_offset));
    ctx.file.read(reinterpret_cast<char*>(directory.data()),
                  static_cast<std::streamsize>(needed));
    require(ctx.file.good(), OMF_ERR_CORRUPT, "truncated chunk directory");

    // Validate every entry before reading any payload, so a bad file is
    // rejected before it can drive a single allocation.
    for (const M4dChunkEntry& entry : directory) {
        require(entry.size_stored <= kMaxChunkBytes, OMF_ERR_LIMIT_EXCEEDED,
                "chunk declares an implausible size");
        require(entry.offset <= ctx.file_size &&
                entry.size_stored <= ctx.file_size - entry.offset,
                OMF_ERR_CORRUPT, "chunk payload extends past end of file");
        require(entry.compression == 0, OMF_ERR_UNSUPPORTED,
                "compressed .m4d chunks are not supported by this build");
    }
    return directory;
}

std::vector<std::byte> read_payload(ReadContext& ctx, const M4dChunkEntry& entry) {
    std::vector<std::byte> bytes(entry.size_stored);
    if (bytes.empty()) return bytes;

    ctx.file.seekg(static_cast<std::streamoff>(entry.offset));
    ctx.file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    require(ctx.file.good(), OMF_ERR_CORRUPT, "truncated chunk payload");

    require(fnv1a32(bytes) == entry.checksum, OMF_ERR_CORRUPT,
            "chunk checksum mismatch -- the file is damaged");
    return bytes;
}

// Copies a POD out of a payload, requiring an exact size match. A
// short read here would leave half the struct uninitialised.
template <typename T>
T decode_pod(std::span<const std::byte> bytes, const char* what) {
    require(bytes.size() == sizeof(T), OMF_ERR_CORRUPT,
            std::string(what) + " chunk is " + std::to_string(bytes.size()) +
            " bytes, expected " + std::to_string(sizeof(T)));
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

}  // namespace

Summary read_summary(const std::string& path) {
    ReadContext ctx = open_and_read_header(path);

    Summary summary;
    summary.format_version = ctx.header.format_version;
    summary.writer_abi = ctx.header.writer_abi;
    summary.width = ctx.header.image_width;
    summary.height = ctx.header.image_height;
    summary.chunk_count = ctx.header.chunk_count;
    summary.has_image = (ctx.header.flags & kFlagHasImage) != 0;
    summary.has_legacy_params = (ctx.header.flags & kFlagHasLegacyParams) != 0;
    summary.has_animation = (ctx.header.flags & kFlagHasAnimation) != 0;
    summary.migrated = (ctx.header.flags & kFlagMigrated) != 0;
    return summary;
}

Document read(const std::string& path) {
    ReadContext ctx = open_and_read_header(path);
    const std::vector<M4dChunkEntry> directory = read_directory(ctx);

    Document document;
    // Start from engine defaults so a file missing a field still yields
    // a renderable scene rather than zeros.
    omf_render_settings_default(&document.active);

    bool saw_active = false;
    std::uint32_t image_width = ctx.header.image_width;
    std::uint32_t image_height = ctx.header.image_height;
    std::uint32_t layer_mask = 0;

    // Layer payloads are collected first, then assembled, because the
    // FrameBuffer needs the full mask up front.
    struct StagedLayer {
        LayerChunkHeader header{};
        std::vector<std::byte> pixels;
    };
    std::vector<StagedLayer> staged_layers;

    for (const M4dChunkEntry& entry : directory) {
        std::vector<std::byte> payload = read_payload(ctx, entry);
        const std::span<const std::byte> bytes(payload);

        switch (entry.type) {
            case CHUNK_SETTINGS_ACTIVE:
                document.active =
                    decode_pod<omf_render_settings_t>(bytes, "active settings");
                saw_active = true;
                break;

            case CHUNK_SETTINGS_LEGACY:
                document.legacy =
                    decode_pod<omf_render_settings_t>(bytes, "legacy settings");
                break;

            case CHUNK_PROVENANCE:
                document.provenance = decode_pod<omf_provenance_t>(bytes, "provenance");
                break;

            case CHUNK_PROFILE:
                // Redundant with the copy inside the settings blob, but a
                // host may want the profile without decoding everything.
                (void)decode_pod<omf_precision_profile_t>(bytes, "profile");
                break;

            case CHUNK_GRADIENT: {
                constexpr std::size_t kExpected =
                    sizeof(std::uint32_t) * OMF_GRADIENT_ENTRIES;
                require(bytes.size() == kExpected, OMF_ERR_CORRUPT,
                        "gradient chunk has the wrong size");
                std::memcpy(document.active.coloring.gradient, bytes.data(), kExpected);
                break;
            }

            case CHUNK_KEYFRAMES: {
                require(bytes.size() % sizeof(omf_keyframe_t) == 0, OMF_ERR_CORRUPT,
                        "keyframe chunk is not a whole number of keyframes");
                const std::size_t count = bytes.size() / sizeof(omf_keyframe_t);
                require(count <= kMaxKeyframes, OMF_ERR_LIMIT_EXCEEDED,
                        "keyframe count exceeds the container limit");
                document.keyframes.resize(count);
                if (count > 0) {
                    std::memcpy(document.keyframes.data(), bytes.data(), bytes.size());
                }
                break;
            }

            case CHUNK_SCENE_NAME:
            case CHUNK_NOTES: {
                require(bytes.size() <= kMaxTextBytes, OMF_ERR_LIMIT_EXCEEDED,
                        "text chunk is implausibly large");
                std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (entry.type == CHUNK_SCENE_NAME) document.name = std::move(text);
                else document.notes = std::move(text);
                break;
            }

            case CHUNK_IMAGE_LAYER: {
                require(bytes.size() >= sizeof(LayerChunkHeader), OMF_ERR_CORRUPT,
                        "image layer chunk is missing its header");

                StagedLayer staged;
                std::memcpy(&staged.header, bytes.data(), sizeof(LayerChunkHeader));

                require(staged.header.width > 0 && staged.header.height > 0 &&
                        staged.header.width <= kMaxImageExtent &&
                        staged.header.height <= kMaxImageExtent,
                        OMF_ERR_CORRUPT, "image layer declares implausible dimensions");

                // Every layer must agree with the header's dimensions;
                // a mismatched layer is how a crafted file would get a
                // short buffer past the FrameBuffer allocation.
                if (image_width == 0 || image_height == 0) {
                    image_width = staged.header.width;
                    image_height = staged.header.height;
                }
                require(staged.header.width == image_width &&
                        staged.header.height == image_height,
                        OMF_ERR_CORRUPT, "image layers disagree on dimensions");

                const std::size_t pixel_bytes = bytes.size() - sizeof(LayerChunkHeader);
                staged.pixels.assign(bytes.begin() + sizeof(LayerChunkHeader), bytes.end());
                (void)pixel_bytes;

                layer_mask |= staged.header.layer;
                staged_layers.push_back(std::move(staged));
                break;
            }

            default: {
                // Unknown chunk from a newer build: keep it byte-for-byte
                // so re-saving does not destroy it.
                OpaqueChunk unknown;
                unknown.type = entry.type;
                unknown.index = entry.index;
                unknown.bytes = std::move(payload);
                document.unknown_chunks.push_back(std::move(unknown));
                break;
            }
        }
    }

    require(saw_active, OMF_ERR_CORRUPT, "'" + path + "' has no settings chunk");

    // The settings blob was written by a build with the same ABI, but a
    // hand-edited file could still carry nonsense here.
    require(document.active.abi_version == OMF_ABI_VERSION, OMF_ERR_ABI_MISMATCH,
            "scene declares ABI " + std::to_string(document.active.abi_version) +
            ", this build is ABI " + std::to_string(OMF_ABI_VERSION));
    require(document.active.width > 0 && document.active.height > 0 &&
            document.active.width <= static_cast<int32_t>(kMaxImageExtent) &&
            document.active.height <= static_cast<int32_t>(kMaxImageExtent),
            OMF_ERR_CORRUPT, "scene declares implausible render dimensions");

    if (!staged_layers.empty()) {
        document.image = std::make_unique<FrameBuffer>(static_cast<int>(image_width),
                                                       static_cast<int>(image_height),
                                                       layer_mask);
        for (const StagedLayer& staged : staged_layers) {
            Layer* layer =
                document.image->layer(static_cast<omf_layer_t>(staged.header.layer));
            if (layer == nullptr) continue;

            // Exact size match required. A short payload would leave the
            // tail of the layer uninitialised; a long one would be a
            // silent inconsistency.
            require(staged.pixels.size() == layer->bytes.size(), OMF_ERR_CORRUPT,
                    "image layer payload does not match its declared geometry");
            std::memcpy(layer->bytes.data(), staged.pixels.data(), staged.pixels.size());
        }
    }

    return document;
}

}  // namespace omf::m4d
