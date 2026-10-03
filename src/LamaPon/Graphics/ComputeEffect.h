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

    // 計算シェーダーで出力テクスチャを生成する。
    class ComputeEffect final
    {
    public:
        // 追加パラメーターのベクトル数
        static constexpr std::size_t CustomParameterCount = 8;
        // HLSLの[numthreads]は8×8×1とし、範囲外のスレッドはHLSL側で除く。
        // グループ内の幅と高さ
        static constexpr std::uint32_t ThreadGroupSize = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        // 計算シェーダーと定数資源を作成する(device: 資源作成デバイス, context: 借用する実行コンテキスト, assets: アセット管理, shaderPath: HLSLまたは計算マニフェスト)。
        // コンテキストは本体より長く保持し、作成・コンパイル失敗は例外で受け取る。
        ComputeEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // 出力サイズを切り上げたグループ数で計算する(inputTextures: t0・t1の入力, output: u0の書込先, width: 出力幅, height: 出力高さ, parameters: b0の追加パラメーター)。
        // 出力不在・サイズ0は無処理とし、実行後はCS・入力SRV・出力UAVを解除して以前の状態は復元しない。
        void Dispatch(
            const std::array<ID3D11ShaderResourceView*, 2>&
                inputTextures,
            ID3D11UnorderedAccessView* output,
            std::uint32_t width,
            std::uint32_t height,
            const CustomParameters& parameters);

    private:
        struct Constants final
        {
            // 追加パラメーターのベクトル列
            CustomParameters parameters{};
            // xyは出力サイズ、zwは逆数
            DirectX::XMFLOAT4 outputSize{};
        };

        // 借用する実行コンテキスト
        ID3D11DeviceContext* m_context{};
        // 計算シェーダー
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_computeShader;
        // b0へ送る定数バッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        // s0の線形クランプサンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_sampler;
    };
}
