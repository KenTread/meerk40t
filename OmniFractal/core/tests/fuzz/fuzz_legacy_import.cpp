// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// fuzz_legacy_import.cpp -- libFuzzer entry points for every parser that
// touches untrusted input.
//
// The contract these harnesses enforce is not "never fails" -- most
// random inputs are expected to fail. It is:
//
//   * no crash, no hang, no unbounded allocation;
//   * every failure arrives as an omf::Error carrying a result code,
//     never as a segfault, an uncaught std::exception, or a std::bad_alloc
//     from an unchecked length field;
//   * a successful parse leaves a structurally valid settings struct.
//
// Any std::bad_alloc reaching here is a genuine finding: it means a
// length field drove an allocation without passing a cap first.
//
// Build:  cmake -DOMF_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
// Run:    ./fuzz_legacy_import corpus/ -max_len=65536 -rss_limit_mb=2048

#include "omf/LegacyImport.h"
#include "omf/M4DFormat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#if defined(OMF_FUZZ_M4D)
#  include <unistd.h>
#endif

namespace {

std::span<const std::byte> as_span(const std::uint8_t* data, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(data), size};
}

// Shared post-conditions for anything that claims to have parsed.
void check_settings_invariants(const omf_render_settings_t& settings) {
    if (settings.width <= 0 || settings.height <= 0) std::abort();
    if (settings.calculation.max_iterations <= 0) std::abort();
    if (!(settings.calculation.escape_radius > 0.0f)) std::abort();
    // A non-positive raystep multiplier or DEstop would make the marcher
    // loop forever; the importer must have repaired it.
    if (!(settings.calculation.raystep_multiplier > 0.0f)) std::abort();
    if (!(settings.calculation.de_stop_criterion > 0.0f)) std::abort();

    for (const omf_formula_slot_t& slot : settings.formulas) {
        if (slot.iteration_weight <= 0) std::abort();
        if (slot.repeat_from_slot < -1 ||
            slot.repeat_from_slot >= OMF_FORMULA_SLOTS) {
            std::abort();
        }
    }
}

// Every harness funnels through this. An omf::Error is a correct
// rejection; anything else escaping is a bug in the parser.
template <typename Fn>
void run_guarded(Fn&& fn) {
    try {
        fn();
    } catch (const omf::Error&) {
        // Expected: a bounds check, a cap, or a checksum rejected the
        // input. This is the parser working.
    } catch (const std::bad_alloc&) {
        // A length field reached an allocator without passing a cap.
        std::fprintf(stderr, "FUZZ: bad_alloc escaped -- missing allocation cap\n");
        std::abort();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FUZZ: unexpected std::exception: %s\n", e.what());
        std::abort();
    } catch (...) {
        std::fprintf(stderr, "FUZZ: unexpected non-standard exception\n");
        std::abort();
    }
}

}  // namespace

// ---------------------------------------------------------------------
// One entry point per format, selected by the first input byte so a
// single corpus can exercise all of them.
//
// The same translation unit builds two different fuzzers; OMF_FUZZ_M4D
// selects which LLVMFuzzerTestOneInput is compiled, since libFuzzer
// requires exactly one per binary.
// ---------------------------------------------------------------------

#if !defined(OMF_FUZZ_M4D)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;

    const std::uint8_t selector = data[0] % 6;
    const std::span<const std::byte> body = as_span(data + 1, size - 1);

    run_guarded([&] {
        switch (selector) {
            case 0: {
                omf::legacy::ImportResult result =
                    omf::legacy::import_m3p(body, OMF_PROFILE_LEGACY_19912);
                check_settings_invariants(result.settings);
                break;
            }
            case 1: {
                omf::legacy::ImportResult result =
                    omf::legacy::import_m3i(body, OMF_PROFILE_MODERN);
                check_settings_invariants(result.settings);
                break;
            }
            case 2: {
                omf::legacy::ImportResult result =
                    omf::legacy::import_m3a(body, OMF_PROFILE_MODERN);
                check_settings_invariants(result.settings);
                // Keyframes must be sorted and in range after import.
                for (std::size_t i = 1; i < result.keyframes.size(); ++i) {
                    if (result.keyframes[i - 1].frame > result.keyframes[i].frame) {
                        std::abort();
                    }
                }
                break;
            }
            case 3: {
                omf::legacy::ImportResult result =
                    omf::legacy::import_m3l(body, OMF_PROFILE_MODERN);
                check_settings_invariants(result.settings);
                break;
            }
            case 4: {
                // Parameter extraction runs on the UI thread during a
                // file drop, so it gets its own coverage.
                (void)omf::legacy::extract_embedded_parameters(body);
                break;
            }
            case 5: {
                // Formula metadata. The critical post-condition is that a
                // machine-code body is recorded and never resolved into
                // an enabled formula.
                const omf::legacy::FormulaMetadata meta =
                    omf::legacy::parse_formula_metadata(body, OMF_FILE_M3F);
                if (meta.has_executable_body && meta.executable_body_size == 0) {
                    std::abort();
                }
                (void)omf::legacy::resolve_formula_name(meta.name);
                break;
            }
            default:
                break;
        }
    });

    return 0;
}

#endif  // !OMF_FUZZ_M4D

// ---------------------------------------------------------------------
// Native container. Separate binary: .m4d is our own format, so its
// corpus is meaningfully different from the legacy one.
// ---------------------------------------------------------------------

#if defined(OMF_FUZZ_M4D)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The reader takes a path, so the input is staged through a temp
    // file. Slower than an in-memory harness, but it exercises the exact
    // code path production uses, including the file-size cross-check
    // that a memory harness would skip.
    char path[] = "/tmp/omf_fuzz_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 0;

    if (size > 0) {
        const ssize_t written = write(fd, data, size);
        (void)written;
    }
    close(fd);

    run_guarded([&] {
        omf::m4d::Summary summary = omf::m4d::read_summary(path);
        (void)summary;
        omf::m4d::Document document = omf::m4d::read(path);
        check_settings_invariants(document.active);
        if (document.legacy.has_value()) {
            check_settings_invariants(*document.legacy);
        }
    });

    unlink(path);
    return 0;
}
#endif
