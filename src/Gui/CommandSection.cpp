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

#include <map>
#include <vector>

#include <Inventor/actions/SoGetBoundingBoxAction.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <App/SectionView.h>

#include "Action.h"
#include "Application.h"
#include "Command.h"
#include "Control.h"
#include "Document.h"
#include "MainWindow.h"
#include "View3DInventor.h"
#include "View3DInventorViewer.h"
#include "ViewProviderSectionView.h"

using namespace Gui;

namespace
{

std::vector<App::SectionView*> visibleSectionViews(App::Document* doc)
{
    std::vector<App::SectionView*> visible;
    if (doc) {
        for (auto obj : doc->getObjectsOfType<App::SectionView>()) {
            if (obj->Visibility.getValue()) {
                visible.push_back(obj);
            }
        }
    }
    return visible;
}

/// The sections Std_SectionView turned off in each document, to turn them on again
std::map<const App::Document*, std::vector<App::DocumentObjectT>>& toggledOff()
{
    static std::map<const App::Document*, std::vector<App::DocumentObjectT>> sections;
    [[maybe_unused]] static auto connection = App::GetApplication().signalDeleteDocument.connect(
        [](const App::Document& doc) { sections.erase(&doc); }
    );
    return sections;
}

View3DInventorViewer* activeViewer()
{
    auto view = qobject_cast<View3DInventor*>(getMainWindow()->activeWindow());
    return view ? view->getViewer() : nullptr;
}

/** Creates a section view cutting through the middle of the model, facing the viewer.
 *
 * Uses the principal plane most parallel to the screen like other CAD programs
 * use the front, top or right plane.
 */
void createSectionView(Gui::Document* guiDoc, View3DInventorViewer* viewer)
{
    App::Document* doc = guiDoc->getDocument();
    guiDoc->openCommand(QT_TRANSLATE_NOOP("Command", "Create section view"));
    auto obj = doc->addObject<App::SectionView>("SectionView");
    if (!obj) {
        guiDoc->abortCommand();
        return;
    }

    if (viewer) {
        const SbVec3f dir = viewer->getViewDirection();
        const Base::Vector3d toViewer(-dir[0], -dir[1], -dir[2]);

        using BasePlane = App::SectionView::BasePlane;
        BasePlane best = BasePlane::XZ;
        double bestDot = 0.0;
        Base::Vector3d bestNormal;
        for (BasePlane base : {BasePlane::XZ, BasePlane::XY, BasePlane::YZ}) {
            Base::Vector3d normal;
            App::SectionView::getPrincipalRotation(base).multVec(Base::Vector3d(0, 0, 1), normal);
            const double dot = normal * toViewer;
            // Ties go to the front plane, then the top plane
            if (std::abs(dot) > std::abs(bestDot) + 1e-3) {
                bestDot = dot;
                best = base;
                bestNormal = normal;
            }
        }
        obj->Plane1Base.setValue(static_cast<long>(best));
        obj->Plane1Flipped.setValue(bestDot < 0.0);

        // Cut through the center of the model
        SoGetBoundingBoxAction action(viewer->getSoRenderManager()->getViewportRegion());
        action.apply(viewer->getObjectGroup());
        const SbBox3f box = action.getBoundingBox();
        if (!box.isEmpty()) {
            const SbVec3f center = box.getCenter();
            const Base::Vector3d pnt(center[0], center[1], center[2]);
            obj->Plane1Offset.setValue(pnt * bestNormal);
        }
    }

    // Showing it hides the sections that cut the same objects
    obj->Visibility.setValue(true);
    if (auto vp = freecad_cast<ViewProviderSectionView*>(guiDoc->getViewProvider(obj))) {
        guiDoc->setEdit(vp, ViewProvider::Default);
    }
}

}  // namespace

//===========================================================================
// Std_SectionView
//===========================================================================

DEF_STD_CMD_AC(StdCmdSectionView)

StdCmdSectionView::StdCmdSectionView()
    : Command("Std_SectionView")
{
    sGroup = "Standard-View";
    sMenuText = QT_TR_NOOP("Section View");
    sToolTipText = QT_TR_NOOP(
        "Toggles the section views of the active document.\n"
        "Cuts the model with planes or a box without changing it and fills the cut faces.\n"
        "Creates a section view if the document has none."
    );
    sWhatsThis = "Std_SectionView";
    sStatusTip = sToolTipText;
    sPixmap = "Std_SectionView";
    eType = Alter3DView;
}

void StdCmdSectionView::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    Gui::Document* guiDoc = getActiveGuiDocument();
    if (!guiDoc) {
        return;
    }
    App::Document* doc = guiDoc->getDocument();

    auto visible = visibleSectionViews(doc);
    if (!visible.empty()) {
        auto& remembered = toggledOff()[doc];
        remembered.clear();
        for (auto obj : visible) {
            // Turning a section off also ends editing it
            auto vp = guiDoc->getViewProvider(obj);
            if (vp && guiDoc->getInEdit() == vp) {
                if (auto dialog = Gui::Control().activeDialog()) {
                    dialog->accept();
                    Gui::Control().closeDialog();
                }
            }
            obj->Visibility.setValue(false);
            remembered.emplace_back(obj);
        }
        return;
    }

    // Turn on the sections that were turned off, or else the last one
    std::vector<App::SectionView*> sections;
    if (auto it = toggledOff().find(doc); it != toggledOff().end()) {
        for (const auto& objT : it->second) {
            auto obj = freecad_cast<App::SectionView*>(objT.getObject());
            if (obj && obj->getDocument() == doc) {
                sections.push_back(obj);
            }
        }
        toggledOff().erase(it);
    }
    if (sections.empty()) {
        sections = doc->getObjectsOfType<App::SectionView>();
        if (sections.size() > 1) {
            sections.erase(sections.begin(), sections.end() - 1);
        }
    }
    if (!sections.empty()) {
        for (auto obj : sections) {
            obj->Visibility.setValue(true);
        }
        return;
    }

    createSectionView(guiDoc, activeViewer());
}

bool StdCmdSectionView::isActive()
{
    auto viewer = activeViewer();
    if (_pcAction) {
        _pcAction->setBlockedChecked(viewer && !visibleSectionViews(getActiveDocument()).empty());
    }
    return viewer != nullptr;
}

Action* StdCmdSectionView::createAction()
{
    Action* action = Command::createAction();
    action->setCheckable(true);
    return action;
}

//===========================================================================
// Std_SectionViewNew
//===========================================================================

DEF_STD_CMD_A(StdCmdSectionViewNew)

StdCmdSectionViewNew::StdCmdSectionViewNew()
    : Command("Std_SectionViewNew")
{
    sGroup = "Standard-View";
    sMenuText = QT_TR_NOOP("New Section View…");
    sToolTipText = QT_TR_NOOP(
        "Creates a new section view and opens it for editing.\n"
        "Section views are kept in the document and can be turned on and off in the tree."
    );
    sWhatsThis = "Std_SectionViewNew";
    sStatusTip = sToolTipText;
    sPixmap = "Std_SectionView";
    eType = Alter3DView;
}

void StdCmdSectionViewNew::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    if (Gui::Document* guiDoc = getActiveGuiDocument()) {
        createSectionView(guiDoc, activeViewer());
    }
}

bool StdCmdSectionViewNew::isActive()
{
    return activeViewer() != nullptr && !Gui::Control().activeDialog();
}

namespace Gui
{

void CreateSectionCommands()
{
    CommandManager& rcCmdMgr = Application::Instance->commandManager();
    rcCmdMgr.addCommand(new StdCmdSectionView());
    rcCmdMgr.addCommand(new StdCmdSectionViewNew());
}

}  // namespace Gui
