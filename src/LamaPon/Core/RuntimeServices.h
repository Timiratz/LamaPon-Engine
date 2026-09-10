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
    // GraphicsDeviceやApplicationへの参照は持たず、描画の初期化から
    // 独立してファイル読み込みやテストへ利用できます。
    class RuntimeServices final
    {
    public:
        RuntimeServices();
        ~RuntimeServices();
        RuntimeServices(const RuntimeServices&) = delete;
        RuntimeServices& operator=(const RuntimeServices&) = delete;

        // COM初期化は呼び出し側の責務です。借用するdevice/context/windowは
        // Shutdownまで有効である必要があります。入力には単一所有者の
        // 制約があるため、再初期化の前にはShutdownまたは
        // PrepareForGraphicsReinitializationが必要です。全生成に成功して
        // から交換し、途中の失敗では既存の状態を保持します。backend付き
        // overloadではbackendも同様に借用し、Prepare/Shutdownより先に
        // backendを停止・再初期化してはいけません。
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND window, bool textureCompression);
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND window, bool textureCompression,
            GraphicsBackend& backend);
        // 描画Backendの再作成前に、旧Deviceを借りるAssetManagerと
        // Windowに結び付くInputだけを破棄します。Audioは描画APIと
        // 無関係なので保持し、短時間の再生成による途切れを避けます。
        // Backend停止前にQuiesceGraphicsWorkを呼び、AssetManagerが
        // 借りているDevice上のbackground処理を先に完了させてください。
        // Asset root、upload/cache予算、Input actionは次のInitializeへ
        // 引き継ぎ、描画API非依存のAudioインスタンスも保持します。
        void QuiesceGraphicsWork() noexcept;
        void PrepareForGraphicsReinitialization() noexcept;
        void Shutdown() noexcept;

        // 初回だけ生成します。nullptrのdevice/contextでもSceneやPrefabの
        // ファイル読み込みが可能です。GPU資源の読み込みには初期化が必要です。
        // backend付きoverloadのlifetime規則はInitializeと同じです。
        [[nodiscard]] AssetManager& EnsureAssets(ID3D11Device* device,
            ID3D11DeviceContext* context, bool textureCompression);
        [[nodiscard]] AssetManager& EnsureAssets(ID3D11Device* device,
            ID3D11DeviceContext* context, bool textureCompression,
            GraphicsBackend& backend);
        [[nodiscard]] AssetManager* TryAssets() const noexcept { return m_assets.get(); }
        // 未初期化またはShutdown後はlogic_errorを通知します。
        [[nodiscard]] AudioSystem& Audio() const;
        [[nodiscard]] InputSystem& Input() const;

    private:
        struct ReinitializationState;

        void InitializeImpl(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            HWND window,
            bool textureCompression,
            GraphicsBackend* backend);
        [[nodiscard]] AssetManager& EnsureAssetsImpl(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            bool textureCompression,
            GraphicsBackend* backend);
        void CaptureAssetSettings() noexcept;
        void RestoreAssetSettings(AssetManager& assets) const;

        std::unique_ptr<ReinitializationState>
            m_reinitializationState;
        std::unique_ptr<AssetManager> m_assets;
        std::unique_ptr<AudioSystem> m_audio;
        std::unique_ptr<InputSystem> m_input;
    };
}
