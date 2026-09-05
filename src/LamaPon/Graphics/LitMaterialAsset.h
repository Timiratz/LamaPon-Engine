#pragma once

#include "LamaPon/Graphics/LitMaterial.h"

#include <filesystem>

namespace LamaPon
{
    class AssetDatabase;
    class AssetManager;

    // assetsを受け取った場合は、マテリアルJSONをそこから読み込みます。
    // これにより、暗号化して配布したゲームでも、エディター上の展開済み
    // ファイルと同じようにマテリアルを読み込めます。展開済みファイルだけを
    // 扱う呼び出し元は省略でき、その場合はファイルを直接読み込みます。
    [[nodiscard]] LitMaterial LoadLitMaterialAsset(
        const std::filesystem::path& path,
        const AssetDatabase* database = nullptr,
        AssetManager* assets = nullptr);
    void SaveLitMaterialAsset(
        const std::filesystem::path& path,
        const LitMaterial& material,
        const AssetDatabase* database = nullptr);
}
