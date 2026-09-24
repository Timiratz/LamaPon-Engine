#pragma once

#include <filesystem>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace LamaPon
{
    class AssetManager;
    class SkeletalModel;

    class GltfImporter final
    {
    public:
        // GPU resourceを作らず、ModelRendererが実際に使うroleを
        // 調べます。戻り値はSkinned、出力引数はForwardが必要ならtrue
        // です。出力引数を省略した既存呼び出しの意味は変わりません。
        [[nodiscard]] static bool RequiresSkinning(
            AssetManager& assets,
            const std::filesystem::path& path,
            bool* requiresForwardRole = nullptr);

        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& path);
    };
}
