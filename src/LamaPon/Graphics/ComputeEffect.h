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

    // 任意HLSLのCompute Shader（CSMain）を、名前付きテクスチャへ
    // 書き出す形で走らせます。絵を1枚作るところまでが仕事で、
    // 出来た絵はSpriteRendererやUI Imageがそのまま表示できます。
    //
    // ピクセルシェーダーと違って「画素ごとに1回」ではなく
    // 「スレッドごとに1回」なので、周りの画素をまとめて読む処理
    // （ぼかし、ヒストグラム、粒子の更新）が書けます。
    class ComputeEffect final
    {
    public:
        static constexpr std::size_t CustomParameterCount = 8;
        // HLSLの[numthreads]と一致させること。ここを変えるときは
        // ドキュメントの雛形も直してください。
        static constexpr std::uint32_t ThreadGroupSize = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        ComputeEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // outputは書き込み先のUAV。inputTexturesはt0/t1へ入ります。
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
            CustomParameters parameters{};
            // xy=出力の幅と高さ, zw=その逆数。
            DirectX::XMFLOAT4 outputSize{};
        };

        ID3D11DeviceContext* m_context{};
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_computeShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_sampler;
    };
}
