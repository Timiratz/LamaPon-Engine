#pragma once

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <memory>
#include <vector>

namespace LamaPon
{
    class D3D12Backend;
}

namespace LamaPon::Detail
{
    class D3D12ComputeEffectRenderer;
    class D3D12EnvironmentPrefilter;
    class D3D12SpriteRenderer;

    class GraphicsDeviceD3D12Resources final
        : public GraphicsDeviceApiResources
    {
    public:
        // D3D12の描画器と描画サービスを作成する(backend: 借用するバックエンド)。
        // バックエンドは本体と各描画器より長く保持する。
        explicit GraphicsDeviceD3D12Resources(D3D12Backend& backend);
        // 実行待ちの効果と所有する描画資源を解放する。
        ~GraphicsDeviceD3D12Resources() noexcept override;

        // 所有するAPI資源のコピーを禁止する。
        GraphicsDeviceD3D12Resources(
            const GraphicsDeviceD3D12Resources&) = delete;
        // 所有するAPI資源のコピー代入を禁止する。
        GraphicsDeviceD3D12Resources& operator=(
            const GraphicsDeviceD3D12Resources&) = delete;

        // D3D12 ExperimentalのAPI種別を返す。
        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        // 非同期の借用作業を行わないため、停止処理を省略する。
        void QuiesceResourceWork() noexcept override;
        // 実行待ちの効果・描画器・サービス・影マップを解放する。
        void ResetHighLevelResources() noexcept override;
        // 所有するD3D12資源を初期化前の状態へ戻す。
        void Reset() noexcept override;
        // 描画サービスを借用し、資源解放後はヌルを返す。
        [[nodiscard]] GraphicsRenderServices*
            TryRenderServices() noexcept override;
        // 設定に応じて三種の影マップを再生成する(backend: 初期化済みのD3D12基盤, settings: 影の品質設定)。
        // 全ての作成が成功してから差し替え、影が無効なら空の窓口を保持する。
        void RecreateShadowMaps(
            GraphicsBackend& backend,
            const GraphicsSettings& settings) override;
        // 方向影の窓口を借用し、未生成・解放後はヌルを返す。
        [[nodiscard]] ShadowMap*
            TryDirectionalShadowMap() const noexcept override;
        // スポット影の窓口を借用し、未生成・解放後はヌルを返す。
        [[nodiscard]] ShadowMap*
            TrySpotShadowMap() const noexcept override;
        // 点ライト影の窓口を借用し、未生成・解放後はヌルを返す。
        [[nodiscard]] ShadowMap*
            TryPointShadowMap() const noexcept override;


        // スプライト描画器を借用し、資源解放後はヌルを返す。
        [[nodiscard]] D3D12SpriteRenderer* TrySpriteRenderer() noexcept;

        // 計算効果の描画器を借用し、資源解放後はヌルを返す。
        [[nodiscard]] D3D12ComputeEffectRenderer*
            TryComputeEffectRenderer() noexcept;

        // 環境の畳込描画器を借用し、資源解放後はヌルを返す。
        [[nodiscard]] D3D12EnvironmentPrefilter*
            TryEnvironmentPrefilter() noexcept;

        // 実行待ちの画面効果の要求
        std::vector<ScreenEffectRequest> queuedScreenEffects;

    private:
        // スプライトと共通描画の描画器
        std::unique_ptr<D3D12SpriteRenderer> m_spriteRenderer;
        // 計算効果の描画器
        std::unique_ptr<D3D12ComputeEffectRenderer> m_computeEffectRenderer;
        // 環境の畳込描画器
        std::unique_ptr<D3D12EnvironmentPrefilter> m_environmentPrefilter;
        // バックエンド共通の描画サービス
        std::unique_ptr<GraphicsRenderServices> m_renderServices;
        // 影を無効にしても再生成後は空の影マップ窓口を保持し、Sceneの判定に使う。
        // 方向ライト影の公開窓口
        std::unique_ptr<ShadowMap> m_directionalShadowMap;
        // スポットライト影の公開窓口
        std::unique_ptr<ShadowMap> m_spotShadowMap;
        // 点ライト影の公開窓口
        std::unique_ptr<ShadowMap> m_pointShadowMap;
    };

    // 初期化済みのD3D12実装からAPI資源を作成し、不適合ならinvalid_argumentを送出する(backend: 借用するバックエンド)。
    [[nodiscard]] std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D12GraphicsDeviceApiResources(
            GraphicsBackend& backend);
}
