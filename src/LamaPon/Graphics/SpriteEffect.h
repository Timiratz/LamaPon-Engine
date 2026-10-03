#pragma once

#include "LamaPon/Graphics/SpriteRendering.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;

    // SpriteBatchの画素処理をHLSLのPSMainへ差し替える。
    class SpriteEffect final
    {
    public:
        // 追加パラメーターのベクトル数
        static constexpr std::size_t CustomParameterCount = 8;
        // 描画時はParameter6へ色、7へ矩形、8へ画面とテクスチャのサイズをエンジンが設定する。
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        // スプライト用の画素シェーダーと定数資源を作成する(device: 資源作成デバイス, context: 借用する描画コンテキスト, assets: アセット管理, shaderPath: HLSLソースパス)。
        // コンテキストは本体より長く保持し、作成・コンパイル失敗は例外で受け取る。
        SpriteEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // 次回適用するb0の値を保存する(parameters: 追加パラメーターのベクトル列)。
        void SetParameters(
            const CustomParameters& parameters) noexcept;

        // 次回適用するb1の照明値を保存する(lighting: 2D照明の定数)。
        void SetLights(
            const Sprite2DLighting& lighting) noexcept;
        // 保存した定数をb0・b1へ送り、画素シェーダーを結合する。
        // 2D照明の有無にかかわらずb1を更新し、以前の結合状態は復元しない。
        void Apply();

    private:
        struct Constants final
        {
            // 追加パラメーターのベクトル列
            CustomParameters parameters{};
        };

        // 借用する描画コンテキスト
        ID3D11DeviceContext* m_context{};
        // PSMainの画素シェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_pixelShader;
        // PSのb0へ送る追加パラメーター
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        // PSのb1へ送る2D照明
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_lightBuffer;
        // 次回適用する追加パラメーター
        Constants m_constants;
        // 次回適用する2D照明
        Sprite2DLighting m_lighting{};
    };
}
