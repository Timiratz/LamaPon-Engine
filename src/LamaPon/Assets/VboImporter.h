#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // VBOの位置・法線・UVをAPI非依存のCPUモデルへ変換する。
    class VboImporter final
    {
    public:
        // VBOを読みCPUモデルへ変換する(assets: ファイルの取得元, path: 元VBOのパス)。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);

        // VBOの配置と範囲を検証してCPUモデルを作る(bytes: VBOのバイト列, sourcePath: 診断・ノード名用のパス)。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> LoadFromMemory(
            std::span<const std::uint8_t> bytes,
            const std::filesystem::path& sourcePath);
    };
}
