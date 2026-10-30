// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>
#include <memory>

#include <Gui/Section/SoFCSectionNodes.h>

using Gui::SoFCSectionFilter;
using Gui::SoFCSectionRouting;

namespace
{

/// Stand-ins for the SoFCSelectionRoot nodes of instances, only compared and never used
SoNode* node(int index)
{
    static std::array<char, 8> storage {};
    return reinterpret_cast<SoNode*>(&storage.at(index));  // NOLINT
}

// An assembly with two parts, and a separate object
const SoFCSectionFilter::Stack assembly {node(0)};
const SoFCSectionFilter::Stack part1 {node(0), node(1)};
const SoFCSectionFilter::Stack part2 {node(0), node(2)};
const SoFCSectionFilter::Stack other {node(3)};

std::shared_ptr<const SoFCSectionFilter> cutOnly(
    std::initializer_list<SoFCSectionFilter::Stack> stacks
)
{
    auto filter = std::make_shared<SoFCSectionFilter>();
    filter->sectionListed = true;
    for (const auto& stack : stacks) {
        filter->addInstance(stack);
    }
    return filter;
}

std::shared_ptr<const SoFCSectionFilter> exclude(
    std::initializer_list<SoFCSectionFilter::Stack> stacks
)
{
    auto filter = std::make_shared<SoFCSectionFilter>();
    for (const auto& stack : stacks) {
        filter->addInstance(stack);
    }
    return filter;
}

}  // namespace

TEST(SectionRouting, NoSection)
{
    SoFCSectionRouting routing;
    EXPECT_EQ(routing.owner(other), SoFCSectionRouting::Uncut);
}

TEST(SectionRouting, SectionCuttingEverything)
{
    SoFCSectionRouting routing;
    routing.filters = {nullptr};
    EXPECT_EQ(routing.owner(assembly), 0);
    EXPECT_EQ(routing.owner(other), 0);
}

TEST(SectionRouting, ExcludedObjectsStayUncut)
{
    SoFCSectionRouting routing;
    routing.filters = {exclude({other})};
    EXPECT_EQ(routing.owner(other), SoFCSectionRouting::Uncut);
    EXPECT_EQ(routing.owner(part1), 0);
}

TEST(SectionRouting, SectionOfNothing)
{
    SoFCSectionRouting routing;
    routing.filters = {cutOnly({})};
    EXPECT_EQ(routing.owner(other), SoFCSectionRouting::Uncut);
}

TEST(SectionRouting, SectionsOfDifferentObjects)
{
    SoFCSectionRouting routing;
    routing.filters = {cutOnly({part1}), cutOnly({other})};
    // Below the assembly the parts are decided one by one
    EXPECT_EQ(routing.owner(assembly), SoFCSectionRouting::Mixed);
    EXPECT_EQ(routing.owner(part1), 0);
    EXPECT_EQ(routing.owner(part2), SoFCSectionRouting::Uncut);
    EXPECT_EQ(routing.owner(other), 1);
}

TEST(SectionRouting, ExcludedObjectsGoToTheNextSection)
{
    SoFCSectionRouting routing;
    routing.filters = {exclude({other}), cutOnly({other})};
    EXPECT_EQ(routing.owner(assembly), 0);
    EXPECT_EQ(routing.owner(other), 1);
}

TEST(SectionRouting, FirstSectionWins)
{
    SoFCSectionRouting routing;
    routing.filters = {cutOnly({part1}), nullptr};
    EXPECT_EQ(routing.owner(assembly), SoFCSectionRouting::Mixed);
    EXPECT_EQ(routing.owner(part1), 0);
    EXPECT_EQ(routing.owner(part2), 1);
    EXPECT_EQ(routing.owner(other), 1);

    // A section of the whole assembly takes its parts too
    routing.filters = {cutOnly({assembly}), cutOnly({part1})};
    EXPECT_EQ(routing.owner(assembly), 0);
    EXPECT_EQ(routing.owner(part1), 0);
}
