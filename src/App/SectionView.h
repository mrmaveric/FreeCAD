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
#include <optional>
#include <string>
#include <vector>

#include <Base/Placement.h>

#include "DocumentObject.h"
#include "PropertyGeo.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"
#include "PropertyUnits.h"

namespace App
{

/** A non-destructive section view of the document.
 *
 * The object only stores how the model is cut. The cutting itself is done by the
 * 3D view on the displayed geometry, so the model stays untouched. While the
 * object is visible the section is shown, and at most one section view of a
 * document is visible at a time.
 *
 * A section is either defined by up to three planes or by a box. Each plane is
 * defined by a base plane (one of the principal planes or a reference to a
 * planar face, datum plane, sketch etc.), an offset along its normal and two
 * tilt angles. The normal of a plane points to the side that is cut away.
 */
class AppExport SectionView: public DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::SectionView);

public:
    SectionView();
    ~SectionView() override;

    static constexpr int MaxPlanes = 3;

    // Section
    PropertyEnumeration SectionType;
    PropertyEnumeration Combination;

    // Planes, indexed 0 to 2
    PropertyBool Plane2Enabled;
    PropertyBool Plane3Enabled;
    PropertyEnumeration Plane1Base;
    PropertyEnumeration Plane2Base;
    PropertyEnumeration Plane3Base;
    PropertyLinkSubGlobal Plane1Reference;
    PropertyLinkSubGlobal Plane2Reference;
    PropertyLinkSubGlobal Plane3Reference;
    PropertyDistance Plane1Offset;
    PropertyDistance Plane2Offset;
    PropertyDistance Plane3Offset;
    PropertyAngle Plane1AngleX;
    PropertyAngle Plane2AngleX;
    PropertyAngle Plane3AngleX;
    PropertyAngle Plane1AngleY;
    PropertyAngle Plane2AngleY;
    PropertyAngle Plane3AngleY;
    PropertyBool Plane1Flipped;
    PropertyBool Plane2Flipped;
    PropertyBool Plane3Flipped;

    // Box
    PropertyPlacement BoxPlacement;
    PropertyLength BoxLength;
    PropertyLength BoxWidth;
    PropertyLength BoxHeight;

    // Objects
    PropertyEnumeration ObjectMode;
    PropertyLinkSubListHidden Objects;

    // Output
    PropertyPlacementList Planes;

    enum class Type
    {
        Planes = 0,
        Box = 1,
    };

    enum class CombinationMode
    {
        /// The region on the cut side of all planes is removed, e.g. a quarter cut
        RemoveCommon = 0,
        /// Only the region on the kept side of all planes remains, e.g. a slab
        KeepCommon = 1,
    };

    enum class BasePlane
    {
        XY = 0,
        XZ = 1,
        YZ = 2,
        Reference = 3,
    };

    enum class ObjectsMode
    {
        /// The listed objects are not cut
        Exclude = 0,
        /// Only the listed objects are cut
        SectionOnly = 1,
    };

    /// A cutting plane: a point on it and the direction of the removed side
    struct Plane
    {
        Base::Vector3d base;
        Base::Vector3d normal;
        /// Orientation of the plane, its Z axis being the normal
        Base::Placement placement;
    };

    Type getType() const;
    CombinationMode getCombination() const;
    ObjectsMode getObjectsMode() const;

    /// Number of planes, 1 to 3, or 6 for the box
    int countPlanes() const;
    bool isPlaneEnabled(int index) const;
    BasePlane getBasePlane(int index) const;

    /** The cutting planes of the section.
     *
     * Planes whose reference can't be resolved fall back to their principal plane.
     */
    std::vector<Plane> getPlanes() const;

    /** Placement of the base plane of a plane definition, before offset and tilt.
     * @param index plane index from 0 to 2
     * @param error set if the reference can't be used
     */
    Base::Placement getBasePlacement(int index, std::string* error = nullptr) const;

    /// Placement of plane @p index including offset, tilt and flip
    Base::Placement getPlanePlacement(int index) const;
    /// Placement of plane @p index applying offset, tilt and flip to @p base
    Base::Placement getPlanePlacement(int index, const Base::Placement& base) const;

    /** Computes the plane of a reference.
     *
     * Planar faces give their plane with the outward normal, objects without a
     * sub-element like datum planes or sketches give the XY plane of their placement.
     */
    static std::optional<Base::Placement> getReferencePlane(
        const DocumentObject* obj,
        const std::string& subname,
        std::string* error = nullptr
    );

    /// Rotation of a principal plane, the Z axis pointing to the removed side
    static Base::Rotation getPrincipalRotation(BasePlane base);

    PropertyEnumeration& getBaseProperty(int index);
    PropertyLinkSubGlobal& getReferenceProperty(int index);
    PropertyDistance& getOffsetProperty(int index);
    PropertyAngle& getAngleXProperty(int index);
    PropertyAngle& getAngleYProperty(int index);
    PropertyBool& getFlippedProperty(int index);

    short mustExecute() const override;
    DocumentObjectExecReturn* execute() override;
    void onDocumentRestored() override;
    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderSectionView";
    }

protected:
    void onChanged(const Property* prop) override;

private:
    void updatePlanes();

    static const char* TypeEnums[];
    static const char* CombinationEnums[];
    static const char* BaseEnums[];
    static const char* ObjectModeEnums[];
};

}  // namespace App
