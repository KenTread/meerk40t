// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// NativeCore.cs -- the managed side of the MB3D_Core boundary.
//
// Uses [LibraryImport] (source-generated marshalling) rather than
// [DllImport]: it is AOT-friendly, and because every struct here is
// blittable the generator emits a direct call with no stub at all.
//
// Zero-copy contract: RenderJob.GetLayer hands back an ImageView that
// points straight into the engine's buffers. Nothing is copied on the
// way out. The job owns that memory, so the view is only valid until the
// job is disposed -- RenderJob enforces that with an ObjectDisposedException
// rather than letting a dangling pointer through.

using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;

namespace MB3D.Interop;

/// <summary>Thrown for any non-zero result code from MB3D_Core.</summary>
public sealed class Mb3dException : Exception
{
    public Mb3dResult Result { get; }

    public Mb3dException(Mb3dResult result, string message) : base(message) => Result = result;
}

internal static partial class NativeMethods
{
    private const string Library = "MB3D_Core";

    // -- lifetime ------------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_abi_version();

    [LibraryImport(Library)]
    internal static partial nint mb3d_version_string();

    [LibraryImport(Library)]
    internal static partial nint mb3d_last_error();

    [LibraryImport(Library)]
    internal static partial int mb3d_context_create(int preferred, out nint outContext);

    [LibraryImport(Library)]
    internal static partial void mb3d_context_destroy(nint context);

    [LibraryImport(Library)]
    internal static partial int mb3d_context_set_log(nint context, nint callback, nint user);

    [LibraryImport(Library)]
    internal static partial int mb3d_context_backend(nint context);

    [LibraryImport(Library)]
    internal static partial nint mb3d_context_device_name(nint context);

    [LibraryImport(Library)]
    internal static partial void mb3d_render_settings_default(out RenderSettings settings);

    // -- rendering -----------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_render_begin(nint context, in RenderSettings settings,
                                                  out nint outJob);

    [LibraryImport(Library)]
    internal static partial int mb3d_job_set_callbacks(nint job, nint progress, nint cancel,
                                                       nint user);

    [LibraryImport(Library)]
    internal static partial int mb3d_job_wait(nint job, int timeoutMs);

    [LibraryImport(Library)]
    internal static partial int mb3d_job_cancel(nint job);

    [LibraryImport(Library)]
    internal static partial int mb3d_job_progress(nint job, out RenderProgress progress);

    [LibraryImport(Library)]
    internal static partial int mb3d_job_layer(nint job, uint layer, out ImageView view);

    [LibraryImport(Library)]
    internal static partial void mb3d_job_release(nint job);

    // -- post ----------------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_post_recompute_normals(nint job, float strength);

    [LibraryImport(Library)]
    internal static partial int mb3d_post_ssao(nint job, float radius, float intensity,
                                               int samples);

    [LibraryImport(Library)]
    internal static partial int mb3d_post_hard_shadows(nint job, float bias);

    [LibraryImport(Library)]
    internal static partial int mb3d_post_depth_of_field(nint job, float focalPlane,
                                                         float strength);

    [LibraryImport(Library)]
    internal static partial int mb3d_post_composite(nint job);

    // -- files ---------------------------------------------------------
    //
    // .m4d is the only format written. The legacy entry points are
    // import-only; there is deliberately no legacy writer to bind to.

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_identify_file(string path);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_create(out nint outScene);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_settings(nint scene, out RenderSettings settings);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_set_settings(nint scene, in RenderSettings settings);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_legacy_settings(nint scene,
                                                           out RenderSettings settings);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_has_legacy_settings(nint scene);

    [LibraryImport(Library)]
    internal static partial int mb3d_scene_provenance(nint scene, out ProvenanceBlock provenance);

    [LibraryImport(Library)]
    internal static partial void mb3d_scene_destroy(nint scene);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_m4d_read(string path, out nint outScene);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_m4d_write(string path, nint scene);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_m4d_write_with_image(string path, nint scene, nint job);

    // -- legacy import (read only) --------------------------------------

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_import_legacy(string path, string profileId,
                                                   out nint outScene,
                                                   out NativeImportReport report);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial int mb3d_m3i_extract_parameters(string path, byte* buffer,
                                                                   nuint bufferSize,
                                                                   out nuint needed);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static unsafe partial int mb3d_inspect_legacy_formula(string path, byte* outName,
                                                                   nuint nameCapacity,
                                                                   out int outClass,
                                                                   out int outResolved);

    // -- profiles and migration -----------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_profile_count();

    [LibraryImport(Library)]
    internal static partial nint mb3d_profile_id_at(int index);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_precision_profile_by_id(string id,
                                                             out PrecisionProfile profile);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_migrate_parameters(in RenderSettings legacy,
                                                        int targetWidth, int targetHeight,
                                                        string targetProfileId,
                                                        out RenderSettings migrated);

    [LibraryImport(Library)]
    internal static partial int mb3d_rescale_for_resolution(ref RenderSettings settings,
                                                            int targetWidth, int targetHeight,
                                                            int lockToLegacyLook);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_write_exr(string path, nint job, int multilayer);

    [LibraryImport(Library)]
    internal static partial int mb3d_formula_count(nint context);

    [LibraryImport(Library)]
    internal static partial nint mb3d_formula_name_at(nint context, int index);

    [LibraryImport(Library)]
    internal static partial uint mb3d_context_precision_support(nint context);

    [LibraryImport(Library)]
    internal static partial int Core_GetABIVersion();

    // -- mesh ----------------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_mesh_extract(nint context, in RenderSettings settings,
                                                  in MeshSettings meshSettings, nint progress,
                                                  nint user, out nint outMesh);

    [LibraryImport(Library)]
    internal static partial int mb3d_mesh_stats(nint mesh, out MeshStats stats);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_mesh_write_stl(nint mesh, string path, int binary);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_mesh_write_step(nint mesh, string path);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_mesh_write_obj(nint mesh, string path);

    [LibraryImport(Library)]
    internal static partial void mb3d_mesh_destroy(nint mesh);

    // -- lua -----------------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_lua_create(nint context, out nint outLua);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_lua_run_file(nint lua, string path);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_lua_run_string(nint lua, string source);

    [LibraryImport(Library)]
    internal static unsafe partial int mb3d_lua_bind_settings(nint lua, RenderSettings* settings);

    [LibraryImport(Library, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int mb3d_lua_call_frame(nint lua, string fnName, int frame,
                                                    double time);

    [LibraryImport(Library)]
    internal static unsafe partial int mb3d_lua_push_fft(nint lua, float* bins, int count);

    [LibraryImport(Library)]
    internal static partial void mb3d_lua_destroy(nint lua);

    // -- animation -----------------------------------------------------

    [LibraryImport(Library)]
    internal static partial int mb3d_animation_keyframe_count(nint scene);

    [LibraryImport(Library)]
    internal static partial int mb3d_animation_keyframe_at(nint scene, int index,
                                                           out Keyframe keyframe);

    [LibraryImport(Library)]
    internal static partial int mb3d_animation_evaluate(nint scene, int frame,
                                                        ref RenderSettings settings);

    // -- utility -------------------------------------------------------

    [LibraryImport(Library)]
    internal static unsafe partial int mb3d_sample_de(nint context, in RenderSettings settings,
                                                      double* point, out double outDe);

    [LibraryImport(Library)]
    internal static unsafe partial int mb3d_trace_ray(nint context, in RenderSettings settings,
                                                      double* origin, double* direction,
                                                      out double distance, double* normal);

    [LibraryImport(Library)]
    internal static unsafe partial int mb3d_mutate(in RenderSettings baseSettings,
                                                   RenderSettings* outArray, int count,
                                                   int seed, float strength);
}

/// <summary>
/// Progress callback delivered from an engine worker thread. Do not
/// touch UI state directly from it -- marshal to the UI thread first.
/// </summary>
public delegate void RenderProgressHandler(in RenderProgress progress);

/// <summary>
/// Owning handle for one in-flight or completed render. Disposing joins
/// the engine's worker thread before freeing, which is what makes the
/// zero-copy layer views safe.
/// </summary>
public sealed class RenderJob : IDisposable
{
    private nint _handle;
    private GCHandle _callbackPin;

    internal RenderJob(nint handle, RenderSettings settings)
    {
        _handle = handle;
        Settings = settings;
    }

    public RenderSettings Settings { get; }

    public bool IsDisposed => _handle == nint.Zero;

    internal nint Handle =>
        _handle != nint.Zero ? _handle : throw new ObjectDisposedException(nameof(RenderJob));

    /// <summary>Blocks until the render finishes. Negative timeout waits forever.</summary>
    /// <returns>False when the timeout elapsed first.</returns>
    public bool Wait(int timeoutMs = -1)
    {
        int result = NativeMethods.mb3d_job_wait(Handle, timeoutMs);
        if (result == (int)Mb3dResult.NotReady) return false;
        NativeCore.Check(result);
        return true;
    }

    public void Cancel() => NativeCore.Check(NativeMethods.mb3d_job_cancel(Handle));

    public RenderProgress GetProgress()
    {
        NativeCore.Check(NativeMethods.mb3d_job_progress(Handle, out RenderProgress progress));
        return progress;
    }

    /// <summary>
    /// Returns a view onto the engine's own buffer -- no copy is made.
    /// The pointer stops being valid the moment this job is disposed.
    /// </summary>
    public ImageView GetLayer(LayerMask layer)
    {
        NativeCore.Check(NativeMethods.mb3d_job_layer(Handle, (uint)layer, out ImageView view));
        return view;
    }

    public void RecomputeNormals(float strength = 1.0f) =>
        NativeCore.Check(NativeMethods.mb3d_post_recompute_normals(Handle, strength));

    public void ApplySsao(float radius, float intensity, int samples) =>
        NativeCore.Check(NativeMethods.mb3d_post_ssao(Handle, radius, intensity, samples));

    public void ApplyHardShadows(float bias = 1e-4f) =>
        NativeCore.Check(NativeMethods.mb3d_post_hard_shadows(Handle, bias));

    public void ApplyDepthOfField(float focalPlane, float strength) =>
        NativeCore.Check(NativeMethods.mb3d_post_depth_of_field(Handle, focalPlane, strength));

    /// <summary>Runs every post pass the scene settings enable, in order.</summary>
    public void Composite() => NativeCore.Check(NativeMethods.mb3d_post_composite(Handle));

    public void SaveExr(string path, bool multilayer = true) =>
        NativeCore.Check(NativeMethods.mb3d_write_exr(path, Handle, multilayer ? 1 : 0));

    public void Dispose()
    {
        if (_handle == nint.Zero) return;

        // Release joins the worker thread, so nothing is writing to the
        // layer buffers by the time they are freed.
        NativeMethods.mb3d_job_release(_handle);
        _handle = nint.Zero;

        if (_callbackPin.IsAllocated) _callbackPin.Free();
        GC.SuppressFinalize(this);
    }

    ~RenderJob() => Dispose();
}

/// <summary>Owning handle for an extracted iso-surface.</summary>
public sealed class MeshHandle : IDisposable
{
    private nint _handle;

    internal MeshHandle(nint handle) => _handle = handle;

    private nint Handle =>
        _handle != nint.Zero ? _handle : throw new ObjectDisposedException(nameof(MeshHandle));

    public MeshStats GetStats()
    {
        NativeCore.Check(NativeMethods.mb3d_mesh_stats(Handle, out MeshStats stats));
        return stats;
    }

    public void SaveStl(string path, bool binary = true) =>
        NativeCore.Check(NativeMethods.mb3d_mesh_write_stl(path is null
            ? throw new ArgumentNullException(nameof(path)) : Handle, path, binary ? 1 : 0));

    public void SaveStep(string path) =>
        NativeCore.Check(NativeMethods.mb3d_mesh_write_step(Handle, path));

    public void SaveObj(string path) =>
        NativeCore.Check(NativeMethods.mb3d_mesh_write_obj(Handle, path));

    public void Dispose()
    {
        if (_handle == nint.Zero) return;
        NativeMethods.mb3d_mesh_destroy(_handle);
        _handle = nint.Zero;
        GC.SuppressFinalize(this);
    }

    ~MeshHandle() => Dispose();
}

/// <summary>Owning handle for a LuaJIT state.</summary>
public sealed class LuaScript : IDisposable
{
    private nint _handle;

    internal LuaScript(nint handle) => _handle = handle;

    private nint Handle =>
        _handle != nint.Zero ? _handle : throw new ObjectDisposedException(nameof(LuaScript));

    public void RunFile(string path) =>
        NativeCore.Check(NativeMethods.mb3d_lua_run_file(Handle, path));

    public void Run(string source) =>
        NativeCore.Check(NativeMethods.mb3d_lua_run_string(Handle, source));

    /// <summary>
    /// Binds a settings struct as the script global `scene`. The caller
    /// must keep <paramref name="settings"/> pinned for as long as the
    /// script may run -- scripts write through the pointer.
    /// </summary>
    public unsafe void BindSettings(RenderSettings* settings) =>
        NativeCore.Check(NativeMethods.mb3d_lua_bind_settings(Handle, settings));

    public void CallFrame(string functionName, int frame, double time) =>
        NativeCore.Check(NativeMethods.mb3d_lua_call_frame(Handle, functionName, frame, time));

    /// <summary>Pushes an FFT magnitude spectrum for audio-reactive scripts.</summary>
    public unsafe void PushFft(ReadOnlySpan<float> bins)
    {
        fixed (float* p = bins)
        {
            NativeCore.Check(NativeMethods.mb3d_lua_push_fft(Handle, p, bins.Length));
        }
    }

    public void Dispose()
    {
        if (_handle == nint.Zero) return;
        NativeMethods.mb3d_lua_destroy(_handle);
        _handle = nint.Zero;
        GC.SuppressFinalize(this);
    }

    ~LuaScript() => Dispose();
}

/// <summary>A parsed scene: parameters and, for .m3a, a timeline.</summary>
public sealed class Scene : IDisposable
{
    private nint _handle;

    internal Scene(nint handle) => _handle = handle;

    internal nint Handle =>
        _handle != nint.Zero ? _handle : throw new ObjectDisposedException(nameof(Scene));

    /// <summary>
    /// The as-imported parameters, preserved byte-for-byte. Present only
    /// on imported scenes. Migration writes the active set and never
    /// touches this, which is what makes a precision-mode switch
    /// reversible.
    /// </summary>
    public bool HasLegacySettings =>
        NativeMethods.mb3d_scene_has_legacy_settings(Handle) != 0;

    public RenderSettings GetLegacySettings()
    {
        Check(NativeMethods.mb3d_scene_legacy_settings(Handle, out RenderSettings settings));
        return settings;
    }

    public ProvenanceBlock GetProvenance()
    {
        Check(NativeMethods.mb3d_scene_provenance(Handle, out ProvenanceBlock provenance));
        return provenance;
    }

    private static void Check(int result) => NativeCore.Check(result);

    public RenderSettings GetSettings()
    {
        NativeCore.Check(NativeMethods.mb3d_scene_settings(Handle, out RenderSettings settings));
        return settings;
    }

    public void SetSettings(in RenderSettings settings) =>
        NativeCore.Check(NativeMethods.mb3d_scene_set_settings(Handle, settings));

    public int KeyframeCount => NativeMethods.mb3d_animation_keyframe_count(Handle);

    public Keyframe GetKeyframe(int index)
    {
        NativeCore.Check(NativeMethods.mb3d_animation_keyframe_at(Handle, index,
                                                                  out Keyframe keyframe));
        return keyframe;
    }

    /// <summary>Interpolates the timeline into <paramref name="settings"/>.</summary>
    public void EvaluateAt(int frame, ref RenderSettings settings) =>
        NativeCore.Check(NativeMethods.mb3d_animation_evaluate(Handle, frame, ref settings));

    public void Dispose()
    {
        if (_handle == nint.Zero) return;
        NativeMethods.mb3d_scene_destroy(_handle);
        _handle = nint.Zero;
        GC.SuppressFinalize(this);
    }

    ~Scene() => Dispose();
}

/// <summary>
/// The engine. One instance per process is the intended usage; it owns
/// the Vulkan device and the formula registry.
/// </summary>
public sealed partial class NativeCore : IDisposable
{
    private nint _context;

    private NativeCore(nint context) => _context = context;

    private nint Handle =>
        _context != nint.Zero ? _context : throw new ObjectDisposedException(nameof(NativeCore));

    public static string VersionString => PtrToString(NativeMethods.mb3d_version_string());

    public static int AbiVersion => NativeMethods.mb3d_abi_version();

    public Mb3dBackend Backend => (Mb3dBackend)NativeMethods.mb3d_context_backend(Handle);

    public string DeviceName => PtrToString(NativeMethods.mb3d_context_device_name(Handle));

    /// <summary>
    /// Creates the engine, verifying the ABI first. A version mismatch
    /// here means the native library and this assembly were built from
    /// different revisions.
    /// </summary>
    public static NativeCore Create(Mb3dBackend preferred = Mb3dBackend.Auto)
    {
        AbiLayout.Verify();

        int nativeAbi = NativeMethods.mb3d_abi_version();
        if (nativeAbi != Abi.Version)
        {
            throw new Mb3dException(Mb3dResult.AbiMismatch,
                $"MB3D_Core reports ABI {nativeAbi}, this build expects {Abi.Version}. " +
                "The native library and the UI are from different revisions.");
        }

        Check(NativeMethods.mb3d_context_create((int)preferred, out nint context));
        return new NativeCore(context);
    }

    /// <summary>Engine defaults: one bulb slot, a key light, a neutral ramp.</summary>
    public static RenderSettings CreateDefaultSettings()
    {
        NativeMethods.mb3d_render_settings_default(out RenderSettings settings);
        return settings;
    }

    public RenderJob BeginRender(in RenderSettings settings,
                                 RenderProgressHandler? onProgress = null)
    {
        Check(NativeMethods.mb3d_render_begin(Handle, settings, out nint job));
        var handle = new RenderJob(job, settings);

        if (onProgress is not null)
        {
            // The callback is invoked from engine threads. Keeping the
            // delegate alive is the caller's problem otherwise, so pin
            // it to the job's lifetime here.
            unsafe
            {
                var thunk = new ProgressThunk(onProgress);
                thunk.Attach(job);
            }
        }
        return handle;
    }

    /// <summary>Renders synchronously and returns the finished job.</summary>
    public RenderJob Render(in RenderSettings settings)
    {
        RenderJob job = BeginRender(settings);
        try
        {
            job.Wait();
            return job;
        }
        catch
        {
            job.Dispose();
            throw;
        }
    }

    public static Mb3dFileKind Identify(string path) =>
        (Mb3dFileKind)NativeMethods.mb3d_identify_file(path);

    /// <summary>Reads a native .m4d scene.</summary>
    public static Scene LoadScene(string path)
    {
        Check(NativeMethods.mb3d_m4d_read(path, out nint scene));
        return new Scene(scene);
    }

    /// <summary>
    /// Writes a native .m4d. This is the only format the engine writes;
    /// the legacy formats are import-only by design.
    /// </summary>
    public static void SaveScene(string path, Scene scene)
    {
        ArgumentNullException.ThrowIfNull(scene);
        Check(NativeMethods.mb3d_m4d_write(path, scene.Handle));
    }

    public static void SaveSceneWithImage(string path, Scene scene, RenderJob job)
    {
        ArgumentNullException.ThrowIfNull(scene);
        ArgumentNullException.ThrowIfNull(job);
        Check(NativeMethods.mb3d_m4d_write_with_image(path, scene.Handle, job.Handle));
    }

    // -- legacy import --------------------------------------------------

    /// <summary>
    /// Imports a legacy file under the named precision profile. The
    /// profile is a required argument, not a default: importing under the
    /// wrong semantics produces an image that is subtly and
    /// inexplicably different from what the author saw.
    /// </summary>
    public static Scene ImportLegacy(string path, string profileId,
                                     out NativeImportReport report)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentException.ThrowIfNullOrWhiteSpace(profileId);

        Check(NativeMethods.mb3d_import_legacy(path, profileId, out nint scene, out report));
        return new Scene(scene);
    }

    /// <summary>
    /// Pulls the parameter block out of an .m3i without decoding pixels.
    /// Two-pass: query the size, then fill.
    /// </summary>
    public static unsafe byte[] ExtractLegacyParameters(string path)
    {
        Check(NativeMethods.mb3d_m3i_extract_parameters(path, null, 0, out nuint needed));
        if (needed == 0) return [];

        byte[] buffer = new byte[needed];
        fixed (byte* p = buffer)
        {
            Check(NativeMethods.mb3d_m3i_extract_parameters(path, p, needed, out _));
        }
        return buffer;
    }

    /// <summary>
    /// Reads .m3f / .d3f / .dSO metadata. The machine-code body in those
    /// files is never loaded, mapped, or executed.
    /// </summary>
    public static unsafe (string Name, int Class, bool Resolved) InspectLegacyFormula(string path)
    {
        const int capacity = Abi.MaxName;
        byte* name = stackalloc byte[capacity];

        Check(NativeMethods.mb3d_inspect_legacy_formula(path, name, capacity,
                                                        out int formulaClass,
                                                        out int resolved));
        return (Marshal.PtrToStringUTF8((nint)name) ?? string.Empty, formulaClass, resolved != 0);
    }

    // -- profiles and migration -----------------------------------------

    public static IReadOnlyList<string> GetProfileIds()
    {
        int count = NativeMethods.mb3d_profile_count();
        var ids = new List<string>(count);
        for (int i = 0; i < count; i++)
        {
            ids.Add(PtrToString(NativeMethods.mb3d_profile_id_at(i)));
        }
        return ids;
    }

    public static PrecisionProfile GetProfile(string id)
    {
        Check(NativeMethods.mb3d_precision_profile_by_id(id, out PrecisionProfile profile));
        return profile;
    }

    /// <summary>
    /// Re-tunes legacy parameters for a target profile and resolution.
    ///
    /// Not a toggle. Legacy values were tuned against legacy's flaws — a
    /// raystep multiplier of 0.1 fighting overstepping, an iteration cap
    /// set where detail stopped mattering at the author's resolution.
    /// Carried forward unchanged the result is slower AND worse, so this
    /// raises the multiplier in proportion to the DE tightening, rescales
    /// DEstop against the new pixel width, and lifts the iteration cap.
    ///
    /// The caller keeps the original and stores both in the .m4d, so the
    /// change is reversible.
    /// </summary>
    public static RenderSettings MigrateParameters(in RenderSettings legacy,
                                                   int targetWidth, int targetHeight,
                                                   string targetProfileId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(targetProfileId);
        Check(NativeMethods.mb3d_migrate_parameters(legacy, targetWidth, targetHeight,
                                                    targetProfileId,
                                                    out RenderSettings migrated));
        return migrated;
    }

    /// <summary>
    /// Changes resolution. <paramref name="lockToLegacyLook"/> scales
    /// DEstop with resolution to preserve the original appearance;
    /// clearing it lets detail increase with pixel count. Genuinely
    /// different intents — label them plainly in the UI.
    /// </summary>
    public static void RescaleForResolution(ref RenderSettings settings,
                                            int targetWidth, int targetHeight,
                                            bool lockToLegacyLook)
    {
        Check(NativeMethods.mb3d_rescale_for_resolution(ref settings, targetWidth,
                                                        targetHeight,
                                                        lockToLegacyLook ? 1 : 0));
    }

    public uint PrecisionSupport => NativeMethods.mb3d_context_precision_support(Handle);

    public bool SupportsTier(PrecisionTier tier) =>
        (PrecisionSupport & (1u << (int)tier)) != 0;

    public MeshHandle ExtractMesh(in RenderSettings settings, in MeshSettings meshSettings)
    {
        Check(NativeMethods.mb3d_mesh_extract(Handle, settings, meshSettings,
                                              nint.Zero, nint.Zero, out nint mesh));
        return new MeshHandle(mesh);
    }

    public LuaScript CreateScript()
    {
        Check(NativeMethods.mb3d_lua_create(Handle, out nint lua));
        return new LuaScript(lua);
    }

    /// <summary>Distance estimate at a world point. Powers the depth picker.</summary>
    public unsafe double SampleDistanceEstimate(in RenderSettings settings,
                                                double x, double y, double z)
    {
        double* point = stackalloc double[3] { x, y, z };
        Check(NativeMethods.mb3d_sample_de(Handle, settings, point, out double de));
        return de;
    }

    /// <summary>
    /// Traces one ray. Returns null when nothing was hit -- which is a
    /// normal outcome for a picker click on empty space, not an error.
    /// </summary>
    public unsafe (double Distance, double NX, double NY, double NZ)? TraceRay(
        in RenderSettings settings,
        double ox, double oy, double oz,
        double dx, double dy, double dz)
    {
        double* origin = stackalloc double[3] { ox, oy, oz };
        double* direction = stackalloc double[3] { dx, dy, dz };
        double* normal = stackalloc double[3];

        int result = NativeMethods.mb3d_trace_ray(Handle, settings, origin, direction,
                                                  out double distance, normal);
        if (result == (int)Mb3dResult.NotReady) return null;
        Check(result);
        return (distance, normal[0], normal[1], normal[2]);
    }

    /// <summary>MutaGen: derive mutated variants of a base parameter set.</summary>
    public static unsafe RenderSettings[] Mutate(in RenderSettings baseSettings, int count,
                                                 int seed, float strength)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(count);

        var variants = new RenderSettings[count];
        fixed (RenderSettings* p = variants)
        {
            Check(NativeMethods.mb3d_mutate(baseSettings, p, count, seed, strength));
        }
        return variants;
    }

    public void Dispose()
    {
        if (_context == nint.Zero) return;
        NativeMethods.mb3d_context_destroy(_context);
        _context = nint.Zero;
        GC.SuppressFinalize(this);
    }

    ~NativeCore() => Dispose();

    // -- helpers -------------------------------------------------------

    internal static void Check(int result)
    {
        if (result == (int)Mb3dResult.Ok) return;

        string message = PtrToString(NativeMethods.mb3d_last_error());
        if (string.IsNullOrEmpty(message)) message = $"MB3D_Core error {result}";
        throw new Mb3dException((Mb3dResult)result, message);
    }

    private static string PtrToString(nint ptr) =>
        ptr == nint.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;

    /// <summary>
    /// Keeps a managed progress delegate alive for the lifetime of a
    /// native job and adapts it to the unmanaged calling convention.
    /// </summary>
    private sealed unsafe class ProgressThunk
    {
        private static readonly List<ProgressThunk> Live = [];

        private readonly RenderProgressHandler _handler;
        private readonly delegate* unmanaged[Cdecl]<RenderProgress*, void*, void> _fn;

        public ProgressThunk(RenderProgressHandler handler)
        {
            _handler = handler;
            _fn = &Invoke;
        }

        public void Attach(nint job)
        {
            lock (Live) Live.Add(this);
            var user = GCHandle.Alloc(this);
            NativeMethods.mb3d_job_set_callbacks(job, (nint)_fn, nint.Zero,
                                                 GCHandle.ToIntPtr(user));
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        private static void Invoke(RenderProgress* progress, void* user)
        {
            if (user is null || progress is null) return;

            var handle = GCHandle.FromIntPtr((nint)user);
            if (handle.Target is ProgressThunk thunk)
            {
                // Exceptions must never cross back into native code.
                try { thunk._handler(in *progress); }
                catch { /* a failing progress handler must not kill the render */ }
            }
        }
    }
}
