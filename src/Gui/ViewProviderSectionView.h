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

#include <App/PropertyGeo.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <App/SectionView.h>

#include "Section/SectionScene.h"
#include "ViewProviderDragger.h"

class SoSwitch;
class SoSeparator;

namespace Gui
{

/** View provider of App::SectionView.
 *
 * While visible, it shows the section in all 3D views of the document. Several
 * section views can be visible as long as they cut different objects: showing one
 * hides the others that cut the same objects.
 */
class GuiExport ViewProviderSectionView: public ViewProviderDragger
{
    PROPERTY_HEADER_WITH_OVERRIDE(Gui::ViewProviderSectionView);

public:
    ViewProviderSectionView();
    ~ViewProviderSectionView() override;

    App::PropertyEnumeration FillMode;
    App::PropertyColor FillColor;
    App::PropertyEnumeration HatchPattern;
    App::PropertyLength HatchSpacing;
    App::PropertyAngle HatchAngle;
    App::PropertyBool HatchAdaptive;
    App::PropertyBool HatchVary;
    App::PropertyFloatConstraint HatchLineWidth;
    App::PropertyBool ShowOutline;
    App::PropertyColor OutlineColor;
    App::PropertyFloatConstraint OutlineWidth;
    App::PropertyBool ShowPlanes;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    bool isPartOfPhysicalObject() const override
    {
        return false;
    }
    bool doubleClicked() override;
    void setupContextMenu(QMenu* menu, QObject* receiver, const char* member) override;
    bool onDelete(const std::vector<std::string>& subNames) override;
    void show() override;
    void hide() override;
    void finishRestoring() override;
    void beforeDelete() override;
    void reattach(App::DocumentObject* obj) override;

    App::SectionView* getSectionView() const;

    /// The section scene, while the section is shown
    Section::SectionScene* getScene() const;

    /// Shows the section planes or box in the 3D view
    void setPlanesVisible(bool on);

    /// Re-reads the section definition from the object
    void updateSection();

    /** Center of the section geometry projected onto plane @p index.
     *
     * Gives the manipulators a position close to the model.
     */
    Base::Vector3d getPlaneCenter(int index) const;

    /// Bounding box of the sectioned geometry, or of all objects if not shown
    Base::BoundBox3d getModelBoundBox() const;

    /// Flips the cut side of all planes
    void flip();

protected:
    void onChanged(const App::Property* prop) override;
    bool setEdit(int modNum) override;
    void unsetEdit(int modNum) override;

private:
    void scheduleActivation();
    void activate();
    void deactivate();
    void hideOverlappingSections();
    void updateDisplaySettings();
    void updateFilter();
    void updatePlaneVisuals();
    std::vector<App::SectionView::Plane> currentPlanes() const;
    std::vector<Section::CutPlane> computeCutPlanes() const;

    std::unique_ptr<Section::SectionScene> scene;
    CoinPtr<SoSwitch> planeSwitch;
    CoinPtr<SoSeparator> planeVisuals;
    bool planesVisible {false};


    static const char* FillModeEnums[];
    static const char* HatchEnums[];
    static App::PropertyFloatConstraint::Constraints lineWidthRange;
};

}  // namespace Gui
