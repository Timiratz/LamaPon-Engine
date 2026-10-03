#pragma once
#include <filesystem>

namespace LamaPon::Cli
{
    // scene pathをassets相対へ正規化します(projectRoot: root, scene: 指定path)
    // 読込側は解決先の範囲と実在を追加検証します。
    [[nodiscard]] std::filesystem::path NormalizeScenePath(
        const std::filesystem::path& projectRoot, const std::filesystem::path& scene);
    // asset pathをassets相対へ正規化します(projectRoot: root, asset: 指定path)
    [[nodiscard]] std::filesystem::path NormalizeAssetPath(
        const std::filesystem::path& projectRoot, const std::filesystem::path& asset);
    // project rootをcanonical pathで返します(requestedProject: 指定root)
    [[nodiscard]] std::filesystem::path CanonicalProjectRoot(
        const std::filesystem::path& requestedProject);
}
