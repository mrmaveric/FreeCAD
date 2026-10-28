// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <format>
#define FC_OS_MACOSX 1
#include "App/ProgramOptionsUtilities.h"
#include "App/Application.h"

#include <src/App/InitApplication.h>


using namespace App::Util;

using Spr = std::pair<std::string, std::string>;


class ApplicationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(ApplicationTest, fCustomSyntaxLookup)
{
    Spr res {customSyntax("-display")};
    Spr exp {"display", "null"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxMac)
{
    Spr res {customSyntax("-psn_stuff")};
    Spr exp {"psn", "stuff"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxWidgetCount)
{
    Spr res {customSyntax("-widgetcount")};
    Spr exp {"widgetcount", ""};
    EXPECT_EQ(res, exp);
}
TEST_F(ApplicationTest, fCustomSyntaxNotFound)
{
    Spr res {customSyntax("-displayx")};
    Spr exp {"", ""};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxAmpersand)
{
    Spr res {customSyntax("@freddie")};
    Spr exp {"response-file", "freddie"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxEmptyIn)
{
    Spr res {customSyntax("")};
    Spr exp {"", ""};
    EXPECT_EQ(res, exp);
};

TEST_F(ApplicationTest, versionStringShowsEdition)
{
    auto& config = App::Application::Config();
    const std::string plain = std::format(
        "{}.{}.{}{}",
        config["BuildVersionMajor"],
        config["BuildVersionMinor"],
        config["BuildVersionPoint"],
        config["BuildVersionSuffix"]
    );

    config.erase("BuildEdition");
    EXPECT_EQ(App::Application::getVersionString(), plain);
    EXPECT_TRUE(App::Application::getNameWithVersion().ends_with(" " + plain));

    config["BuildEdition"] = "Test Edition";
    EXPECT_EQ(App::Application::getVersionString(), plain + " (Test Edition)");
    EXPECT_TRUE(App::Application::getNameWithVersion().ends_with(plain + " (Test Edition)"));
    config.erase("BuildEdition");
}
