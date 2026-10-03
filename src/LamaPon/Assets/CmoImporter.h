#pragma once

#include <filesystem>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // CMOを描画APIに依存しないCPUモデルへ変換する。
    class CmoImporter final
    {
    public:
        // メッシュとスキンを読み込む(assets: ファイルの取得元, path: 元CMOのパス)。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);
        // CMOのアニメーションクリップは再生用データへ変換しない。
    };
}
