#include "ProjectPaths.h"
#include "LamaPon/Core/PathUtils.h"
#include <stdexcept>

namespace LamaPon::Cli
{
    // scene pathをassets相対へ正規化します(projectRoot: root, scene: 指定path)
    [[nodiscard]] std::filesystem::path NormalizeScenePath(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        // absolute指定かでproject内の相対化方法を分けます。
        if (scene.is_absolute())
        {
            // project assets基準の相対pathです。
            const auto relative = scene.lexically_relative(
                projectRoot / L"assets");
            // project assets外のabsolute pathは拒否します。
            if (relative.empty()
                || relative.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "The scene is outside the project's"
                    " assets folder: "
                    + LamaPon::PathToUtf8(scene));
            }
            return relative;
        }
        // scene pathの先頭componentです。
        auto iterator = scene.begin();
        // project-relative assets pathなら先頭を除きます。
        if (iterator != scene.end()
            && *iterator == L"assets")
        {
            // assets prefixを除いたscene pathです。
            std::filesystem::path stripped;
            // 残りのpath componentを結合します。
            for (++iterator;
                iterator != scene.end();
                ++iterator)
            {
                stripped /= *iterator;
            }
            return stripped;
        }
        return scene;
    }

    // asset pathをassets内へ正規化し範囲外を拒否します(projectRoot: root, asset: 指定path)
    [[nodiscard]] std::filesystem::path NormalizeAssetPath(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& asset)
    {
        // filesystem aliasを解決したproject assets rootです。
        const auto assetsRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        // absolute assetはcanonical pathで範囲を検証します。
        if (asset.is_absolute())
        {
            // assets root基準に変換したasset pathです。
            const auto relative =
                std::filesystem::weakly_canonical(asset)
                    .lexically_relative(assetsRoot);
            // assets directory外のabsolute pathを拒否します。
            if (relative.empty()
                || relative.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "The asset is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(asset));
            }
            return relative.lexically_normal();
        }

        // asset pathの先頭componentです。
        auto iterator = asset.begin();
        // project-relative assets pathならprefixを除きます。
        if (iterator != asset.end()
            && *iterator == L"assets")
        {
            // assets prefixを除いたasset pathです。
            std::filesystem::path stripped;
            // 残りのpath componentを結合します。
            for (++iterator;
                iterator != asset.end();
                ++iterator)
            {
                stripped /= *iterator;
            }
            return stripped.lexically_normal();
        }
        // 絶対指定以外をlexically normalizeしたpathです。
        const auto normalized = asset.lexically_normal();
        // project assets外へ出る相対pathを拒否します。
        if (normalized.empty()
            || normalized.native().starts_with(L".."))
        {
            throw std::invalid_argument(
                "The asset path must stay inside the project's assets folder.");
        }
        return normalized;
    }

    // LamaPon project rootを検証しcanonical pathで返します(requestedProject: 指定root)
    [[nodiscard]] std::filesystem::path CanonicalProjectRoot(
        const std::filesystem::path& requestedProject)
    {
        // relative指定も含めたcanonical project rootです。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(requestedProject));
        // project marker fileのpathです。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // marker fileが通常fileでなければprojectではありません。
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        return projectRoot;
    }

}
