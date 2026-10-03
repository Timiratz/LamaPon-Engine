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
        // GPUを作らずスキン描画の要否を返す(assets: ファイルの取得元, path: 元モデルのパス, requiresForwardRole: 通常描画の要否の任意返却先)。
        // 全ノードの描画対象を調べ、スキン付きと通常描画が混在すれば両方を要求する。
        [[nodiscard]] static bool RequiresSkinning(
            AssetManager& assets,
            const std::filesystem::path& path,
            bool* requiresForwardRole = nullptr);

        // glTF/GLBの三角形・スキン・アニメーションを読む(device: D3D11デバイス、空はCPU経路, context: キャッシュAPI互換用, assets: ファイルと画像の取得元, path: 元モデルのパス)。
        // 全ノードの三角形を対象とし、スキンはCPU経路も含め72ボーン以内に制限する。
        [[nodiscard]] static std::shared_ptr<SkeletalModel> Load(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& path);
    };
}
