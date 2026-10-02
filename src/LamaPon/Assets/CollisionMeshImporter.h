#pragma once

#include <filesystem>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class CollisionMesh;

    namespace CollisionMeshImporter
    {
        // 呼び出し側で直列化し、変換済み衝突形状を解決済みパス別に共有する(assets: ファイルの取得元, path: glTF・FBXのパス)。
        [[nodiscard]] std::shared_ptr<const CollisionMesh>
            Load(
                AssetManager& assets,
                const std::filesystem::path& path);

        // 呼び出し側で直列化して索引を消去し、返却済み形状の寿命は残る共有参照に任せる。
        void ClearCache();
    }
}
