// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD Project Association AISBL               *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Base/BoundBox.h>
#include <Base/Vector3D.h>

#include <FCGlobal.h>

/** @file
 * Geometry kernel of the section view.
 *
 * The section view cuts the tessellation that is displayed in the 3D view instead
 * of the exact B-Rep geometry. This keeps the cap faces exactly aligned with the
 * clipped surfaces, works for any kind of object (shapes, meshes, links, ...) and
 * is fast enough to follow the section plane interactively.
 *
 * The functions in here have no dependency on Coin3D so they can be unit tested.
 */

namespace Gui::Section
{

/** A cutting plane.
 *
 * Points with a non-negative signed distance are kept, the others are cut away.
 * This matches the convention of SoClipPlane.
 */
struct GuiExport CutPlane
{
    /// Unit normal pointing into the kept half-space
    Base::Vector3d normal {0.0, 0.0, 1.0};
    /// Distance of the plane from the origin along the normal
    double distance {0.0};

    CutPlane() = default;
    CutPlane(const Base::Vector3d& base, const Base::Vector3d& dir);

    double signedDistance(const Base::Vector3d& pnt) const
    {
        return normal * pnt - distance;
    }

    /// The plane with the kept and removed half-spaces swapped
    CutPlane flipped() const;

    /// A point on the plane
    Base::Vector3d base() const
    {
        return normal * distance;
    }

    /** Two unit vectors spanning the plane.
     *
     * The axes are chosen such that \c u x \c v points into the removed half-space,
     * so polygons that are counter-clockwise in (u, v) face away from the material.
     */
    void getAxes(Base::Vector3d& uDir, Base::Vector3d& vDir) const;

    /// Returns -1 if the box is entirely removed, 1 if it is entirely kept, 0 if it is cut
    int classify(const Base::BoundBox3d& box) const;
};

/// A triangle mesh with shared vertices
struct GuiExport IndexedMesh
{
    std::vector<Base::Vector3d> points;
    std::vector<std::array<uint32_t, 3>> triangles;
    Base::BoundBox3d boundBox;

    /// Unique edges, filled by buildEdges()
    std::vector<std::array<uint32_t, 2>> edges;
    /// Edges of each triangle: (0,1), (1,2), (2,0)
    std::vector<std::array<uint32_t, 3>> triangleEdges;

    bool empty() const
    {
        return triangles.empty();
    }

    /// Computes the edge topology which speeds up slicing the mesh repeatedly
    void buildEdges();
};

/** Builds an indexed mesh from a triangle soup.
 *
 * The tessellation of a shape usually has its own copy of the vertices for every
 * face, and those copies don't always match exactly. Vertices closer than
 * @p tolerance are merged so that the sliced contours form closed loops.
 * Triangles that collapse while welding are dropped.
 *
 * @param soup three consecutive points per triangle
 * @param tolerance the merge distance
 */
GuiExport IndexedMesh weldTriangleSoup(const std::vector<Base::Vector3d>& soup, double tolerance);

/// Suggested welding tolerance for a mesh with the given extent
GuiExport double defaultWeldTolerance(const Base::BoundBox3d& box);

struct GuiExport Polyline
{
    std::vector<Base::Vector3d> points;
    bool closed {false};
};

struct GuiExport SliceResult
{
    /// The intersection contours. Only closed ones can be capped.
    std::vector<Polyline> contours;
    /// Number of contours that could not be closed (open meshes)
    std::size_t openContours {0};
};

/** Intersects a mesh with a plane.
 *
 * Vertices lying exactly on the plane are treated as kept which makes the
 * result topologically consistent even for planes passing through vertices.
 * Small gaps in the contours (cracks in the tessellation) up to @p gapTolerance
 * are bridged.
 */
GuiExport SliceResult sliceMesh(const IndexedMesh& mesh, const CutPlane& plane, double gapTolerance);

/// Cap face of a sectioned solid
struct GuiExport CapMesh
{
    std::vector<Base::Vector3d> vertices;
    /// Three vertex indices per triangle, counter-clockwise seen from the removed side
    std::vector<uint32_t> triangles;
    /// Closed outlines of the cap, as vertex index loops
    std::vector<std::vector<uint32_t>> outlines;

    bool empty() const
    {
        return triangles.empty();
    }
    double area() const;
};

/** Fills the closed contours lying in @p plane.
 *
 * Nested contours are resolved with the even-odd rule: a contour inside an
 * outer contour is a hole, a contour inside a hole is an island, and so on.
 * Open contours are ignored.
 */
GuiExport CapMesh buildCap(const std::vector<Polyline>& contours, const CutPlane& plane);

using Point2d = std::array<double, 2>;

/** Triangulates a simple polygon with holes.
 *
 * @param rings the first ring is the outer boundary, the others are holes.
 * Orientation doesn't matter.
 * @return three indices per triangle, referring to the points of all rings in
 * the order given. Triangles are counter-clockwise.
 */
GuiExport std::vector<uint32_t> triangulatePolygon(const std::vector<std::vector<Point2d>>& rings);

}  // namespace Gui::Section
