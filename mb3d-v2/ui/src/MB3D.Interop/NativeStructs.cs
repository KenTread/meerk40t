// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// NativeStructs.cs -- managed mirrors of the structs in Renderer.h.
//
// Blittable by construction: fixed-size buffers instead of arrays,
// explicit int32/float/double, no bool, no string. That is what lets a
// whole struct be passed by pointer with no marshalling stub, and it is
// why every one of these is `unsafe`.
//
// Layout drift between this file and Renderer.h would silently corrupt
// every render setting in a way that is very hard to diagnose, so both
// sides assert. Renderer.cpp static_asserts sizeof AND offsetof;
// AbiLayout.Verify() below re-checks the sizes from managed code at
// startup and throws a readable exception instead of rendering garbage.

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace MB3D.Interop;

public enum Mb3dResult
{
    Ok = 0,
    Unknown = -1,
    InvalidArgument = -2,
    OutOfMemory = -3,
    Io = -4,
    Parse = -5,
    Unsupported = -6,
    NoDevice = -7,
    ShaderCompile = -8,
    Lua = -9,
    Cancelled = -10,
    AbiMismatch = -11,
    NotReady = -12,
    Geometry = -13,
    FormatVersion = -14,
    Corrupt = -15,
    LimitExceeded = -16,
    Untrusted = -17,
    WriteForbidden = -18,
}

public enum Mb3dBackend { Auto = 0, Vulkan = 1, Cpu = 2 }

/// <summary>
/// Arithmetic width. No fp128 and no 80-bit tier: Vulkan tops out at
/// VK_KHR_shader_float64, and computing wider than the original does not
/// converge toward it — it converges toward a different attractor.
/// </summary>
public enum PrecisionTier
{
    Fp32 = 0,          // interactive navigator, preview
    Fp64 = 1,          // default; every legacy profile
    DoubleDouble = 2,  // ~106 bits, deep zoom beyond legacy's reach
}

public enum DeMethod { Auto = 0, Analytic = 1, Numeric4Point = 2 }

public enum BailoutCompare { R2Greater = 0, R2GreaterEqual = 1, RGreater = 2 }

public enum HybridMode
{
    Off = 0, Sequential = 1, Alternate = 2, Interpolate = 3, DeCombinate = 4,
}

/// <summary>DEcombinate sub-operation: how two slots' estimates merge.</summary>
public enum DecombOp
{
    Min = 0, Max = 1, InvMax = 2, MinLinear = 3, MinNonLinear = 4, Mix1 = 5, Mix2 = 6,
}

public enum FormulaDimensionClass { ThreeD = 0, ThreeDA = 1, FourD = 2, FourDA = 3 }

public enum FormulaKind
{
    None = 0, Bulb = 1, Box = 2, Fold = 3,
    Transform = 4, Ifs = 5, Quaternion = 6, Imported = 7,
}

public enum TrapKind { None = 0, Point = 1, Line = 2, Cross = 3, Box = 4, Plane = 5, Sphere = 6 }

public enum ColorSource
{
    Iteration = 0, OrbitTrap = 1, LastLengthIncrease = 2, RoutAngle = 3,
    InputVector = 4, OutputVector = 5, Depth = 6,
}

public enum LightKind { Off = 0, Positional = 1, Directional = 2, Map = 3 }

public enum StereoMode { Off = 0, Anaglyph = 1, SideBySide = 2, CrossEye = 3, OverUnder = 4 }

[Flags]
public enum LayerMask : uint
{
    None = 0,
    Rgba = 1 << 0,
    Rgba32F = 1 << 1,
    Depth = 1 << 2,
    Normal = 1 << 3,
    Ssao = 1 << 4,
    Shadow = 1 << 5,
    Motion = 1 << 6,
    Object = 1 << 7,
}

public enum PixelFormat { Bgra8 = 0, R32F = 1, Rg32F = 2, Rgb32F = 3, Rgba32F = 4, U32 = 5 }

public enum JobState { Idle = 0, Running = 1, Done = 2, Failed = 3, Cancelled = 4 }

public enum Mb3dFileKind
{
    Unknown = 0, M4d = 1, M3p = 2, M3i = 3, M3a = 4, M3l = 5,
    M3c = 6, M3v = 7, M3f = 8, D3f = 9, Dso = 10,
}

public static class Abi
{
    public const int Version = 1;
    public const int M4dFormatVersion = 1;
    public const int FormulaSlots = 6;
    public const int LightChannels = 6;
    public const int GradientEntries = 256;
    public const int FormulaParams = 12;
    public const int FormulaIParams = 4;
    public const int MaxName = 64;
    public const int MaxPath = 260;

    public const string ProfileModern = "modern";
    public const string ProfileLegacy19912 = "legacy-1.99.12";
    public const string ProfileLegacy19935 = "legacy-1.99.35";
}

/// <summary>
/// A named, versioned bundle of evaluation semantics. Locking DE method,
/// sampling, and the iteration cap reproduces the legacy look at full
/// fp64 GPU speed; matching the original's rounding is neither possible
/// nor necessary.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct PrecisionProfile
{
    public fixed byte Id[Abi.MaxName];
    public int Tier;
    public int DeMethodValue;

    /// <summary>
    /// 1 = a single non-analytic slot downgrades the whole chain to the
    /// numeric estimate, as legacy did. This one switch accounts for more
    /// visual difference than every other field combined.
    /// </summary>
    public int DowngradeChainOnMixedDe;

    public float ProbeEpsilon;
    public int BailoutCompareValue;
    public int SlotOrderPolicy;
    public int HybridArithmetic;
    public int RngStream;
    public int LockSampling;
    public int LockIterationCap;

    /// <summary>Pixel width (FOV radians per pixel) DEstop was defined against.</summary>
    public float DestopPixelReference;
    private int _pad0;

    public string GetId()
    {
        fixed (byte* p = Id) return Marshal.PtrToStringUTF8((nint)p) ?? string.Empty;
    }
}

/// <summary>Where a scene came from, and under which semantics it was read.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct ProvenanceBlock
{
    public int SourceFormat;
    /// <summary>major*10000 + minor*100 + patch; 0 when undetectable.</summary>
    public int SourceVersion;
    public fixed byte SourceVersionText[Abi.MaxName];
    public fixed byte SourceFilename[Abi.MaxPath];
    public fixed byte ProfileId[Abi.MaxName];
    public long ImportUnixTime;
    public int ImporterAbiVersion;
    public int Migrated;
    public int MigrationTargetWidth;
    public int MigrationTargetHeight;
    public int UnmappedFieldCount;
    private int _pad0;

    public string GetProfileId()
    {
        fixed (byte* p = ProfileId) return Marshal.PtrToStringUTF8((nint)p) ?? string.Empty;
    }

    public string GetSourceFilename()
    {
        fixed (byte* p = SourceFilename) return Marshal.PtrToStringUTF8((nint)p) ?? string.Empty;
    }
}

/// <summary>
/// Tiling is mandatory, not an optimisation. ~36 bytes/pixel across the
/// layer stack means a gigapixel frame is ~36 GB resident; 64-bit moves
/// the wall from ~100 Mpx to ~100 Gpx, it does not remove it.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct TilingSettings
{
    public int TileWidth;
    public int TileHeight;
    /// <summary>Overlap so post passes with a kernel radius do not seam.</summary>
    public int TileOverlap;
    public int MaxResidentTiles;
    /// <summary>Host memory ceiling in MiB. 0 => 60% of physical RAM.</summary>
    public long MemoryBudgetMib;
    public int SpillToDisk;
    private int _pad0;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CameraTransform
{
    public fixed double Position[3];
    public fixed double Target[3];
    public fixed double Up[3];
    public fixed double Rotation[3];
    public double Distance;
    public double Zoom;
    public double FovDegrees;
    public double Aspect;
    public double NearClip;
    public double FarClip;
    public double DofFocalPlane;
    public double DofStrength;
    public double StereoIpd;
    public int StereoMode;
    public int Projection;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct FormulaSlot
{
    public fixed byte Name[Abi.MaxName];
    public int Kind;
    public int FormulaClass;
    public int Enabled;
    public int HybridMode;
    public int DecombOp;
    public int IterationWeight;
    public int StartIteration;
    /// <summary>-1 => active until the global cap.</summary>
    public int StopIteration;
    /// <summary>"Repeat from here" target slot; -1 disables.</summary>
    public int RepeatFromSlot;
    public float RBailout;
    public int MinIterations;
    public int MaxIterations;
    public float InterpolateFactor;
    public fixed float JuliaC[4];
    public int JuliaEnabled;
    /// <summary>0 here can downgrade the whole chain's DE — see PrecisionProfile.</summary>
    public int AnalyticDe;
    public fixed float Params[Abi.FormulaParams];
    public fixed int IParams[Abi.FormulaIParams];
    public fixed float Rotation[3];
    private float _pad0;
    private float _pad1;

    public string GetName()
    {
        fixed (byte* p = Name) return Marshal.PtrToStringUTF8((nint)p) ?? string.Empty;
    }

    public void SetName(string value)
    {
        // Truncate rather than throw: an over-long name is a display
        // problem, not a reason to refuse a scene.
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(value);
        int count = Math.Min(bytes.Length, Abi.MaxName - 1);
        fixed (byte* p = Name)
        {
            new Span<byte>(p, Abi.MaxName).Clear();
            bytes.AsSpan(0, count).CopyTo(new Span<byte>(p, Abi.MaxName));
        }
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct LightChannel
{
    public int Kind;
    public int Enabled;
    public fixed float Color[3];
    public float Intensity;
    public fixed float Position[3];
    public float Falloff;
    public float Specular;
    public float SpecularExponent;
    public float Diffuse;
    public float AmbientMix;
    /// <summary>Name only. Assets are never bundled — see LICENSE §10.3.</summary>
    public fixed byte LightMap[Abi.MaxName];
    public int CastsShadow;
    public float ShadowSoftness;
    public float Volumetric;
    private float _pad0;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct LightingSettings
{
    public LightChannel Channel0;
    public LightChannel Channel1;
    public LightChannel Channel2;
    public LightChannel Channel3;
    public LightChannel Channel4;
    public LightChannel Channel5;

    public fixed float AmbientColorA[3];
    private float _pad0;
    public fixed float AmbientColorB[3];
    private float _pad1;
    public float AmbientFalloff;

    public fixed float FogColorA[3];
    public fixed float FogColorB[3];
    public float FogStart;
    public float FogEnd;
    public float FogDensity;
    public float FarFogDensity;
    public float IterationFogDensity;

    public float Gamma;
    public float Contrast;
    public float Brightness;
    public float Saturation;
    public float Exposure;

    public int BackgroundMode;
    public fixed float BackgroundA[3];
    public fixed float BackgroundB[3];
    public fixed byte BackgroundImage[Abi.MaxName];

    public int SsaoEnabled;
    public float SsaoRadius;
    public float SsaoIntensity;
    public int SsaoSamples;
    public int DeaoEnabled;
    public int DeaoSamples;
    public float DeaoRadius;

    public int HardShadows;
    public int SmoothShadows;
    public float ShadowBias;

    public float Reflectivity;
    public int ReflectionBounces;
    public float Transparency;
    public float RefractionIndex;
    private float _pad2;
    private float _pad3;

    /// <summary>
    /// Indexer over the six channels. C# cannot declare a fixed buffer of
    /// a struct type, so they are separate fields walked by pointer —
    /// safe precisely because the layout is sequential and asserted.
    /// </summary>
    public ref LightChannel Channel(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, Abi.LightChannels);
        fixed (LightChannel* first = &Channel0) return ref Unsafe.AsRef<LightChannel>(first + index);
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct ColoringSettings
{
    public fixed uint Gradient[Abi.GradientEntries];
    public int ColorSource;
    public int TrapKind;
    public int TrapDimensions;
    public fixed float TrapCenter[3];
    public fixed float TrapNormal[3];
    public float TrapSize;
    public float TrapInfluence;
    public int TrapMinIteration;
    public float ColorSpeed;
    public float ColorOffset;
    public float ColorCycle;
    public float DepthColorMix;
    public int SmoothIteration;
    public int InsideRendering;
    public uint InsideColor;
    private float _pad0;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CalculationSettings
{
    public int MaxIterations;
    public float EscapeRadius;

    // Sampling — profile item 2, second only to DE method selection.
    public float RaystepMultiplier;
    public float StepwidthLimiter;
    public float DeStopCriterion;
    public int VaryDestopOnFov;
    public int FirstStepRandom;
    public int RaystepSubDestop;

    public int DeMaxSteps;
    public int BinarySearchSteps;
    public int SmoothNormals;
    public int NormalsOnDe;
    public float NormalEpsilon;

    public int BoundSphereEnabled;
    public fixed float BoundSphereCenter[3];
    public float BoundSphereRadius;
    public int BoundBoxEnabled;
    public fixed float BoundBoxMin[3];
    public fixed float BoundBoxMax[3];

    public float DeScale;
    private int _pad0;
    private int _pad1;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct RenderSettings
{
    public int AbiVersion;
    public int Width;
    public int Height;
    public int Supersample;
    public uint LayerMask;
    public int Backend;
    public int ThreadCount;
    private int _pad0;

    public int CropX;
    public int CropY;
    public int CropW;
    public int CropH;

    public PrecisionProfile Profile;
    public TilingSettings Tiling;
    public CameraTransform Camera;
    public CalculationSettings Calculation;
    public ColoringSettings Coloring;
    public LightingSettings Lighting;

    private FormulaSlot _formula0;
    private FormulaSlot _formula1;
    private FormulaSlot _formula2;
    private FormulaSlot _formula3;
    private FormulaSlot _formula4;
    private FormulaSlot _formula5;

    public int HybridMasterMode;
    public int Seed;
    public int MonteCarloSamples;
    private int _pad1;

    public ref FormulaSlot Formula(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, Abi.FormulaSlots);
        fixed (FormulaSlot* first = &_formula0) return ref Unsafe.AsRef<FormulaSlot>(first + index);
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct ImageView
{
    public nint Data;
    public nuint SizeBytes;
    public int Width;
    public int Height;
    public int StrideBytes;
    public int Format;
    public uint Layer;
    private int _pad0;

    /// <summary>
    /// Wraps the native buffer without copying. Valid only while the
    /// owning job is alive.
    /// </summary>
    public unsafe Span<byte> AsSpan() => new((void*)Data, checked((int)SizeBytes));
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct RenderProgress
{
    public int State;
    public float Fraction;
    public long PixelsDone;
    public long PixelsTotal;
    public double ElapsedSeconds;
    public double EstimatedRemaining;
    public int TilesDone;
    public int TilesTotal;
    /// <summary>
    /// 0..1. fp64 halves register-limited occupancy, which is often a
    /// bigger real cost than the issue-rate ratio — so it is surfaced.
    /// </summary>
    public float GpuOccupancy;
    public int RaysActive;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct MeshSettings
{
    public fixed int Resolution[3];
    public fixed float BoundsMin[3];
    public fixed float BoundsMax[3];
    public float IsoLevel;
    public int CloseSurface;
    public int BaseClipEnabled;
    public float BaseClipHeight;
    public int WeldVertices;
    public float WeldEpsilon;
    public int SmoothIterations;
    public int GenerateNormals;
    public float ScaleToMm;
    private int _pad0;

    /// <summary>Defaults tuned for a printable model: 256³, welded, base-clipped.</summary>
    public static MeshSettings CreateDefault()
    {
        var settings = default(MeshSettings);
        for (int i = 0; i < 3; i++)
        {
            settings.Resolution[i] = 256;
            settings.BoundsMin[i] = -1.5f;
            settings.BoundsMax[i] = 1.5f;
        }
        settings.CloseSurface = 1;
        settings.WeldVertices = 1;
        settings.WeldEpsilon = 0.0f;   // 0 => the core derives it from cell size
        settings.GenerateNormals = 1;
        settings.ScaleToMm = 25.0f;
        return settings;
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct MeshStats
{
    public long VertexCount;
    public long TriangleCount;
    public int IsManifold;
    public int IsWatertight;
    public int OpenEdgeCount;
    public int ShellCount;
    public fixed float BoundsMin[3];
    public fixed float BoundsMax[3];
    public double VolumeMm3;
    public double SurfaceAreaMm2;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct Keyframe
{
    public int Frame;
    public int Interpolation;
    public float Tension;
    public float Bias;
    public float Continuity;
    public CameraTransform Camera;
    public fixed float FormulaParams[Abi.FormulaSlots * Abi.FormulaParams];
}

/// <summary>Blittable mirror of mb3d_import_report_t.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct NativeImportReport
{
    public int SourceFormat;
    public int SourceVersion;
    public int KeyframeCount;
    public int LayerCount;
    public int ImageWidth;
    public int ImageHeight;
    public int HasParameters;
    public int UnmappedFieldCount;
    /// <summary>Slots whose legacy name did not resolve. They are disabled, not guessed.</summary>
    public int UnresolvedFormulaCount;
    /// <summary>Non-zero when the source carried machine code. It was skipped, never run.</summary>
    public int ContainedExecutablePayload;
    public long BytesRead;
    private int _pad0;
    private int _pad1;
}

/// <summary>
/// Startup guard. These numbers are generated from the native header and
/// mirrored in Renderer.cpp's static_asserts; if they disagree the
/// managed and native builds are from different revisions.
/// </summary>
public static class AbiLayout
{
    private static readonly (string Name, int Expected, int Actual)[] Expectations =
    [
        ("PrecisionProfile",    112, Unsafe.SizeOf<PrecisionProfile>()),
        ("ProvenanceBlock",     432, Unsafe.SizeOf<ProvenanceBlock>()),
        ("TilingSettings",       32, Unsafe.SizeOf<TilingSettings>()),
        ("CameraTransform",     176, Unsafe.SizeOf<CameraTransform>()),
        ("FormulaSlot",         224, Unsafe.SizeOf<FormulaSlot>()),
        ("LightChannel",        136, Unsafe.SizeOf<LightChannel>()),
        ("LightingSettings",   1072, Unsafe.SizeOf<LightingSettings>()),
        ("ColoringSettings",   1104, Unsafe.SizeOf<ColoringSettings>()),
        ("CalculationSettings", 112, Unsafe.SizeOf<CalculationSettings>()),
        ("RenderSettings",     4016, Unsafe.SizeOf<RenderSettings>()),
        ("ImageView",            40, Unsafe.SizeOf<ImageView>()),
        ("RenderProgress",       56, Unsafe.SizeOf<RenderProgress>()),
        ("MeshSettings",         76, Unsafe.SizeOf<MeshSettings>()),
        ("MeshStats",            72, Unsafe.SizeOf<MeshStats>()),
        ("Keyframe",            488, Unsafe.SizeOf<Keyframe>()),
        ("NativeImportReport",   56, Unsafe.SizeOf<NativeImportReport>()),
    ];

    // Field offsets the native side also asserts. A size match with a
    // shifted field is the failure mode that would otherwise slip
    // through, so the load-bearing offsets are checked explicitly.
    private static readonly (string Name, int Expected, int Actual)[] Offsets =
    [
        ("RenderSettings.Profile",     48,   (int)Marshal.OffsetOf<RenderSettings>("Profile")),
        ("RenderSettings.Tiling",      160,  (int)Marshal.OffsetOf<RenderSettings>("Tiling")),
        ("RenderSettings.Camera",      192,  (int)Marshal.OffsetOf<RenderSettings>("Camera")),
        ("RenderSettings.Calculation", 368,  (int)Marshal.OffsetOf<RenderSettings>("Calculation")),
        ("RenderSettings.Coloring",    480,  (int)Marshal.OffsetOf<RenderSettings>("Coloring")),
        ("RenderSettings.Lighting",    1584, (int)Marshal.OffsetOf<RenderSettings>("Lighting")),
    ];

    public static void Verify()
    {
        List<string>? problems = null;

        foreach ((string name, int expected, int actual) in Expectations)
        {
            if (expected == actual) continue;
            (problems ??= []).Add($"{name}: managed {actual} bytes, native expects {expected}");
        }
        foreach ((string name, int expected, int actual) in Offsets)
        {
            if (expected == actual) continue;
            (problems ??= []).Add($"{name}: managed offset {actual}, native expects {expected}");
        }

        if (problems is not null)
        {
            throw new InvalidOperationException(
                "MB3D_Core ABI layout mismatch — the native library and this assembly were " +
                "built from different revisions:\n  " + string.Join("\n  ", problems));
        }
    }
}
