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

#include <memory>
#include <string>
#include <vector>

#include <App/DocumentObserver.h>
#include <Base/Color.h>

#include "SectionGeometry.h"

class SoPath;

namespace Gui
{
class Document;
class View3DInventorViewer;
}  // namespace Gui

namespace Gui::Section
{

/// How the half-spaces of several planes are combined
enum class Combination
{
    /// Only the material on the kept side of all planes remains (slab, box)
    KeepCommon,
    /// The material on the cut side of all planes is removed (quarter or corner cut)
    RemoveCommon,
};

/// Display settings of the cut faces
struct GuiExport DisplaySettings
{
    enum class FillColor
    {
        /// Each cap uses the color of the object it belongs to
        Object,
        /// All caps use the same color
        Uniform,
    };

    enum class Hatch
    {
        None,
        Lines,
        Cross,
    };

    bool showCaps {true};
    FillColor fillColor {FillColor::Object};
    Base::Color uniformColor {0.80F, 0.80F, 0.80F};
    Hatch hatch {Hatch::Lines};
    /// Distance of the hatch lines in model units, 0 for automatic
    double hatchSpacing {0.0};
    /// Angle of the hatch lines in degrees
    double hatchAngle {45.0};
    /// Coarsen or refine the spacing in steps of two to keep the on-screen density readable
    bool adaptiveHatch {true};
    /// Alternate angle and spacing between neighboring objects, like on drawings
    bool varyHatch {true};
    float hatchLineWidth {1.0F};
    bool showOutline {true};
    Base::Color outlineColor {0.10F, 0.10F, 0.10F};
    float outlineWidth {2.0F};

    bool operator==(const DisplaySettings&) const = default;
};

/// Statistics of the current section, for display in the UI
struct GuiExport SectionStats
{
    std::size_t objects {0};
    std::size_t sectioned {0};
    std::size_t capped {0};
    /// Objects with contours that couldn't be closed, i.e. open meshes or surfaces
    std::size_t open {0};
};

/** The section view of a document.
 *
 * Collects the tessellation displayed in the 3D views, cuts it with the section
 * planes and builds the cap faces. The resulting scene is shared by all 3D views
 * of the document.
 *
 * Several scenes can be shown at once. Each object is cut by the scene shown last
 * among those that cut it.
 */
class GuiExport SectionScene
{
public:
    explicit SectionScene(Gui::Document* doc);
    ~SectionScene();

    SectionScene(const SectionScene&) = delete;
    SectionScene& operator=(const SectionScene&) = delete;

    /** Sets the cutting planes.
     *
     * A plane change only re-slices the cached geometry, so this is fast enough
     * to be called while dragging.
     */
    void setPlanes(const std::vector<CutPlane>& planes, Combination combination);
    const std::vector<CutPlane>& getPlanes() const;

    void setDisplaySettings(const DisplaySettings& settings);
    const DisplaySettings& getDisplaySettings() const;

    /** Sets the objects that are treated specially.
     *
     * @param items objects, given by their top level object and sub-object path
     * @param sectionListed if true only the listed objects are cut, otherwise they
     * are excluded from the section.
     */
    void setFilter(const std::vector<App::SubObjectT>& items, bool sectionListed);

    /// Makes the section visible in all 3D views of the document, in front of other sections
    void attach();
    /// Removes the section from all 3D views
    void detach();
    bool isAttached() const;

    /// Re-collects the displayed geometry later
    void invalidateGeometry();
    /// Re-collects the displayed geometry now
    void updateGeometry();

    SectionStats getStats() const;

    /// Bounding box of the geometry that can be sectioned
    Base::BoundBox3d getBoundBox() const;

    /** Returns the object a picked cap face belongs to.
     * @param path the picked path
     * @param subname set to the sub-object path relative to the returned object
     */
    App::DocumentObject* getPickedObject(const SoPath* path, std::string& subname) const;

    /// Whether both scenes cut at least one same object, as far as their geometry is collected
    bool cutsSameObjects(const SectionScene& other) const;

    /// The section scenes that are currently attached to @p viewer
    static std::vector<SectionScene*> getAttached(const View3DInventorViewer* viewer);

    /** Resolves a pick on a cap face of any section scene.
     * @see getPickedObject()
     */
    static App::DocumentObject* resolvePickedCap(const SoPath* path, std::string& subname);

private:
    class Private;
    std::unique_ptr<Private> d;
};

/** Computes hatch line segments filling a set of rings with the even-odd rule.
 *
 * @param rings closed polygons in 2D
 * @param angle direction of the lines in radians
 * @param spacing distance between the lines
 * @param phase offset of the line pattern, in multiples of the spacing
 * @return two points per segment
 */
GuiExport std::vector<Point2d> hatchRings(
    const std::vector<std::vector<Point2d>>& rings,
    double angle,
    double spacing,
    double phase
);

}  // namespace Gui::Section
