/* SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary */
/* Copyright (c) 2026 KenTread. All Rights Reserved. */
/* Proprietary and confidential -- see LICENSE for terms. */
/*
 * Renderer.h -- MB3D_Core public C ABI.
 *
 * MB3D-V2 is an independent 64-bit fractal renderer. It reads the
 * Mandelbulb 3D file suite for interoperability and converts it to its
 * own .m4d container; it is not a version of, successor to, or
 * derivative of that application. See LICENSE section 10.
 *
 * This header is the single interop boundary between the C++20 engine
 * and any host: the C# .NET 9 front-end, the LuaJIT FFI layer, the CLI
 * renderer, and the distributed tile workers.
 *
 * ABI rules (do not break these without bumping MB3D_ABI_VERSION):
 *   - Plain C. No exceptions escape, no C++ types in signatures, and an
 *     explicit calling convention on every exported function.
 *   - All structs are standard-layout, explicitly sized, 8-byte aligned,
 *     and padded to a fixed size. Renderer.cpp static_asserts both the
 *     sizeof() and the offsetof() of every field that a managed mirror
 *     depends on; NativeStructs.cs asserts the same sizes from the
 *     other side.
 *   - Every function returning mb3d_result_t returns MB3D_OK (0) on
 *     success and a negative value on failure. Use mb3d_last_error()
 *     for a human-readable message (thread-local).
 *   - Buffers exposed through mb3d_image_view_t stay owned by the core
 *     and remain valid until the owning job is released. That is what
 *     makes the C# side zero-copy: it wraps the pointer rather than
 *     marshalling it.
 *
 * WHAT THIS ENGINE DOES NOT PROMISE
 *   Importing a legacy parameter file loads its parameters faithfully.
 *   It does NOT reproduce the original image bit-for-bit, and cannot:
 *   the originating application evaluated in 80-bit x87, which is a
 *   different dynamical system from any IEEE format, and in a chaotic
 *   iterated map a last-bit difference becomes a visible surface change
 *   within a few hundred iterations. Precision profiles (below) exist to
 *   reproduce the *semantics* that dominate visual difference, which is
 *   a far larger effect than rounding. Do not let host UI copy claim
 *   otherwise.
 */

#ifndef MB3D_RENDERER_H
#define MB3D_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(MB3D_CORE_BUILD)
#    define MB3D_API __declspec(dllexport)
#  else
#    define MB3D_API __declspec(dllimport)
#  endif
#  define MB3D_CALL __cdecl
#else
#  if defined(MB3D_CORE_BUILD)
#    define MB3D_API __attribute__((visibility("default")))
#  else
#    define MB3D_API
#  endif
/* x86-64 System V and AArch64 AAPCS each define exactly one C calling
 * convention, so there is nothing to select; __attribute__((cdecl)) is
 * an x86-32-only spelling and warns on 64-bit targets. The macro exists
 * so the Windows side can say __cdecl explicitly. */
#  define MB3D_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */

#define MB3D_ABI_VERSION      1
#define MB3D_VERSION_MAJOR    0
#define MB3D_VERSION_MINOR    1
#define MB3D_VERSION_PATCH    0

/* .m4d container format version. Independent of the ABI version: the
 * ABI can change without the on-disk format changing, and vice versa.
 * The reader refuses unknown values rather than guessing. */
#define MB3D_M4D_FORMAT_VERSION 1

/* Structural limits. The six formula slots and six light channels match
 * what the legacy formats can express, so an import is never lossy for
 * structural reasons. */
#define MB3D_FORMULA_SLOTS        6
#define MB3D_LIGHT_CHANNELS       6
#define MB3D_GRADIENT_ENTRIES   256
#define MB3D_FORMULA_PARAMS      12
#define MB3D_FORMULA_IPARAMS      4
#define MB3D_MAX_NAME            64
#define MB3D_MAX_PATH           260

/* ------------------------------------------------------------------ */
/* Result codes                                                        */
/* ------------------------------------------------------------------ */

typedef int32_t mb3d_result_t;

enum {
    MB3D_OK                   =   0,
    MB3D_ERR_UNKNOWN          =  -1,
    MB3D_ERR_INVALID_ARG      =  -2,
    MB3D_ERR_OUT_OF_MEMORY    =  -3,
    MB3D_ERR_IO               =  -4,
    MB3D_ERR_PARSE            =  -5,
    MB3D_ERR_UNSUPPORTED      =  -6,
    MB3D_ERR_NO_DEVICE        =  -7,   /* no Vulkan device, no fallback  */
    MB3D_ERR_SHADER_COMPILE   =  -8,
    MB3D_ERR_LUA              =  -9,
    MB3D_ERR_CANCELLED        = -10,
    MB3D_ERR_ABI_MISMATCH     = -11,
    MB3D_ERR_NOT_READY        = -12,
    MB3D_ERR_GEOMETRY         = -13,   /* mesh not manifold / empty      */
    MB3D_ERR_FORMAT_VERSION   = -14,   /* .m4d from a newer build        */
    MB3D_ERR_CORRUPT          = -15,   /* structurally invalid input     */
    MB3D_ERR_LIMIT_EXCEEDED   = -16,   /* input demanded too much memory */
    MB3D_ERR_UNTRUSTED        = -17,   /* input carries executable code  */
    MB3D_ERR_WRITE_FORBIDDEN  = -18    /* legacy formats are read-only   */
};

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */

typedef struct mb3d_context_s* mb3d_context;   /* engine + device      */
typedef struct mb3d_job_s*     mb3d_job;       /* one render in flight */
typedef struct mb3d_scene_s*   mb3d_scene;     /* a document           */
typedef struct mb3d_mesh_s*    mb3d_mesh;      /* extracted surface    */
typedef struct mb3d_lua_s*     mb3d_lua;       /* LuaJIT state         */

/* ------------------------------------------------------------------ */
/* Precision model                                                     */
/* ------------------------------------------------------------------ */

/* Arithmetic width. There is deliberately no fp128 and no 80-bit path:
 * Vulkan tops out at VK_KHR_shader_float64, and an 80-bit CPU path would
 * force a toolchain change (MSVC aliases long double to double) to
 * reproduce a rounding mode that was never the reference anyway. */
typedef enum mb3d_precision_tier_e {
    MB3D_PRECISION_FP32   = 0,  /* interactive navigator, preview      */
    MB3D_PRECISION_FP64   = 1,  /* default; all legacy profiles        */
    MB3D_PRECISION_DD     = 2   /* double-double, ~106 bits, deep zoom */
} mb3d_precision_tier_t;

/* How the distance estimate is derived. */
typedef enum mb3d_de_method_e {
    MB3D_DE_AUTO           = 0,  /* analytic where every slot supports it */
    MB3D_DE_ANALYTIC       = 1,  /* force running-derivative estimate     */
    MB3D_DE_NUMERIC_4POINT = 2   /* force the 4-point probe              */
} mb3d_de_method_t;

/* Bailout comparison semantics. These are not equivalent in floating
 * point at the boundary, and the difference is visible on thin
 * filaments, so the profile pins it rather than leaving it to the
 * compiler. */
typedef enum mb3d_bailout_compare_e {
    MB3D_BAILOUT_R2_GT   = 0,   /* dot(z,z) >  R*R                     */
    MB3D_BAILOUT_R2_GE   = 1,   /* dot(z,z) >= R*R                     */
    MB3D_BAILOUT_R_GT    = 2    /* length(z) >  R  (extra rounding)    */
} mb3d_bailout_compare_t;

/*
 * A named, versioned bundle of evaluation semantics.
 *
 * Rationale: the artefacts users actually depend on come overwhelmingly
 * from evaluation semantics, not from rounding. Ranked by contribution
 * to visual difference:
 *   1. DE tightness and method selection -- mixing an analytic formula
 *      with a non-analytic one downgrades the whole chain to the
 *      numeric 4-point DE. Dominant source of overstepping.
 *   2. Sampling: raystep multiplier, stepwidth limiter, first-step
 *      random, raystep-sub-DEstop.
 *   3. Iteration cap truncation.
 *   4. Arithmetic precision -- a distant fourth, until deep zoom.
 * Locking 1-3 and running plain fp64 on the GPU reproduces the look at
 * full speed. That is what this struct does.
 *
 * Profiles are versioned per legacy release because rendering behaviour
 * changed between them; a single "legacy" mode guarantees a permanent
 * stream of "doesn't match, I made this in 1.99.35" reports.
 */
typedef struct mb3d_precision_profile_s {
    char    id[MB3D_MAX_NAME];       /* "legacy-1.99.12", "modern", ... */

    int32_t tier;                    /* mb3d_precision_tier_t           */
    int32_t de_method;               /* mb3d_de_method_t                */

    /* 1 => a single non-analytic slot downgrades the entire chain to
     * the numeric DE, as the legacy evaluator did. 0 => each slot
     * contributes its own best estimate. This one switch accounts for
     * more visual difference than every other field here combined. */
    int32_t downgrade_chain_on_mixed_de;

    /* 4-point probe epsilon, relative to hit distance. */
    float   probe_epsilon;

    int32_t bailout_compare;         /* mb3d_bailout_compare_t          */

    /* 0 => weighted round-robin over enabled slots.
     * 1 => strict legacy slot order, honouring repeat-from-here. */
    int32_t slot_order_policy;

    /* Arithmetic variant used by Interpolate and DEcombinate. Legacy
     * releases differed here; see docs/PRECISION.md for the table. */
    int32_t hybrid_arithmetic;

    /* Identifies the RNG stream feeding first-step-random. Reproducing
     * the sequence matters because the jitter is baked into the grain of
     * every legacy render. */
    int32_t rng_stream;

    /* When set, the migration pass refuses to touch sampling parameters
     * / the iteration cap for scenes using this profile. */
    int32_t lock_sampling;
    int32_t lock_iteration_cap;

    /* Pixel width (in radians of FOV per pixel) that DEstop was defined
     * against. Needed to rescale DEstop when re-rendering at a different
     * resolution -- see mb3d_migrate_parameters. */
    float   destop_pixel_reference;

    int32_t _pad0;
} mb3d_precision_profile_t;

/* Built-in profile identifiers accepted by mb3d_precision_profile_by_id. */
#define MB3D_PROFILE_MODERN         "modern"
#define MB3D_PROFILE_LEGACY_19912   "legacy-1.99.12"
#define MB3D_PROFILE_LEGACY_19935   "legacy-1.99.35"

/* ------------------------------------------------------------------ */
/* Provenance                                                          */
/* ------------------------------------------------------------------ */

/* Recorded into every .m4d written from an import, so a scene always
 * knows where it came from and under which semantics it was read. */
typedef struct mb3d_provenance_s {
    int32_t source_format;                    /* mb3d_file_kind_t       */
    /* Detected originating application version encoded as
     * major*10000 + minor*100 + patch, e.g. 1.99.12 -> 19912. Zero when
     * the file carried no usable version marker. */
    int32_t source_version;
    char    source_version_text[MB3D_MAX_NAME];
    char    source_filename[MB3D_MAX_PATH];
    char    profile_id[MB3D_MAX_NAME];        /* profile used on import */
    int64_t import_unix_time;
    int32_t importer_abi_version;
    /* 1 when modern parameters were derived from the legacy ones. The
     * legacy values are still present -- migration is reversible. */
    int32_t migrated;
    int32_t migration_target_width;
    int32_t migration_target_height;
    /* Count of fields the reader could not interpret. Non-zero means the
     * import was lossy and the UI should say so. */
    int32_t unmapped_field_count;
    int32_t _pad0;
} mb3d_provenance_t;

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

typedef enum mb3d_backend_e {
    MB3D_BACKEND_AUTO   = 0,
    MB3D_BACKEND_VULKAN = 1,
    MB3D_BACKEND_CPU    = 2
} mb3d_backend_t;

/* How slots combine per iteration. */
typedef enum mb3d_hybrid_mode_e {
    MB3D_HYBRID_OFF          = 0,
    MB3D_HYBRID_SEQUENTIAL   = 1,  /* slot order, weight = repeat count */
    MB3D_HYBRID_ALTERNATE    = 2,
    MB3D_HYBRID_INTERPOLATE  = 3,
    MB3D_HYBRID_DECOMBINATE  = 4   /* see mb3d_decomb_op_t              */
} mb3d_hybrid_mode_t;

/* DEcombinate sub-operation: how two slots' distance estimates merge. */
typedef enum mb3d_decomb_op_e {
    MB3D_DECOMB_MIN      = 0,
    MB3D_DECOMB_MAX      = 1,
    MB3D_DECOMB_INV_MAX  = 2,
    MB3D_DECOMB_MIN_LIN  = 3,   /* linear blend toward the minimum      */
    MB3D_DECOMB_MIN_NLIN = 4,   /* non-linear (smooth-min) blend        */
    MB3D_DECOMB_MIX1     = 5,
    MB3D_DECOMB_MIX2     = 6
} mb3d_decomb_op_t;

/* Formula dimensional class. The 'a' variants carry an extra alternate
 * component that participates in the fold but not in the bailout. */
typedef enum mb3d_formula_class_e {
    MB3D_CLASS_3D  = 0,
    MB3D_CLASS_3DA = 1,
    MB3D_CLASS_4D  = 2,
    MB3D_CLASS_4DA = 3
} mb3d_formula_class_t;

typedef enum mb3d_formula_kind_e {
    MB3D_FORMULA_NONE       = 0,
    MB3D_FORMULA_BULB       = 1,   /* power-N bulb                      */
    MB3D_FORMULA_BOX        = 2,   /* Mandelbox / Amazing Box / Tglad   */
    MB3D_FORMULA_FOLD       = 3,   /* kaleidoscopic / Menger folds      */
    MB3D_FORMULA_TRANSFORM  = 4,   /* rotate, invert, tile, repeat      */
    MB3D_FORMULA_IFS        = 5,
    MB3D_FORMULA_QUATERNION = 6,
    MB3D_FORMULA_IMPORTED   = 7    /* mapped from a legacy formula name */
} mb3d_formula_kind_t;

typedef enum mb3d_trap_kind_e {
    MB3D_TRAP_NONE   = 0,
    MB3D_TRAP_POINT  = 1,
    MB3D_TRAP_LINE   = 2,
    MB3D_TRAP_CROSS  = 3,
    MB3D_TRAP_BOX    = 4,
    MB3D_TRAP_PLANE  = 5,
    MB3D_TRAP_SPHERE = 6
} mb3d_trap_kind_t;

/* Where the colouring index comes from. */
typedef enum mb3d_color_source_e {
    MB3D_COLOR_ITERATION      = 0,
    MB3D_COLOR_ORBIT_TRAP     = 1,
    MB3D_COLOR_LAST_LENGTH_INC= 2,  /* last-length-increase            */
    MB3D_COLOR_ROUT_ANGLE     = 3,  /* Rout angles                     */
    MB3D_COLOR_INPUT_VECTOR   = 4,  /* map on input vector             */
    MB3D_COLOR_OUTPUT_VECTOR  = 5,  /* map on output vector            */
    MB3D_COLOR_DEPTH          = 6
} mb3d_color_source_t;

typedef enum mb3d_light_kind_e {
    MB3D_LIGHT_OFF         = 0,
    MB3D_LIGHT_POSITIONAL  = 1,
    MB3D_LIGHT_DIRECTIONAL = 2,
    MB3D_LIGHT_MAP         = 3
} mb3d_light_kind_t;

typedef enum mb3d_stereo_mode_e {
    MB3D_STEREO_OFF       = 0,
    MB3D_STEREO_ANAGLYPH  = 1,
    MB3D_STEREO_SIDE      = 2,
    MB3D_STEREO_CROSSEYE  = 3,
    MB3D_STEREO_OVERUNDER = 4
} mb3d_stereo_mode_t;

typedef enum mb3d_layer_e {
    MB3D_LAYER_RGBA    = 1u << 0,   /* 8-bit BGRA, display ready       */
    MB3D_LAYER_RGBA32F = 1u << 1,   /* linear float                    */
    MB3D_LAYER_DEPTH   = 1u << 2,
    MB3D_LAYER_NORMAL  = 1u << 3,
    MB3D_LAYER_SSAO    = 1u << 4,
    MB3D_LAYER_SHADOW  = 1u << 5,
    MB3D_LAYER_MOTION  = 1u << 6,
    MB3D_LAYER_OBJECT  = 1u << 7    /* which slot produced the hit     */
} mb3d_layer_t;

typedef enum mb3d_pixel_format_e {
    MB3D_PF_BGRA8   = 0,
    MB3D_PF_R32F    = 1,
    MB3D_PF_RG32F   = 2,
    MB3D_PF_RGB32F  = 3,
    MB3D_PF_RGBA32F = 4,
    MB3D_PF_U32     = 5
} mb3d_pixel_format_t;

typedef enum mb3d_job_state_e {
    MB3D_JOB_IDLE      = 0,
    MB3D_JOB_RUNNING   = 1,
    MB3D_JOB_DONE      = 2,
    MB3D_JOB_FAILED    = 3,
    MB3D_JOB_CANCELLED = 4
} mb3d_job_state_t;

/* File kinds. Only M4D is written; everything else is import-only. */
typedef enum mb3d_file_kind_e {
    MB3D_FILE_UNKNOWN = 0,
    MB3D_FILE_M4D     = 1,   /* native container -- read AND write     */
    MB3D_FILE_M3P     = 2,   /* compressed parameter preset            */
    MB3D_FILE_M3I     = 3,   /* multi-layer image + embedded .m3p      */
    MB3D_FILE_M3A     = 4,   /* animation timeline                     */
    MB3D_FILE_M3L     = 5,   /* compressed lighting preset             */
    MB3D_FILE_M3C     = 6,   /* Monte Carlo renderer state             */
    MB3D_FILE_M3V     = 7,   /* voxel stack                            */
    MB3D_FILE_M3F     = 8,   /* formula metadata + machine-code body   */
    MB3D_FILE_D3F     = 9,
    MB3D_FILE_DSO     = 10
} mb3d_file_kind_t;

/* ------------------------------------------------------------------ */
/* POD structs -- mirrored 1:1 in NativeStructs.cs                     */
/* ------------------------------------------------------------------ */

typedef struct mb3d_camera_transform_s {
    double  position[3];
    double  target[3];
    double  up[3];
    double  rotation[3];       /* Euler yaw/pitch/roll, degrees        */
    double  distance;
    double  zoom;
    double  fov_degrees;       /* 0 => orthographic                    */
    double  aspect;            /* 0 => derive from width/height        */
    double  near_clip;
    double  far_clip;
    double  dof_focal_plane;
    double  dof_strength;      /* 0 disables the DOF pass              */
    double  stereo_ipd;
    int32_t stereo_mode;
    int32_t projection;        /* 0 persp, 1 ortho, 2 equirect         */
} mb3d_camera_transform_t;

typedef struct mb3d_formula_slot_s {
    char    name[MB3D_MAX_NAME];

    int32_t kind;                          /* mb3d_formula_kind_t      */
    int32_t formula_class;                 /* mb3d_formula_class_t     */
    int32_t enabled;
    int32_t hybrid_mode;                   /* mb3d_hybrid_mode_t       */
    int32_t decomb_op;                     /* mb3d_decomb_op_t         */

    int32_t iteration_weight;              /* iterations before handoff */
    int32_t start_iteration;
    int32_t stop_iteration;                /* -1 => until max_iter     */
    /* Slot index the sequence jumps back to once this slot's weight is
     * spent; -1 disables. This is "repeat from here". */
    int32_t repeat_from_slot;

    /* Per-slot bailout radius; 0 falls back to the global one. */
    float   r_bailout;
    int32_t min_iterations;
    int32_t max_iterations;                /* 0 => global cap          */

    float   interpolate_factor;
    float   julia_c[4];
    int32_t julia_enabled;

    /* 1 when this slot supplies an analytic running derivative. A single
     * 0 here can downgrade the whole chain -- see the profile field
     * downgrade_chain_on_mixed_de. */
    int32_t analytic_de;

    float   params[MB3D_FORMULA_PARAMS];
    int32_t iparams[MB3D_FORMULA_IPARAMS];
    float   rotation[3];
    /* Padded so sizeof() is a multiple of 8 and the formulas[] array
     * inside render_settings keeps every following member 8-aligned. */
    float   _pad0;
    float   _pad1;
} mb3d_formula_slot_t;

typedef struct mb3d_light_channel_s {
    int32_t kind;
    int32_t enabled;
    float   color[3];
    float   intensity;
    float   position[3];
    float   falloff;
    float   specular;
    float   specular_exponent;
    float   diffuse;
    float   ambient_mix;
    char    lightmap[MB3D_MAX_NAME];   /* name only; assets are never   */
                                       /* bundled -- see LICENSE 10.3   */
    int32_t casts_shadow;
    float   shadow_softness;
    /* Volumetric contribution along the ray toward this light. */
    float   volumetric;
    float   _pad0;
} mb3d_light_channel_t;

typedef struct mb3d_lighting_settings_s {
    mb3d_light_channel_t channels[MB3D_LIGHT_CHANNELS];

    float   ambient_color_a[3];
    float   _pad0;
    float   ambient_color_b[3];
    float   _pad1;
    float   ambient_falloff;

    float   fog_color_a[3];
    float   fog_color_b[3];
    float   fog_start;
    float   fog_end;
    float   fog_density;
    /* Far fog and iteration-driven fog are separate channels in the
     * legacy model and behave differently, so they stay separate here. */
    float   far_fog_density;
    float   iteration_fog_density;

    float   gamma;
    float   contrast;
    float   brightness;
    float   saturation;
    float   exposure;

    int32_t background_mode;        /* 0 flat, 1 gradient, 2 image     */
    float   background_a[3];
    float   background_b[3];
    char    background_image[MB3D_MAX_NAME];

    int32_t ssao_enabled;
    float   ssao_radius;
    float   ssao_intensity;
    int32_t ssao_samples;
    /* Ambient shadows / DEAO: randomized 24-bit occlusion sampled
     * against the DE field rather than the depth buffer. */
    int32_t deao_enabled;
    int32_t deao_samples;
    float   deao_radius;

    int32_t hard_shadows;
    int32_t smooth_shadows;
    float   shadow_bias;

    /* Reflection and transparency. */
    float   reflectivity;
    int32_t reflection_bounces;
    float   transparency;
    float   refraction_index;
    float   _pad2[2];
} mb3d_lighting_settings_t;

typedef struct mb3d_coloring_settings_s {
    uint32_t gradient[MB3D_GRADIENT_ENTRIES];  /* 0xAARRGGBB           */

    int32_t  color_source;          /* mb3d_color_source_t             */
    int32_t  trap_kind;
    int32_t  trap_dimensions;       /* 1, 2 or 3                       */
    float    trap_center[3];
    float    trap_normal[3];
    float    trap_size;
    float    trap_influence;
    int32_t  trap_min_iteration;

    float    color_speed;
    float    color_offset;
    float    color_cycle;
    float    depth_color_mix;
    int32_t  smooth_iteration;      /* continuous iteration count      */
    /* Colouring for surfaces reached from inside the set. */
    int32_t  inside_rendering;
    uint32_t inside_color;
    float    _pad0;
} mb3d_coloring_settings_t;

typedef struct mb3d_calculation_settings_s {
    int32_t max_iterations;
    float   escape_radius;

    /* -- sampling: profile item 2, second only to DE method ---------- */
    float   raystep_multiplier;
    float   stepwidth_limiter;
    float   de_stop_criterion;
    /* Scales DEstop with the field of view so a zoom does not silently
     * change detail level. */
    int32_t vary_destop_on_fov;
    /* Randomise the first step along each ray to trade banding for
     * grain. The RNG stream is pinned by the precision profile. */
    int32_t first_step_random;
    /* Allows a raystep shorter than DEstop; off by default because it is
     * an easy way to make a render never terminate. */
    int32_t raystep_sub_destop;

    int32_t de_max_steps;
    int32_t binary_search_steps;
    int32_t smooth_normals;
    int32_t normals_on_de;          /* vs. normals from the Z-buffer   */
    float   normal_epsilon;

    int32_t bound_sphere_enabled;
    float   bound_sphere_center[3];
    float   bound_sphere_radius;
    int32_t bound_box_enabled;
    float   bound_box_min[3];
    float   bound_box_max[3];

    float   de_scale;
    int32_t _pad0;
    int32_t _pad1;
} mb3d_calculation_settings_t;

/* Tiling is not optional. RGBA32F + Z + normals + SSAO is ~36 bytes per
 * pixel before DoF or motion buffers, so a gigapixel frame is ~36 GB
 * resident. The 64-bit address space moves the wall from ~100 Mpx to
 * ~100 Gpx; it does not remove it. */
typedef struct mb3d_tiling_settings_s {
    int32_t tile_width;
    int32_t tile_height;
    /* Overlap in pixels, so post passes with a kernel radius do not seam
     * at tile boundaries. */
    int32_t tile_overlap;
    /* Cap on simultaneously resident tiles; 0 derives it from the
     * available budget below. */
    int32_t max_resident_tiles;
    /* Hard ceiling on host memory the render graph may hold, in MiB.
     * 0 => 60% of physical RAM. */
    int64_t memory_budget_mib;
    /* When set, finished tiles are streamed to a scratch file instead of
     * being held resident. Required for frames that exceed the budget. */
    int32_t spill_to_disk;
    int32_t _pad0;
} mb3d_tiling_settings_t;

typedef struct mb3d_render_settings_s {
    int32_t abi_version;            /* must equal MB3D_ABI_VERSION     */
    int32_t width;
    int32_t height;
    int32_t supersample;            /* 1..4 per axis                   */
    uint32_t layer_mask;
    int32_t backend;
    int32_t thread_count;           /* 0 => hardware_concurrency       */
    int32_t _pad0;

    /* Crop region. width/height are the full frame; a non-empty crop
     * renders only that rectangle. Drives both region re-render and the
     * distributed dispatcher. */
    int32_t crop_x, crop_y, crop_w, crop_h;

    mb3d_precision_profile_t    profile;
    mb3d_tiling_settings_t      tiling;
    mb3d_camera_transform_t     camera;
    mb3d_calculation_settings_t calculation;
    mb3d_coloring_settings_t    coloring;
    mb3d_lighting_settings_t    lighting;
    mb3d_formula_slot_t         formulas[MB3D_FORMULA_SLOTS];

    int32_t hybrid_master_mode;
    int32_t seed;
    int32_t monte_carlo_samples;    /* 0 => deterministic raymarch     */
    int32_t _pad1;
} mb3d_render_settings_t;

typedef struct mb3d_image_view_s {
    void*    data;
    size_t   size_bytes;
    int32_t  width;
    int32_t  height;
    int32_t  stride_bytes;
    int32_t  format;
    uint32_t layer;
    int32_t  _pad0;
} mb3d_image_view_t;

typedef struct mb3d_progress_s {
    int32_t state;
    float   fraction;
    int64_t pixels_done;
    int64_t pixels_total;
    double  elapsed_seconds;
    double  estimated_remaining;
    int32_t tiles_done;
    int32_t tiles_total;
    /* Live GPU occupancy, 0..1, for the profiler overlay. fp64 halves
     * register-limited occupancy, which is often a bigger real cost than
     * the issue-rate ratio -- so it is surfaced, not hidden. */
    float   gpu_occupancy;
    int32_t rays_active;
} mb3d_progress_t;

typedef struct mb3d_mesh_settings_s {
    int32_t resolution[3];
    float   bounds_min[3];
    float   bounds_max[3];
    float   iso_level;
    int32_t close_surface;
    int32_t base_clip_enabled;      /* flat bottom for 3D printing     */
    float   base_clip_height;
    int32_t weld_vertices;
    float   weld_epsilon;
    int32_t smooth_iterations;
    int32_t generate_normals;
    float   scale_to_mm;
    int32_t _pad0;
} mb3d_mesh_settings_t;

typedef struct mb3d_mesh_stats_s {
    int64_t vertex_count;
    int64_t triangle_count;
    int32_t is_manifold;
    int32_t is_watertight;
    int32_t open_edge_count;
    int32_t shell_count;
    float   bounds_min[3];
    float   bounds_max[3];
    double  volume_mm3;
    double  surface_area_mm2;
} mb3d_mesh_stats_t;

typedef struct mb3d_keyframe_s {
    int32_t frame;
    int32_t interpolation;   /* 0 step, 1 linear, 2 catmull-rom, 3 bez */
    float   tension;
    float   bias;
    float   continuity;
    mb3d_camera_transform_t camera;
    float   formula_params[MB3D_FORMULA_SLOTS][MB3D_FORMULA_PARAMS];
} mb3d_keyframe_t;

/* Result of an import, before the scene is committed to a .m4d. */
typedef struct mb3d_import_report_s {
    int32_t source_format;
    int32_t source_version;
    int32_t keyframe_count;
    int32_t layer_count;
    int32_t image_width;
    int32_t image_height;
    int32_t has_parameters;
    int32_t unmapped_field_count;
    /* Formula slots whose legacy name did not resolve to a known
     * formula. Those slots are disabled rather than guessed at. */
    int32_t unresolved_formula_count;
    /* Non-zero when the source carried an executable payload, which is
     * recorded and skipped, never run. */
    int32_t contained_executable_payload;
    int64_t bytes_read;
    int32_t _pad0;
    int32_t _pad1;
} mb3d_import_report_t;

typedef void (MB3D_CALL* mb3d_progress_fn)(const mb3d_progress_t* p, void* user);
typedef void (MB3D_CALL* mb3d_log_fn)(int32_t level, const char* msg, void* user);
typedef int32_t (MB3D_CALL* mb3d_cancel_fn)(void* user);

/* ------------------------------------------------------------------ */
/* Version and diagnostics                                             */
/* ------------------------------------------------------------------ */

/* Stable symbol every host checks before doing anything else. A
 * mismatch means the native library and the host were built from
 * different revisions; refuse to continue rather than reinterpret
 * structs. */
MB3D_API int32_t MB3D_CALL Core_GetABIVersion(void);

MB3D_API int32_t MB3D_CALL mb3d_abi_version(void);
MB3D_API int32_t MB3D_CALL mb3d_m4d_format_version(void);
MB3D_API const char* MB3D_CALL mb3d_version_string(void);
MB3D_API const char* MB3D_CALL mb3d_last_error(void);

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

MB3D_API mb3d_result_t MB3D_CALL mb3d_context_create(int32_t preferred_backend,
                                                     mb3d_context* out_ctx);
MB3D_API void MB3D_CALL mb3d_context_destroy(mb3d_context ctx);
MB3D_API mb3d_result_t MB3D_CALL mb3d_context_set_log(mb3d_context ctx,
                                                      mb3d_log_fn fn, void* user);
MB3D_API int32_t MB3D_CALL mb3d_context_backend(mb3d_context ctx);
MB3D_API const char* MB3D_CALL mb3d_context_device_name(mb3d_context ctx);
/* Which precision tiers this device supports. Bit i set => tier i is
 * usable. fp64 is absent on some mobile and older integrated parts. */
MB3D_API uint32_t MB3D_CALL mb3d_context_precision_support(mb3d_context ctx);

/* ------------------------------------------------------------------ */
/* Profiles and defaults                                               */
/* ------------------------------------------------------------------ */

MB3D_API int32_t MB3D_CALL mb3d_profile_count(void);
MB3D_API const char* MB3D_CALL mb3d_profile_id_at(int32_t index);
/* Looks up a built-in profile by id. Returns MB3D_ERR_INVALID_ARG for
 * an unknown id rather than silently substituting "modern". */
MB3D_API mb3d_result_t MB3D_CALL mb3d_precision_profile_by_id(
    const char* id, mb3d_precision_profile_t* out);

/* Fills `out` with engine defaults under the "modern" profile. Always
 * call this before mutating a settings struct so fields added in a
 * later revision get defined values. */
MB3D_API void MB3D_CALL mb3d_render_settings_default(mb3d_render_settings_t* out);

/* ------------------------------------------------------------------ */
/* Parameter migration                                                 */
/* ------------------------------------------------------------------ */

/*
 * Re-tunes legacy parameters for a target profile and resolution.
 *
 * This is not a toggle. Legacy values were tuned against legacy's flaws:
 * an artist dropped raystep multiplier to 0.1 to fight overstepping, and
 * capped iterations where detail stopped mattering at their working
 * resolution. Carried forward unchanged, the result is slower AND worse
 * -- a tight DE makes 0.1 pure wasted work, while a stale iteration cap
 * starves structure that is finally resolvable. The user toggles back
 * and concludes the upgrade is broken.
 *
 * So this function:
 *   - raises raystep multiplier in proportion to the DE tightening,
 *   - rescales DEstop against the new pixel width (DEstop is defined
 *     relative to pixel size; re-rendering a 1080p parameter set at 16K
 *     without rescaling changes detail, overstepping and antialiasing
 *     character -- arguably better, but not fidelity),
 *   - lifts the iteration cap to match newly resolvable detail.
 *
 * `out_migrated` receives the re-tuned set. The caller keeps `legacy`
 * and stores both in the .m4d, so the change is reversible.
 */
MB3D_API mb3d_result_t MB3D_CALL mb3d_migrate_parameters(
    const mb3d_render_settings_t* legacy,
    int32_t target_width, int32_t target_height,
    const char* target_profile_id,
    mb3d_render_settings_t* out_migrated);

/* Upscales a parameter set to a new resolution.
 * `lock_to_legacy_look` scales DEstop *with* resolution to preserve the
 * original appearance at higher pixel counts; clearing it lets detail
 * increase with resolution. These are genuinely different intents and
 * the host must label them plainly. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_rescale_for_resolution(
    mb3d_render_settings_t* settings,
    int32_t target_width, int32_t target_height,
    int32_t lock_to_legacy_look);

/* Renders a low-resolution A/B pair (legacy vs migrated) into two jobs
 * so the host can show a side-by-side before committing. On a fractal,
 * "better or merely different" is not judgeable from memory. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_migration_preview(
    mb3d_context ctx,
    const mb3d_render_settings_t* legacy,
    const mb3d_render_settings_t* migrated,
    int32_t preview_size,
    mb3d_job* out_legacy_job, mb3d_job* out_migrated_job);

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

MB3D_API mb3d_result_t MB3D_CALL mb3d_render_begin(mb3d_context ctx,
                                                   const mb3d_render_settings_t* settings,
                                                   mb3d_job* out_job);
MB3D_API mb3d_result_t MB3D_CALL mb3d_job_set_callbacks(mb3d_job job,
                                                        mb3d_progress_fn progress,
                                                        mb3d_cancel_fn cancel,
                                                        void* user);
MB3D_API mb3d_result_t MB3D_CALL mb3d_job_wait(mb3d_job job, int32_t timeout_ms);
MB3D_API mb3d_result_t MB3D_CALL mb3d_job_cancel(mb3d_job job);
MB3D_API mb3d_result_t MB3D_CALL mb3d_job_progress(mb3d_job job, mb3d_progress_t* out);
MB3D_API mb3d_result_t MB3D_CALL mb3d_job_layer(mb3d_job job, uint32_t layer,
                                                mb3d_image_view_t* out_view);
MB3D_API void MB3D_CALL mb3d_job_release(mb3d_job job);

MB3D_API mb3d_result_t MB3D_CALL mb3d_post_recompute_normals(mb3d_job job, float strength);
MB3D_API mb3d_result_t MB3D_CALL mb3d_post_ssao(mb3d_job job, float radius,
                                                float intensity, int32_t samples);
MB3D_API mb3d_result_t MB3D_CALL mb3d_post_hard_shadows(mb3d_job job, float bias);
MB3D_API mb3d_result_t MB3D_CALL mb3d_post_depth_of_field(mb3d_job job, float focal_plane,
                                                          float strength);
MB3D_API mb3d_result_t MB3D_CALL mb3d_post_composite(mb3d_job job);

/* ------------------------------------------------------------------ */
/* Scenes and the native .m4d container                                */
/* ------------------------------------------------------------------ */

/* Sniffs by magic bytes first, extension second. */
MB3D_API int32_t MB3D_CALL mb3d_identify_file(const char* path);

MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_create(mb3d_scene* out_scene);
MB3D_API void MB3D_CALL mb3d_scene_destroy(mb3d_scene scene);

/* A scene holds two parameter sets. The as-imported legacy values are
 * never overwritten by migration, so a precision-mode switch is
 * non-destructive and reversible. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_settings(mb3d_scene scene,
                                                     mb3d_render_settings_t* out);
MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_set_settings(mb3d_scene scene,
                                                         const mb3d_render_settings_t* in);
MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_legacy_settings(mb3d_scene scene,
                                                            mb3d_render_settings_t* out);
MB3D_API int32_t MB3D_CALL mb3d_scene_has_legacy_settings(mb3d_scene scene);
MB3D_API mb3d_result_t MB3D_CALL mb3d_scene_provenance(mb3d_scene scene,
                                                       mb3d_provenance_t* out);

/* .m4d is the only format this engine writes. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_read(const char* path, mb3d_scene* out_scene);
MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_write(const char* path, mb3d_scene scene);
/* Writes the scene together with a job's rendered layers. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_write_with_image(const char* path,
                                                           mb3d_scene scene, mb3d_job job);
/* Loads only the image payload of a .m4d into a job-shaped handle. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_m4d_read_image(const char* path, mb3d_job* out_job);

/* ------------------------------------------------------------------ */
/* Legacy import -- read only, never write                             */
/* ------------------------------------------------------------------ */

/*
 * SECURITY: every legacy file is untrusted input. The readers
 * bounds-check every offset, cap every allocation, and never execute
 * anything. .m3f / .d3f / .dSO carry raw machine code; the importer
 * parses their declarative metadata, maps the formula name against a
 * known-formula registry, and disables the slot when the name does not
 * resolve. The code body is never loaded, mapped, or run. There is no
 * flag to make it run.
 */
MB3D_API mb3d_result_t MB3D_CALL mb3d_import_legacy(const char* path,
                                                    const char* profile_id,
                                                    mb3d_scene* out_scene,
                                                    mb3d_import_report_t* out_report);

MB3D_API mb3d_result_t MB3D_CALL mb3d_import_legacy_memory(const void* data, size_t size,
                                                           int32_t kind,
                                                           const char* profile_id,
                                                           mb3d_scene* out_scene,
                                                           mb3d_import_report_t* out_report);

/* Extracts the parameter block embedded in a .m3i without decoding
 * pixels. Pass buffer=NULL to query the required size. This is what a
 * host calls on file drop. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_m3i_extract_parameters(const char* path,
                                                             void* buffer,
                                                             size_t buffer_size,
                                                             size_t* out_needed);

/* Reads .m3f / .d3f / .dSO metadata only. `out_resolved` receives 1 when
 * the declared formula mapped onto a known implementation. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_inspect_legacy_formula(const char* path,
                                                             char* out_name,
                                                             size_t name_capacity,
                                                             int32_t* out_class,
                                                             int32_t* out_resolved);

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

MB3D_API int32_t MB3D_CALL mb3d_animation_keyframe_count(mb3d_scene scene);
MB3D_API mb3d_result_t MB3D_CALL mb3d_animation_keyframe_at(mb3d_scene scene, int32_t index,
                                                            mb3d_keyframe_t* out);
MB3D_API mb3d_result_t MB3D_CALL mb3d_animation_evaluate(mb3d_scene scene, int32_t frame,
                                                         mb3d_render_settings_t* settings);

/* ------------------------------------------------------------------ */
/* Mesh extraction and CAD export                                      */
/* ------------------------------------------------------------------ */

MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_extract(mb3d_context ctx,
                                                   const mb3d_render_settings_t* settings,
                                                   const mb3d_mesh_settings_t* mesh_settings,
                                                   mb3d_progress_fn progress, void* user,
                                                   mb3d_mesh* out_mesh);
MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_stats(mb3d_mesh mesh, mb3d_mesh_stats_t* out);
MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_stl(mb3d_mesh mesh, const char* path,
                                                     int32_t binary);
/* ISO 10303 AP214 via dynamically-linked OpenCASCADE. Returns
 * MB3D_ERR_UNSUPPORTED when the core was built without OCCT. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_step(mb3d_mesh mesh, const char* path);
MB3D_API mb3d_result_t MB3D_CALL mb3d_mesh_write_obj(mb3d_mesh mesh, const char* path);
MB3D_API void MB3D_CALL mb3d_mesh_destroy(mb3d_mesh mesh);

MB3D_API mb3d_result_t MB3D_CALL mb3d_write_exr(const char* path, mb3d_job job,
                                                int32_t multilayer);

/* ------------------------------------------------------------------ */
/* LuaJIT scripting                                                    */
/* ------------------------------------------------------------------ */

/* Scripts are untrusted. The state has io, os, package, require,
 * dofile, loadfile and ffi removed; ffi in particular would grant
 * arbitrary dlopen and raw process memory access. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_create(mb3d_context ctx, mb3d_lua* out_lua);
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_run_file(mb3d_lua lua, const char* path);
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_run_string(mb3d_lua lua, const char* source);
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_bind_settings(mb3d_lua lua,
                                                        mb3d_render_settings_t* settings);
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_call_frame(mb3d_lua lua, const char* fn_name,
                                                     int32_t frame, double time);
MB3D_API mb3d_result_t MB3D_CALL mb3d_lua_push_fft(mb3d_lua lua, const float* bins,
                                                   int32_t count);
MB3D_API void MB3D_CALL mb3d_lua_destroy(mb3d_lua lua);

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

MB3D_API int32_t MB3D_CALL mb3d_formula_count(mb3d_context ctx);
MB3D_API const char* MB3D_CALL mb3d_formula_name_at(mb3d_context ctx, int32_t index);

/* Distance estimate at a world point -- the Navigator depth picker and
 * Bulb Tracer distance-curve analysis. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_sample_de(mb3d_context ctx,
                                                const mb3d_render_settings_t* settings,
                                                const double point[3], double* out_de);
MB3D_API mb3d_result_t MB3D_CALL mb3d_trace_ray(mb3d_context ctx,
                                                const mb3d_render_settings_t* settings,
                                                const double origin[3],
                                                const double direction[3],
                                                double* out_distance,
                                                double out_normal[3]);
/* MutaGen: derive `count` mutated variants of `base`. */
MB3D_API mb3d_result_t MB3D_CALL mb3d_mutate(const mb3d_render_settings_t* base,
                                             mb3d_render_settings_t* out_array,
                                             int32_t count, int32_t seed, float strength);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MB3D_RENDERER_H */
