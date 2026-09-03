#pragma once

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

    // 画面全体を入力テクスチャとして受け取る、任意HLSL用のポストエフェクトです。
    // VSMain と PSMain を持つHLSLを、フルスクリーン三角形として実行します。
    class ScreenEffect final
    {
    public:
        static constexpr std::size_t CustomParameterCount = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        ScreenEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // depthはt3へ刺すシーンの深度（不要ならnullptr）。
        // depthParametersはそれを距離へ直す係数で、
        // x=射影の_33, y=射影の_43, z=深度が有効なら1。
        // depthUnprojectionはビュー空間の位置（＝法線の再構成）用で、
        // x=1/射影の_11, y=1/射影の_22。
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
            CustomParameters parameters{};
            DirectX::XMFLOAT4 screenSize{};
            // 末尾へ足すこと。既存の自作Shaderはこの行を持たない
            // cbufferを宣言しているので、間へ入れると全部ずれます。
            DirectX::XMFLOAT4 depthParameters{};
            DirectX::XMFLOAT4 depthUnprojection{};
        };

        ID3D11DeviceContext* m_context{};
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_pixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_sampler;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            m_depthDisabled;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>
            m_rasterizer;
    };
}
