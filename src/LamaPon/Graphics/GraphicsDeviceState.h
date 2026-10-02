#pragma once

#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

namespace LamaPon
{
    struct GraphicsDevice::State final
    {
        // 共有ゲートとサービスを作成し、計測通知先を設定する(owner: 借用する所有デバイス)。
        explicit State(GraphicsDevice* owner);
        // 所有資源を宣言の逆順で解放する。
        ~State();

        // 所有状態のコピーを禁止する。
        State(const State&) = delete;
        // 所有状態のコピー代入を禁止する。
        State& operator=(const State&) = delete;

        // 共有寿命ゲートは最後に解放し、GPU資源はバックエンドより先に解放する。
        // デバイスの共有寿命ゲート
        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState>
            m_resourceLeaseState;

        // ネイティブ描画基盤を所有する窓口
        std::unique_ptr<GraphicsBackend> m_backend;
        // アセット管理は借用するシェーダーワーカーより長く保持する。
        // アセット・音声などの実行サービス
        std::unique_ptr<RuntimeServices> m_services;
        // 描画APIごとの所有資源
        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            m_apiResources;
        // 代替画像用の白テクスチャ
        GraphicsTextureHandle m_whiteTexture;
        // 白テクスチャの読込ビュー
        GraphicsViewHandle m_whiteTextureView;
        // インスタンス描画の共有バッファー
        GraphicsBufferHandle m_instanceBuffer;
        // 実行中の深度パス種別
        DepthPassKind m_depthPass{ DepthPassKind::None };
        // 通知先をプロファイラーより長く保持するため、この宣言順を保つ。
        // フレームの描画区間の記録
        FrameDebugger m_frameDebugger;
        // GPU区間の計測
        GpuProfiler m_gpuProfiler;

        // デバッグ形状の描画器
        std::unique_ptr<DebugRenderer> m_debugRenderer;
        // クラスタ照明の所有状態
        mutable std::unique_ptr<ClusteredLights> m_clusteredLights;
        // クラスタ照明の作成失敗情報
        mutable BuiltInFailure m_clustersFailure;
        // シーン合成の描画先
        std::unique_ptr<RenderTarget> m_sceneCompositionTarget;
        // シーン合成で使う射影行列
        DirectX::XMFLOAT4X4 m_sceneProjection{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        // 名前で参照する描画テクスチャ群
        std::unordered_map<
            std::string,
            std::unique_ptr<RenderTarget>>
            m_renderTextures;
        // パス別のバリアント宣言キャッシュ
        mutable std::unordered_map<
            std::filesystem::path,
            ShaderVariantDeclaration>
            m_shaderVariants;
        // 非同期コンパイルの有効有無
        bool m_asyncShaderCompilation{ true };
        // 材質シェーダーの更新世代
        mutable std::uint64_t m_materialShaderGeneration{};
        // スプライトシェーダーの更新世代
        mutable std::uint64_t m_spriteShaderGeneration{};
        // 画面効果シェーダーの更新世代
        mutable std::uint64_t m_screenShaderGeneration{};
        // 現在のシーン照明の状態
        LightingState m_lightingState;
        // 現在の描画品質設定
        GraphicsSettings m_graphicsSettings =
            GraphicsSettingsForPreset(GraphicsQualityPreset::High);
        // 描画出力の幅
        std::uint32_t m_width{};
        // 描画出力の高さ
        std::uint32_t m_height{};
        // UI描画領域の幅
        std::uint32_t m_uiWidth{};
        // UI描画領域の高さ
        std::uint32_t m_uiHeight{};
        // 2Dスプライトの座標補正
        DirectX::XMFLOAT2 m_sprite2DOffset{};
        // 現在のフレーム描画統計
        mutable FrameStatistics m_frameStatistics;
        // 描画資源のメモリー統計
        GraphicsMemoryStatistics m_memoryStatistics;
        // 最後にメモリー統計を採取した時刻
        std::chrono::steady_clock::time_point
            m_lastMemoryStatisticsSample{};
        // 起動時に要求された描画API
        RenderingApi m_startupRenderingApi{
            RenderingApi::DirectX11 };
        // 代替の描画APIを使う理由
        RenderingApiFallbackReason m_renderingApiFallbackReason{
            RenderingApiFallbackReason::None };
        // 起動時の描画機能構成
        GraphicsStartupProfile m_graphicsStartupProfile{
            GraphicsStartupProfile::FullRenderer };
    };
}
