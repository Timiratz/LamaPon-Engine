#pragma once

#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

struct ImGuiContext;

namespace LamaPon
{
    class D3D11EditorGuiRenderer final
        : public EditorGuiRenderer
    {
    public:
        // 未初期化のD3D11 GUI描画処理を作る。
        D3D11EditorGuiRenderer() = default;
        // ShutdownでGUI資源と描画装置の使用権を解放する。
        ~D3D11EditorGuiRenderer() override;

        // 描画資源の共有を禁止する。
        D3D11EditorGuiRenderer(
            const D3D11EditorGuiRenderer&) = delete;
        // 描画資源の共有を禁止する。
        D3D11EditorGuiRenderer& operator=(
            const D3D11EditorGuiRenderer&) = delete;

        // DirectX11を返す。
        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
        }
        // GUI資源が初期化済みかを返す。
        [[nodiscard]] bool
            IsInitialized() const noexcept override
        {
            return m_initialized;
        }

        // 現在のImGui contextにD3D11描画資源を初期化する(graphics: Shutdownまで存続する描画装置)。
        void Initialize(GraphicsDevice& graphics) override;
        // 前フレームのSRV参照を解放して次のImGuiフレームを始める。
        void NewFrame() override;
        // 次のNewFrameまでSRVを保持し現在のフレームのtexture参照を返す(texture: 描画するtextureの借用)。
        [[nodiscard]] ImTextureRef TextureReference(
            const TextureAsset& texture) override;
        // 次のNewFrameまでSRVを保持し描画先の表示参照を返す(target: 表示する描画先の借用)。
        [[nodiscard]] ImTextureRef DisplayTextureReference(
            const RenderTarget& target) override;
        // 初期化時のcontextでGUIを描く(drawData: 非nullのフレーム描画命令)。
        void RenderDrawData(ImDrawData* drawData) override;
        // 初期化時のcontextが存続する間にGUI資源を解放し元のcurrent contextへ戻す。
        void Shutdown() noexcept override;

    private:
        // 初期化済みかつ初期化時のImGui contextがcurrentでなければ例外を出す。
        void RequireCurrentContext() const;

        // 初期化中に借用する描画装置
        GraphicsDevice* m_graphics{};
        // Shutdownまでの描画装置の使用権
        GraphicsDeviceResourceLease
            m_graphicsResourceLease;
        // 初期化時のImGui context
        ImGuiContext* m_imguiContext{};
        // 現フレームのSRVの所有参照
        std::vector<Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>> m_frameTexturePins;
        // 描画資源が初期化済みか
        bool m_initialized{};
    };
}
