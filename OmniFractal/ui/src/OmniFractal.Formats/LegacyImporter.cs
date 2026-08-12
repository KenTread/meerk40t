// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// LegacyImporter.cs -- the managed import surface.
//
// Import is one-way by design. The legacy formats are read for
// interoperability and converted to .m4d; nothing here writes them, and
// the native core refuses to (OMF_ERR_WRITE_FORBIDDEN). If you are
// looking for a legacy save path, there isn't one, and that is
// deliberate — see LICENSE §10.
//
// This class owns two responsibilities the native importer cannot:
//
//   1. Deciding WHICH precision profile to import under. That is a
//      product decision driven by the detected originating version, and
//      it is surfaced to the user rather than chosen silently, because
//      picking the wrong one produces an image that is subtly and
//      inexplicably different from what the author saw.
//   2. Telling the user what the import cost them. An import that
//      silently drops three formula slots is worse than one that says so.

using System.Runtime.InteropServices;
using OmniFractal.Interop;

namespace OmniFractal.Formats;

public enum LegacyFormat
{
    Unknown = 0,
    Native = 1,        // .m4d — not legacy; listed so Identify has one enum
    Parameters = 2,    // .m3p
    Image = 3,         // .m3i
    Animation = 4,     // .m3a
    Lighting = 5,      // .m3l
    MonteCarlo = 6,    // .m3c
    VoxelStack = 7,    // .m3v
    Formula = 8,       // .m3f
    FormulaDe = 9,     // .d3f
    FormulaCompiled = 10,  // .dSO
}

/// <summary>
/// What an import actually produced, including what it could not.
/// Surface this — a lossy import that looks clean is a support ticket
/// six months later.
/// </summary>
public sealed record ImportReport
{
    public required LegacyFormat SourceFormat { get; init; }

    /// <summary>
    /// Detected originating version as major*10000 + minor*100 + patch
    /// (1.99.12 → 19912). Zero when the file carried no usable marker.
    /// </summary>
    public required int SourceVersion { get; init; }

    public required string ProfileId { get; init; }
    public int KeyframeCount { get; init; }
    public int LayerCount { get; init; }
    public int ImageWidth { get; init; }
    public int ImageHeight { get; init; }
    public bool HasParameters { get; init; }

    /// <summary>Records the reader did not recognise. Non-zero means lossy.</summary>
    public int UnmappedFieldCount { get; init; }

    /// <summary>
    /// Formula slots whose legacy name did not resolve. Those slots are
    /// DISABLED, not guessed at — substituting a similar formula renders
    /// a confidently wrong image, which is worse than a visibly missing one.
    /// </summary>
    public int UnresolvedFormulaCount { get; init; }

    /// <summary>
    /// True when the source carried a machine-code payload. It was
    /// recorded and skipped. It was not, and cannot be, executed.
    /// </summary>
    public bool ContainedExecutablePayload { get; init; }

    public long BytesRead { get; init; }

    public bool IsLossless => UnmappedFieldCount == 0 && UnresolvedFormulaCount == 0;

    public string SourceVersionText => SourceVersion == 0
        ? "unknown"
        : $"{SourceVersion / 10000}.{SourceVersion / 100 % 100}.{SourceVersion % 100}";

    /// <summary>A sentence fit for a status bar. Empty when nothing was lost.</summary>
    public string DescribeLosses()
    {
        if (IsLossless) return string.Empty;

        List<string> parts = [];
        if (UnresolvedFormulaCount > 0)
        {
            parts.Add($"{UnresolvedFormulaCount} formula slot"
                      + (UnresolvedFormulaCount == 1 ? "" : "s")
                      + " could not be resolved and " +
                      (UnresolvedFormulaCount == 1 ? "was" : "were") + " disabled");
        }
        if (UnmappedFieldCount > 0)
        {
            parts.Add($"{UnmappedFieldCount} unrecognised field"
                      + (UnmappedFieldCount == 1 ? "" : "s") + " skipped");
        }
        return string.Join("; ", parts);
    }
}

/// <summary>
/// A precision profile the user can import under.
/// </summary>
public sealed record ProfileChoice(string Id, string DisplayName, string Rationale);

/// <summary>
/// Reads the Mandelbulb 3D file suite and converts it to the native
/// .m4d container. Read-only: there is no export path to these formats.
/// </summary>
public static class LegacyImporter
{
    /// <summary>
    /// Identifies a file by magic bytes first, extension second. Magic
    /// wins because a renamed file is still what it is — and dropped
    /// files get renamed constantly in practice.
    /// </summary>
    public static LegacyFormat Identify(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (!File.Exists(path)) return LegacyFormat.Unknown;

        Span<byte> head = stackalloc byte[16];
        int read = 0;
        try
        {
            using FileStream stream = File.OpenRead(path);
            read = stream.Read(head);
        }
        catch (IOException)
        {
            // A locked file is still identifiable by extension.
        }

        if (read >= 4)
        {
            if (head[0] == 'M' && head[1] == 'B' && head[2] == '4' && head[3] == 'D')
            {
                return LegacyFormat.Native;
            }
            if (head[0] == 'M' && head[1] == '3' && head[2] == 'I') return LegacyFormat.Image;
            if (head[0] == 'M' && head[1] == '3' && head[2] == 'V')
            {
                return LegacyFormat.VoxelStack;
            }
        }

        return Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".m4d" => LegacyFormat.Native,
            ".m3p" => LegacyFormat.Parameters,
            ".m3i" => LegacyFormat.Image,
            ".m3a" => LegacyFormat.Animation,
            ".m3l" => LegacyFormat.Lighting,
            ".m3c" => LegacyFormat.MonteCarlo,
            ".m3v" => LegacyFormat.VoxelStack,
            ".m3f" => LegacyFormat.Formula,
            ".d3f" => LegacyFormat.FormulaDe,
            ".dso" => LegacyFormat.FormulaCompiled,
            _ => LegacyFormat.Unknown,
        };
    }

    public static bool IsImportable(LegacyFormat format) => format switch
    {
        LegacyFormat.Parameters or LegacyFormat.Image or
        LegacyFormat.Animation or LegacyFormat.Lighting => true,
        _ => false,
    };

    /// <summary>
    /// Profiles offered for a detected source version, best match first.
    ///
    /// Deliberately a list rather than a single answer. Version detection
    /// is a heuristic on a marker string, and the user knows which
    /// release they authored in better than we can infer it.
    /// </summary>
    public static IReadOnlyList<ProfileChoice> SuggestProfiles(int sourceVersion)
    {
        var legacy19912 = new ProfileChoice(
            "legacy-1.99.12",
            "Legacy 1.99.12",
            "Reproduces 1.99.12 evaluation semantics: whole-chain DE downgrade on " +
            "mixed formulas, original probe epsilon, locked sampling. Closest match " +
            "for widely circulated parameter sets.");

        var legacy19935 = new ProfileChoice(
            "legacy-1.99.35",
            "Legacy 1.99.35",
            "Reproduces the later release, which tightened the estimate on " +
            "all-analytic chains. Use this if you authored in 1.99.2x or newer.");

        var modern = new ProfileChoice(
            "modern",
            "Modern",
            "Tight analytic DE per slot, fp64, sampling free to be re-tuned. " +
            "Faster and sharper — but it will not look like the original.");

        // Rendering behaviour changed between releases, so the ordering
        // follows the detected marker rather than always defaulting to
        // one "legacy" mode.
        return sourceVersion switch
        {
            0 => [legacy19912, legacy19935, modern],
            >= 19920 => [legacy19935, legacy19912, modern],
            _ => [legacy19912, legacy19935, modern],
        };
    }

    /// <summary>
    /// Reads the version marker without inflating the payload. Cheap
    /// enough to run inside a drag-and-drop handler, which is the point:
    /// the profile chooser needs this before the user commits.
    /// </summary>
    public static int PeekSourceVersion(string path)
    {
        try
        {
            using FileStream stream = File.OpenRead(path);
            Span<byte> head = stackalloc byte[64];
            int read = stream.Read(head);
            if (read <= 0) return 0;

            int end = 0;
            while (end < read)
            {
                byte c = head[end];
                if (c is (byte)'\n' or (byte)'\r' or 0) break;
                if (c < 0x20 || c > 0x7E) return 0;
                end++;
            }

            string marker = System.Text.Encoding.ASCII.GetString(head[..end]).ToLowerInvariant();
            return marker switch
            {
                "mandelbulb3dv16" => 19908,
                "mandelbulb3dv17" => 19910,
                "mandelbulb3dv18" => 19912,
                "mandelbulb3dv19" => 19919,
                "mandelbulb3dv20" => 19935,
                _ => 0,
            };
        }
        catch (IOException)
        {
            return 0;
        }
    }

    /// <summary>
    /// Imports a legacy file into a scene under the given profile.
    /// The caller owns the returned <see cref="Scene"/>.
    /// </summary>
    public static (Scene Scene, ImportReport Report) Import(string path, string profileId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentException.ThrowIfNullOrWhiteSpace(profileId);

        LegacyFormat format = Identify(path);
        if (format == LegacyFormat.Native)
        {
            throw new InvalidOperationException(
                $"'{Path.GetFileName(path)}' is already a native .m4d scene; open it directly.");
        }
        if (format is LegacyFormat.Formula or LegacyFormat.FormulaDe
                   or LegacyFormat.FormulaCompiled)
        {
            throw new InvalidOperationException(
                $"'{Path.GetFileName(path)}' is a formula file. Formula files contain raw " +
                "machine code and are never loaded or executed; use InspectFormula to read " +
                "their metadata.");
        }
        if (!IsImportable(format))
        {
            throw new NotSupportedException(
                $"'{Path.GetFileName(path)}' ({format}) cannot be imported by this build.");
        }

        Scene scene = NativeCore.ImportLegacy(path, profileId, out NativeImportReport native);

        var report = new ImportReport
        {
            SourceFormat = format,
            SourceVersion = native.SourceVersion,
            ProfileId = profileId,
            KeyframeCount = native.KeyframeCount,
            LayerCount = native.LayerCount,
            ImageWidth = native.ImageWidth,
            ImageHeight = native.ImageHeight,
            HasParameters = native.HasParameters != 0,
            UnmappedFieldCount = native.UnmappedFieldCount,
            UnresolvedFormulaCount = native.UnresolvedFormulaCount,
            ContainedExecutablePayload = native.ContainedExecutablePayload != 0,
            BytesRead = native.BytesRead,
        };

        return (scene, report);
    }

    /// <summary>
    /// Pulls the parameter block out of an .m3i without decoding pixels.
    /// Two seeks and a bounded read, so it is safe on the UI thread.
    /// </summary>
    public static byte[] ExtractParameters(string path) =>
        NativeCore.ExtractLegacyParameters(path);

    /// <summary>
    /// Reads formula-file metadata. The machine-code body is never
    /// loaded, mapped, or executed — only its presence is reported.
    /// </summary>
    public static FormulaInfo InspectFormula(string path)
    {
        (string name, int formulaClass, bool resolved) = NativeCore.InspectLegacyFormula(path);
        return new FormulaInfo(name, (FormulaDimensionClass)formulaClass, resolved);
    }
}

/// <summary>
/// Declarative metadata from a .m3f / .d3f / .dSO. Carries no code.
/// </summary>
public sealed record FormulaInfo(string Name, FormulaDimensionClass Class, bool Resolved)
{
    public string StatusText => Resolved
        ? $"'{Name}' maps to a built-in {Class} formula."
        : $"'{Name}' has no built-in equivalent; slots using it will be disabled.";
}
