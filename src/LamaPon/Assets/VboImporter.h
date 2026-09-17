#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // Windows 8 ResourceLoading sample由来のVBOを、D3D11 Deviceに依存しない
    // CPU modelへ読み込みます。DirectXTK11経路は従来loaderを維持します。
    class VboImporter final
    {
    public:
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);

        [[nodiscard]] static std::shared_ptr<SkeletalModel> LoadFromMemory(
            std::span<const std::uint8_t> bytes,
            const std::filesystem::path& sourcePath);
    };
}
