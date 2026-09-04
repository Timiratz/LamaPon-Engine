#include "ProjectPaths.h"
#include "LamaPon/Core/PathUtils.h"
#include <stdexcept>

namespace LamaPon::Cli
{
    // --sceneの絶対・プロジェクト相対・アセット相対指定をそろえます。
    [[nodiscard]] std::filesystem::path NormalizeScenePath(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        if (scene.is_absolute())
        {
            const auto relative = scene.lexically_relative(
                projectRoot / L"assets");
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
        auto iterator = scene.begin();
        if (iterator != scene.end()
            && *iterator == L"assets")
        {
            std::filesystem::path stripped;
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

    [[nodiscard]] std::filesystem::path NormalizeAssetPath(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& asset)
    {
        const auto assetsRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        if (asset.is_absolute())
        {
            const auto relative =
                std::filesystem::weakly_canonical(asset)
                    .lexically_relative(assetsRoot);
            if (relative.empty()
                || relative.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "The asset is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(asset));
            }
            return relative.lexically_normal();
        }

        auto iterator = asset.begin();
        if (iterator != asset.end()
            && *iterator == L"assets")
        {
            std::filesystem::path stripped;
            for (++iterator;
                iterator != asset.end();
                ++iterator)
            {
                stripped /= *iterator;
            }
            return stripped.lexically_normal();
        }
        const auto normalized = asset.lexically_normal();
        if (normalized.empty()
            || normalized.native().starts_with(L".."))
        {
            throw std::invalid_argument(
                "The asset path must stay inside the project's assets folder.");
        }
        return normalized;
    }

    [[nodiscard]] std::filesystem::path CanonicalProjectRoot(
        const std::filesystem::path& requestedProject)
    {
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(requestedProject));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
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
