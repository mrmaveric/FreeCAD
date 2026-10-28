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

#include <QMenu>
#include <QTimer>

#include <Inventor/actions/SoGetBoundingBoxAction.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoShapeHints.h>
#include <Inventor/nodes/SoSwitch.h>

#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <App/SectionView.h>

#include "ActionFunction.h"
#include "Application.h"
#include "BitmapFactory.h"
#include "Command.h"
#include "Control.h"
#include "Document.h"
#include "DocumentObserver.h"
#include "TaskSectionView.h"
#include "View3DInventor.h"
#include "View3DInventorViewer.h"
#include "ViewProviderSectionView.h"

using namespace Gui;

PROPERTY_SOURCE(Gui::ViewProviderSectionView, Gui::ViewProviderDragger)

const char* ViewProviderSectionView::FillModeEnums[]
    = {"Object color", "Uniform color", "None", nullptr};
const char* ViewProviderSectionView::HatchEnums[] = {"None", "Lines", "Cross", nullptr};
App::PropertyFloatConstraint::Constraints ViewProviderSectionView::lineWidthRange = {0.5, 10.0, 0.5};

namespace
{
SbVec3f toSb(const Base::Vector3d& vec)
{
    return {static_cast<float>(vec.x), static_cast<float>(vec.y), static_cast<float>(vec.z)};
}
}  // namespace

ViewProviderSectionView::ViewProviderSectionView()
{
    static const char* group = "Section";
    ADD_PROPERTY_TYPE(
        FillMode,
        (0L),
        group,
        App::Prop_None,
        "Color of the cut faces, or None to only clip the model"
    );
    FillMode.setEnums(FillModeEnums);
    ADD_PROPERTY_TYPE(
        FillColor,
        (Base::Color(0.80F, 0.80F, 0.80F)),
        group,
        App::Prop_None,
        "Color of the cut faces in 'Uniform color' mode"
    );
    ADD_PROPERTY_TYPE(HatchPattern, (1L), group, App::Prop_None, "Hatch pattern of the cut faces");
    HatchPattern.setEnums(HatchEnums);
    ADD_PROPERTY_TYPE(
        HatchSpacing,
        (0.0),
        group,
        App::Prop_None,
        "Distance between the hatch lines, 0 for automatic"
    );
    ADD_PROPERTY_TYPE(HatchAngle, (45.0), group, App::Prop_None, "Angle of the hatch lines");
    ADD_PROPERTY_TYPE(
        HatchAdaptive,
        (true),
        group,
        App::Prop_None,
        "Adapt the hatch spacing in steps of two while zooming so it stays readable"
    );
    ADD_PROPERTY_TYPE(
        HatchVary,
        (true),
        group,
        App::Prop_None,
        "Use different hatch angles and spacings for neighboring objects"
    );
    ADD_PROPERTY_TYPE(HatchLineWidth, (1.0), group, App::Prop_None, "Line width of the hatch");
    HatchLineWidth.setConstraints(&lineWidthRange);
    ADD_PROPERTY_TYPE(ShowOutline, (true), group, App::Prop_None, "Draw the outline of the cut faces");
    ADD_PROPERTY_TYPE(
        OutlineColor,
        (Base::Color(0.10F, 0.10F, 0.10F)),
        group,
        App::Prop_None,
        "Color of the outline of the cut faces"
    );
    ADD_PROPERTY_TYPE(OutlineWidth, (2.0), group, App::Prop_None, "Line width of the outline");
    OutlineWidth.setConstraints(&lineWidthRange);
    ADD_PROPERTY_TYPE(
        ShowPlanes,
        (false),
        group,
        App::Prop_None,
        "Show the section planes also when not editing the section"
    );

    // Properties of the transform dragger that don't apply
    TransformOrigin.setStatus(App::Property::Hidden, true);
    ShowPlacement.setStatus(App::Property::Hidden, true);

    planeSwitch = new SoSwitch;
    planeSwitch->whichChild = SO_SWITCH_NONE;
    planeVisuals = new SoSeparator;
    planeSwitch->addChild(planeVisuals);

    sPixmap = "Std_SectionView";
}

ViewProviderSectionView::~ViewProviderSectionView()
{
    scene.reset();
}

void ViewProviderSectionView::attach(App::DocumentObject* obj)
{
    ViewProviderDragger::attach(obj);

    auto root = new SoSeparator;
    auto pickStyle = new SoPickStyle;
    pickStyle->style = SoPickStyle::UNPICKABLE;
    root->addChild(pickStyle);
    root->addChild(planeSwitch);
    addDisplayMaskMode(root, "Section");

    // A new section view is visible from the start without show() being called
    scheduleActivation();
}

std::vector<std::string> ViewProviderSectionView::getDisplayModes() const
{
    return {"Section"};
}

void ViewProviderSectionView::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("Section");
    ViewProviderDragger::setDisplayMode(mode);
}

App::SectionView* ViewProviderSectionView::getSectionView() const
{
    return freecad_cast<App::SectionView*>(getObject());
}

Section::SectionScene* ViewProviderSectionView::getScene() const
{
    return scene.get();
}

void ViewProviderSectionView::scheduleActivation()
{
    QTimer::singleShot(0, [objT = App::DocumentObjectT(getObject())]() {
        auto self = freecad_cast<ViewProviderSectionView*>(
            Application::Instance->getViewProvider(objT.getObject())
        );
        if (self && self->isShow() && !self->scene) {
            self->activate();
        }
    });
}

void ViewProviderSectionView::show()
{
    ViewProviderDragger::show();
    activate();
}

void ViewProviderSectionView::beforeDelete()
{
    // The object may be kept for undo, but its section must go
    deactivate();
    ViewProviderDragger::beforeDelete();
}

void ViewProviderSectionView::reattach(App::DocumentObject* obj)
{
    ViewProviderDragger::reattach(obj);
    scheduleActivation();
}

void ViewProviderSectionView::hide()
{
    deactivate();
    ViewProviderDragger::hide();
}

void ViewProviderSectionView::finishRestoring()
{
    ViewProviderDragger::finishRestoring();
    if (isShow()) {
        activate();
    }
}

void ViewProviderSectionView::activate()
{
    auto obj = getSectionView();
    if (!obj || !getDocument()) {
        return;
    }
    if (obj->isRestoring() || obj->getDocument()->testStatus(App::Document::Restoring)) {
        // finishRestoring() takes care of it
        return;
    }

    // Only one section is shown at a time
    for (auto other : obj->getDocument()->getObjectsOfType<App::SectionView>()) {
        if (other != obj && other->Visibility.getValue()) {
            other->Visibility.setValue(false);
        }
    }

    if (!scene) {
        scene = std::make_unique<Section::SectionScene>(getDocument());
    }
    updateDisplaySettings();
    updateFilter();
    updateSection();
    scene->attach();
    updatePlaneVisuals();
}

void ViewProviderSectionView::deactivate()
{
    if (scene) {
        scene->detach();
        scene.reset();
    }
}

std::vector<App::SectionView::Plane> ViewProviderSectionView::currentPlanes() const
{
    // The object keeps its planes up to date, including referenced faces
    std::vector<App::SectionView::Plane> planes;
    if (auto obj = getSectionView()) {
        for (const auto& placement : obj->Planes.getValues()) {
            App::SectionView::Plane plane;
            plane.placement = placement;
            plane.base = placement.getPosition();
            placement.getRotation().multVec(Base::Vector3d(0, 0, 1), plane.normal);
            planes.push_back(plane);
        }
    }
    return planes;
}

std::vector<Section::CutPlane> ViewProviderSectionView::computeCutPlanes() const
{
    std::vector<Section::CutPlane> planes;
    for (const auto& plane : currentPlanes()) {
        // The normal of a section plane points to the removed side
        planes.emplace_back(plane.base, -plane.normal);
    }
    return planes;
}

void ViewProviderSectionView::updateSection()
{
    auto obj = getSectionView();
    if (!scene || !obj) {
        return;
    }
    const auto combination = obj->getType() == App::SectionView::Type::Box
            || obj->getCombination() == App::SectionView::CombinationMode::KeepCommon
        ? Section::Combination::KeepCommon
        : Section::Combination::RemoveCommon;
    scene->setPlanes(computeCutPlanes(), combination);
    updatePlaneVisuals();
}

void ViewProviderSectionView::updateFilter()
{
    auto obj = getSectionView();
    if (!scene || !obj) {
        return;
    }
    std::vector<App::SubObjectT> items;
    const auto& objs = obj->Objects.getValues();
    const auto& subs = obj->Objects.getSubValues();
    for (std::size_t i = 0; i < objs.size(); ++i) {
        items.emplace_back(objs[i], i < subs.size() ? subs[i].c_str() : "");
    }
    scene->setFilter(items, obj->getObjectsMode() == App::SectionView::ObjectsMode::SectionOnly);
}

void ViewProviderSectionView::updateDisplaySettings()
{
    if (!scene) {
        return;
    }
    Section::DisplaySettings settings;
    settings.showCaps = FillMode.getValue() != 2;
    settings.fillColor = FillMode.getValue() == 1 ? Section::DisplaySettings::FillColor::Uniform
                                                  : Section::DisplaySettings::FillColor::Object;
    settings.uniformColor = FillColor.getValue();
    settings.hatch = static_cast<Section::DisplaySettings::Hatch>(HatchPattern.getValue());
    settings.hatchSpacing = HatchSpacing.getValue();
    settings.hatchAngle = HatchAngle.getValue();
    settings.adaptiveHatch = HatchAdaptive.getValue();
    settings.varyHatch = HatchVary.getValue();
    settings.hatchLineWidth = static_cast<float>(HatchLineWidth.getValue());
    settings.showOutline = ShowOutline.getValue();
    settings.outlineColor = OutlineColor.getValue();
    settings.outlineWidth = static_cast<float>(OutlineWidth.getValue());
    scene->setDisplaySettings(settings);
}

void ViewProviderSectionView::updateData(const App::Property* prop)
{
    auto obj = getSectionView();
    if (obj && !obj->isRestoring()) {
        if (prop == &obj->Objects || prop == &obj->ObjectMode) {
            updateFilter();
        }
        else if (prop == &obj->Planes) {
            // Every change of the section definition ends up here
            updateSection();
        }
    }
    ViewProviderDragger::updateData(prop);
}

void ViewProviderSectionView::onChanged(const App::Property* prop)
{
    if (prop == &ShowPlanes) {
        updatePlaneVisuals();
    }
    else if (prop != &Visibility && prop != &DisplayMode && prop != &SelectionStyle) {
        updateDisplaySettings();
    }
    ViewProviderDragger::onChanged(prop);
}

Base::BoundBox3d ViewProviderSectionView::getModelBoundBox() const
{
    if (scene && scene->getBoundBox().IsValid()) {
        return scene->getBoundBox();
    }

    // Not shown yet: use the physical objects of a 3D view
    Base::BoundBox3d box;
    if (auto doc = getDocument()) {
        if (auto view = dynamic_cast<View3DInventor*>(doc->getActiveView())) {
            auto viewer = view->getViewer();
            SoGetBoundingBoxAction action(viewer->getSoRenderManager()->getViewportRegion());
            action.apply(viewer->getObjectGroup());
            SbBox3f sbox = action.getBoundingBox();
            if (!sbox.isEmpty()) {
                box = Base::BoundBox3d(
                    sbox.getMin()[0],
                    sbox.getMin()[1],
                    sbox.getMin()[2],
                    sbox.getMax()[0],
                    sbox.getMax()[1],
                    sbox.getMax()[2]
                );
            }
        }
    }
    return box;
}

Base::Vector3d ViewProviderSectionView::getPlaneCenter(int index) const
{
    auto obj = getSectionView();
    Base::BoundBox3d box = getModelBoundBox();
    if (!obj) {
        return Base::Vector3d();
    }
    std::vector<App::SectionView::Plane> planes = currentPlanes();
    if (obj->getType() == App::SectionView::Type::Planes) {
        // Only the enabled planes are listed
        int k = 0;
        for (int i = 0; i < index; ++i) {
            if (obj->isPlaneEnabled(i)) {
                ++k;
            }
        }
        index = k;
    }
    if (index < 0 || index >= static_cast<int>(planes.size())) {
        return Base::Vector3d();
    }
    const auto& plane = planes[index];
    const Base::Vector3d center = box.IsValid() ? box.GetCenter() : plane.base;
    return center - plane.normal * ((center - plane.base) * plane.normal);
}

void ViewProviderSectionView::setPlanesVisible(bool on)
{
    planesVisible = on;
    updatePlaneVisuals();
}

void ViewProviderSectionView::updatePlaneVisuals()
{
    auto obj = getSectionView();
    const bool visible = obj && (planesVisible || (ShowPlanes.getValue() && scene));
    planeVisuals->removeAllChildren();
    planeSwitch->whichChild = visible ? 0 : SO_SWITCH_NONE;
    if (!visible) {
        return;
    }

    Base::BoundBox3d box = getModelBoundBox();
    if (!box.IsValid()) {
        box = Base::BoundBox3d(-50, -50, -50, 50, 50, 50);
    }

    auto lightModel = new SoLightModel;
    lightModel->model = SoLightModel::BASE_COLOR;
    planeVisuals->addChild(lightModel);
    auto hints = new SoShapeHints;
    hints->vertexOrdering = SoShapeHints::COUNTERCLOCKWISE;
    hints->shapeType = SoShapeHints::UNKNOWN_SHAPE_TYPE;
    planeVisuals->addChild(hints);
    auto style = new SoDrawStyle;
    style->lineWidth = 1.5F;
    planeVisuals->addChild(style);

    const SbColor planeColor(0.35F, 0.55F, 0.90F);

    if (obj->getType() == App::SectionView::Type::Box) {
        const Base::Placement& placement = obj->BoxPlacement.getValue();
        const Base::Vector3d size(
            obj->BoxLength.getValue(),
            obj->BoxWidth.getValue(),
            obj->BoxHeight.getValue()
        );
        auto coords = new SoCoordinate3;
        for (int i = 0; i < 8; ++i) {
            Base::Vector3d pnt((i & 1) ? size.x : 0.0, (i & 2) ? size.y : 0.0, (i & 4) ? size.z : 0.0);
            placement.multVec(pnt, pnt);
            coords->point.set1Value(i, toSb(pnt));
        }
        auto color = new SoBaseColor;
        color->rgb.setValue(planeColor);
        auto lines = new SoIndexedLineSet;
        const int32_t edges[] = {0, 1, 3,  2, 0, -1, 4, 5, 7,  6, 4, -1,
                                 0, 4, -1, 1, 5, -1, 2, 6, -1, 3, 7, -1};
        lines->coordIndex.setValues(0, sizeof(edges) / sizeof(edges[0]), edges);
        planeVisuals->addChild(color);
        planeVisuals->addChild(coords);
        planeVisuals->addChild(lines);
        return;
    }

    const auto planes = currentPlanes();
    for (const auto& plane : planes) {
        Base::Vector3d uDir;
        Base::Vector3d vDir;
        plane.placement.getRotation().multVec(Base::Vector3d(1, 0, 0), uDir);
        plane.placement.getRotation().multVec(Base::Vector3d(0, 1, 0), vDir);

        // Size the plane to the extent of the model
        double minU = std::numeric_limits<double>::max();
        double maxU = std::numeric_limits<double>::lowest();
        double minV = minU;
        double maxV = maxU;
        for (unsigned short i = 0; i < 8; ++i) {
            const Base::Vector3d corner = box.CalcPoint(i) - plane.base;
            minU = std::min(minU, corner * uDir);
            maxU = std::max(maxU, corner * uDir);
            minV = std::min(minV, corner * vDir);
            maxV = std::max(maxV, corner * vDir);
        }
        const double margin = 0.1 * std::max(maxU - minU, maxV - minV);
        minU -= margin;
        maxU += margin;
        minV -= margin;
        maxV += margin;

        auto sep = new SoSeparator;
        auto coords = new SoCoordinate3;
        const std::array<Base::Vector3d, 4> corners {
            plane.base + uDir * minU + vDir * minV,
            plane.base + uDir * maxU + vDir * minV,
            plane.base + uDir * maxU + vDir * maxV,
            plane.base + uDir * minU + vDir * maxV,
        };
        for (int i = 0; i < 4; ++i) {
            coords->point.set1Value(i, toSb(corners[i]));
        }
        coords->point.set1Value(4, toSb(corners[0]));
        sep->addChild(coords);

        auto material = new SoMaterial;
        material->diffuseColor.setValue(planeColor);
        material->transparency = 0.85F;
        sep->addChild(material);
        auto face = new SoFaceSet;
        face->numVertices.setValue(4);
        sep->addChild(face);

        auto color = new SoBaseColor;
        color->rgb.setValue(planeColor);
        sep->addChild(color);
        auto border = new SoLineSet;
        border->numVertices.setValue(5);
        sep->addChild(border);

        planeVisuals->addChild(sep);
    }
}

void ViewProviderSectionView::flip()
{
    auto obj = getSectionView();
    if (!obj || obj->getType() != App::SectionView::Type::Planes) {
        return;
    }
    auto doc = getDocument();
    const bool transaction = doc && !doc->hasPendingCommand();
    if (transaction) {
        doc->openCommand(QT_TRANSLATE_NOOP("Command", "Flip section view"));
    }
    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        if (obj->isPlaneEnabled(i)) {
            auto& flipped = obj->getFlippedProperty(i);
            flipped.setValue(!flipped.getValue());
        }
    }
    if (transaction) {
        doc->commitCommand();
    }
}

bool ViewProviderSectionView::doubleClicked()
{
    if (auto doc = getDocument()) {
        return doc->setEdit(this, ViewProvider::Default);
    }
    return false;
}

void ViewProviderSectionView::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    QAction* edit = menu->addAction(QObject::tr("Edit Section View"), receiver, member);
    edit->setData(QVariant(static_cast<int>(ViewProvider::Default)));

    auto func = new Gui::ActionFunction(menu);
    QAction* toggle = menu->addAction(
        isShow() ? QObject::tr("Turn Section Off") : QObject::tr("Turn Section On")
    );
    func->trigger(toggle, [this]() {
        if (auto obj = getObject()) {
            obj->Visibility.setValue(!obj->Visibility.getValue());
        }
    });

    auto obj = getSectionView();
    if (obj && obj->getType() == App::SectionView::Type::Planes) {
        QAction* flipAction = menu->addAction(QObject::tr("Flip Section"));
        func->trigger(flipAction, [this]() { flip(); });
    }
}

bool ViewProviderSectionView::onDelete(const std::vector<std::string>& subNames)
{
    deactivate();
    return ViewProviderDragger::onDelete(subNames);
}

bool ViewProviderSectionView::setEdit(int modNum)
{
    if (modNum != ViewProvider::Default) {
        return ViewProviderDragger::setEdit(modNum);
    }

    // The section is shown while editing it
    if (!isShow()) {
        getObject()->Visibility.setValue(true);
    }

    auto dialog = Gui::Control().activeDialog();
    if (auto sectionDialog = qobject_cast<TaskDlgSectionView*>(dialog)) {
        if (sectionDialog->getViewProvider() != this) {
            return false;
        }
    }
    else if (dialog) {
        return false;
    }
    else {
        Gui::Control().showDialog(new TaskDlgSectionView(this));
    }
    setPlanesVisible(true);
    return true;
}

void ViewProviderSectionView::unsetEdit(int modNum)
{
    if (modNum != ViewProvider::Default) {
        ViewProviderDragger::unsetEdit(modNum);
        return;
    }
    setPlanesVisible(false);
    if (auto doc = getDocument()) {
        Gui::Control().closeDialog(doc->getDocument());
    }
}
