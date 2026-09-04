#pragma once
#include <filesystem>

namespace LamaPon::Cli
{
    // assets相対・project相対・絶対の指定をassets相対へ正規化します。
    // ファイルを読むコマンドは正規化後も解決先の範囲・実在を検証します。
    [[nodiscard]] std::filesystem::path NormalizeScenePath(
        const std::filesystem::path& projectRoot, const std::filesystem::path& scene);
    [[nodiscard]] std::filesystem::path NormalizeAssetPath(
        const std::filesystem::path& projectRoot, const std::filesystem::path& asset);
    [[nodiscard]] std::filesystem::path CanonicalProjectRoot(
        const std::filesystem::path& requestedProject);
}
