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
#include <set>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/ElementNamingUtils.h>
#include <App/GeoFeature.h>
#include <App/ObjectIdentifier.h>
#include <App/SectionView.h>
#include <Base/Console.h>
#include <Base/Converter.h>
#include <Base/Tools.h>

#include "Application.h"
#include "BitmapFactory.h"
#include "Document.h"
#include "Inventor/Draggers/Gizmo.h"
#include "Inventor/Draggers/SoLinearDragger.h"
#include "Inventor/Draggers/SoRotationDragger.h"
#include "QuantitySpinBox.h"
#include "TaskSectionView.h"
#include "Utilities.h"
#include "View3DInventor.h"
#include "View3DInventorViewer.h"
#include "ViewProviderSectionView.h"
#include "Widgets.h"

using namespace Gui;

namespace
{

QuantitySpinBox* makeLengthBox()
{
    auto box = new QuantitySpinBox;
    box->setUnit(Base::Unit::Length);
    box->setMinimum(-1e9);
    box->setMaximum(1e9);
    box->setSingleStep(1.0);
    return box;
}

QuantitySpinBox* makeAngleBox()
{
    auto box = new QuantitySpinBox;
    box->setUnit(Base::Unit::Angle);
    box->setMinimum(-360.0);
    box->setMaximum(360.0);
    box->setSingleStep(5.0);
    return box;
}

void setBoxValue(QuantitySpinBox* box, double value)
{
    if (std::abs(box->rawValue() - value) > 1e-9) {
        QSignalBlocker blocker(box);
        box->setValue(value);
    }
}

bool hasGeometry(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    auto linked = obj->getLinkedObject(true);
    auto geo = freecad_cast<App::GeoFeature*>(linked ? linked : obj);
    return geo && geo->getPropertyOfGeometry();
}

/// The component a picked sub-element belongs to, e.g. the part of an assembly
App::SubObjectT componentOf(App::DocumentObject* top, const std::string& subname)
{
    if (hasGeometry(top)) {
        return {top, ""};
    }
    const std::string path = Data::noElementName(subname.c_str());
    std::string prefix;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t dot = path.find('.', start);
        if (dot == std::string::npos) {
            break;
        }
        prefix = path.substr(0, dot + 1);
        if (hasGeometry(top->getSubObject(prefix.c_str()))) {
            return {top, prefix.c_str()};
        }
        start = dot + 1;
    }
    return {top, path.c_str()};
}

QString labelOf(const App::SubObjectT& item)
{
    QStringList names;
    for (auto obj : item.getSubObjectList()) {
        if (obj) {
            names << QString::fromUtf8(obj->Label.getValue());
        }
    }
    names.removeDuplicates();
    return names.join(QStringLiteral(" › "));
}

SoDragger* getDragger(Gizmo* gizmo)
{
    if (auto linear = dynamic_cast<LinearGizmo*>(gizmo)) {
        return linear->getDraggerContainer()->getDragger();
    }
    if (auto rotation = dynamic_cast<RotationGizmo*>(gizmo)) {
        return rotation->getDraggerContainer()->getDragger();
    }
    return nullptr;
}

bool isDragging(Gizmo* gizmo)
{
    auto dragger = getDragger(gizmo);
    return dragger && dragger->isActive.getValue();
}

}  // namespace

// ----------------------------------------------------------------------------

TaskSectionView::TaskSectionView(ViewProviderSectionView* vp)
    : SelectionObserver(true, ResolveMode::NoResolve)
    , vp(vp)
    , objectT(vp->getObject())
{
    setupSectionWidget();
    setupObjectsWidget();
    setupDisplayWidget();
    connectSignals();

    refreshTimer.setSingleShot(true);
    refreshTimer.setInterval(0);
    connect(&refreshTimer, &QTimer::timeout, this, &TaskSectionView::refresh);
    statsTimer.setInterval(500);
    connect(&statsTimer, &QTimer::timeout, this, &TaskSectionView::refreshStats);
    statsTimer.start();

    if (auto obj = getObject()) {
        connectChanged = obj->getDocument()->signalChangedObject.connect(
            [this](const App::DocumentObject& changed, const App::Property&) {
                if (&changed == getObject()) {
                    scheduleRefresh();
                }
            }
        );
    }

    refresh();
    setupGizmos();
}

TaskSectionView::~TaskSectionView()
{
    if (pickMode != PickMode::None) {
        Gui::Selection().rmvSelectionGate();
    }
    gizmoContainer.reset();
}

App::SectionView* TaskSectionView::getObject() const
{
    return freecad_cast<App::SectionView*>(objectT.getObject());
}

void TaskSectionView::setupSectionWidget()
{
    sectionWidget = new QWidget;
    sectionWidget->setWindowTitle(tr("Section"));
    auto layout = new QVBoxLayout(sectionWidget);

    auto form = new QFormLayout;
    type = new QComboBox;
    type->addItem(tr("Planes"));
    type->addItem(tr("Box"));
    type->setToolTip(
        tr("Planes: cut the model with up to three planes\nBox: show only what is inside a box")
    );
    form->addRow(tr("Type"), type);
    layout->addLayout(form);

    stack = new QStackedWidget;
    auto planesPage = new QWidget;
    auto planesLayout = new QVBoxLayout(planesPage);
    planesLayout->setContentsMargins(0, 0, 0, 0);
    planeTabs = new QTabWidget;
    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        setupPlanePage(i);
        planeTabs->addTab(planes[i].page, tr("Plane %1").arg(i + 1));
    }
    planesLayout->addWidget(planeTabs);

    auto combinationForm = new QFormLayout;
    combination = new QComboBox;
    combination->addItem(tr("Remove intersection"));
    combination->setItemData(
        0,
        tr("Cut away the region on the removed side of all planes, e.g. a quarter of the model"),
        Qt::ToolTipRole
    );
    combination->addItem(tr("Keep intersection"));
    combination->setItemData(
        1,
        tr("Show only the region on the kept side of all planes, e.g. a slab"),
        Qt::ToolTipRole
    );
    combinationLabel = new QLabel(tr("Combine planes"));
    combinationForm->addRow(combinationLabel, combination);
    planesLayout->addLayout(combinationForm);
    stack->addWidget(planesPage);

    setupBoxPage();
    layout->addWidget(stack);

    if (GizmoContainer::isEnabled()) {
        hint = new QLabel;
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }
}

void TaskSectionView::setupPlanePage(int index)
{
    auto& w = planes[index];
    w.page = new QWidget;
    auto grid = new QGridLayout(w.page);
    int row = 0;

    if (index > 0) {
        w.enabled = new QCheckBox(tr("Use this plane"));
        w.enabled->setToolTip(tr("Combine this plane with the other enabled planes"));
        grid->addWidget(w.enabled, row++, 0, 1, 3);
    }

    w.base = new QComboBox;
    w.base->addItem(tr("XY plane (top)"));
    w.base->addItem(tr("XZ plane (front)"));
    w.base->addItem(tr("YZ plane (right)"));
    w.base->addItem(tr("Reference"));
    w.base->setToolTip(tr("The plane the section plane is based on"));
    grid->addWidget(new QLabel(tr("Base plane")), row, 0);
    grid->addWidget(w.base, row++, 1, 1, 2);

    w.reference = new QLabel;
    w.reference->setTextInteractionFlags(Qt::TextSelectableByMouse);
    w.selectReference = new QPushButton(tr("Select…"));
    w.selectReference->setCheckable(true);
    w.selectReference->setToolTip(
        tr("Select a planar face, datum plane or sketch in the 3D view or the tree")
    );
    grid->addWidget(new QLabel(tr("Reference")), row, 0);
    grid->addWidget(w.reference, row, 1);
    grid->addWidget(w.selectReference, row++, 2);

    w.offset = makeLengthBox();
    w.offset->setToolTip(tr("Distance of the section plane from its base plane"));
    grid->addWidget(new QLabel(tr("Offset")), row, 0);
    grid->addWidget(w.offset, row++, 1, 1, 2);

    w.angleX = makeAngleBox();
    w.angleX->setToolTip(tr("Rotation around the X axis of the base plane"));
    grid->addWidget(new QLabel(tr("Tilt X")), row, 0);
    grid->addWidget(w.angleX, row++, 1, 1, 2);

    w.angleY = makeAngleBox();
    w.angleY->setToolTip(tr("Rotation around the Y axis of the base plane"));
    grid->addWidget(new QLabel(tr("Tilt Y")), row, 0);
    grid->addWidget(w.angleY, row++, 1, 1, 2);

    w.flip = new QPushButton(tr("Flip Side"));
    w.flip->setToolTip(tr("Cut away the other side of the plane"));
    w.alignToView = new QPushButton(tr("Align to View"));
    w.alignToView->setToolTip(
        tr("Orient the plane parallel to the screen, cutting away the side facing you")
    );
    auto buttons = new QHBoxLayout;
    buttons->addWidget(w.flip);
    buttons->addWidget(w.alignToView);
    grid->addLayout(buttons, row++, 0, 1, 3);
    grid->setRowStretch(row, 1);

    if (auto obj = getObject()) {
        w.offset->bind(App::ObjectIdentifier(obj->getOffsetProperty(index)));
        w.angleX->bind(App::ObjectIdentifier(obj->getAngleXProperty(index)));
        w.angleY->bind(App::ObjectIdentifier(obj->getAngleYProperty(index)));
    }
}

void TaskSectionView::setupBoxPage()
{
    auto page = new QWidget;
    auto grid = new QGridLayout(page);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(new QLabel(tr("Min")), 0, 1, Qt::AlignHCenter);
    grid->addWidget(new QLabel(tr("Max")), 0, 2, Qt::AlignHCenter);
    const std::array<QString, 3> axes {tr("X"), tr("Y"), tr("Z")};
    for (int axis = 0; axis < 3; ++axis) {
        grid->addWidget(new QLabel(axes[axis]), axis + 1, 0);
        for (int side = 0; side < 2; ++side) {
            auto box = makeLengthBox();
            boxExtents[2 * axis + side] = box;
            grid->addWidget(box, axis + 1, side + 1);
        }
    }
    fitBox = new QPushButton(tr("Fit to Model"));
    fitBox->setToolTip(tr("Make the box enclose the whole model"));
    grid->addWidget(fitBox, 4, 1, 1, 2);
    auto note = new QLabel(tr("The extents are in the coordinate system of the box placement."));
    note->setWordWrap(true);
    grid->addWidget(note, 5, 0, 1, 3);
    grid->setRowStretch(6, 1);
    stack->addWidget(page);
}

void TaskSectionView::setupObjectsWidget()
{
    objectsWidget = new QWidget;
    objectsWidget->setWindowTitle(tr("Objects"));
    auto layout = new QVBoxLayout(objectsWidget);

    auto form = new QFormLayout;
    objectMode = new QComboBox;
    objectMode->addItem(tr("Exclude listed objects"));
    objectMode->setItemData(0, tr("The listed objects are not cut"), Qt::ToolTipRole);
    objectMode->addItem(tr("Cut only listed objects"));
    objectMode->setItemData(1, tr("Only the listed objects are cut"), Qt::ToolTipRole);
    form->addRow(tr("Mode"), objectMode);
    layout->addLayout(form);

    objectList = new QListWidget;
    objectList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    objectList->setMinimumHeight(60);
    objectList->setMaximumHeight(140);
    layout->addWidget(objectList);

    auto buttons = new QHBoxLayout;
    addObjects = new QPushButton(tr("Add…"));
    addObjects->setCheckable(true);
    addObjects->setToolTip(
        tr("Click objects in the 3D view or the tree to add them. Click the button again to stop.")
    );
    removeObjects = new QPushButton(tr("Remove"));
    buttons->addWidget(addObjects);
    buttons->addWidget(removeObjects);
    layout->addLayout(buttons);
}

void TaskSectionView::setupDisplayWidget()
{
    displayWidget = new QWidget;
    displayWidget->setWindowTitle(tr("Display"));
    auto grid = new QGridLayout(displayWidget);
    int row = 0;

    fillMode = new QComboBox;
    fillMode->addItem(tr("Object color"));
    fillMode->addItem(tr("Uniform color"));
    fillMode->addItem(tr("None (clip only)"));
    fillColor = new ColorButton;
    grid->addWidget(new QLabel(tr("Cut faces")), row, 0);
    grid->addWidget(fillMode, row, 1);
    grid->addWidget(fillColor, row++, 2);

    hatchPattern = new QComboBox;
    hatchPattern->addItem(tr("None"));
    hatchPattern->addItem(tr("Lines"));
    hatchPattern->addItem(tr("Cross"));
    grid->addWidget(new QLabel(tr("Hatch")), row, 0);
    grid->addWidget(hatchPattern, row++, 1, 1, 2);

    autoSpacing = new QCheckBox(tr("Auto"));
    autoSpacing->setToolTip(tr("Derive the spacing from the size of the model"));
    hatchSpacing = makeLengthBox();
    hatchSpacing->setMinimum(0.0);
    grid->addWidget(new QLabel(tr("Spacing")), row, 0);
    grid->addWidget(hatchSpacing, row, 1);
    grid->addWidget(autoSpacing, row++, 2);

    hatchAngle = makeAngleBox();
    grid->addWidget(new QLabel(tr("Angle")), row, 0);
    grid->addWidget(hatchAngle, row++, 1, 1, 2);

    hatchAdaptive = new QCheckBox(tr("Adapt spacing to zoom"));
    hatchAdaptive->setToolTip(
        tr("Coarsen or refine the hatch in steps of two while zooming so it stays readable")
    );
    grid->addWidget(hatchAdaptive, row++, 0, 1, 3);

    hatchVary = new QCheckBox(tr("Vary hatch between objects"));
    hatchVary->setToolTip(
        tr("Use different hatch angles and spacings for neighboring objects, like on drawings")
    );
    grid->addWidget(hatchVary, row++, 0, 1, 3);

    showOutline = new QCheckBox(tr("Outline"));
    outlineColor = new ColorButton;
    grid->addWidget(showOutline, row, 0, 1, 2);
    grid->addWidget(outlineColor, row++, 2);

    stats = new QLabel;
    stats->setWordWrap(true);
    grid->addWidget(stats, row++, 0, 1, 3);
}

void TaskSectionView::connectSignals()
{
    auto withObject = [this](auto func) {
        return [this, func](auto... args) {
            if (refreshing) {
                return;
            }
            if (auto obj = getObject()) {
                func(obj, args...);
            }
        };
    };

    connect(
        type,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        withObject([this](App::SectionView* obj, int index) {
            if (index == 1 && obj->BoxPlacement.getValue().isIdentity()
                && obj->BoxLength.getValue() == 10.0 && obj->BoxWidth.getValue() == 10.0
                && obj->BoxHeight.getValue() == 10.0) {
                // First time in box mode: start with the model's extent
                fitBoxToModel();
            }
            obj->SectionType.setValue(index);
            setPickMode(PickMode::None);
        })
    );

    connect(
        combination,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        withObject([](App::SectionView* obj, int index) { obj->Combination.setValue(index); })
    );

    connect(planeTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (offsetGizmo) {
            offsetGizmo->setProperty(planes[index].offset);
            angleXGizmo->setProperty(planes[index].angleX);
            angleYGizmo->setProperty(planes[index].angleY);
        }
        setPickMode(PickMode::None);
        setGizmoPositions();
    });

    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        auto& w = planes[i];
        if (w.enabled) {
            connect(w.enabled, &QCheckBox::toggled, this, withObject([i](App::SectionView* obj, bool on) {
                        (i == 1 ? obj->Plane2Enabled : obj->Plane3Enabled).setValue(on);
                    }));
        }
        connect(
            w.base,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            withObject([this, i](App::SectionView* obj, int index) {
                obj->getBaseProperty(i).setValue(index);
                const bool isReference = index
                    == static_cast<int>(App::SectionView::BasePlane::Reference);
                if (isReference && !obj->getReferenceProperty(i).getValue()) {
                    setPickMode(PickMode::Reference, i);
                }
                else if (!isReference && pickMode == PickMode::Reference && pickPlane == i) {
                    setPickMode(PickMode::None);
                }
            })
        );
        connect(w.selectReference, &QPushButton::toggled, this, [this, i](bool on) {
            if (!refreshing) {
                setPickMode(on ? PickMode::Reference : PickMode::None, i);
            }
        });
        connect(
            w.offset,
            qOverload<double>(&QuantitySpinBox::valueChanged),
            this,
            withObject([i](App::SectionView* obj, double value) {
                obj->getOffsetProperty(i).setValue(value);
            })
        );
        connect(
            w.angleX,
            qOverload<double>(&QuantitySpinBox::valueChanged),
            this,
            withObject([i](App::SectionView* obj, double value) {
                obj->getAngleXProperty(i).setValue(value);
            })
        );
        connect(
            w.angleY,
            qOverload<double>(&QuantitySpinBox::valueChanged),
            this,
            withObject([i](App::SectionView* obj, double value) {
                obj->getAngleYProperty(i).setValue(value);
            })
        );
        connect(w.flip, &QPushButton::clicked, this, withObject([i](App::SectionView* obj, bool) {
                    auto& flipped = obj->getFlippedProperty(i);
                    flipped.setValue(!flipped.getValue());
                }));
        connect(w.alignToView, &QPushButton::clicked, this, [this, i]() { alignToView(i); });
    }

    for (int k = 0; k < 6; ++k) {
        connect(
            boxExtents[k],
            qOverload<double>(&QuantitySpinBox::valueChanged),
            this,
            [this, k](double value) {
                if (!refreshing) {
                    onBoxExtentChanged(k / 2, (k % 2) == 1, value);
                }
            }
        );
    }
    connect(fitBox, &QPushButton::clicked, this, &TaskSectionView::fitBoxToModel);

    connect(
        objectMode,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        withObject([](App::SectionView* obj, int index) { obj->ObjectMode.setValue(index); })
    );
    connect(addObjects, &QPushButton::toggled, this, [this](bool on) {
        if (!refreshing) {
            setPickMode(on ? PickMode::Objects : PickMode::None);
        }
    });
    connect(removeObjects, &QPushButton::clicked, this, &TaskSectionView::removeSelectedObjects);

    // Display properties live on the view provider
    auto withVp = [this](auto func) {
        return [this, func](auto... args) {
            if (!refreshing && vp) {
                func(args...);
            }
        };
    };
    connect(fillMode, qOverload<int>(&QComboBox::currentIndexChanged), this, withVp([this](int index) {
                vp->FillMode.setValue(index);
                updateVisibility();
            }));
    connect(fillColor, &ColorButton::changed, this, withVp([this]() {
                vp->FillColor.setValue(Base::Color::fromValue<QColor>(fillColor->color()));
            }));
    connect(hatchPattern, qOverload<int>(&QComboBox::currentIndexChanged), this, withVp([this](int index) {
                vp->HatchPattern.setValue(index);
                updateVisibility();
            }));
    connect(autoSpacing, &QCheckBox::toggled, this, withVp([this](bool on) {
                if (on) {
                    vp->HatchSpacing.setValue(0.0);
                }
                else if (vp->HatchSpacing.getValue() <= 0.0) {
                    const auto box = vp->getModelBoundBox();
                    const double spacing = box.IsValid() ? box.CalcDiagonalLength() / 100.0 : 1.0;
                    vp->HatchSpacing.setValue(std::max(spacing, 0.01));
                }
                refresh();
            }));
    connect(
        hatchSpacing,
        qOverload<double>(&QuantitySpinBox::valueChanged),
        this,
        withVp([this](double value) { vp->HatchSpacing.setValue(value); })
    );
    connect(
        hatchAngle,
        qOverload<double>(&QuantitySpinBox::valueChanged),
        this,
        withVp([this](double value) { vp->HatchAngle.setValue(value); })
    );
    connect(hatchAdaptive, &QCheckBox::toggled, this, withVp([this](bool on) {
                vp->HatchAdaptive.setValue(on);
            }));
    connect(hatchVary, &QCheckBox::toggled, this, withVp([this](bool on) {
                vp->HatchVary.setValue(on);
            }));
    connect(showOutline, &QCheckBox::toggled, this, withVp([this](bool on) {
                vp->ShowOutline.setValue(on);
                updateVisibility();
            }));
    connect(outlineColor, &ColorButton::changed, this, withVp([this]() {
                vp->OutlineColor.setValue(Base::Color::fromValue<QColor>(outlineColor->color()));
            }));
}

void TaskSectionView::scheduleRefresh()
{
    if (!refreshing) {
        refreshTimer.start();
    }
}

void TaskSectionView::refresh()
{
    auto obj = getObject();
    if (!obj || !vp) {
        return;
    }
    refreshing = true;

    {
        QSignalBlocker blockType(type);
        QSignalBlocker blockCombination(combination);
        type->setCurrentIndex(obj->SectionType.getValue());
        combination->setCurrentIndex(obj->Combination.getValue());
    }

    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        auto& w = planes[i];
        if (w.enabled) {
            QSignalBlocker blocker(w.enabled);
            w.enabled->setChecked(obj->isPlaneEnabled(i));
        }
        {
            QSignalBlocker blocker(w.base);
            w.base->setCurrentIndex(obj->getBaseProperty(i).getValue());
        }
        setBoxValue(w.offset, obj->getOffsetProperty(i).getValue());
        setBoxValue(w.angleX, obj->getAngleXProperty(i).getValue());
        setBoxValue(w.angleY, obj->getAngleYProperty(i).getValue());

        auto& ref = obj->getReferenceProperty(i);
        QString refText = tr("None");
        if (auto refObj = ref.getValue()) {
            const auto& subs = ref.getSubValues();
            refText = labelOf(App::SubObjectT(refObj, subs.empty() ? "" : subs.front().c_str()));
            if (!subs.empty()) {
                if (const char* element = Data::findElementName(subs.front().c_str());
                    element && *element) {
                    refText += QStringLiteral(" (%1)").arg(QString::fromUtf8(element));
                }
            }
            std::string error;
            obj->getBasePlacement(i, &error);
            if (!error.empty()) {
                refText += QStringLiteral(" - ") + QString::fromStdString(error);
            }
        }
        w.reference->setText(refText);
    }

    const auto extents = getBoxExtents();
    for (int k = 0; k < 6; ++k) {
        setBoxValue(boxExtents[k], extents[k]);
    }

    {
        QSignalBlocker blocker(objectMode);
        objectMode->setCurrentIndex(obj->ObjectMode.getValue());
    }
    refreshObjects();

    {
        QSignalBlocker b1(fillMode);
        QSignalBlocker b2(fillColor);
        QSignalBlocker b3(hatchPattern);
        QSignalBlocker b4(autoSpacing);
        QSignalBlocker b5(hatchAdaptive);
        QSignalBlocker b6(hatchVary);
        QSignalBlocker b7(showOutline);
        QSignalBlocker b8(outlineColor);
        fillMode->setCurrentIndex(vp->FillMode.getValue());
        fillColor->setColor(vp->FillColor.getValue().asValue<QColor>());
        hatchPattern->setCurrentIndex(vp->HatchPattern.getValue());
        autoSpacing->setChecked(vp->HatchSpacing.getValue() <= 0.0);
        hatchAdaptive->setChecked(vp->HatchAdaptive.getValue());
        hatchVary->setChecked(vp->HatchVary.getValue());
        showOutline->setChecked(vp->ShowOutline.getValue());
        outlineColor->setColor(vp->OutlineColor.getValue().asValue<QColor>());
    }
    setBoxValue(hatchSpacing, vp->HatchSpacing.getValue());
    setBoxValue(hatchAngle, vp->HatchAngle.getValue());

    refreshing = false;
    updateVisibility();
    setGizmoPositions();
    refreshStats();
}

void TaskSectionView::refreshObjects()
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    QSignalBlocker blocker(objectList);
    objectList->clear();
    const auto& objs = obj->Objects.getValues();
    const auto& subs = obj->Objects.getSubValues();
    for (std::size_t i = 0; i < objs.size(); ++i) {
        App::SubObjectT item(objs[i], i < subs.size() ? subs[i].c_str() : "");
        auto listItem = new QListWidgetItem(labelOf(item), objectList);
        listItem->setData(Qt::UserRole, static_cast<int>(i));
    }
}

void TaskSectionView::refreshStats()
{
    if (!vp || !vp->getScene()) {
        stats->clear();
        return;
    }
    const auto info = vp->getScene()->getStats();
    QString text = tr("%n object(s) cut", nullptr, static_cast<int>(info.sectioned));
    if (info.sectioned != info.objects) {
        text += QStringLiteral(", ")
            + tr("%n not cut", nullptr, static_cast<int>(info.objects - info.sectioned));
    }
    if (info.open > 0) {
        text += QStringLiteral("\n")
            + tr("%n open shape(s) can't be filled", nullptr, static_cast<int>(info.open));
    }
    stats->setText(text);
}

void TaskSectionView::updateVisibility()
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    const bool isBox = obj->getType() == App::SectionView::Type::Box;
    stack->setCurrentIndex(isBox ? 1 : 0);
    if (hint) {
        hint->setText(
            isBox ? tr("Drag the arrows to move the faces of the box.")
                  : tr("Drag the arrow to move the section and the arcs to tilt it.")
        );
    }

    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        auto& w = planes[i];
        const bool enabled = obj->isPlaneEnabled(i);
        const bool isReference = obj->getBasePlane(i) == App::SectionView::BasePlane::Reference;
        for (QWidget* widget : std::initializer_list<QWidget*> {
                 w.base,
                 w.offset,
                 w.angleX,
                 w.angleY,
                 w.flip,
                 w.alignToView,
                 w.selectReference
             }) {
            widget->setEnabled(enabled);
        }
        w.reference->setEnabled(enabled && isReference);
        planeTabs->tabBar()->setTabTextColor(
            i,
            enabled ? planeTabs->palette().color(QPalette::WindowText)
                    : planeTabs->palette().color(QPalette::Disabled, QPalette::WindowText)
        );
    }

    const bool multiple = obj->countPlanes() > 1;
    combination->setVisible(multiple);
    combinationLabel->setVisible(multiple);

    fillColor->setEnabled(fillMode->currentIndex() == 1);
    const bool caps = fillMode->currentIndex() != 2;
    hatchPattern->setEnabled(caps);
    const bool hatched = caps && hatchPattern->currentIndex() != 0;
    autoSpacing->setEnabled(hatched);
    hatchSpacing->setEnabled(hatched && !autoSpacing->isChecked());
    hatchAngle->setEnabled(hatched);
    hatchAdaptive->setEnabled(hatched);
    hatchVary->setEnabled(hatched);
    showOutline->setEnabled(caps);
    outlineColor->setEnabled(caps && showOutline->isChecked());
}

std::array<double, 6> TaskSectionView::getBoxExtents() const
{
    std::array<double, 6> extents {};
    auto obj = getObject();
    if (!obj) {
        return extents;
    }
    const Base::Placement& placement = obj->BoxPlacement.getValue();
    Base::Vector3d corner;
    placement.getRotation().inverse().multVec(placement.getPosition(), corner);
    const std::array<double, 3> size {
        obj->BoxLength.getValue(),
        obj->BoxWidth.getValue(),
        obj->BoxHeight.getValue()
    };
    for (int axis = 0; axis < 3; ++axis) {
        extents[2 * axis] = corner[axis];
        extents[2 * axis + 1] = corner[axis] + size[axis];
    }
    return extents;
}

void TaskSectionView::onBoxExtentChanged(int axis, bool max, double value)
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    auto extents = getBoxExtents();
    constexpr double minSize = 1e-3;
    if (max) {
        extents[2 * axis + 1] = std::max(value, extents[2 * axis] + minSize);
    }
    else {
        extents[2 * axis] = std::min(value, extents[2 * axis + 1] - minSize);
    }

    Base::Placement placement = obj->BoxPlacement.getValue();
    Base::Vector3d corner(extents[0], extents[2], extents[4]);
    placement.getRotation().multVec(corner, corner);
    placement.setPosition(corner);
    obj->BoxPlacement.setValue(placement);
    obj->BoxLength.setValue(extents[1] - extents[0]);
    obj->BoxWidth.setValue(extents[3] - extents[2]);
    obj->BoxHeight.setValue(extents[5] - extents[4]);
}

void TaskSectionView::fitBoxToModel()
{
    auto obj = getObject();
    if (!obj || !vp) {
        return;
    }
    Base::BoundBox3d box = vp->getModelBoundBox();
    if (!box.IsValid()) {
        return;
    }
    // Slightly bigger than the model so the faces are visible
    box.Enlarge(box.CalcDiagonalLength() * 0.01);
    obj->BoxPlacement.setValue(Base::Placement(Base::Vector3d(box.MinX, box.MinY, box.MinZ), {}));
    obj->BoxLength.setValue(box.LengthX());
    obj->BoxWidth.setValue(box.LengthY());
    obj->BoxHeight.setValue(box.LengthZ());
}

void TaskSectionView::alignToView(int index)
{
    auto obj = getObject();
    auto view = dynamic_cast<View3DInventor*>(Application::Instance->activeView());
    if (!obj || !view) {
        return;
    }
    const SbVec3f dir = view->getViewer()->getViewDirection();
    // The removed side faces the viewer
    const Base::Vector3d toViewer(-dir[0], -dir[1], -dir[2]);

    // Choose the principal plane closest to the screen to keep the angles small
    using BasePlane = App::SectionView::BasePlane;
    BasePlane best = BasePlane::XY;
    double bestDot = 0.0;
    for (BasePlane base : {BasePlane::XY, BasePlane::XZ, BasePlane::YZ}) {
        Base::Vector3d normal;
        App::SectionView::getPrincipalRotation(base).multVec(Base::Vector3d(0, 0, 1), normal);
        const double dot = normal * toViewer;
        if (std::abs(dot) > std::abs(bestDot)) {
            bestDot = dot;
            best = base;
        }
    }

    const bool flipped = bestDot < 0.0;
    Base::Vector3d target = flipped ? -toViewer : toViewer;
    App::SectionView::getPrincipalRotation(best).inverse().multVec(target, target);
    // Tilt = Rx(ax) * Ry(ay), applied to Z gives (sin ay, -cos ay sin ax, cos ay cos ax)
    const double angleY = std::asin(std::clamp(target.x, -1.0, 1.0));
    const double angleX = std::atan2(-target.y, target.z);

    obj->getBaseProperty(index).setValue(static_cast<long>(best));
    obj->getAngleXProperty(index).setValue(Base::toDegrees(angleX));
    obj->getAngleYProperty(index).setValue(Base::toDegrees(angleY));
    obj->getFlippedProperty(index).setValue(flipped);
}

void TaskSectionView::setPickMode(PickMode mode, int plane)
{
    pickMode = mode;
    pickPlane = plane;

    refreshing = true;
    for (int i = 0; i < App::SectionView::MaxPlanes; ++i) {
        QSignalBlocker blocker(planes[i].selectReference);
        planes[i].selectReference->setChecked(mode == PickMode::Reference && i == plane);
    }
    {
        QSignalBlocker blocker(addObjects);
        addObjects->setChecked(mode == PickMode::Objects);
    }
    refreshing = false;

    if (mode != PickMode::None) {
        Gui::Selection().clearSelection();
    }
}

void TaskSectionView::onSelectionChanged(const SelectionChanges& msg)
{
    if (msg.Type != SelectionChanges::AddSelection || pickMode == PickMode::None) {
        return;
    }
    auto obj = getObject();
    App::DocumentObject* top = msg.Object.getObject();
    if (!obj || !top || top == obj) {
        return;
    }
    const std::string subname = msg.pSubName ? msg.pSubName : "";

    if (pickMode == PickMode::Reference && pickPlane >= 0) {
        std::string error;
        if (!App::SectionView::getReferencePlane(top, subname, &error)) {
            Base::Console().warning("Section view: %s\n", error.c_str());
        }
        else {
            // Start at the picked face, like when choosing a plane in other CAD programs
            obj->getReferenceProperty(pickPlane).setValue(top, {subname});
            obj->getBaseProperty(pickPlane).setValue(
                static_cast<long>(App::SectionView::BasePlane::Reference)
            );
            obj->getOffsetProperty(pickPlane).setValue(0.0);
            obj->getAngleXProperty(pickPlane).setValue(0.0);
            obj->getAngleYProperty(pickPlane).setValue(0.0);
            obj->getFlippedProperty(pickPlane).setValue(false);
            setPickMode(PickMode::None);
        }
    }
    else if (pickMode == PickMode::Objects) {
        const App::SubObjectT item = componentOf(top, subname);
        auto objs = obj->Objects.getValues();
        auto subs = obj->Objects.getSubValues();
        bool present = false;
        for (std::size_t i = 0; i < objs.size(); ++i) {
            if (objs[i] == item.getObject() && i < subs.size() && subs[i] == item.getSubName()) {
                present = true;
            }
        }
        if (!present) {
            objs.push_back(item.getObject());
            subs.push_back(item.getSubName());
            obj->Objects.setValues(objs, subs);
        }
    }

    // Don't keep the picked object selected, it hides the section
    QTimer::singleShot(0, []() { Gui::Selection().clearSelection(); });
}

void TaskSectionView::removeSelectedObjects()
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    std::set<int> rows;
    for (auto item : objectList->selectedItems()) {
        rows.insert(item->data(Qt::UserRole).toInt());
    }
    if (rows.empty()) {
        return;
    }
    const auto& oldObjs = obj->Objects.getValues();
    const auto& oldSubs = obj->Objects.getSubValues();
    std::vector<App::DocumentObject*> objs;
    std::vector<std::string> subs;
    for (std::size_t i = 0; i < oldObjs.size(); ++i) {
        if (!rows.contains(static_cast<int>(i))) {
            objs.push_back(oldObjs[i]);
            subs.push_back(i < oldSubs.size() ? oldSubs[i] : std::string());
        }
    }
    obj->Objects.setValues(objs, subs);
}

void TaskSectionView::setupGizmos()
{
    if (!GizmoContainer::isEnabled() || !vp) {
        return;
    }

    const int index = planeTabs->currentIndex();
    offsetGizmo = new LinearGizmo(planes[index].offset);
    angleXGizmo = new RotationGizmo(planes[index].angleX);
    angleYGizmo = new RotationGizmo(planes[index].angleY);
    for (int k = 0; k < 6; ++k) {
        boxGizmos[k] = new LinearGizmo(boxExtents[k]);
    }

    gizmoContainer = GizmoContainer::create(
        {offsetGizmo,
         angleXGizmo,
         angleYGizmo,
         boxGizmos[0],
         boxGizmos[1],
         boxGizmos[2],
         boxGizmos[3],
         boxGizmos[4],
         boxGizmos[5]},
        vp
    );

    // The tilt arcs follow the offset arrow
    for (auto gizmo : {angleXGizmo, angleYGizmo}) {
        gizmo->placeOverLinearGizmo(offsetGizmo);
        gizmo->automaticOrientation = false;
        gizmo->sepDistance = 0.0;
    }
    // Dragging a min face outwards decreases its coordinate
    for (int axis = 0; axis < 3; ++axis) {
        boxGizmos[2 * axis]->setMultFactor(-1.0);
    }

    // The gizmos aren't moved while one of them is dragged (see setGizmoPositions), so they
    // are placed again when the drag ends, e.g. the offset arrow along a tilted plane. Later,
    // so the dragger has finished handling the mouse event.
    std::vector<Gizmo*> all {offsetGizmo, angleXGizmo, angleYGizmo};
    all.insert(all.end(), boxGizmos.begin(), boxGizmos.end());
    for (auto gizmo : all) {
        if (auto dragger = getDragger(gizmo)) {
            dragger->addFinishCallback(
                [](void* data, SoDragger*) {
                    auto self = static_cast<TaskSectionView*>(data);
                    QTimer::singleShot(0, self, &TaskSectionView::setGizmoPositions);
                },
                this
            );
        }
    }

    setGizmoPositions();
}

void TaskSectionView::setGizmoPositions()
{
    auto obj = getObject();
    if (!gizmoContainer || !obj || !vp) {
        return;
    }

    std::vector<Gizmo*> all {offsetGizmo, angleXGizmo, angleYGizmo};
    all.insert(all.end(), boxGizmos.begin(), boxGizmos.end());
    for (auto gizmo : all) {
        // Moving a gizmo while it is dragged confuses it
        if (isDragging(gizmo)) {
            return;
        }
    }

    const bool isBox = obj->getType() == App::SectionView::Type::Box;
    const int index = planeTabs->currentIndex();
    const bool showPlane = !isBox && obj->isPlaneEnabled(index);
    offsetGizmo->setVisibility(showPlane);
    angleXGizmo->setVisibility(showPlane);
    angleYGizmo->setVisibility(showPlane);
    for (auto gizmo : boxGizmos) {
        gizmo->setVisibility(isBox);
    }

    if (showPlane) {
        const Base::Placement base = obj->getBasePlacement(index);
        const Base::Rotation& baseRot = base.getRotation();
        const Base::Rotation tiltX(
            Base::Vector3d(1, 0, 0),
            Base::toRadians(obj->getAngleXProperty(index).getValue())
        );
        const Base::Rotation tiltY(
            Base::Vector3d(0, 1, 0),
            Base::toRadians(obj->getAngleYProperty(index).getValue())
        );

        Base::Vector3d offsetDir;
        (baseRot * tiltX * tiltY).multVec(Base::Vector3d(0, 0, 1), offsetDir);
        const double offset = obj->getOffsetProperty(index).getValue();
        const Base::Vector3d center = vp->getPlaneCenter(index);
        // Anchor the arrow at the plane, so it only shows the distance being dragged
        offsetGizmo->Gizmo::setDraggerPlacement(center, offsetDir);
        offsetGizmo->setAddFactor(-offset);

        // The arc of a tilt gizmo starts at the plane's orientation for a zero angle
        Base::Vector3d xAxis;
        Base::Vector3d yAxis;
        Base::Vector3d tiltedY;
        baseRot.multVec(Base::Vector3d(1, 0, 0), xAxis);
        baseRot.multVec(Base::Vector3d(0, 1, 0), yAxis);
        (baseRot * tiltX).multVec(Base::Vector3d(0, 1, 0), tiltedY);
        auto orient =
            [](RotationGizmo* gizmo, const Base::Vector3d& pointer, const Base::Vector3d& axis) {
                auto container = gizmo->getDraggerContainer();
                container->setPointerDirection(Base::convertTo<SbVec3f>(pointer));
                container->setArcNormalDirection(Base::convertTo<SbVec3f>(axis));
            };
        orient(angleXGizmo, yAxis, xAxis);
        orient(angleYGizmo, xAxis, tiltedY);
        angleXGizmo->setRotAngle(obj->getAngleXProperty(index).getValue());
        angleYGizmo->setRotAngle(obj->getAngleYProperty(index).getValue());
    }

    if (isBox) {
        const Base::Placement& placement = obj->BoxPlacement.getValue();
        const auto extents = getBoxExtents();
        for (int axis = 0; axis < 3; ++axis) {
            Base::Vector3d local;
            for (int other = 0; other < 3; ++other) {
                local[other] = other == axis ? 0.0
                                             : (extents[2 * other] + extents[2 * other + 1]) * 0.5;
            }
            Base::Vector3d base;
            Base::Vector3d dir;
            placement.getRotation().multVec(local, base);
            Base::Vector3d unit;
            unit[axis] = 1.0;
            placement.getRotation().multVec(unit, dir);

            // Anchored at the faces, see the offset gizmo
            const double low = extents[2 * axis];
            const double high = extents[2 * axis + 1];
            boxGizmos[2 * axis]->Gizmo::setDraggerPlacement(base + dir * low, -dir);
            boxGizmos[2 * axis]->setAddFactor(low);
            boxGizmos[2 * axis + 1]->Gizmo::setDraggerPlacement(base + dir * high, dir);
            boxGizmos[2 * axis + 1]->setAddFactor(-high);
        }
    }

    gizmoContainer->calculateScaleAndOrientation();
}

// ----------------------------------------------------------------------------

TaskDlgSectionView::TaskDlgSectionView(ViewProviderSectionView* vp)
    : viewProvider(vp)
    , panel(std::make_unique<TaskSectionView>(vp))
{
    const QPixmap icon = BitmapFactory().pixmap("Std_SectionView");
    addTaskBox(icon, panel->getSectionWidget());
    addTaskBox(panel->getObjectsWidget());
    addTaskBox(panel->getDisplayWidget());
    if (vp && vp->getObject()) {
        setDocumentName(vp->getObject()->getDocument()->getName());
    }
}

TaskDlgSectionView::~TaskDlgSectionView() = default;

ViewProviderSectionView* TaskDlgSectionView::getViewProvider() const
{
    return viewProvider.get();
}

void TaskDlgSectionView::open()
{
    TaskDialog::open();
    if (auto vp = viewProvider.get()) {
        if (auto doc = vp->getDocument(); doc && !doc->hasPendingCommand()) {
            doc->openCommand(QT_TRANSLATE_NOOP("Command", "Edit section view"));
        }
    }
}

bool TaskDlgSectionView::accept()
{
    if (auto vp = viewProvider.get()) {
        if (auto doc = vp->getDocument()) {
            doc->commitCommand();
            doc->resetEdit();
        }
    }
    return TaskDialog::accept();
}

bool TaskDlgSectionView::reject()
{
    if (auto vp = viewProvider.get()) {
        if (auto doc = vp->getDocument()) {
            panel->detachSelection();
            // Aborting deletes the section view if it was just created, which also
            // ends editing it
            doc->abortCommand();
            if (!viewProvider.expired()) {
                doc->resetEdit();
            }
        }
    }
    return TaskDialog::reject();
}

#include "moc_TaskSectionView.cpp"
