#pragma once

#include "LamaPon/Graphics/LitMaterial.h"

#include <filesystem>

namespace LamaPon
{
    class AssetDatabase;
    class AssetManager;

    // 材質JSONを読み、GUIDを優先して画像を解決する(path: 材質のパス, database: 任意のGUID管理, assets: 任意のアーカイブ管理)。
    // assets指定時はアーカイブにも対応し、省略時はファイルを直接読む。
    [[nodiscard]] LitMaterial LoadLitMaterialAsset(
        const std::filesystem::path& path,
        const AssetDatabase* database = nullptr,
        AssetManager* assets = nullptr);
    // 材質JSONをファイルへ上書き保存する(path: 保存パス, material: 保存する材質, database: 任意のGUID管理)。
    // 保存先の親フォルダーは作成せず、失敗時に書きかけのファイルを元へ戻さない。
    void SaveLitMaterialAsset(
        const std::filesystem::path& path,
        const LitMaterial& material,
        const AssetDatabase* database = nullptr);
}
