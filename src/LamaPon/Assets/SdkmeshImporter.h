#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // SDKMESHの101版・200版を描画APIに依存しないCPUモデルへ変換する。
    // 先頭の頂点ストリームと三角形だけを使い、フレーム変換とスキンは描画へ適用しない。
    class SdkmeshImporter final
    {
    public:
        // SDKMESHを読み込む(assets: ファイルと画像の取得元, path: 元モデルのパス)。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);


        // バイト列からSDKMESHを読み込む(assets: 画像の取得元, bytes: 呼出中に保持する元バイト列, sourcePath: 画像解決と診断用のパス)。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> LoadFromMemory(
            AssetManager& assets,
            std::span<const std::uint8_t> bytes,
            const std::filesystem::path& sourcePath);
    };
}
