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

#include <algorithm>

#include <Inventor/SoFullPath.h>
#include <Inventor/actions/SoAudioRenderAction.h>
#include <Inventor/actions/SoCallbackAction.h>
#include <Inventor/actions/SoGLRenderAction.h>
#include <Inventor/actions/SoGetBoundingBoxAction.h>
#include <Inventor/actions/SoGetMatrixAction.h>
#include <Inventor/actions/SoGetPrimitiveCountAction.h>
#include <Inventor/actions/SoHandleEventAction.h>
#include <Inventor/actions/SoPickAction.h>
#include <Inventor/actions/SoSearchAction.h>
#include <Inventor/elements/SoCacheElement.h>
#include <Inventor/misc/SoChildList.h>
#include <Inventor/nodes/SoClipPlane.h>

#include "SoFCSectionNodes.h"

using namespace Gui;

// ----------------------------------------------------------------------------

void SoFCSectionFilter::addInstance(Stack stack)
{
    if (stack.empty()) {
        return;
    }
    heads.insert(stack.front());
    instances.push_back(std::move(stack));
}

SoFCSectionFilter::Relation SoFCSectionFilter::relation(const Stack& stack) const
{
    if (stack.empty() || !heads.contains(stack.front())) {
        return Relation::Unrelated;
    }

    bool ancestor = false;
    for (const auto& instance : instances) {
        const std::size_t common = std::min(instance.size(), stack.size());
        if (!std::equal(instance.begin(), instance.begin() + common, stack.begin())) {
            continue;
        }
        if (stack.size() >= instance.size()) {
            return Relation::Inside;
        }
        ancestor = true;
    }
    return ancestor ? Relation::Ancestor : Relation::Unrelated;
}

bool SoFCSectionFilter::isSectioned(const Stack& stack) const
{
    const bool inside = relation(stack) == Relation::Inside;
    return inside == sectionListed;
}

// ----------------------------------------------------------------------------

int SoFCSectionRouting::owner(const Stack& stack) const
{
    for (std::size_t i = 0; i < filters.size(); ++i) {
        const auto& filter = filters[i];
        if (!filter) {
            return static_cast<int>(i);
        }
        switch (filter->relation(stack)) {
            case SoFCSectionFilter::Relation::Ancestor:
                // Decided further down
                return Mixed;
            case SoFCSectionFilter::Relation::Inside:
                if (filter->sectionListed) {
                    return static_cast<int>(i);
                }
                break;
            case SoFCSectionFilter::Relation::Unrelated:
                if (!filter->sectionListed) {
                    return static_cast<int>(i);
                }
                break;
        }
    }
    return Uncut;
}

// ----------------------------------------------------------------------------

std::atomic<int> SoFCSectionPass::routedPasses {0};

SO_NODE_SOURCE(SoFCSectionPass)

void SoFCSectionPass::initClass()
{
    SO_NODE_INIT_CLASS(SoFCSectionPass, SoSeparator, "Separator");
}

void SoFCSectionPass::finish()
{
    atexit_cleanup();
}

SoFCSectionPass::SoFCSectionPass()
{
    SO_NODE_CONSTRUCTOR(SoFCSectionPass);
    // The content is cached by the view providers themselves
    renderCaching = SoSeparator::OFF;
}

SoFCSectionPass::~SoFCSectionPass()
{
    if (routing) {
        --routedPasses;
    }
}

void SoFCSectionPass::setup(
    const std::vector<SoClipPlane*>& planes,
    SoNode* content,
    std::shared_ptr<const SoFCSectionRouting> passRouting,
    int passSection
)
{
    // Avoid touching the scene if nothing changed, it invalidates paths and caches
    std::vector<SoNode*> wanted(planes.begin(), planes.end());
    if (content) {
        wanted.push_back(content);
    }
    bool same = routing == passRouting && section == passSection
        && getNumChildren() == static_cast<int>(wanted.size());
    for (int i = 0; same && i < getNumChildren(); ++i) {
        same = getChild(i) == wanted[i];
    }
    if (same) {
        return;
    }

    removeAllChildren();
    section = passSection;
    for (auto plane : planes) {
        addChild(plane);
    }
    if (content) {
        addChild(content);
    }

    if (routing && !passRouting) {
        --routedPasses;
    }
    else if (!routing && passRouting) {
        ++routedPasses;
    }
    routing = std::move(passRouting);
}

bool SoFCSectionPass::traverseRoot(SoAction* action, const SoFCSectionFilter::Stack& stack)
{
    if (routedPasses.load(std::memory_order_relaxed) == 0) {
        return true;
    }

    auto path = static_cast<const SoFullPath*>(action->getCurPath());
    if (!path) {
        return true;
    }

    const SoFCSectionPass* pass = nullptr;
    for (int i = 0, count = path->getLength(); i < count; ++i) {
        SoNode* node = path->getNode(i);
        if (node->isOfType(SoFCSectionPass::getClassTypeId())) {
            pass = static_cast<const SoFCSectionPass*>(node);
            break;
        }
    }
    if (!pass || !pass->routing) {
        return true;
    }

    // What gets rendered now depends on the pass, so no cache above this node
    // may record it
    if (action->isOfType(SoGLRenderAction::getClassTypeId())) {
        SoCacheElement::invalidate(action->getState());
    }

    const int owner = pass->routing->owner(stack);
    return owner == SoFCSectionRouting::Mixed || owner == pass->section;
}

// ----------------------------------------------------------------------------

SO_NODE_SOURCE(SoFCSectionRoot)

void SoFCSectionRoot::initClass()
{
    SO_NODE_INIT_CLASS(SoFCSectionRoot, SoGroup, "Group");
}

void SoFCSectionRoot::finish()
{
    atexit_cleanup();
}

SoFCSectionRoot::SoFCSectionRoot()
{
    SO_NODE_CONSTRUCTOR(SoFCSectionRoot);
}

SoFCSectionRoot::~SoFCSectionRoot()
{
    if (content) {
        content->unref();
    }
}

void SoFCSectionRoot::setContent(SoNode* node)
{
    if (node) {
        node->ref();
    }
    if (content) {
        content->unref();
    }
    content = node;
    clearSections();
}

void SoFCSectionRoot::clearSections()
{
    setSections({});
}

void SoFCSectionRoot::setSections(const std::vector<Section>& sections)
{
    // With several sections, or one that doesn't cut everything, each pass only
    // renders the objects of its section
    std::shared_ptr<const SoFCSectionRouting> newRouting;
    bool cutsAll = false;
    if (sections.size() > 1 || (sections.size() == 1 && sections.front().filter)) {
        auto candidate = std::make_shared<SoFCSectionRouting>();
        for (const auto& section : sections) {
            candidate->filters.push_back(section.filter);
            cutsAll = cutsAll || !section.filter;
        }
        // Keep the passes untouched if nothing changed
        if (routing && *routing == *candidate) {
            newRouting = routing;
        }
        else {
            newRouting = std::move(candidate);
        }
    }
    routing = newRouting;

    struct PassSetup
    {
        const std::vector<SoClipPlane*>* planes;
        int section;
    };
    static const std::vector<SoClipPlane*> noPlanes;
    std::vector<PassSetup> passes;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        for (const auto& planes : sections[i].passes) {
            passes.push_back({&planes, static_cast<int>(i)});
        }
    }
    if (routing && !cutsAll) {
        passes.push_back({&noPlanes, SoFCSectionRouting::Uncut});
    }
    if (passes.empty()) {
        passes.push_back({&noPlanes, 0});
    }

    // Reuse the existing passes so that paths into the scene stay valid as far
    // as possible
    int index = 0;
    for (const auto& setup : passes) {
        SoFCSectionPass* pass = nullptr;
        if (index < getNumChildren() && getChild(index)->isOfType(SoFCSectionPass::getClassTypeId())) {
            pass = static_cast<SoFCSectionPass*>(getChild(index));
        }
        else {
            pass = new SoFCSectionPass;
            insertChild(pass, index);
        }
        pass->setup(*setup.planes, content, routing, setup.section);
        ++index;
    }

    while (getNumChildren() > index) {
        removeChild(getNumChildren() - 1);
    }
    for (const auto& section : sections) {
        if (section.caps) {
            addChild(section.caps);
        }
    }
    sectioned = !sections.empty();
}

void SoFCSectionRoot::traversePrimary(SoAction* action)
{
    int numIndices = 0;
    const int* indices = nullptr;
    switch (action->getPathCode(numIndices, indices)) {
        case SoAction::IN_PATH:
            // Follow the path, whatever pass it goes through
            inherited::doAction(action);
            break;
        case SoAction::NO_PATH:
        case SoAction::BELOW_PATH:
            if (getNumChildren() > 0) {
                getChildren()->traverse(action, 0);
            }
            break;
        case SoAction::OFF_PATH:
            // All children are separators and don't affect the state
            break;
    }
}

void SoFCSectionRoot::doAction(SoAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::callback(SoCallbackAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::getBoundingBox(SoGetBoundingBoxAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::getMatrix(SoGetMatrixAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::handleEvent(SoHandleEventAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::pick(SoPickAction* action)
{
    // All passes and the caps
    inherited::doAction(action);
}

void SoFCSectionRoot::search(SoSearchAction* action)
{
    SoNode::search(action);
    if (action->isFound()) {
        return;
    }
    traversePrimary(action);
}

void SoFCSectionRoot::getPrimitiveCount(SoGetPrimitiveCountAction* action)
{
    traversePrimary(action);
}

void SoFCSectionRoot::audioRender(SoAudioRenderAction* action)
{
    traversePrimary(action);
}
