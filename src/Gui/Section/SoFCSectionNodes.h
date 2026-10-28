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

#include <atomic>
#include <memory>
#include <unordered_set>
#include <vector>

#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoSeparator.h>

#include <FCGlobal.h>

class SoAction;
class SoClipPlane;

namespace Gui
{

/** Instances that are treated differently by a section view.
 *
 * An instance is identified by the sequence of SoFCSelectionRoot nodes from the
 * top level object down to the instance, which is the same key the selection
 * contexts use. A prefix identifies a whole sub-tree, e.g. a link in an assembly.
 */
class GuiExport SoFCSectionFilter
{
public:
    using Stack = std::vector<SoNode*>;

    enum class Relation
    {
        Unrelated,
        Inside,    ///< the node is a listed instance or below one
        Ancestor,  ///< a listed instance is below the node
    };

    /// When true only the listed instances are cut, otherwise they are excluded
    bool sectionListed {false};

    void addInstance(Stack stack);
    bool empty() const
    {
        return instances.empty();
    }

    bool operator==(const SoFCSectionFilter& other) const
    {
        return sectionListed == other.sectionListed && instances == other.instances;
    }

    Relation relation(const Stack& stack) const;
    /// Whether the instance with the given stack is cut by the section
    bool isSectioned(const Stack& stack) const;

private:
    std::vector<Stack> instances;
    std::unordered_set<SoNode*> heads;
};

/** One rendering pass of the sectioned scene.
 *
 * Holds the clip planes of the pass followed by the object group of the viewer.
 */
class GuiExport SoFCSectionPass: public SoSeparator
{
    using inherited = SoSeparator;
    SO_NODE_HEADER(Gui::SoFCSectionPass);

public:
    static void initClass();
    static void finish();
    SoFCSectionPass();

    enum class Mode
    {
        /// Renders the sectioned objects clipped by the planes of this pass
        Clipped,
        /// Renders the objects excluded from the section
        Unclipped,
    };

    void setup(
        Mode mode,
        const std::vector<SoClipPlane*>& planes,
        SoNode* content,
        std::shared_ptr<const SoFCSectionFilter> filter
    );

    Mode getMode() const
    {
        return mode;
    }

    /** Decides whether a SoFCSelectionRoot is traversed in the current pass.
     *
     * Called by SoFCSelectionRoot during rendering and picking.
     * @param action the current action
     * @param stack the SoFCSelectionRoot stack including the querying node
     */
    static bool traverseRoot(SoAction* action, const SoFCSectionFilter::Stack& stack);

protected:
    ~SoFCSectionPass() override;

private:
    Mode mode {Mode::Clipped};
    std::shared_ptr<const SoFCSectionFilter> filter;

    static std::atomic<int> filteredPasses;
};

/** Root of the physical objects of a 3D view.
 *
 * Without an active section it has a single pass without clip planes. With a
 * section it may render the objects in several passes, for instance to cut away
 * a corner of the model or to show excluded objects uncut, followed by the cap
 * faces.
 *
 * Only rendering and picking traverse all passes. Other actions only see the
 * first pass so they find every object exactly once.
 */
class GuiExport SoFCSectionRoot: public SoGroup
{
    using inherited = SoGroup;
    SO_NODE_HEADER(Gui::SoFCSectionRoot);

public:
    static void initClass();
    static void finish();
    SoFCSectionRoot();

    void setContent(SoNode* content);
    SoNode* getContent() const
    {
        return content;
    }

    struct PassSetup
    {
        SoFCSectionPass::Mode mode {SoFCSectionPass::Mode::Clipped};
        std::vector<SoClipPlane*> planes;
    };

    /** Configures the section.
     * @param passes the passes to render. The first one must be Clipped.
     * @param filter instances that are excluded from the section or the only ones sectioned
     * @param caps scene graph of the cap faces, may be null
     */
    void setSection(
        const std::vector<PassSetup>& passes,
        std::shared_ptr<const SoFCSectionFilter> filter,
        SoNode* caps
    );
    /// Restores the plain scene without any clipping
    void clearSection();
    bool hasSection() const
    {
        return sectioned;
    }

    void doAction(SoAction* action) override;
    void callback(SoCallbackAction* action) override;
    void getBoundingBox(SoGetBoundingBoxAction* action) override;
    void getMatrix(SoGetMatrixAction* action) override;
    void handleEvent(SoHandleEventAction* action) override;
    void pick(SoPickAction* action) override;
    void search(SoSearchAction* action) override;
    void getPrimitiveCount(SoGetPrimitiveCountAction* action) override;
    void audioRender(SoAudioRenderAction* action) override;

protected:
    ~SoFCSectionRoot() override;

private:
    void traversePrimary(SoAction* action);

    SoNode* content {nullptr};
    bool sectioned {false};
};

}  // namespace Gui
