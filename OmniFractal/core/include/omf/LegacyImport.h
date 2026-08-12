// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// LegacyImport.h -- read-only importers for the Mandelbulb 3D file suite.
//
// These formats are read for interoperability. Nothing here writes them,
// and there is no code path that will: omf_* export functions return
// OMF_ERR_WRITE_FORBIDDEN for every legacy kind. The native container
// is .m4d.
//
// THREAT MODEL
// Every byte reaching this header is untrusted. Parameter files circulate
// on forums and arrive by drag-and-drop from strangers. Formula files
// (.m3f / .d3f / .dSO) contain raw machine code by design. Accordingly:
//
//   * Every read goes through ByteReader, which bounds-checks against the
//     buffer end before it moves the cursor. There is no unchecked
//     pointer arithmetic in the importers.
//   * Every length field read from a file is validated against a cap
//     before it drives an allocation. A corrupt 32-bit count cannot
//     become a multi-gigabyte reserve.
//   * Decompression is bounded twice: by an absolute output cap and by a
//     compression-ratio cap, which is what actually stops a zip bomb.
//   * Machine code is NEVER loaded, mapped, relocated, or executed. The
//     formula importer reads declarative metadata only, maps the declared
//     name against a known-formula registry, and disables the slot when
//     the name does not resolve. There is no flag to change this.
//
// PROVENANCE
// Written from an understanding of the formats' byte layout, for
// interoperability. No legacy source was translated. File formats are not
// copyrightable subject matter; see LICENSE section 10 and
// THIRD-PARTY-NOTICES.md.

#pragma once

#include "omf/Internal.hpp"

#include <optional>

namespace omf::legacy {

// ---------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------

// Deliberately generous relative to real files, tight relative to
// address space. A legitimate .m3p is a few kilobytes.
struct Limits {
    static constexpr std::size_t   kMaxFileBytes        = 4ull << 30;   // 4 GiB
    static constexpr std::size_t   kMaxParameterBytes   = 16u << 20;    // 16 MiB
    static constexpr std::size_t   kMaxDecompressed     = 256u << 20;   // 256 MiB
    // Ratio cap. 1000:1 is far above anything a parameter block achieves
    // and far below what a bomb needs.
    static constexpr std::size_t   kMaxCompressionRatio = 1000;
    static constexpr std::uint32_t kMaxImageExtent      = 1u << 20;
    static constexpr std::uint32_t kMaxLayers           = 64;
    static constexpr std::uint32_t kMaxKeyframes        = 1u << 20;
    static constexpr std::uint32_t kMaxRecords          = 1u << 20;
    static constexpr std::size_t   kMaxStringBytes      = 4096;
};

// ---------------------------------------------------------------------
// Bounds-checked reader
// ---------------------------------------------------------------------

// Every legacy parse goes through this. It is the single place where a
// buffer overrun could occur, which is what makes it auditable.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : data_(data) {}

    std::size_t position() const { return cursor_; }
    std::size_t remaining() const { return data_.size() - cursor_; }
    bool        exhausted() const { return cursor_ >= data_.size(); }

    void seek(std::size_t offset) {
        require(offset <= data_.size(), OMF_ERR_CORRUPT,
                "seek past end of buffer (" + std::to_string(offset) + " > " +
                std::to_string(data_.size()) + ")");
        cursor_ = offset;
    }

    void skip(std::size_t count) {
        require(count <= remaining(), OMF_ERR_CORRUPT, "skip past end of buffer");
        cursor_ += count;
    }

    // Little-endian fixed-width reads. Legacy files were written by an
    // x86 application, so LE is not an assumption but a fact of the
    // format.
    std::uint8_t  u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int32_t  i32();
    float         f32();
    double        f64();

    std::span<const std::byte> bytes(std::size_t count);

    // Length-prefixed string, capped. Non-UTF-8 bytes are replaced
    // rather than rejected -- legacy files carry Windows-1252 text.
    std::string prefixed_string(std::size_t length_bytes);
    std::string fixed_string(std::size_t count);

    // Reads `count` values into `out`, validating count against `cap`
    // BEFORE allocating. This is the pattern that keeps a corrupt count
    // field from becoming an OOM.
    template <typename T, typename Fn>
    std::vector<T> counted(std::size_t count, std::size_t cap, const char* what, Fn&& read_one) {
        require(count <= cap, OMF_ERR_LIMIT_EXCEEDED,
                std::string(what) + " count " + std::to_string(count) +
                " exceeds the limit of " + std::to_string(cap));
        std::vector<T> values;
        values.reserve(std::min<std::size_t>(count, 4096));
        for (std::size_t i = 0; i < count; ++i) values.push_back(read_one(*this));
        return values;
    }

private:
    void need(std::size_t count) const {
        require(count <= remaining(), OMF_ERR_CORRUPT,
                "read of " + std::to_string(count) + " bytes overruns the buffer (" +
                std::to_string(remaining()) + " remaining)");
    }

    std::span<const std::byte> data_;
    std::size_t cursor_{0};
};

// ---------------------------------------------------------------------
// Decompression
// ---------------------------------------------------------------------

// Raw-DEFLATE or zlib inflate with both an absolute and a ratio cap.
// `expected_size` is a hint from the container; 0 means unknown, in
// which case growth is bounded solely by the caps.
std::vector<std::byte> inflate(std::span<const std::byte> compressed,
                               std::size_t expected_size);

// ---------------------------------------------------------------------
// Parsed results
// ---------------------------------------------------------------------

struct ImportResult {
    omf_render_settings_t settings{};
    std::vector<omf_keyframe_t> keyframes;
    omf_provenance_t provenance{};
    omf_import_report_t report{};
    // Present for .m3i.
    std::unique_ptr<FrameBuffer> image;
};

// Detected originating version, encoded major*10000 + minor*100 + patch.
struct VersionMarker {
    std::int32_t encoded{0};
    std::string  text;
};

// ---------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------

omf_file_kind_t identify(const std::string& path);
omf_file_kind_t identify_bytes(std::span<const std::byte> head,
                                std::string_view extension);

// Reads a whole file, capped at Limits::kMaxFileBytes.
std::vector<std::byte> read_file(const std::string& path);

// `profile_id` selects the evaluation semantics applied to the imported
// parameters. Passing an unknown id is an error, not a fallback.
ImportResult import_any(std::span<const std::byte> data, omf_file_kind_t kind,
                        std::string_view profile_id, std::string_view source_name);

ImportResult import_m3p(std::span<const std::byte> data, std::string_view profile_id);
ImportResult import_m3i(std::span<const std::byte> data, std::string_view profile_id);
ImportResult import_m3a(std::span<const std::byte> data, std::string_view profile_id);
ImportResult import_m3l(std::span<const std::byte> data, std::string_view profile_id);

// Pulls the embedded parameter block out of an .m3i without decoding any
// pixel data. Two seeks and one bounded read.
std::vector<std::byte> extract_embedded_parameters(std::span<const std::byte> data);

// ---------------------------------------------------------------------
// Formula files -- metadata only
// ---------------------------------------------------------------------

struct FormulaMetadata {
    std::string name;
    std::string display_name;
    omf_formula_class_t formula_class{OMF_CLASS_3D};
    bool analytic_de{false};
    int  parameter_count{0};
    std::array<std::string, OMF_FORMULA_PARAMS> parameter_names{};
    std::array<float, OMF_FORMULA_PARAMS> parameter_defaults{};
    // True when the file carries a machine-code body. Recorded so the UI
    // can say why the slot was not enabled. The body is never touched.
    bool has_executable_body{false};
    std::size_t executable_body_offset{0};
    std::size_t executable_body_size{0};
};

// Parses declarative metadata. Never loads or executes the code body.
FormulaMetadata parse_formula_metadata(std::span<const std::byte> data,
                                       omf_file_kind_t kind);

// Maps a legacy formula name onto a built-in implementation. Returns
// nullopt when the name does not resolve -- the caller disables the slot
// rather than substituting something that looks vaguely similar, because
// a wrong formula renders a confidently incorrect image.
std::optional<std::string> resolve_formula_name(std::string_view legacy_name);

}  // namespace omf::legacy
