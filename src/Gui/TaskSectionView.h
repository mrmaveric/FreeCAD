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
#include <memory>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <App/DocumentObserver.h>

#include "DocumentObserver.h"
#include "Selection/Selection.h"
#include "TaskView/TaskDialog.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;

namespace App
{
class Property;
class SectionView;
}  // namespace App

namespace Gui
{

class ColorButton;
class GizmoContainer;
class LinearGizmo;
class QuantitySpinBox;
class RotationGizmo;
class ViewProviderSectionView;

/// The controls of the section view task dialog
class TaskSectionView: public QObject, public SelectionObserver
{
    Q_OBJECT

public:
    explicit TaskSectionView(ViewProviderSectionView* vp);
    ~TaskSectionView() override;

    QWidget* getSectionWidget() const
    {
        return sectionWidget;
    }
    QWidget* getObjectsWidget() const
    {
        return objectsWidget;
    }
    QWidget* getDisplayWidget() const
    {
        return displayWidget;
    }

private:
    struct PlaneWidgets
    {
        QWidget* page {nullptr};
        QCheckBox* enabled {nullptr};
        QComboBox* base {nullptr};
        QPushButton* selectReference {nullptr};
        QLabel* reference {nullptr};
        QuantitySpinBox* offset {nullptr};
        QuantitySpinBox* angleX {nullptr};
        QuantitySpinBox* angleY {nullptr};
        QPushButton* flip {nullptr};
        QPushButton* alignToView {nullptr};
    };

    enum class PickMode
    {
        None,
        Reference,
        Objects,
    };

    App::SectionView* getObject() const;

    void setupSectionWidget();
    void setupPlanePage(int index);
    void setupBoxPage();
    void setupObjectsWidget();
    void setupDisplayWidget();
    void setupGizmos();
    void connectSignals();

    void refresh();
    void refreshObjects();
    void refreshStats();
    void updateVisibility();
    void setGizmoPositions();
    void scheduleRefresh();

    void onSelectionChanged(const SelectionChanges& msg) override;
    void setPickMode(PickMode mode, int plane = -1);

    void onBoxExtentChanged(int axis, bool max, double value);
    void fitBoxToModel();
    void alignToView(int index);
    void removeSelectedObjects();

    std::array<double, 6> getBoxExtents() const;

    ViewProviderSectionView* vp;
    App::DocumentObjectT objectT;
    fastsignals::scoped_connection connectChanged;

    QWidget* sectionWidget {nullptr};
    QWidget* objectsWidget {nullptr};
    QWidget* displayWidget {nullptr};

    // Section
    QComboBox* type {nullptr};
    QComboBox* combination {nullptr};
    QLabel* combinationLabel {nullptr};
    QStackedWidget* stack {nullptr};
    QTabWidget* planeTabs {nullptr};
    std::array<PlaneWidgets, 3> planes;
    std::array<QuantitySpinBox*, 6> boxExtents {};
    QPushButton* fitBox {nullptr};
    QLabel* hint {nullptr};

    // Objects
    QComboBox* objectMode {nullptr};
    QListWidget* objectList {nullptr};
    QPushButton* addObjects {nullptr};
    QPushButton* removeObjects {nullptr};

    // Display
    QComboBox* fillMode {nullptr};
    ColorButton* fillColor {nullptr};
    QComboBox* hatchPattern {nullptr};
    QCheckBox* autoSpacing {nullptr};
    QuantitySpinBox* hatchSpacing {nullptr};
    QuantitySpinBox* hatchAngle {nullptr};
    QCheckBox* hatchAdaptive {nullptr};
    QCheckBox* hatchVary {nullptr};
    QCheckBox* showOutline {nullptr};
    ColorButton* outlineColor {nullptr};
    QLabel* stats {nullptr};

    PickMode pickMode {PickMode::None};
    int pickPlane {-1};

    std::unique_ptr<GizmoContainer> gizmoContainer;
    LinearGizmo* offsetGizmo {nullptr};
    RotationGizmo* angleXGizmo {nullptr};
    RotationGizmo* angleYGizmo {nullptr};
    std::array<LinearGizmo*, 6> boxGizmos {};

    QTimer refreshTimer;
    QTimer statsTimer;
    bool refreshing {false};
};

/// Task dialog creating and editing a section view
class TaskDlgSectionView: public TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgSectionView(ViewProviderSectionView* vp);
    ~TaskDlgSectionView() override;

    ViewProviderSectionView* getViewProvider() const;

    void open() override;
    bool accept() override;
    bool reject() override;
    bool isAllowedAlterDocument() const override
    {
        return false;
    }
    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    WeakPtrT<ViewProviderSectionView> viewProvider;
    std::unique_ptr<TaskSectionView> panel;
};

}  // namespace Gui
