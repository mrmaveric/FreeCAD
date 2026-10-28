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

#include <cmath>
#include <numbers>

#include <Base/Interpreter.h>
#include <Base/Tools.h>

#include "ComplexGeoData.h"
#include "ComplexGeoDataPy.h"
#include "ElementNamingUtils.h"
#include "SectionView.h"

using namespace App;

PROPERTY_SOURCE(App::SectionView, App::DocumentObject)

// clang-format off
const char* SectionView::TypeEnums[] = {"Planes", "Box", nullptr};
const char* SectionView::CombinationEnums[] = {"Remove intersection", "Keep intersection", nullptr};
const char* SectionView::BaseEnums[] = {"XY", "XZ", "YZ", "Reference", nullptr};
const char* SectionView::ObjectModeEnums[] = {"Exclude", "Section only", nullptr};
// clang-format on

#define ADD_PLANE_PROPERTIES(N, BASE, GROUP) \
    ADD_PROPERTY_TYPE( \
        Plane##N##Base, \
        (long(BASE)), \
        GROUP, \
        Prop_None, \
        "The plane the section plane is based on" \
    ); \
    Plane##N##Base.setEnums(BaseEnums); \
    ADD_PROPERTY_TYPE( \
        Plane##N##Reference, \
        (nullptr), \
        GROUP, \
        Prop_None, \
        "Planar face, datum plane or other object defining the base plane" \
    ); \
    ADD_PROPERTY_TYPE( \
        Plane##N##Offset, \
        (0.0), \
        GROUP, \
        Prop_None, \
        "Distance of the section plane from its base plane" \
    ); \
    ADD_PROPERTY_TYPE( \
        Plane##N##AngleX, \
        (0.0), \
        GROUP, \
        Prop_None, \
        "Rotation of the section plane around the X axis of its base plane" \
    ); \
    ADD_PROPERTY_TYPE( \
        Plane##N##AngleY, \
        (0.0), \
        GROUP, \
        Prop_None, \
        "Rotation of the section plane around the Y axis of its base plane" \
    ); \
    ADD_PROPERTY_TYPE(Plane##N##Flipped, (false), GROUP, Prop_None, "Cut away the other side of the plane")

SectionView::SectionView()
{
    ADD_PROPERTY_TYPE(
        SectionType,
        (long(Type::Planes)),
        "Section",
        Prop_None,
        "Planes: cut with up to three planes\nBox: show only what is inside a box"
    );
    SectionType.setEnums(TypeEnums);
    ADD_PROPERTY_TYPE(
        Combination,
        (long(CombinationMode::RemoveCommon)),
        "Section",
        Prop_None,
        "How several planes are combined\n"
        "Remove intersection: cut away the region on the removed side of all planes, "
        "e.g. a quarter of the model\n"
        "Keep intersection: show only the region on the kept side of all planes, e.g. a slab"
    );
    Combination.setEnums(CombinationEnums);

    ADD_PLANE_PROPERTIES(1, BasePlane::XZ, "Plane 1");
    ADD_PROPERTY_TYPE(Plane2Enabled, (false), "Plane 2", Prop_None, "Use a second section plane");
    ADD_PLANE_PROPERTIES(2, BasePlane::YZ, "Plane 2");
    ADD_PROPERTY_TYPE(Plane3Enabled, (false), "Plane 3", Prop_None, "Use a third section plane");
    ADD_PLANE_PROPERTIES(3, BasePlane::XY, "Plane 3");

    ADD_PROPERTY_TYPE(
        BoxPlacement,
        (Base::Placement()),
        "Box",
        Prop_None,
        "Position and orientation of the box"
    );
    ADD_PROPERTY_TYPE(BoxLength, (10.0), "Box", Prop_None, "Size of the box along its X axis");
    ADD_PROPERTY_TYPE(BoxWidth, (10.0), "Box", Prop_None, "Size of the box along its Y axis");
    ADD_PROPERTY_TYPE(BoxHeight, (10.0), "Box", Prop_None, "Size of the box along its Z axis");

    ADD_PROPERTY_TYPE(
        ObjectMode,
        (long(ObjectsMode::Exclude)),
        "Objects",
        Prop_None,
        "Exclude: the listed objects are not cut\nSection only: only the listed objects are cut"
    );
    ObjectMode.setEnums(ObjectModeEnums);
    ADD_PROPERTY_TYPE(
        Objects,
        (nullptr),
        "Objects",
        Prop_None,
        "Objects excluded from the section, or the only ones cut, depending on ObjectMode"
    );

    ADD_PROPERTY_TYPE(
        Planes,
        (),
        "Section",
        PropertyType(Prop_Output | Prop_ReadOnly | Prop_Transient | Prop_NoRecompute),
        "The resulting cutting planes. The Z axis of each placement points to the side "
        "that is cut away."
    );
    updatePlanes();
}

SectionView::~SectionView() = default;

SectionView::Type SectionView::getType() const
{
    return static_cast<Type>(SectionType.getValue());
}

SectionView::CombinationMode SectionView::getCombination() const
{
    return static_cast<CombinationMode>(Combination.getValue());
}

SectionView::ObjectsMode SectionView::getObjectsMode() const
{
    return static_cast<ObjectsMode>(ObjectMode.getValue());
}

int SectionView::countPlanes() const
{
    if (getType() == Type::Box) {
        return 6;
    }
    int count = 0;
    for (int i = 0; i < MaxPlanes; ++i) {
        if (isPlaneEnabled(i)) {
            ++count;
        }
    }
    return count;
}

bool SectionView::isPlaneEnabled(int index) const
{
    switch (index) {
        case 0:
            return true;
        case 1:
            return Plane2Enabled.getValue();
        case 2:
            return Plane3Enabled.getValue();
        default:
            return false;
    }
}

SectionView::BasePlane SectionView::getBasePlane(int index) const
{
    return static_cast<BasePlane>(
        const_cast<SectionView*>(this)->getBaseProperty(index).getValue()  // NOLINT
    );
}

PropertyEnumeration& SectionView::getBaseProperty(int index)
{
    std::array<PropertyEnumeration*, MaxPlanes> props {&Plane1Base, &Plane2Base, &Plane3Base};
    return *props.at(index);
}

PropertyLinkSubGlobal& SectionView::getReferenceProperty(int index)
{
    std::array<PropertyLinkSubGlobal*, MaxPlanes> props {
        &Plane1Reference,
        &Plane2Reference,
        &Plane3Reference
    };
    return *props.at(index);
}

PropertyDistance& SectionView::getOffsetProperty(int index)
{
    std::array<PropertyDistance*, MaxPlanes> props {&Plane1Offset, &Plane2Offset, &Plane3Offset};
    return *props.at(index);
}

PropertyAngle& SectionView::getAngleXProperty(int index)
{
    std::array<PropertyAngle*, MaxPlanes> props {&Plane1AngleX, &Plane2AngleX, &Plane3AngleX};
    return *props.at(index);
}

PropertyAngle& SectionView::getAngleYProperty(int index)
{
    std::array<PropertyAngle*, MaxPlanes> props {&Plane1AngleY, &Plane2AngleY, &Plane3AngleY};
    return *props.at(index);
}

PropertyBool& SectionView::getFlippedProperty(int index)
{
    std::array<PropertyBool*, MaxPlanes> props {&Plane1Flipped, &Plane2Flipped, &Plane3Flipped};
    return *props.at(index);
}

Base::Rotation SectionView::getPrincipalRotation(BasePlane base)
{
    // The normals point towards the viewer of the matching standard view, so
    // the section shows the material behind the plane.
    switch (base) {
        case BasePlane::XZ:
            // Front view: normal -Y
            return Base::Rotation(Base::Vector3d(1, 0, 0), std::numbers::pi / 2.0);
        case BasePlane::YZ:
            // Right view: normal +X
            return Base::Rotation(Base::Vector3d(1, 1, 1), 2.0 * std::numbers::pi / 3.0);
        case BasePlane::XY:
        case BasePlane::Reference:
        default:
            // Top view: normal +Z
            return {};
    }
}

std::optional<Base::Placement> SectionView::getReferencePlane(
    const DocumentObject* obj,
    const std::string& subname,
    std::string* error
)
{
    auto setError = [error](const char* msg) {
        if (error) {
            *error = msg;
        }
    };

    if (!obj || !obj->isAttachedToDocument()) {
        setError("No reference");
        return std::nullopt;
    }

    const char* element = Data::findElementName(subname.c_str());
    if (!element || !*element) {
        // Datum planes, sketches and other objects: their XY plane
        Base::Matrix4D matrix;
        if (!obj->getSubObject(subname.c_str(), nullptr, &matrix)) {
            setError("Reference not found");
            return std::nullopt;
        }
        // The matrix may contain scaling, so only take its axes
        Base::Vector3d xDir(matrix[0][0], matrix[1][0], matrix[2][0]);
        Base::Vector3d zDir(matrix[0][2], matrix[1][2], matrix[2][2]);
        const Base::Vector3d origin(matrix[0][3], matrix[1][3], matrix[2][3]);
        return Base::Placement(
            origin,
            Base::Rotation::makeRotationByAxes(xDir, Base::Vector3d(), zDir, "ZXY")
        );
    }

    // A face: get its geometry in global coordinates
    Base::PyGILStateLocker lock;
    PyObject* pyObj = nullptr;
    Base::Matrix4D matrix;
    try {
        obj->getSubObject(subname.c_str(), &pyObj, &matrix);
    }
    catch (const Base::Exception& e) {
        setError(e.what());
        return std::nullopt;
    }
    catch (...) {
        setError("Reference can't be evaluated");
        return std::nullopt;
    }
    if (!pyObj) {
        setError("Reference not found");
        return std::nullopt;
    }
    Py::Object guard(pyObj, true);
    if (!PyObject_TypeCheck(pyObj, &Data::ComplexGeoDataPy::Type)) {
        setError("Reference has no geometry");
        return std::nullopt;
    }

    const Data::ComplexGeoData* data
        = static_cast<Data::ComplexGeoDataPy*>(pyObj)->getComplexGeoDataPtr();
    std::vector<Base::Vector3d> points;
    std::vector<Data::ComplexGeoData::Facet> facets;
    try {
        // Planar faces are tessellated exactly, so a coarse accuracy is enough
        const Base::BoundBox3d bounds = data->getBoundBox();
        const double accuracy = bounds.IsValid() ? std::max(bounds.CalcDiagonalLength() * 0.05, 1e-3)
                                                 : 0.1;
        data->getFaces(points, facets, accuracy);
    }
    catch (const Base::Exception& e) {
        setError(e.what());
        return std::nullopt;
    }
    catch (...) {
        setError("Reference can't be evaluated");
        return std::nullopt;
    }
    if (facets.empty()) {
        setError("Reference is not a planar face");
        return std::nullopt;
    }

    // Area weighted normal and centroid of the tessellation
    Base::Vector3d normal;
    Base::Vector3d centroid;
    double area = 0.0;
    for (const auto& facet : facets) {
        const auto& a = points[facet.I1];
        const auto& b = points[facet.I2];
        const auto& c = points[facet.I3];
        const Base::Vector3d cross = (b - a) % (c - a);
        const double facetArea = cross.Length() * 0.5;
        normal += cross;
        centroid += (a + b + c) * (facetArea / 3.0);
        area += facetArea;
    }
    if (area <= 0.0 || normal.Length() <= 0.0) {
        setError("Reference is not a planar face");
        return std::nullopt;
    }
    normal.Normalize();
    centroid /= area;

    Base::BoundBox3d box;
    for (const auto& pnt : points) {
        box.Add(pnt);
    }
    const double tolerance = std::max(box.CalcDiagonalLength() * 1e-6, 1e-7);
    for (const auto& pnt : points) {
        if (std::abs((pnt - centroid) * normal) > tolerance) {
            setError("Reference is not a planar face");
            return std::nullopt;
        }
    }

    // Align the X axis of the plane with a global axis when possible
    Base::Vector3d xDir(1, 0, 0);
    if (std::abs(xDir * normal) > 0.9) {
        xDir = Base::Vector3d(0, 1, 0);
    }
    xDir = xDir - normal * (xDir * normal);
    xDir.Normalize();
    return Base::Placement(
        centroid,
        Base::Rotation::makeRotationByAxes(xDir, normal % xDir, normal, "ZXY")
    );
}

Base::Placement SectionView::getBasePlacement(int index, std::string* error) const
{
    const BasePlane base = getBasePlane(index);
    if (base == BasePlane::Reference) {
        auto& ref = const_cast<SectionView*>(this)->getReferenceProperty(index);  // NOLINT
        const auto& subs = ref.getSubValues();
        const std::string subname = subs.empty() ? std::string() : subs.front();
        if (auto placement = getReferencePlane(ref.getValue(), subname, error)) {
            return *placement;
        }
        // Fall back to the XY plane
        return {};
    }
    return {Base::Vector3d(), getPrincipalRotation(base)};
}

Base::Placement SectionView::getPlanePlacement(int index) const
{
    return getPlanePlacement(index, getBasePlacement(index));
}

Base::Placement SectionView::getPlanePlacement(int index, const Base::Placement& base) const
{
    auto self = const_cast<SectionView*>(this);  // NOLINT
    const Base::Rotation tilt = Base::Rotation(
                                    Base::Vector3d(1, 0, 0),
                                    Base::toRadians(self->getAngleXProperty(index).getValue())
                                )
        * Base::Rotation(Base::Vector3d(0, 1, 0),
                         Base::toRadians(self->getAngleYProperty(index).getValue()));
    Base::Placement placement = base * Base::Placement(Base::Vector3d(), tilt)
        * Base::Placement(Base::Vector3d(0, 0, self->getOffsetProperty(index).getValue()),
                          Base::Rotation());
    if (self->getFlippedProperty(index).getValue()) {
        placement = placement
            * Base::Placement(
                        Base::Vector3d(),
                        Base::Rotation(Base::Vector3d(1, 0, 0), std::numbers::pi)
            );
    }
    return placement;
}

std::vector<SectionView::Plane> SectionView::getPlanes() const
{
    std::vector<Plane> planes;
    auto addPlane = [&planes](const Base::Placement& placement) {
        Plane plane;
        plane.placement = placement;
        plane.base = placement.getPosition();
        placement.getRotation().multVec(Base::Vector3d(0, 0, 1), plane.normal);
        planes.push_back(plane);
    };

    if (getType() == Type::Box) {
        const Base::Placement& box = BoxPlacement.getValue();
        const std::array<double, 3> size {
            BoxLength.getValue(),
            BoxWidth.getValue(),
            BoxHeight.getValue()
        };
        // Rotations taking Z to +X, +Y and +Z
        const std::array<Base::Rotation, 3> toAxis {
            Base::Rotation(Base::Vector3d(0, 1, 0), std::numbers::pi / 2.0),
            Base::Rotation(Base::Vector3d(1, 0, 0), -std::numbers::pi / 2.0),
            Base::Rotation(),
        };
        const Base::Rotation flip(Base::Vector3d(1, 0, 0), std::numbers::pi);
        for (int axis = 0; axis < 3; ++axis) {
            Base::Vector3d far;
            far[axis] = size[axis];
            // The removed side is outside the box
            addPlane(box * Base::Placement(Base::Vector3d(), toAxis[axis] * flip));
            addPlane(box * Base::Placement(far, toAxis[axis]));
        }
        return planes;
    }

    for (int i = 0; i < MaxPlanes; ++i) {
        if (isPlaneEnabled(i)) {
            addPlane(getPlanePlacement(i));
        }
    }
    return planes;
}

short SectionView::mustExecute() const
{
    for (int i = 0; i < MaxPlanes; ++i) {
        if (const_cast<SectionView*>(this)->getReferenceProperty(i).isTouched()) {  // NOLINT
            return 1;
        }
    }
    return DocumentObject::mustExecute();
}

void SectionView::onChanged(const Property* prop)
{
    if (prop != &Planes && prop != &Label && prop != &Label2 && prop != &Visibility
        && !isRestoring()) {
        updatePlanes();
    }
    DocumentObject::onChanged(prop);
}

void SectionView::onDocumentRestored()
{
    DocumentObject::onDocumentRestored();
    updatePlanes();
}

void SectionView::updatePlanes()
{
    std::vector<Base::Placement> placements;
    for (const auto& plane : getPlanes()) {
        placements.push_back(plane.placement);
    }
    Planes.setValues(placements);
}

DocumentObjectExecReturn* SectionView::execute()
{
    updatePlanes();

    // Only check that the references are usable, the 3D view does the work
    for (int i = 0; i < MaxPlanes; ++i) {
        if (!isPlaneEnabled(i) || getBasePlane(i) != BasePlane::Reference) {
            continue;
        }
        std::string error;
        getBasePlacement(i, &error);
        if (!error.empty()) {
            return new DocumentObjectExecReturn("Plane " + std::to_string(i + 1) + ": " + error, this);
        }
    }
    return DocumentObject::StdReturn;
}
