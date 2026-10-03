#pragma once

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/PrefilteredEnvironment.h"
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace LamaPon
{
    class RenderTarget;
}

namespace LamaPon::Detail
{
    // 検証済みの反射プローブ入力です。
    struct D3D12ReflectionProbeBindings final
    {
        // プローブ入力の有効化
        bool active{};
        // 第2プローブの混合有無
        bool blended{};
        // 主スペキュラの記述子
        D3D12_GPU_DESCRIPTOR_HANDLE specular{};
        // 主放射照度の記述子
        D3D12_GPU_DESCRIPTOR_HANDLE irradiance{};
        // 第2スペキュラの記述子
        D3D12_GPU_DESCRIPTOR_HANDLE secondarySpecular{};
        // 第2放射照度の記述子
        D3D12_GPU_DESCRIPTOR_HANDLE secondaryIrradiance{};
        // x=強度、y=有効、z=最終ミップ
        DirectX::XMFLOAT4 environmentParameters{};
        // 主ボックスの中心座標
        DirectX::XMFLOAT4 boxCenter{};
        // xyz=半径、w=射影有効
        DirectX::XMFLOAT4 boxParameters{};
        // 第2ボックスの中心座標
        DirectX::XMFLOAT4 secondaryBoxCenter{};
        // xyz=半径、w=射影有効
        DirectX::XMFLOAT4 secondaryBoxParameters{};
        // x=混合率、y=第2最終ミップ
        DirectX::XMFLOAT4 blendParameters{};
    };

    // プローブ入力を検証して解決します(backend: 現在の資源世代, probe: 主・第2プローブ情報)。
    // キューブ寸法・最終ミップ・有限値の検査に失敗した場合はactive=falseで空を返します。
    [[nodiscard]] D3D12ReflectionProbeBindings ResolveD3D12ReflectionProbe(
        const D3D12Backend& backend,
        const ReflectionProbeEnvironment& probe) noexcept;

    class D3D12EnvironmentPrefilter final
    {
    public:
        // 環境光生成器を作ります(backend: 初期化済みのバックエンド)。
        // backendは生成器より長く生存させます。
        explicit D3D12EnvironmentPrefilter(D3D12Backend& backend);
        // 所有する畳み込み資源を破棄します。
        ~D3D12EnvironmentPrefilter() noexcept;

        // 環境光生成器のコピーを禁止します。
        D3D12EnvironmentPrefilter(const D3D12EnvironmentPrefilter&) = delete;
        // 環境光生成器のコピー代入を禁止します。
        D3D12EnvironmentPrefilter& operator=(
            const D3D12EnvironmentPrefilter&) = delete;

        // 同じ入力とキーなら前回結果を返します(source: 入力キューブ, cacheKey: 環境光の識別キー)。
        // Sky用に1組だけ保持し、ディスクへ保存しません。
        [[nodiscard]] PrefilteredEnvironmentViews Prefilter(
            const GraphicsViewHandle& source,
            std::uint64_t cacheKey);
        // Sky用キャッシュを変えず毎回生成します(source: 入力キューブ)。
        [[nodiscard]] PrefilteredEnvironmentViews CreatePrefiltered(
            const GraphicsViewHandle& source);
        // 128pxの6面を描いて左右反転し畳み込みます(renderFace: 各面を同期描画する関数)。
        // 描画先は呼び出し側で復元し、ベイク中の再入はlogic_errorです。
        [[nodiscard]] PrefilteredEnvironmentViews BakeReflectionProbe(
            const EnvironmentProbeFaceRenderer& renderFace);

    private:
        // 全ミップの6面へ畳み込みます(pipeline: 生成用PSO, source: 入力キューブ, target: 書き込み先, size: 先頭ミップ寸法, mipLevels: ミップ数)。
        void Convolve(
            ID3D12PipelineState* pipeline,
            const GraphicsViewHandle& source,
            const D3D12Backend::ComputeCubeTarget& target,
            std::uint32_t size,
            std::uint32_t mipLevels);
        // 描画済みの面を左右反転して写します(face: 0～5の面番号)。
        void CopyMirroredFace(std::uint32_t face);

        // 借用する描画バックエンド
        D3D12Backend* m_backend{};
        // 畳み込み用のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        // GGXスペキュラ生成PSO
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_specularPipeline;
        // 放射照度生成PSO
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_irradiancePipeline;
        // 左右反転のコピーPSO
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_mirrorPipeline;
        // 再利用判定する入力ビュー
        GraphicsViewHandle m_source;
        // 前回の環境光識別キー
        std::uint64_t m_cacheKey{};
        // 前回生成した環境光
        PrefilteredEnvironmentViews m_views;

        // プローブ1面のHDR描画先
        std::unique_ptr<RenderTarget> m_probeFaceTarget;
        // 左右反転済みの6面画像
        D3D12Backend::ComputeCubeTarget m_probeCube;
        // プローブのベイク中
        bool m_probeBakeActive{};
    };
}
