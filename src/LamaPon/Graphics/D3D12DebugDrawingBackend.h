#pragma once

#include "LamaPon/Graphics/DebugRenderer.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>

namespace LamaPon
{
    class D3D12Backend;

    // グリッド・操作ハンドル・境界の線を現在のD3D12描画先へ重ねる。
    class D3D12DebugDrawingBackend final : public DebugDrawingBackend
    {
    public:
        // 内蔵の線分シェーダーを用意する(backend: 寿命が長い初期化済み基盤)。
        explicit D3D12DebugDrawingBackend(D3D12Backend& backend);

        // 線分を深度に隠さずアルファ混合で重ねる(lines: 呼出し中借用する線分列, view: ビュー行列, projection: 射影行列)。
        void DrawLines(
            std::span<const DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) override;

    private:
        // 形式に対応するパイプラインを遅延生成して借用する(colorFormat: 描画先の色形式, depthFormat: 描画先の深度形式)。
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            DXGI_FORMAT colorFormat,
            DXGI_FORMAT depthFormat);

        // 借用するD3D12の描画基盤
        D3D12Backend* m_backend{};
        // 線分描画のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        // 線分の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        // 線分の色シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        // 二種の色形式と三種の深度形式ごとのパイプラインを保持する。
        // 描画先形式別のパイプライン
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 6>
            m_pipelineStates;
    };
}
