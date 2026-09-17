#pragma once

#include <filesystem>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    // Visual Studio 3D Starter KitのCMOを、D3D11 Deviceに依存しない
    // CPU modelへ読み込みます。DirectXTK11経路は従来loaderを維持し、
    // D3D12などのBackendだけがこのImporterを使用します。
    class CmoImporter final
    {
    public:
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            AssetManager& assets,
            const std::filesystem::path& path);
    };
}
