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

std::atomic<int> SoFCSectionPass::filteredPasses {0};

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
    if (filter) {
        --filteredPasses;
    }
}

void SoFCSectionPass::setup(
    Mode passMode,
    const std::vector<SoClipPlane*>& planes,
    SoNode* content,
    std::shared_ptr<const SoFCSectionFilter> passFilter
)
{
    if (passFilter && passFilter->empty()) {
        passFilter.reset();
    }

    // Avoid touching the scene if nothing changed, it invalidates paths and caches
    std::vector<SoNode*> wanted(planes.begin(), planes.end());
    if (content) {
        wanted.push_back(content);
    }
    bool same = mode == passMode && filter == passFilter
        && getNumChildren() == static_cast<int>(wanted.size());
    for (int i = 0; same && i < getNumChildren(); ++i) {
        same = getChild(i) == wanted[i];
    }
    if (same) {
        return;
    }

    removeAllChildren();
    mode = passMode;
    for (auto plane : planes) {
        addChild(plane);
    }
    if (content) {
        addChild(content);
    }

    if (filter && !passFilter) {
        --filteredPasses;
    }
    else if (!filter && passFilter) {
        ++filteredPasses;
    }
    filter = std::move(passFilter);
}

bool SoFCSectionPass::traverseRoot(SoAction* action, const SoFCSectionFilter::Stack& stack)
{
    if (filteredPasses.load(std::memory_order_relaxed) == 0) {
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
    if (!pass || !pass->filter) {
        return true;
    }

    // What gets rendered now depends on the pass, so no cache above this node
    // may record it
    if (action->isOfType(SoGLRenderAction::getClassTypeId())) {
        SoCacheElement::invalidate(action->getState());
    }

    const auto relation = pass->filter->relation(stack);
    if (relation == SoFCSectionFilter::Relation::Ancestor) {
        return true;
    }
    const bool sectioned = (relation == SoFCSectionFilter::Relation::Inside)
        == pass->filter->sectionListed;
    return sectioned == (pass->mode == Mode::Clipped);
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
    clearSection();
}

void SoFCSectionRoot::clearSection()
{
    std::vector<PassSetup> passes(1);
    setSection(passes, nullptr, nullptr);
    sectioned = false;
}

void SoFCSectionRoot::setSection(
    const std::vector<PassSetup>& passes,
    std::shared_ptr<const SoFCSectionFilter> filter,
    SoNode* caps
)
{
    // Reuse the existing passes so that paths into the scene stay valid as far
    // as possible
    const int numPasses = std::max<int>(1, static_cast<int>(passes.size()));
    int index = 0;
    for (; index < numPasses; ++index) {
        SoFCSectionPass* pass = nullptr;
        if (index < getNumChildren() && getChild(index)->isOfType(SoFCSectionPass::getClassTypeId())) {
            pass = static_cast<SoFCSectionPass*>(getChild(index));
        }
        else {
            pass = new SoFCSectionPass;
            insertChild(pass, index);
        }
        if (index < static_cast<int>(passes.size())) {
            pass->setup(passes[index].mode, passes[index].planes, content, filter);
        }
        else {
            pass->setup(SoFCSectionPass::Mode::Clipped, {}, content, nullptr);
        }
    }

    while (getNumChildren() > index) {
        removeChild(getNumChildren() - 1);
    }
    if (caps) {
        addChild(caps);
    }
    sectioned = true;
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
