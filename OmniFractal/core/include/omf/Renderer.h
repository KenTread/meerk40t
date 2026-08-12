/* SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary */
/* Copyright (c) 2026 KenTread. All Rights Reserved. */
/* Proprietary and confidential -- see LICENSE for terms. */
/*
 * Renderer.h -- OmniFractalCore public C ABI.
 *
 * OmniFractal is an independent 64-bit fractal renderer. It reads the
 * Mandelbulb 3D file suite for interoperability and converts it to its
 * own .m4d container; it is not a version of, successor to, or
 * derivative of that application. See LICENSE section 10.
 *
 * This header is the single interop boundary between the C++20 engine
 * and any host: the C# .NET 9 front-end, the LuaJIT FFI layer, the CLI
 * renderer, and the distributed tile workers.
 *
 * ABI rules (do not break these without bumping OMF_ABI_VERSION):
 *   - Plain C. No exceptions escape, no C++ types in signatures, and an
 *     explicit calling convention on every exported function.
 *   - All structs are standard-layout, explicitly sized, 8-byte aligned,
 *     and padded to a fixed size. Renderer.cpp static_asserts both the
 *     sizeof() and the offsetof() of every field that a managed mirror
 *     depends on; NativeStructs.cs asserts the same sizes from the
 *     other side.
 *   - Every function returning omf_result_t returns OMF_OK (0) on
 *     success and a negative value on failure. Use omf_last_error()
 *     for a human-readable message (thread-local).
 *   - Buffers exposed through omf_image_view_t stay owned by the core
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

#ifndef OMF_RENDERER_H
#define OMF_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(OMF_CORE_BUILD)
#    define OMF_API __declspec(dllexport)
#  else
#    define OMF_API __declspec(dllimport)
#  endif
#  define OMF_CALL __cdecl
#else
#  if defined(OMF_CORE_BUILD)
#    define OMF_API __attribute__((visibility("default")))
#  else
#    define OMF_API
#  endif
/* x86-64 System V and AArch64 AAPCS each define exactly one C calling
 * convention, so there is nothing to select; __attribute__((cdecl)) is
 * an x86-32-only spelling and warns on 64-bit targets. The macro exists
 * so the Windows side can say __cdecl explicitly. */
#  define OMF_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */

#define OMF_ABI_VERSION      1
#define OMF_VERSION_MAJOR    0
#define OMF_VERSION_MINOR    1
#define OMF_VERSION_PATCH    0

/* .m4d container format version. Independent of the ABI version: the
 * ABI can change without the on-disk format changing, and vice versa.
 * The reader refuses unknown values rather than guessing. */
#define OMF_M4D_FORMAT_VERSION 1

/* Structural limits. The six formula slots and six light channels match
 * what the legacy formats can express, so an import is never lossy for
 * structural reasons. */
#define OMF_FORMULA_SLOTS        6
#define OMF_LIGHT_CHANNELS       6
#define OMF_GRADIENT_ENTRIES   256
#define OMF_FORMULA_PARAMS      12
#define OMF_FORMULA_IPARAMS      4
#define OMF_MAX_NAME            64
#define OMF_MAX_PATH           260

/* ------------------------------------------------------------------ */
/* Result codes                                                        */
/* ------------------------------------------------------------------ */

typedef int32_t omf_result_t;

enum {
    OMF_OK                   =   0,
    OMF_ERR_UNKNOWN          =  -1,
    OMF_ERR_INVALID_ARG      =  -2,
    OMF_ERR_OUT_OF_MEMORY    =  -3,
    OMF_ERR_IO               =  -4,
    OMF_ERR_PARSE            =  -5,
    OMF_ERR_UNSUPPORTED      =  -6,
    OMF_ERR_NO_DEVICE        =  -7,   /* no Vulkan device, no fallback  */
    OMF_ERR_SHADER_COMPILE   =  -8,
    OMF_ERR_LUA              =  -9,
    OMF_ERR_CANCELLED        = -10,
    OMF_ERR_ABI_MISMATCH     = -11,
    OMF_ERR_NOT_READY        = -12,
    OMF_ERR_GEOMETRY         = -13,   /* mesh not manifold / empty      */
    OMF_ERR_FORMAT_VERSION   = -14,   /* .m4d from a newer build        */
    OMF_ERR_CORRUPT          = -15,   /* structurally invalid input     */
    OMF_ERR_LIMIT_EXCEEDED   = -16,   /* input demanded too much memory */
    OMF_ERR_UNTRUSTED        = -17,   /* input carries executable code  */
    OMF_ERR_WRITE_FORBIDDEN  = -18    /* legacy formats are read-only   */
};

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */

typedef struct omf_context_s* omf_context;   /* engine + device      */
typedef struct omf_job_s*     omf_job;       /* one render in flight */
typedef struct omf_scene_s*   omf_scene;     /* a document           */
typedef struct omf_mesh_s*    omf_mesh;      /* extracted surface    */
typedef struct omf_lua_s*     omf_lua;       /* LuaJIT state         */

/* ------------------------------------------------------------------ */
/* Precision model                                                     */
/* ------------------------------------------------------------------ */

/* Arithmetic width. There is deliberately no fp128 and no 80-bit path:
 * Vulkan tops out at VK_KHR_shader_float64, and an 80-bit CPU path would
 * force a toolchain change (MSVC aliases long double to double) to
 * reproduce a rounding mode that was never the reference anyway. */
typedef enum omf_precision_tier_e {
    OMF_PRECISION_FP32   = 0,  /* interactive navigator, preview      */
    OMF_PRECISION_FP64   = 1,  /* default; all legacy profiles        */
    OMF_PRECISION_DD     = 2   /* double-double, ~106 bits, deep zoom */
} omf_precision_tier_t;

/* How the distance estimate is derived. */
typedef enum omf_de_method_e {
    OMF_DE_AUTO           = 0,  /* analytic where every slot supports it */
    OMF_DE_ANALYTIC       = 1,  /* force running-derivative estimate     */
    OMF_DE_NUMERIC_4POINT = 2   /* force the 4-point probe              */
} omf_de_method_t;

/* Bailout comparison semantics. These are not equivalent in floating
 * point at the boundary, and the difference is visible on thin
 * filaments, so the profile pins it rather than leaving it to the
 * compiler. */
typedef enum omf_bailout_compare_e {
    OMF_BAILOUT_R2_GT   = 0,   /* dot(z,z) >  R*R                     */
    OMF_BAILOUT_R2_GE   = 1,   /* dot(z,z) >= R*R                     */
    OMF_BAILOUT_R_GT    = 2    /* length(z) >  R  (extra rounding)    */
} omf_bailout_compare_t;

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
typedef struct omf_precision_profile_s {
    char    id[OMF_MAX_NAME];       /* "legacy-1.99.12", "modern", ... */

    int32_t tier;                    /* omf_precision_tier_t           */
    int32_t de_method;               /* omf_de_method_t                */

    /* 1 => a single non-analytic slot downgrades the entire chain to
     * the numeric DE, as the legacy evaluator did. 0 => each slot
     * contributes its own best estimate. This one switch accounts for
     * more visual difference than every other field here combined. */
    int32_t downgrade_chain_on_mixed_de;

    /* 4-point probe epsilon, relative to hit distance. */
    float   probe_epsilon;

    int32_t bailout_compare;         /* omf_bailout_compare_t          */

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
     * resolution -- see omf_migrate_parameters. */
    float   destop_pixel_reference;

    int32_t _pad0;
} omf_precision_profile_t;

/* Built-in profile identifiers accepted by omf_precision_profile_by_id. */
#define OMF_PROFILE_MODERN         "modern"
#define OMF_PROFILE_LEGACY_19912   "legacy-1.99.12"
#define OMF_PROFILE_LEGACY_19935   "legacy-1.99.35"

/* ------------------------------------------------------------------ */
/* Provenance                                                          */
/* ------------------------------------------------------------------ */

/* Recorded into every .m4d written from an import, so a scene always
 * knows where it came from and under which semantics it was read. */
typedef struct omf_provenance_s {
    int32_t source_format;                    /* omf_file_kind_t       */
    /* Detected originating application version encoded as
     * major*10000 + minor*100 + patch, e.g. 1.99.12 -> 19912. Zero when
     * the file carried no usable version marker. */
    int32_t source_version;
    char    source_version_text[OMF_MAX_NAME];
    char    source_filename[OMF_MAX_PATH];
    char    profile_id[OMF_MAX_NAME];        /* profile used on import */
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
} omf_provenance_t;

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

typedef enum omf_backend_e {
    OMF_BACKEND_AUTO   = 0,
    OMF_BACKEND_VULKAN = 1,
    OMF_BACKEND_CPU    = 2
} omf_backend_t;

/* How slots combine per iteration. */
typedef enum omf_hybrid_mode_e {
    OMF_HYBRID_OFF          = 0,
    OMF_HYBRID_SEQUENTIAL   = 1,  /* slot order, weight = repeat count */
    OMF_HYBRID_ALTERNATE    = 2,
    OMF_HYBRID_INTERPOLATE  = 3,
    OMF_HYBRID_DECOMBINATE  = 4   /* see omf_decomb_op_t              */
} omf_hybrid_mode_t;

/* DEcombinate sub-operation: how two slots' distance estimates merge. */
typedef enum omf_decomb_op_e {
    OMF_DECOMB_MIN      = 0,
    OMF_DECOMB_MAX      = 1,
    OMF_DECOMB_INV_MAX  = 2,
    OMF_DECOMB_MIN_LIN  = 3,   /* linear blend toward the minimum      */
    OMF_DECOMB_MIN_NLIN = 4,   /* non-linear (smooth-min) blend        */
    OMF_DECOMB_MIX1     = 5,
    OMF_DECOMB_MIX2     = 6
} omf_decomb_op_t;

/* Formula dimensional class. The 'a' variants carry an extra alternate
 * component that participates in the fold but not in the bailout. */
typedef enum omf_formula_class_e {
    OMF_CLASS_3D  = 0,
    OMF_CLASS_3DA = 1,
    OMF_CLASS_4D  = 2,
    OMF_CLASS_4DA = 3
} omf_formula_class_t;

typedef enum omf_formula_kind_e {
    OMF_FORMULA_NONE       = 0,
    OMF_FORMULA_BULB       = 1,   /* power-N bulb                      */
    OMF_FORMULA_BOX        = 2,   /* Mandelbox / Amazing Box / Tglad   */
    OMF_FORMULA_FOLD       = 3,   /* kaleidoscopic / Menger folds      */
    OMF_FORMULA_TRANSFORM  = 4,   /* rotate, invert, tile, repeat      */
    OMF_FORMULA_IFS        = 5,
    OMF_FORMULA_QUATERNION = 6,
    OMF_FORMULA_IMPORTED   = 7    /* mapped from a legacy formula name */
} omf_formula_kind_t;

typedef enum omf_trap_kind_e {
    OMF_TRAP_NONE   = 0,
    OMF_TRAP_POINT  = 1,
    OMF_TRAP_LINE   = 2,
    OMF_TRAP_CROSS  = 3,
    OMF_TRAP_BOX    = 4,
    OMF_TRAP_PLANE  = 5,
    OMF_TRAP_SPHERE = 6
} omf_trap_kind_t;

/* Where the colouring index comes from. */
typedef enum omf_color_source_e {
    OMF_COLOR_ITERATION      = 0,
    OMF_COLOR_ORBIT_TRAP     = 1,
    OMF_COLOR_LAST_LENGTH_INC= 2,  /* last-length-increase            */
    OMF_COLOR_ROUT_ANGLE     = 3,  /* Rout angles                     */
    OMF_COLOR_INPUT_VECTOR   = 4,  /* map on input vector             */
    OMF_COLOR_OUTPUT_VECTOR  = 5,  /* map on output vector            */
    OMF_COLOR_DEPTH          = 6
} omf_color_source_t;

typedef enum omf_light_kind_e {
    OMF_LIGHT_OFF         = 0,
    OMF_LIGHT_POSITIONAL  = 1,
    OMF_LIGHT_DIRECTIONAL = 2,
    OMF_LIGHT_MAP         = 3
} omf_light_kind_t;

typedef enum omf_stereo_mode_e {
    OMF_STEREO_OFF       = 0,
    OMF_STEREO_ANAGLYPH  = 1,
    OMF_STEREO_SIDE      = 2,
    OMF_STEREO_CROSSEYE  = 3,
    OMF_STEREO_OVERUNDER = 4
} omf_stereo_mode_t;

typedef enum omf_layer_e {
    OMF_LAYER_RGBA    = 1u << 0,   /* 8-bit BGRA, display ready       */
    OMF_LAYER_RGBA32F = 1u << 1,   /* linear float                    */
    OMF_LAYER_DEPTH   = 1u << 2,
    OMF_LAYER_NORMAL  = 1u << 3,
    OMF_LAYER_SSAO    = 1u << 4,
    OMF_LAYER_SHADOW  = 1u << 5,
    OMF_LAYER_MOTION  = 1u << 6,
    OMF_LAYER_OBJECT  = 1u << 7    /* which slot produced the hit     */
} omf_layer_t;

typedef enum omf_pixel_format_e {
    OMF_PF_BGRA8   = 0,
    OMF_PF_R32F    = 1,
    OMF_PF_RG32F   = 2,
    OMF_PF_RGB32F  = 3,
    OMF_PF_RGBA32F = 4,
    OMF_PF_U32     = 5
} omf_pixel_format_t;

typedef enum omf_job_state_e {
    OMF_JOB_IDLE      = 0,
    OMF_JOB_RUNNING   = 1,
    OMF_JOB_DONE      = 2,
    OMF_JOB_FAILED    = 3,
    OMF_JOB_CANCELLED = 4
} omf_job_state_t;

/* File kinds. Only M4D is written; everything else is import-only. */
typedef enum omf_file_kind_e {
    OMF_FILE_UNKNOWN = 0,
    OMF_FILE_M4D     = 1,   /* native container -- read AND write     */
    OMF_FILE_M3P     = 2,   /* compressed parameter preset            */
    OMF_FILE_M3I     = 3,   /* multi-layer image + embedded .m3p      */
    OMF_FILE_M3A     = 4,   /* animation timeline                     */
    OMF_FILE_M3L     = 5,   /* compressed lighting preset             */
    OMF_FILE_M3C     = 6,   /* Monte Carlo renderer state             */
    OMF_FILE_M3V     = 7,   /* voxel stack                            */
    OMF_FILE_M3F     = 8,   /* formula metadata + machine-code body   */
    OMF_FILE_D3F     = 9,
    OMF_FILE_DSO     = 10
} omf_file_kind_t;

/* ------------------------------------------------------------------ */
/* POD structs -- mirrored 1:1 in NativeStructs.cs                     */
/* ------------------------------------------------------------------ */

typedef struct omf_camera_transform_s {
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
} omf_camera_transform_t;

typedef struct omf_formula_slot_s {
    char    name[OMF_MAX_NAME];

    int32_t kind;                          /* omf_formula_kind_t      */
    int32_t formula_class;                 /* omf_formula_class_t     */
    int32_t enabled;
    int32_t hybrid_mode;                   /* omf_hybrid_mode_t       */
    int32_t decomb_op;                     /* omf_decomb_op_t         */

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

    float   params[OMF_FORMULA_PARAMS];
    int32_t iparams[OMF_FORMULA_IPARAMS];
    float   rotation[3];
    /* Padded so sizeof() is a multiple of 8 and the formulas[] array
     * inside render_settings keeps every following member 8-aligned. */
    float   _pad0;
    float   _pad1;
} omf_formula_slot_t;

typedef struct omf_light_channel_s {
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
    char    lightmap[OMF_MAX_NAME];   /* name only; assets are never   */
                                       /* bundled -- see LICENSE 10.3   */
    int32_t casts_shadow;
    float   shadow_softness;
    /* Volumetric contribution along the ray toward this light. */
    float   volumetric;
    float   _pad0;
} omf_light_channel_t;

typedef struct omf_lighting_settings_s {
    omf_light_channel_t channels[OMF_LIGHT_CHANNELS];

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
    char    background_image[OMF_MAX_NAME];

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
} omf_lighting_settings_t;

typedef struct omf_coloring_settings_s {
    uint32_t gradient[OMF_GRADIENT_ENTRIES];  /* 0xAARRGGBB           */

    int32_t  color_source;          /* omf_color_source_t             */
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
} omf_coloring_settings_t;

typedef struct omf_calculation_settings_s {
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
} omf_calculation_settings_t;

/* Tiling is not optional. RGBA32F + Z + normals + SSAO is ~36 bytes per
 * pixel before DoF or motion buffers, so a gigapixel frame is ~36 GB
 * resident. The 64-bit address space moves the wall from ~100 Mpx to
 * ~100 Gpx; it does not remove it. */
typedef struct omf_tiling_settings_s {
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
} omf_tiling_settings_t;

typedef struct omf_render_settings_s {
    int32_t abi_version;            /* must equal OMF_ABI_VERSION     */
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

    omf_precision_profile_t    profile;
    omf_tiling_settings_t      tiling;
    omf_camera_transform_t     camera;
    omf_calculation_settings_t calculation;
    omf_coloring_settings_t    coloring;
    omf_lighting_settings_t    lighting;
    omf_formula_slot_t         formulas[OMF_FORMULA_SLOTS];

    int32_t hybrid_master_mode;
    int32_t seed;
    int32_t monte_carlo_samples;    /* 0 => deterministic raymarch     */
    int32_t _pad1;
} omf_render_settings_t;

typedef struct omf_image_view_s {
    void*    data;
    size_t   size_bytes;
    int32_t  width;
    int32_t  height;
    int32_t  stride_bytes;
    int32_t  format;
    uint32_t layer;
    int32_t  _pad0;
} omf_image_view_t;

typedef struct omf_progress_s {
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
} omf_progress_t;

typedef struct omf_mesh_settings_s {
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
} omf_mesh_settings_t;

typedef struct omf_mesh_stats_s {
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
} omf_mesh_stats_t;

typedef struct omf_keyframe_s {
    int32_t frame;
    int32_t interpolation;   /* 0 step, 1 linear, 2 catmull-rom, 3 bez */
    float   tension;
    float   bias;
    float   continuity;
    omf_camera_transform_t camera;
    float   formula_params[OMF_FORMULA_SLOTS][OMF_FORMULA_PARAMS];
} omf_keyframe_t;

/* Result of an import, before the scene is committed to a .m4d. */
typedef struct omf_import_report_s {
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
} omf_import_report_t;

typedef void (OMF_CALL* omf_progress_fn)(const omf_progress_t* p, void* user);
typedef void (OMF_CALL* omf_log_fn)(int32_t level, const char* msg, void* user);
typedef int32_t (OMF_CALL* omf_cancel_fn)(void* user);

/* ------------------------------------------------------------------ */
/* Version and diagnostics                                             */
/* ------------------------------------------------------------------ */

/* Stable symbol every host checks before doing anything else. A
 * mismatch means the native library and the host were built from
 * different revisions; refuse to continue rather than reinterpret
 * structs. */
OMF_API int32_t OMF_CALL Core_GetABIVersion(void);

OMF_API int32_t OMF_CALL omf_abi_version(void);
OMF_API int32_t OMF_CALL omf_m4d_format_version(void);
OMF_API const char* OMF_CALL omf_version_string(void);
OMF_API const char* OMF_CALL omf_last_error(void);

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

OMF_API omf_result_t OMF_CALL omf_context_create(int32_t preferred_backend,
                                                     omf_context* out_ctx);
OMF_API void OMF_CALL omf_context_destroy(omf_context ctx);
OMF_API omf_result_t OMF_CALL omf_context_set_log(omf_context ctx,
                                                      omf_log_fn fn, void* user);
OMF_API int32_t OMF_CALL omf_context_backend(omf_context ctx);
OMF_API const char* OMF_CALL omf_context_device_name(omf_context ctx);
/* Which precision tiers this device supports. Bit i set => tier i is
 * usable. fp64 is absent on some mobile and older integrated parts. */
OMF_API uint32_t OMF_CALL omf_context_precision_support(omf_context ctx);

/* ------------------------------------------------------------------ */
/* Profiles and defaults                                               */
/* ------------------------------------------------------------------ */

OMF_API int32_t OMF_CALL omf_profile_count(void);
OMF_API const char* OMF_CALL omf_profile_id_at(int32_t index);
/* Looks up a built-in profile by id. Returns OMF_ERR_INVALID_ARG for
 * an unknown id rather than silently substituting "modern". */
OMF_API omf_result_t OMF_CALL omf_precision_profile_by_id(
    const char* id, omf_precision_profile_t* out);

/* Fills `out` with engine defaults under the "modern" profile. Always
 * call this before mutating a settings struct so fields added in a
 * later revision get defined values. */
OMF_API void OMF_CALL omf_render_settings_default(omf_render_settings_t* out);

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
OMF_API omf_result_t OMF_CALL omf_migrate_parameters(
    const omf_render_settings_t* legacy,
    int32_t target_width, int32_t target_height,
    const char* target_profile_id,
    omf_render_settings_t* out_migrated);

/* Upscales a parameter set to a new resolution.
 * `lock_to_legacy_look` scales DEstop *with* resolution to preserve the
 * original appearance at higher pixel counts; clearing it lets detail
 * increase with resolution. These are genuinely different intents and
 * the host must label them plainly. */
OMF_API omf_result_t OMF_CALL omf_rescale_for_resolution(
    omf_render_settings_t* settings,
    int32_t target_width, int32_t target_height,
    int32_t lock_to_legacy_look);

/* Renders a low-resolution A/B pair (legacy vs migrated) into two jobs
 * so the host can show a side-by-side before committing. On a fractal,
 * "better or merely different" is not judgeable from memory. */
OMF_API omf_result_t OMF_CALL omf_migration_preview(
    omf_context ctx,
    const omf_render_settings_t* legacy,
    const omf_render_settings_t* migrated,
    int32_t preview_size,
    omf_job* out_legacy_job, omf_job* out_migrated_job);

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

OMF_API omf_result_t OMF_CALL omf_render_begin(omf_context ctx,
                                                   const omf_render_settings_t* settings,
                                                   omf_job* out_job);
OMF_API omf_result_t OMF_CALL omf_job_set_callbacks(omf_job job,
                                                        omf_progress_fn progress,
                                                        omf_cancel_fn cancel,
                                                        void* user);
OMF_API omf_result_t OMF_CALL omf_job_wait(omf_job job, int32_t timeout_ms);
OMF_API omf_result_t OMF_CALL omf_job_cancel(omf_job job);
OMF_API omf_result_t OMF_CALL omf_job_progress(omf_job job, omf_progress_t* out);
OMF_API omf_result_t OMF_CALL omf_job_layer(omf_job job, uint32_t layer,
                                                omf_image_view_t* out_view);
OMF_API void OMF_CALL omf_job_release(omf_job job);

OMF_API omf_result_t OMF_CALL omf_post_recompute_normals(omf_job job, float strength);
OMF_API omf_result_t OMF_CALL omf_post_ssao(omf_job job, float radius,
                                                float intensity, int32_t samples);
OMF_API omf_result_t OMF_CALL omf_post_hard_shadows(omf_job job, float bias);
OMF_API omf_result_t OMF_CALL omf_post_depth_of_field(omf_job job, float focal_plane,
                                                          float strength);
OMF_API omf_result_t OMF_CALL omf_post_composite(omf_job job);

/* ------------------------------------------------------------------ */
/* Scenes and the native .m4d container                                */
/* ------------------------------------------------------------------ */

/* Sniffs by magic bytes first, extension second. */
OMF_API int32_t OMF_CALL omf_identify_file(const char* path);

OMF_API omf_result_t OMF_CALL omf_scene_create(omf_scene* out_scene);
OMF_API void OMF_CALL omf_scene_destroy(omf_scene scene);

/* A scene holds two parameter sets. The as-imported legacy values are
 * never overwritten by migration, so a precision-mode switch is
 * non-destructive and reversible. */
OMF_API omf_result_t OMF_CALL omf_scene_settings(omf_scene scene,
                                                     omf_render_settings_t* out);
OMF_API omf_result_t OMF_CALL omf_scene_set_settings(omf_scene scene,
                                                         const omf_render_settings_t* in);
OMF_API omf_result_t OMF_CALL omf_scene_legacy_settings(omf_scene scene,
                                                            omf_render_settings_t* out);
OMF_API int32_t OMF_CALL omf_scene_has_legacy_settings(omf_scene scene);
OMF_API omf_result_t OMF_CALL omf_scene_provenance(omf_scene scene,
                                                       omf_provenance_t* out);

/* .m4d is the only format this engine writes. */
OMF_API omf_result_t OMF_CALL omf_m4d_read(const char* path, omf_scene* out_scene);
OMF_API omf_result_t OMF_CALL omf_m4d_write(const char* path, omf_scene scene);
/* Writes the scene together with a job's rendered layers. */
OMF_API omf_result_t OMF_CALL omf_m4d_write_with_image(const char* path,
                                                           omf_scene scene, omf_job job);
/* Loads only the image payload of a .m4d into a job-shaped handle. */
OMF_API omf_result_t OMF_CALL omf_m4d_read_image(const char* path, omf_job* out_job);

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
OMF_API omf_result_t OMF_CALL omf_import_legacy(const char* path,
                                                    const char* profile_id,
                                                    omf_scene* out_scene,
                                                    omf_import_report_t* out_report);

OMF_API omf_result_t OMF_CALL omf_import_legacy_memory(const void* data, size_t size,
                                                           int32_t kind,
                                                           const char* profile_id,
                                                           omf_scene* out_scene,
                                                           omf_import_report_t* out_report);

/* Extracts the parameter block embedded in a .m3i without decoding
 * pixels. Pass buffer=NULL to query the required size. This is what a
 * host calls on file drop. */
OMF_API omf_result_t OMF_CALL omf_m3i_extract_parameters(const char* path,
                                                             void* buffer,
                                                             size_t buffer_size,
                                                             size_t* out_needed);

/* Reads .m3f / .d3f / .dSO metadata only. `out_resolved` receives 1 when
 * the declared formula mapped onto a known implementation. */
OMF_API omf_result_t OMF_CALL omf_inspect_legacy_formula(const char* path,
                                                             char* out_name,
                                                             size_t name_capacity,
                                                             int32_t* out_class,
                                                             int32_t* out_resolved);

/* ------------------------------------------------------------------ */
/* Animation                                                           */
/* ------------------------------------------------------------------ */

OMF_API int32_t OMF_CALL omf_animation_keyframe_count(omf_scene scene);
OMF_API omf_result_t OMF_CALL omf_animation_keyframe_at(omf_scene scene, int32_t index,
                                                            omf_keyframe_t* out);
OMF_API omf_result_t OMF_CALL omf_animation_evaluate(omf_scene scene, int32_t frame,
                                                         omf_render_settings_t* settings);

/* ------------------------------------------------------------------ */
/* Mesh extraction and CAD export                                      */
/* ------------------------------------------------------------------ */

OMF_API omf_result_t OMF_CALL omf_mesh_extract(omf_context ctx,
                                                   const omf_render_settings_t* settings,
                                                   const omf_mesh_settings_t* mesh_settings,
                                                   omf_progress_fn progress, void* user,
                                                   omf_mesh* out_mesh);
OMF_API omf_result_t OMF_CALL omf_mesh_stats(omf_mesh mesh, omf_mesh_stats_t* out);
OMF_API omf_result_t OMF_CALL omf_mesh_write_stl(omf_mesh mesh, const char* path,
                                                     int32_t binary);
/* ISO 10303 AP214 via dynamically-linked OpenCASCADE. Returns
 * OMF_ERR_UNSUPPORTED when the core was built without OCCT. */
OMF_API omf_result_t OMF_CALL omf_mesh_write_step(omf_mesh mesh, const char* path);
OMF_API omf_result_t OMF_CALL omf_mesh_write_obj(omf_mesh mesh, const char* path);
OMF_API void OMF_CALL omf_mesh_destroy(omf_mesh mesh);

OMF_API omf_result_t OMF_CALL omf_write_exr(const char* path, omf_job job,
                                                int32_t multilayer);

/* ------------------------------------------------------------------ */
/* LuaJIT scripting                                                    */
/* ------------------------------------------------------------------ */

/* Scripts are untrusted. The state has io, os, package, require,
 * dofile, loadfile and ffi removed; ffi in particular would grant
 * arbitrary dlopen and raw process memory access. */
OMF_API omf_result_t OMF_CALL omf_lua_create(omf_context ctx, omf_lua* out_lua);
OMF_API omf_result_t OMF_CALL omf_lua_run_file(omf_lua lua, const char* path);
OMF_API omf_result_t OMF_CALL omf_lua_run_string(omf_lua lua, const char* source);
OMF_API omf_result_t OMF_CALL omf_lua_bind_settings(omf_lua lua,
                                                        omf_render_settings_t* settings);
OMF_API omf_result_t OMF_CALL omf_lua_call_frame(omf_lua lua, const char* fn_name,
                                                     int32_t frame, double time);
OMF_API omf_result_t OMF_CALL omf_lua_push_fft(omf_lua lua, const float* bins,
                                                   int32_t count);
OMF_API void OMF_CALL omf_lua_destroy(omf_lua lua);

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

OMF_API int32_t OMF_CALL omf_formula_count(omf_context ctx);
OMF_API const char* OMF_CALL omf_formula_name_at(omf_context ctx, int32_t index);

/* Distance estimate at a world point -- the Navigator depth picker and
 * Bulb Tracer distance-curve analysis. */
OMF_API omf_result_t OMF_CALL omf_sample_de(omf_context ctx,
                                                const omf_render_settings_t* settings,
                                                const double point[3], double* out_de);
OMF_API omf_result_t OMF_CALL omf_trace_ray(omf_context ctx,
                                                const omf_render_settings_t* settings,
                                                const double origin[3],
                                                const double direction[3],
                                                double* out_distance,
                                                double out_normal[3]);
/* MutaGen: derive `count` mutated variants of `base`. */
OMF_API omf_result_t OMF_CALL omf_mutate(const omf_render_settings_t* base,
                                             omf_render_settings_t* out_array,
                                             int32_t count, int32_t seed, float strength);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* OMF_RENDERER_H */
