#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // DirectX SDKのSDKMESH（version 101 / 200）を、D3D11 Deviceに依存しない
    // CPU modelへ読み込みます。DirectXTK11経路は従来loaderを維持し、
    // D3D12などのBackendだけがこのImporterを使用します。
    class SdkmeshImporter final
    {
    public:
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);

        // Asset archiveや検証コードが、ディスク上へ中間ファイルを作らずに
        // SDKMESHを同じparserへ渡すための入力境界です。sourcePathは相対
        // textureの解決と診断名にだけ使います。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> LoadFromMemory(
            AssetManager& assets,
            std::span<const std::uint8_t> bytes,
            const std::filesystem::path& sourcePath);
    };
}
