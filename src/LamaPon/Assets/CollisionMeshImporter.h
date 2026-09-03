#pragma once

#include <filesystem>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class CollisionMesh;

    namespace CollisionMeshImporter
    {
        // モデルファイル（.gltf / .glb / .fbx）から衝突用の
        // 三角形メッシュを読み込みます。頂点はモデルのノード
        // 変換適用済み（バインドポーズ）で、パスをキーに
        // キャッシュします。失敗時は例外を投げます。
        [[nodiscard]] std::shared_ptr<const CollisionMesh>
            Load(
                AssetManager& assets,
                const std::filesystem::path& path);

        // キャッシュを破棄します（再インポート時に使用）。
        void ClearCache();
    }
}
