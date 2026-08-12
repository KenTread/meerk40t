// SPDX-License-Identifier: LicenseRef-OmniFractal-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// StepExporter.cpp -- ISO 10303-21 (STEP AP214) export via OpenCASCADE.
//
// A fractal iso-surface is a triangle soup, and STEP is a B-Rep format,
// so the conversion goes:
//     triangles -> TopoDS_Face per triangle (sewn) -> TopoDS_Shell
//     -> TopoDS_Solid -> STEPControl_Writer
//
// Sewing is the expensive step and the one that can fail. We give it a
// tolerance derived from the mesh's own bounding box rather than a fixed
// constant, because these meshes are authored in fractal-space units
// that can be anywhere from 1e-3 to 1e3 across.
//
// When OCCT is not available the entry point returns OMF_ERR_UNSUPPORTED
// rather than silently writing a different format.

#include "omf/MarchingCubes.hpp"

#if defined(OMF_WITH_OPENCASCADE)

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRep_Builder.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>

namespace omf {

void write_step(const Mesh& mesh, const std::string& path, float scale_to_mm) {
    require(!mesh.indices.empty(), OMF_ERR_GEOMETRY, "cannot export an empty mesh to STEP");

    const double k = scale_to_mm > 0.0f ? static_cast<double>(scale_to_mm) : 1.0;

    // Bounding diagonal drives the sewing tolerance.
    Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (std::size_t v = 0; v < mesh.vertex_count(); ++v) {
        const Vec3 p{mesh.positions[v * 3] * k, mesh.positions[v * 3 + 1] * k,
                     mesh.positions[v * 3 + 2] * k};
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const double diagonal = length(hi - lo);
    const double tolerance = std::max(diagonal * 1e-6, 1e-7);

    BRepBuilderAPI_Sewing sewing(tolerance);
    sewing.SetNonManifoldMode(Standard_False);

    std::size_t skipped = 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1],
                            ic = mesh.indices[i + 2];

        const gp_Pnt a(mesh.positions[ia * 3] * k, mesh.positions[ia * 3 + 1] * k,
                       mesh.positions[ia * 3 + 2] * k);
        const gp_Pnt b(mesh.positions[ib * 3] * k, mesh.positions[ib * 3 + 1] * k,
                       mesh.positions[ib * 3 + 2] * k);
        const gp_Pnt c(mesh.positions[ic * 3] * k, mesh.positions[ic * 3 + 1] * k,
                       mesh.positions[ic * 3 + 2] * k);

        // Degenerate facets make MakeFace throw; drop them instead.
        if (a.Distance(b) < tolerance || b.Distance(c) < tolerance ||
            a.Distance(c) < tolerance) {
            ++skipped;
            continue;
        }

        try {
            BRepBuilderAPI_MakePolygon polygon(a, b, c, Standard_True);
            if (!polygon.IsDone()) { ++skipped; continue; }
            BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
            if (!face.IsDone()) { ++skipped; continue; }
            sewing.Add(face.Face());
        } catch (const Standard_Failure&) {
            ++skipped;
        }
    }

    require(skipped < mesh.triangle_count(), OMF_ERR_GEOMETRY,
            "every facet was rejected during STEP face construction");

    sewing.Perform();
    TopoDS_Shape sewn = sewing.SewedShape();
    require(!sewn.IsNull(), OMF_ERR_GEOMETRY, "OCCT sewing produced a null shape");

    // Promote closed shells to solids so downstream CAD sees a body
    // rather than a surface set. Open shells are still exported -- a
    // fractal cropped by the bounding box legitimately has boundaries.
    TopoDS_Shape result = sewn;
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    int solids = 0, shells = 0;
    for (TopExp_Explorer explorer(sewn, TopAbs_SHELL); explorer.More(); explorer.Next()) {
        const TopoDS_Shell shell = TopoDS::Shell(explorer.Current());
        ++shells;
        if (shell.Closed()) {
            try {
                BRepBuilderAPI_MakeSolid solid(shell);
                if (solid.IsDone()) {
                    builder.Add(compound, solid.Solid());
                    ++solids;
                    continue;
                }
            } catch (const Standard_Failure&) {
                // fall through and add the shell as-is
            }
        }
        builder.Add(compound, shell);
    }
    if (shells > 0) result = compound;

    STEPControl_Writer writer;
    // AP214 is the interchange schema every mainstream CAD package reads.
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    Interface_Static::SetCVal("write.step.product.name", "Mandelbulb3D_v2_Export");
    // Model is already scaled to millimetres by `scale_to_mm`.
    Interface_Static::SetCVal("write.step.unit", "MM");

    const IFSelect_ReturnStatus transferred = writer.Transfer(result, STEPControl_AsIs);
    require(transferred == IFSelect_RetDone, OMF_ERR_GEOMETRY,
            "STEPControl_Writer::Transfer failed");

    const IFSelect_ReturnStatus written = writer.Write(path.c_str());
    require(written == IFSelect_RetDone, OMF_ERR_IO,
            "STEPControl_Writer::Write failed for '" + path + "'");

    (void)solids;
}

}  // namespace omf

#else  // !OMF_WITH_OPENCASCADE

namespace omf {

void write_step(const Mesh&, const std::string&, float) {
    fail(OMF_ERR_UNSUPPORTED,
         "STEP export requires OpenCASCADE; rebuild with -DOMF_WITH_OPENCASCADE=ON");
}

}  // namespace omf

#endif
