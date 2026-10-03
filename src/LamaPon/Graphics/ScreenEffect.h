#pragma once

#include "LamaPon/Graphics/ShaderProgram.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;

    // 画面全体にHLSLまたはマニフェスト先頭パスの効果を適用する。
    class ScreenEffect final
    {
    public:
        // 追加パラメーターのベクトル数
        static constexpr std::size_t CustomParameterCount = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        // 全画面描画のシェーダーと状態を作成する(device: 資源作成デバイス, context: 借用する描画コンテキスト, assets: アセット管理, shaderPath: HLSLまたは画面効果マニフェスト)。
        // コンテキストは本体より長く保持し、作成・コンパイル失敗は例外で受け取る。
        ScreenEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);


        // 全画面三角形に効果を適用する(source: t0の元画像, auxiliaryTextures: t1・t2の補助画像, depth: 任意のt3深度, depthParameters: 深度復元係数, depthUnprojection: 位置復元係数, destination: 描画先, width: 出力幅, height: 出力高さ, parameters: b0の追加パラメーター)。
        // 深度係数は射影_33・_43・有効印をxyzへ、位置係数は射影_11・_22の逆数をxyへ渡す。
        // 元画像・描画先不在は無処理とし、出力サイズは最小1で、入力SRV以外の描画状態は復元しない。
        void Apply(
            ID3D11ShaderResourceView* source,
            const std::array<ID3D11ShaderResourceView*, 2>&
                auxiliaryTextures,
            ID3D11ShaderResourceView* depth,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const CustomParameters& parameters);

    private:
        struct Constants final
        {
            // 追加パラメーターのベクトル列
            CustomParameters parameters{};
            // xyは画面サイズ、zwは逆数
            DirectX::XMFLOAT4 screenSize{};
            // 射影_33・_43と深度の有効印
            // 既存HLSLの定数配置を保つため、深度用の定数はscreenSizeより後ろに置く。
            DirectX::XMFLOAT4 depthParameters{};
            // xyは射影_11・_22の逆数
            DirectX::XMFLOAT4 depthUnprojection{};
        };

        // 借用する描画コンテキスト
        ID3D11DeviceContext* m_context{};
        // 全画面描画のシェーダー資源
        ShaderProgram m_program;
        // VS・PSのb0へ送る定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        // s0の線形リピートサンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_sampler;
        // 深度検査と書込の無効化状態
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            m_depthDisabled;
        // カリングしないラスタライズ状態
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>
            m_rasterizer;
    };
}
