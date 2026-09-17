#pragma once

// DirectX 12 ExperimentalでSkyのcubemapから環境光（IBL）の事前畳み込みを
// 作ります。D3D11のEnvironmentRenderer::CreatePrefilteredEnvironmentと同じ
// GGXスペキュラ（128px、8ミップ）と放射照度（16px）を、Compute Shaderで
// 各ミップの6面へ書きます。リフレクションプローブの6面ベイクと、描画時の
// プローブ入力の解決も受け持ちます。Runtime内部headerで、SDKにはinstallしません。
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
    // D3D11のGraphicsDevice::TrySetLitEffectReflectionProbeと
    // LitEffect::SetEnvironmentOverrideD3D11が決める、プローブの入力です。
    struct D3D12ReflectionProbeBindings final
    {
        bool active{};
        bool blended{};
        D3D12_GPU_DESCRIPTOR_HANDLE specular{};
        D3D12_GPU_DESCRIPTOR_HANDLE irradiance{};
        D3D12_GPU_DESCRIPTOR_HANDLE secondarySpecular{};
        D3D12_GPU_DESCRIPTOR_HANDLE secondaryIrradiance{};
        // x=強さ, y=有効, z=事前畳み込み済みスペキュラの最終ミップ番号
        DirectX::XMFLOAT4 environmentParameters{};
        DirectX::XMFLOAT4 boxCenter{};
        DirectX::XMFLOAT4 boxParameters{};
        DirectX::XMFLOAT4 secondaryBoxCenter{};
        DirectX::XMFLOAT4 secondaryBoxParameters{};
        // x=2個目を混ぜる比率, y=2個目の最終ミップ番号
        DirectX::XMFLOAT4 blendParameters{};
    };

    // 範囲に入ったプローブを、D3D11と同じ検査（両方のcube、有限値、
    // 最終ミップ番号、cubeの大きさ、混ぜるときは2個目も同じ）で解決します。
    // 通らないプローブはactive=falseで、呼び出し側はSkyのIBLを使います。
    [[nodiscard]] D3D12ReflectionProbeBindings ResolveD3D12ReflectionProbe(
        const D3D12Backend& backend,
        const ReflectionProbeEnvironment& probe) noexcept;

    class D3D12EnvironmentPrefilter final
    {
    public:
        explicit D3D12EnvironmentPrefilter(D3D12Backend& backend);
        ~D3D12EnvironmentPrefilter() noexcept;

        D3D12EnvironmentPrefilter(const D3D12EnvironmentPrefilter&) = delete;
        D3D12EnvironmentPrefilter& operator=(
            const D3D12EnvironmentPrefilter&) = delete;

        // sourceのTextureCubeを畳み込み、同じsourceとcacheKeyの間は前回の
        // 結果を返します。D3D11のGetPrefilteredEnvironmentと同じくSky用の
        // 1組だけを持ちます（D3D11のディスクキャッシュは使いません）。
        [[nodiscard]] PrefilteredEnvironmentViews Prefilter(
            const GraphicsViewHandle& source,
            std::uint64_t cacheKey);
        // Skyの結果を差し替えずに、sourceを毎回新しい2本へ畳み込みます。
        [[nodiscard]] PrefilteredEnvironmentViews CreatePrefiltered(
            const GraphicsViewHandle& source);
        // D3D11のEnvironmentRenderer::BakeReflectionProbeと同じく、128pxの
        // HDR描画先へ6面を描き、左右反転してcubeへ写してから畳み込みます。
        // renderFaceは各面で同期的に1回呼ばれます。描画先は呼び出し側が
        // 戻してください。ベイク中の再入はlogic_errorです。
        [[nodiscard]] PrefilteredEnvironmentViews BakeReflectionProbe(
            const EnvironmentProbeFaceRenderer& renderFace);

    private:
        void Convolve(
            ID3D12PipelineState* pipeline,
            const GraphicsViewHandle& source,
            const D3D12Backend::ComputeCubeTarget& target,
            std::uint32_t size,
            std::uint32_t mipLevels);
        void CopyMirroredFace(std::uint32_t face);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_specularPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_irradiancePipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_mirrorPipeline;
        GraphicsViewHandle m_source;
        std::uint64_t m_cacheKey{};
        PrefilteredEnvironmentViews m_views;
        // プローブの面を描くHDR描画先と、左右反転した6面を集めるcubeです。
        std::unique_ptr<RenderTarget> m_probeFaceTarget;
        D3D12Backend::ComputeCubeTarget m_probeCube;
        bool m_probeBakeActive{};
    };
}
