#pragma once

#include <d3d11.h>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class AudioSystem;
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
        // 制約があるため、再初期化の前にはShutdownが必要です。全生成に
        // 成功してから交換し、途中の失敗ではファイル専用の状態を保持します。
        void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
            HWND window, bool textureCompression);
        void Shutdown() noexcept;

        // 初回だけ生成します。nullptrのdevice/contextでもSceneやPrefabの
        // ファイル読み込みが可能です。GPU資源の読み込みには初期化が必要です。
        [[nodiscard]] AssetManager& EnsureAssets(ID3D11Device* device,
            ID3D11DeviceContext* context, bool textureCompression);
        [[nodiscard]] AssetManager* TryAssets() const noexcept { return m_assets.get(); }
        // 未初期化またはShutdown後はlogic_errorを通知します。
        [[nodiscard]] AudioSystem& Audio() const;
        [[nodiscard]] InputSystem& Input() const;

    private:
        std::unique_ptr<AssetManager> m_assets;
        std::unique_ptr<AudioSystem> m_audio;
        std::unique_ptr<InputSystem> m_input;
    };
}
