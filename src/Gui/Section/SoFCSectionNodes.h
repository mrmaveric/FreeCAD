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

/** Decides which of the shown sections renders an instance.
 *
 * Every instance is rendered once: cut by one section, or uncut if no section cuts
 * it. Where several sections cut the same instance, the first one wins.
 */
class GuiExport SoFCSectionRouting
{
public:
    using Stack = SoFCSectionFilter::Stack;

    /// No section cuts the instance
    static constexpr int Uncut = -1;
    /// The instance has descendants with different owners
    static constexpr int Mixed = -2;

    /// The objects cut by each section in order of priority, null for all objects
    std::vector<std::shared_ptr<const SoFCSectionFilter>> filters;

    bool operator==(const SoFCSectionRouting& other) const
    {
        return filters == other.filters;
    }

    /// The index of the section that renders the instance and all below it, Uncut or Mixed
    int owner(const Stack& stack) const;
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

    /** Configures the pass.
     * @param planes the clip planes of the pass
     * @param content the objects to render
     * @param routing decides which objects the pass renders, null for all of them
     * @param section the index of the section in @p routing whose objects the pass
     * renders, SoFCSectionRouting::Uncut for the objects no section cuts
     */
    void setup(
        const std::vector<SoClipPlane*>& planes,
        SoNode* content,
        std::shared_ptr<const SoFCSectionRouting> routing,
        int section
    );

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
    std::shared_ptr<const SoFCSectionRouting> routing;
    int section {0};

    static std::atomic<int> routedPasses;
};

/** Root of the physical objects of a 3D view.
 *
 * Without an active section it has a single pass without clip planes. With
 * sections it may render the objects in several passes, for instance to cut away
 * a corner of the model, to cut different objects with different sections or to
 * show excluded objects uncut, followed by the cap faces.
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

    struct Section
    {
        /// The clip planes of each pass rendering the objects the section cuts
        std::vector<std::vector<SoClipPlane*>> passes;
        /// The objects the section cuts, null for all objects
        std::shared_ptr<const SoFCSectionFilter> filter;
        /// Scene graph of the cap faces, may be null
        SoNode* caps {nullptr};
    };

    /** Shows sections.
     * @param sections the sections in order of priority, see SoFCSectionRouting
     */
    void setSections(const std::vector<Section>& sections);
    /// Restores the plain scene without any clipping
    void clearSections();
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
    std::shared_ptr<const SoFCSectionRouting> routing;
    bool sectioned {false};
};

}  // namespace Gui
