#pragma once

#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/Lighting.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;
}

namespace LamaPon::Detail
{
    struct D3D11ClusteredLightsState final
        : ClusteredLightsBackendState
    {
        // 初期化済みで全計算資源とビューが揃うか判定する。
        [[nodiscard]] bool HasNativeResources() const noexcept;
        // ライト情報の読込ビューを借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            LightShaderResourceView() const noexcept;
        // クラスタ別ライト索引の読込ビューを借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            IndexListShaderResourceView() const noexcept;
        // クラスタ別ライト数の読込ビューを借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            CountShaderResourceView() const noexcept;

        // ライトカリングのシェーダーとバッファーを作成する(device: 資源作成デバイス, assets: アセット管理, shaderPath: カリング用HLSL)。
        // 作成・コンパイル失敗は例外とし、再初期化の途中で失敗した状態は利用しない。
        void Initialize(
            ID3D11Device* device,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);
        // クラスタ別のライト一覧を計算して照明状態へ公開する(context: 実行コンテキスト, lighting: 入力ライトと結果の状態, view: ビュー行列, projection: 右手透視射影, width: 出力幅, height: 出力高さ)。
        // 結果を先に解除し、非透視・空のライト・無効資源・転送失敗は従来の照明経路へ戻す。
        // PSのt16～t18とCSの入力SRV・出力UAV・シェーダーを解除し、以前の状態は復元しない。
        void Update(
            ID3D11DeviceContext* context,
            LightingState& lighting,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            std::uint32_t width,
            std::uint32_t height);

        // ライトカリングの計算シェーダー
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_cullingShader;
        // カリング条件の定数バッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        // ライト情報の動的バッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightBuffer;
        // ライト情報の読込ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_lightShaderResourceView;
        // クラスタ別ライト索引バッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_indexListBuffer;
        // ライト索引の書込ビュー
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_indexListUnorderedView;
        // ライト索引の読込ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_indexListShaderResourceView;
        // クラスタ別ライト数バッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_countBuffer;
        // ライト数の書込ビュー
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_countUnorderedView;
        // ライト数の読込ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_countShaderResourceView;
    };
}
