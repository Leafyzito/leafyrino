// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Version.hpp"
#include "Test.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <semver/semver.hpp>

using namespace chatterino;

namespace {

const QString X64_INSTALLER_SHA =
    "1111111111111111111111111111111111111111111111111111111111111111";
const QString X64_PORTABLE_SHA =
    "2222222222222222222222222222222222222222222222222222222222222222";
const QString ARM_INSTALLER_SHA =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

QJsonObject parse(const QString &json)
{
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QJsonObject validInfo()
{
    return parse(
        QString(R"({
        "build": 6921,
        "commit": "53961699d8318abaeec35f563adf5ea838658f1f",
        "windows": {
            "x86-64": {
                "installer": {"url": "https://example.com/x64.exe", "sha256": "%1"},
                "portable": {"url": "https://example.com/x64.zip", "sha256": "%2"}
            },
            "arm64": {
                "installer": {"url": "https://example.com/arm.exe", "sha256": "%3"}
            }
        }
    })")
            .arg(X64_INSTALLER_SHA, X64_PORTABLE_SHA, ARM_INSTALLER_SHA));
}

}  // namespace

TEST(Updates, ParseBuildInfo)
{
    auto info = Updates::parseBuildInfo(validInfo(), "x86-64");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->build, 6921);
    EXPECT_EQ(info->commit, "53961699d8318abaeec35f563adf5ea838658f1f");

    ASSERT_TRUE(info->installer.has_value());
    EXPECT_EQ(info->installer->url, "https://example.com/x64.exe");
    EXPECT_EQ(info->installer->sha256, X64_INSTALLER_SHA);

    ASSERT_TRUE(info->portable.has_value());
    EXPECT_EQ(info->portable->url, "https://example.com/x64.zip");
    EXPECT_EQ(info->portable->sha256, X64_PORTABLE_SHA);
}

TEST(Updates, ParseBuildInfoPicksArch)
{
    auto info = Updates::parseBuildInfo(validInfo(), "arm64");
    ASSERT_TRUE(info.has_value());

    ASSERT_TRUE(info->installer.has_value());
    EXPECT_EQ(info->installer->url, "https://example.com/arm.exe");
    // checksums are compared in lowercase
    EXPECT_EQ(info->installer->sha256, ARM_INSTALLER_SHA.toLower());

    EXPECT_FALSE(info->portable.has_value());
}

TEST(Updates, ParseBuildInfoUnknownArch)
{
    auto info = Updates::parseBuildInfo(validInfo(), "riscv64");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->build, 6921);
    EXPECT_FALSE(info->installer.has_value());
    EXPECT_FALSE(info->portable.has_value());
}

TEST(Updates, ParseBuildInfoRequiresBuild)
{
    EXPECT_FALSE(Updates::parseBuildInfo({}, "x86-64").has_value());
    EXPECT_FALSE(Updates::parseBuildInfo(parse(R"({"build": 0})"), "x86-64")
                     .has_value());
    EXPECT_FALSE(
        Updates::parseBuildInfo(parse(R"({"build": "6921"})"), "x86-64")
            .has_value());
    EXPECT_TRUE(Updates::parseBuildInfo(parse(R"({"build": 1})"), "x86-64")
                    .has_value());
}

TEST(Updates, ParseBuildInfoRejectsBadDownloads)
{
    auto info = Updates::parseBuildInfo(parse(QString(R"({
            "build": 6921,
            "windows": {
                "x86-64": {
                    "installer": {"url": "http://example.com/x64.exe", "sha256": "%1"},
                    "portable": {"url": "https://example.com/x64.zip", "sha256": "1234"}
                }
            }
        })")
                                                  .arg(X64_INSTALLER_SHA)),
                                        "x86-64");
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->installer.has_value()) << "plain http must be rejected";
    EXPECT_FALSE(info->portable.has_value())
        << "a malformed checksum must be rejected";
}

TEST(Updates, ValidateCurrentVersion)
{
    EXPECT_NO_THROW([[maybe_unused]] auto v = semver::from_string(
                        Version::instance().version().toStdString()))
        << "Current version must be valid semver";
    EXPECT_EQ(Version::instance().version(), CHATTERINO_VERSION);
}
