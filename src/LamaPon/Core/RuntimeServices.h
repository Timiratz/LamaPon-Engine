#pragma once

#include <d3d11.h>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class AudioSystem;
    class GraphicsBackend;
    class InputSystem;

    // アセット・音声・入力の生成と破棄順を管理します。
    // GraphicsDeviceやApplicationへの参照は持たず、描画の初期化から独立してファイル読み込みやテストへ利用できます。
    class RuntimeServices final
    {
    public:
        // サービスの再初期化設定を準備します。
        RuntimeServices();
        // 入力・音声・アセットの順に所有資源を破棄します。
        ~RuntimeServices();
        // サービスの所有権のコピーを禁止します。
        RuntimeServices(const RuntimeServices&) = delete;
        // サービスのコピー代入を禁止します。
        RuntimeServices& operator=(const RuntimeServices&) = delete;

        // サービスを初期化します(device: 借用する描画デバイス, context: 借用する描画コンテキスト, window: 入力先ウィンドウ, textureCompression: テクスチャ圧縮の有効化)。
        // COMを事前に初期化し、借用先はShutdownまたはPrepareForGraphicsReinitializationまで維持します。
        // 入力が初期化済みならlogic_errorを送出し、各サービスの生成失敗では既存の所有状態を保持します。
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND window, bool textureCompression);
        // 描画バックエンドを指定して初期化します(device: 借用する描画デバイス, context: 借用する描画コンテキスト, window: 入力先ウィンドウ, textureCompression: テクスチャ圧縮の有効化, backend: 借用する描画バックエンド)。
        // backendの停止・再初期化より前にPrepareForGraphicsReinitializationまたはShutdownを呼びます。
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND window, bool textureCompression,
            GraphicsBackend& backend);
        // アセットが借用中の描画デバイス上の処理を完了させます。
        void QuiesceGraphicsWork() noexcept;
        // 描画処理を完了して入力とアセットを解放し、音声と再初期化設定を保持します。
        // アセットルート・アップロード閾値・文字キャッシュ予算・入力アクションを次回のInitializeへ引き継ぎます。
        void PrepareForGraphicsReinitialization() noexcept;
        // 全サービスを入力・音声・アセットの順に解放し、再初期化設定も消去します。
        void Shutdown() noexcept;

        // アセット管理を初回だけ生成して返します(device: 借用する描画デバイス, context: 借用する描画コンテキスト, textureCompression: テクスチャ圧縮の有効化)。
        // device/contextがnullでもシーン文書は読めますが、GPU資源の読み込みには描画の初期化が必要です。
        [[nodiscard]] AssetManager& EnsureAssets(ID3D11Device* device,
            ID3D11DeviceContext* context, bool textureCompression);
        // バックエンド付きのアセット管理を初回だけ生成します(device: 借用する描画デバイス, context: 借用する描画コンテキスト, textureCompression: テクスチャ圧縮の有効化, backend: 借用する描画バックエンド)。
        // 借用先の寿命はInitializeと同じ規則に従います。
        [[nodiscard]] AssetManager& EnsureAssets(ID3D11Device* device,
            ID3D11DeviceContext* context, bool textureCompression,
            GraphicsBackend& backend);
        // アセット管理への借用参照を返し、未生成ならnullを返します。
        [[nodiscard]] AssetManager* TryAssets() const noexcept { return m_assets.get(); }
        // 音声サービスを返し、未初期化ならlogic_errorを送出します。
        [[nodiscard]] AudioSystem& Audio() const;
        // 入力サービスを返し、未初期化ならlogic_errorを送出します。
        [[nodiscard]] InputSystem& Input() const;

    private:
        struct ReinitializationState;

        // 全サービスを生成して交換します(device: 借用デバイス, context: 借用コンテキスト, window: 入力先, textureCompression: 圧縮の有効化, backend: 借用バックエンド、未指定ならnull)。
        void InitializeImpl(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            HWND window,
            bool textureCompression,
            GraphicsBackend* backend);
        // アセット管理の初回生成を行います(device: 借用デバイス, context: 借用コンテキスト, textureCompression: 圧縮の有効化, backend: 借用バックエンド、未指定ならnull)。
        [[nodiscard]] AssetManager& EnsureAssetsImpl(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            bool textureCompression,
            GraphicsBackend* backend);
        // 現在のアセット設定を再初期化用に保存します。
        void CaptureAssetSettings() noexcept;
        // 保存したアセット設定を復元します(assets: 設定を適用するアセット管理)。
        void RestoreAssetSettings(AssetManager& assets) const;

        // 描画再初期化で引き継ぐ設定
        std::unique_ptr<ReinitializationState>
            m_reinitializationState;
        // 所有するアセット管理
        std::unique_ptr<AssetManager> m_assets;
        // 所有する音声サービス
        std::unique_ptr<AudioSystem> m_audio;
        // 所有する入力サービス
        std::unique_ptr<InputSystem> m_input;
    };
}
