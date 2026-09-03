#pragma once

#include "LamaPon/Graphics/LitMaterial.h"

#include <filesystem>

namespace LamaPon
{
    class AssetDatabase;
    class AssetManager;

    // When "assets" is provided, the material JSON is read through it so
    // that shipped, encrypted games can load materials just like loose
    // files in the editor. Tests and other loose-file-only callers may
    // omit it and fall back to reading the file directly.
    [[nodiscard]] LitMaterial LoadLitMaterialAsset(
        const std::filesystem::path& path,
        const AssetDatabase* database = nullptr,
        AssetManager* assets = nullptr);
    void SaveLitMaterialAsset(
        const std::filesystem::path& path,
        const LitMaterial& material,
        const AssetDatabase* database = nullptr);
}
