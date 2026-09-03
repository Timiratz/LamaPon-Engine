#pragma once

#include <filesystem>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    class FbxImporter final
    {
    public:
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& path);
    };
}
