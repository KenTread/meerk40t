// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// MainForm.cs -- the docking shell.
//
// Render results reach the screen without a pixel copy: the engine's
// BGRA layer is wrapped in a Bitmap that points straight at the native
// buffer (Bitmap's scan0 constructor), so a 4K frame costs one pointer
// handoff rather than 33 MB of memcpy per update. The RenderJob that
// owns that memory is therefore kept alive exactly as long as the
// Bitmap is, which is what _currentJob tracks.

using System.Drawing.Imaging;
using MB3D.Formats;
using MB3D.Interop;
using WeifenLuo.WinFormsUI.Docking;

namespace MB3D.App;

public sealed class MainForm : Form
{
    private readonly DockPanel _dockPanel;
    private readonly PictureBox _preview;
    private readonly StatusStrip _status;
    private readonly ToolStripStatusLabel _statusText;
    private readonly ToolStripProgressBar _progress;

    private readonly NativeCore _core;
    private RenderSettings _settings;

    // Held for as long as _preview.Image points into its buffers.
    private RenderJob? _currentJob;
    private Bitmap? _currentBitmap;
    private CancellationTokenSource? _renderCts;

    public MainForm(NativeCore core)
    {
        _core = core;
        _settings = NativeCore.CreateDefaultSettings();

        Text = $"MB3D-V2 — {_core.Backend} ({_core.DeviceName})";
        Width = 1600;
        Height = 950;
        AllowDrop = true;

        _dockPanel = new DockPanel
        {
            Dock = DockStyle.Fill,
            DocumentStyle = DocumentStyle.DockingWindow,
            Theme = new VS2015DarkTheme(),
        };

        _preview = new PictureBox
        {
            Dock = DockStyle.Fill,
            SizeMode = PictureBoxSizeMode.Zoom,
            BackColor = Color.FromArgb(24, 24, 28),
        };

        _statusText = new ToolStripStatusLabel("Ready") { Spring = true, TextAlign = ContentAlignment.MiddleLeft };
        _progress = new ToolStripProgressBar { Visible = false, Width = 220 };
        _status = new StatusStrip();
        _status.Items.Add(_statusText);
        _status.Items.Add(_progress);

        var previewHost = new DockContent { Text = "Render", CloseButton = false };
        previewHost.Controls.Add(_preview);

        Controls.Add(_dockPanel);
        Controls.Add(BuildMenu());
        Controls.Add(_status);

        Load += (_, _) => previewHost.Show(_dockPanel, DockState.Document);
        DragEnter += OnDragEnter;
        DragDrop += OnDragDrop;
        FormClosed += (_, _) => ReleaseCurrentFrame();
    }

    private MenuStrip BuildMenu()
    {
        var file = new ToolStripMenuItem("&File");
        file.DropDownItems.Add("&Open…", null, (_, _) => OpenFileDialogAndLoad());
        file.DropDownItems.Add("Save &Parameters…", null, (_, _) => SaveParameters());
        file.DropDownItems.Add(new ToolStripSeparator());
        file.DropDownItems.Add("Export &STL…", null, async (_, _) => await ExportMeshAsync(MeshFormat.Stl));
        file.DropDownItems.Add("Export ST&EP…", null, async (_, _) => await ExportMeshAsync(MeshFormat.Step));
        file.DropDownItems.Add("Export Open&EXR…", null, (_, _) => ExportExr());
        file.DropDownItems.Add(new ToolStripSeparator());
        file.DropDownItems.Add("E&xit", null, (_, _) => Close());

        var render = new ToolStripMenuItem("&Render");
        render.DropDownItems.Add("&Start", null, async (_, _) => await StartRenderAsync());
        render.DropDownItems.Add("&Cancel", null, (_, _) => _currentJob?.Cancel());

        var menu = new MenuStrip();
        menu.Items.Add(file);
        menu.Items.Add(render);
        return menu;
    }

    // -----------------------------------------------------------------
    // Rendering
    // -----------------------------------------------------------------

    private async Task StartRenderAsync()
    {
        _renderCts?.Cancel();
        _renderCts = new CancellationTokenSource();
        CancellationToken token = _renderCts.Token;

        // Every layer the post chain and the exporters might want. The
        // engine only allocates what is asked for, so this is the one
        // place that decides render cost versus flexibility.
        _settings.LayerMask = (uint)(LayerMask.Rgba | LayerMask.Rgba32F | LayerMask.Depth |
                                     LayerMask.Normal | LayerMask.Ssao);

        _progress.Visible = true;
        _progress.Value = 0;
        _statusText.Text = "Rendering…";

        RenderSettings settings = _settings;
        RenderJob? job = null;

        try
        {
            job = await Task.Run(() =>
            {
                RenderJob started = _core.BeginRender(settings, OnProgress);
                while (!started.Wait(50))
                {
                    if (!token.IsCancellationRequested) continue;
                    started.Cancel();
                    started.Wait();
                    break;
                }
                started.Composite();
                return started;
            }, token);

            ShowFrame(job);
            job = null;   // ownership moved into ShowFrame
        }
        catch (OperationCanceledException)
        {
            _statusText.Text = "Cancelled";
        }
        catch (Mb3dException ex)
        {
            _statusText.Text = $"Render failed: {ex.Message}";
            MessageBox.Show(this, ex.Message, "Render failed", MessageBoxButtons.OK,
                            MessageBoxIcon.Error);
        }
        finally
        {
            job?.Dispose();
            _progress.Visible = false;
        }
    }

    private void OnProgress(in RenderProgress progress)
    {
        // Called from an engine worker thread.
        int percent = (int)Math.Clamp(progress.Fraction * 100.0f, 0, 100);
        double remaining = progress.EstimatedRemaining;

        BeginInvoke(() =>
        {
            if (IsDisposed) return;
            _progress.Value = percent;
            _statusText.Text = $"Rendering… {percent}%  ({remaining:F1}s remaining)";
        });
    }

    /// <summary>
    /// Publishes a finished job to the preview without copying pixels.
    /// The previous frame's job is released only after the new Bitmap is
    /// in place, so the control never paints from freed memory.
    /// </summary>
    private unsafe void ShowFrame(RenderJob job)
    {
        ImageView view = job.GetLayer(LayerMask.Rgba);

        // Wraps the engine's buffer directly. Format32bppPArgb matches
        // the BGRA8 the core writes, so GDI+ blits it with no conversion.
        var bitmap = new Bitmap(view.Width, view.Height, view.StrideBytes,
                                PixelFormat.Format32bppPArgb, view.Data);

        Image? previousImage = _preview.Image;
        RenderJob? previousJob = _currentJob;
        Bitmap? previousBitmap = _currentBitmap;

        _preview.Image = bitmap;
        _currentBitmap = bitmap;
        _currentJob = job;

        previousImage?.Dispose();
        previousBitmap?.Dispose();
        previousJob?.Dispose();

        RenderProgress progress = job.GetProgress();
        _statusText.Text = $"Rendered {view.Width}×{view.Height} in {progress.ElapsedSeconds:F2}s";
    }

    private void ReleaseCurrentFrame()
    {
        _preview.Image = null;
        _currentBitmap?.Dispose();
        _currentBitmap = null;
        _currentJob?.Dispose();
        _currentJob = null;
    }

    // -----------------------------------------------------------------
    // File handling
    // -----------------------------------------------------------------

    private void OnDragEnter(object? sender, DragEventArgs e)
    {
        e.Effect = e.Data?.GetDataPresent(DataFormats.FileDrop) == true
            ? DragDropEffects.Copy
            : DragDropEffects.None;
    }

    private void OnDragDrop(object? sender, DragEventArgs e)
    {
        if (e.Data?.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        LoadFile(paths[0]);
    }

    private void OpenFileDialogAndLoad()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "Mandelbulb 3D files|*.m3p;*.m3i;*.m3a;*.m3l;*.m3c;*.m3f;*.d3f|" +
                     "Parameters (*.m3p)|*.m3p|" +
                     "Images (*.m3i)|*.m3i|" +
                     "Animations (*.m3a)|*.m3a|" +
                     "All files|*.*",
        };
        if (dialog.ShowDialog(this) == DialogResult.OK) LoadFile(dialog.FileName);
    }

    /// <summary>
    /// Identifies and loads any file in the suite. Dropping an .m3i
    /// pulls its embedded .m3p straight out of the header, so the
    /// parameter panels populate without decoding a single pixel.
    /// </summary>
    private void LoadFile(string path)
    {
        try
        {
            MB3DFileKind kind = MB3DFileParser.Identify(path);

            switch (kind)
            {
                case MB3DFileKind.Parameters:
                case MB3DFileKind.Image:
                case MB3DFileKind.Animation:
                case MB3DFileKind.Lighting:
                case MB3DFileKind.Gradient:
                {
                    using Scene scene = NativeCore.LoadScene(path);
                    _settings = scene.GetSettings();

                    string detail = kind == MB3DFileKind.Image
                        ? $" (parameters extracted from {Path.GetFileName(path)})"
                        : string.Empty;
                    _statusText.Text = $"Loaded {kind}{detail}";
                    break;
                }

                case MB3DFileKind.Formula:
                    _core.RegisterFormulaFile(path);
                    _statusText.Text =
                        $"Registered formula '{Path.GetFileNameWithoutExtension(path)}'";
                    break;

                case MB3DFileKind.CompiledFormula:
                    _statusText.Text =
                        "Compiled .dSO formulas are not loadable — supply the .m3f/.d3f source.";
                    break;

                default:
                    _statusText.Text = $"Unrecognised file: {Path.GetFileName(path)}";
                    break;
            }
        }
        catch (Exception ex) when (ex is Mb3dException or IOException or InvalidDataException)
        {
            MessageBox.Show(this, ex.Message, "Could not open file", MessageBoxButtons.OK,
                            MessageBoxIcon.Warning);
        }
    }

    private void SaveParameters()
    {
        using var dialog = new SaveFileDialog { Filter = "Parameters (*.m3p)|*.m3p", DefaultExt = "m3p" };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        try
        {
            NativeCore.SaveM3p(dialog.FileName, _settings);
            _statusText.Text = $"Saved {Path.GetFileName(dialog.FileName)}";
        }
        catch (Mb3dException ex)
        {
            MessageBox.Show(this, ex.Message, "Save failed", MessageBoxButtons.OK,
                            MessageBoxIcon.Error);
        }
    }

    private void ExportExr()
    {
        if (_currentJob is null)
        {
            _statusText.Text = "Render something before exporting an EXR.";
            return;
        }

        using var dialog = new SaveFileDialog { Filter = "OpenEXR (*.exr)|*.exr", DefaultExt = "exr" };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        try
        {
            _currentJob.SaveExr(dialog.FileName, multilayer: true);
            _statusText.Text = $"Exported {Path.GetFileName(dialog.FileName)}";
        }
        catch (Mb3dException ex)
        {
            MessageBox.Show(this, ex.Message, "Export failed", MessageBoxButtons.OK,
                            MessageBoxIcon.Error);
        }
    }

    private enum MeshFormat { Stl, Step }

    private async Task ExportMeshAsync(MeshFormat format)
    {
        using var dialog = new SaveFileDialog
        {
            Filter = format == MeshFormat.Stl
                ? "Binary STL (*.stl)|*.stl"
                : "STEP AP214 (*.step)|*.step;*.stp",
            DefaultExt = format == MeshFormat.Stl ? "stl" : "step",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        string path = dialog.FileName;
        RenderSettings settings = _settings;
        MeshSettings meshSettings = MeshSettings.CreateDefault();

        _statusText.Text = "Extracting iso-surface…";
        try
        {
            MeshStats stats = await Task.Run(() =>
            {
                using MeshHandle mesh = _core.ExtractMesh(settings, meshSettings);
                if (format == MeshFormat.Stl) mesh.SaveStl(path, binary: true);
                else mesh.SaveStep(path);
                return mesh.GetStats();
            });

            // Surfacing watertightness matters: a non-manifold mesh will
            // be rejected or silently repaired by a slicer, and the user
            // should know which happened before they print.
            string integrity = stats.IsWatertight != 0
                ? "watertight"
                : $"NOT watertight ({stats.OpenEdgeCount} open edges)";

            _statusText.Text =
                $"Exported {stats.TriangleCount:N0} triangles — {integrity}, " +
                $"{stats.VolumeMm3 / 1000.0:F2} cm³";
        }
        catch (Mb3dException ex)
        {
            MessageBox.Show(this, ex.Message, "Mesh export failed", MessageBoxButtons.OK,
                            MessageBoxIcon.Error);
            _statusText.Text = "Mesh export failed";
        }
    }
}
