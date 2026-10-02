#pragma once

#include "LamaPon/Core/VersionCompare.h"

#include <string>
#include <string_view>

namespace LamaPon::Hub
{

    struct UpdateCheckResult final
    {
        // 現行版より新しい版の有無
        bool updateAvailable{};
        // 先頭vを除いた最新タグ
        std::string latestVersion;
        // GitHubのリリースURL
        std::string releaseUrl;
    };


    using LamaPon::ParseVersionNumbers;
    using LamaPon::IsNewerVersion;

    // 通信せず最新リリースJSONを比較する(responseJson: GitHub応答JSON, currentVersion: 比較する現行版)。
    [[nodiscard]] UpdateCheckResult ParseLatestRelease(
        std::string_view responseJson,
        std::string_view currentVersion);

    // 最新リリースを取得し、通信失敗や不正JSONなら更新なしとする。
    [[nodiscard]] UpdateCheckResult CheckForEngineUpdate();
}
