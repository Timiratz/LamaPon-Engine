#pragma once

#include "LamaPon/Core/VersionCompare.h"

#include <string>
#include <string_view>

namespace LamaPon::Hub
{
    // GitHub Releasesの最新版チェック結果。
    struct UpdateCheckResult final
    {
        bool updateAvailable{};
        // "2026.8.1" のような先頭vなしのバージョン。
        std::string latestVersion;
        // リリースページのURL（ブラウザーで開く用）。
        std::string releaseUrl;
    };

    // バージョン比較はエンジン共通実装（Core/VersionCompare）を
    // 使います。既存の呼び出し・テストのために同名で公開します。
    using LamaPon::ParseVersionNumbers;
    using LamaPon::IsNewerVersion;

    // GitHub APIの releases/latest 応答JSONを解釈し、現在の
    // バージョンと比較した結果を返します（純関数、通信しません）。
    [[nodiscard]] UpdateCheckResult ParseLatestRelease(
        std::string_view responseJson,
        std::string_view currentVersion);

    // GitHubから最新リリースを取得して現在のエンジン版と比較します。
    // オフライン・応答不正の場合は updateAvailable=false を
    // 返します（例外は投げません）。
    [[nodiscard]] UpdateCheckResult CheckForEngineUpdate();
}
