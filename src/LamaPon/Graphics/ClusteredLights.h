#pragma once

#include "LamaPon/Graphics/Lighting.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;

    // クラスタライトカリング（Forward+）。
    //
    // 視錐台を16×9×24のクラスタに切り、Compute Shaderで
    // クラスタごとの「届くライトの番号表」を作ります。Litシェーダーは
    // 自分のクラスタの表だけを見るので、シーン全体では数百灯あっても
    // ピクセルの計算量は近くのライト数で決まります。
    //
    // 従来の「シーン全体で先着16灯」の定数バッファは残っています
    // （自作ShaderとDirectXTKフォールバックの互換のため）。
    // LamaPonLit.hlslだけがこのクラスタ経路を使います。
    class ClusteredLights final
    {
    public:
        // グリッドの分割数。シェーダー側と一致させてください。
        static constexpr std::uint32_t GridWidth = 16;
        static constexpr std::uint32_t GridHeight = 9;
        static constexpr std::uint32_t GridDepth = 24;
        static constexpr std::uint32_t ClusterCount =
            GridWidth * GridHeight * GridDepth;
        // 1クラスタに入るライトの上限。超えた分は落とします
        // （1つの狭い領域に33灯以上重なる状況は稀で、落ちても
        // 暗くなるだけで破綻はしません）。
        static constexpr std::uint32_t
            MaximumLightsPerCluster = 32;

        ClusteredLights(
            ID3D11Device* device,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        ClusteredLights(const ClusteredLights&) = delete;
        ClusteredLights& operator=(
            const ClusteredLights&) = delete;

        // ライト一覧をGPUへ送り、カリングを実行して、lightingへ
        // バインド情報（SRVと定数）を書き込みます。
        // width/heightは描画先のピクセルサイズ（Litシェーダーが
        // ピクセル座標からクラスタを引くのに使います）。
        // 射影が透視でない（正射影＝2Dシーン）場合や、ライトが
        // 0灯の場合はclustered.enabledをfalseにします。
        void Update(
            ID3D11DeviceContext* context,
            LightingState& lighting,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            std::uint32_t width,
            std::uint32_t height);

    private:
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_cullingShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        // ライト一覧（CPUから毎フレーム書き込み）。
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_lightShaderResourceView;
        // クラスタごとの番号表と灯数（GPUが書き込み）。
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_indexListBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_indexListUnorderedView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_indexListShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_countBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_countUnorderedView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_countShaderResourceView;
    };
}
