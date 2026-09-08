#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <memory>

struct ImDrawData;

namespace LamaPon
{
    class GraphicsDevice;

    // Dear ImGuiの描画API固有処理をEditorLayerから分離するための
    // 最小契約です。Win32側の入力・フレーム処理は含みません。
    // InitializeしたImGui contextはShutdown完了まで生存させます。
    class EditorGuiRenderer
    {
    public:
        virtual ~EditorGuiRenderer() = default;

        EditorGuiRenderer() = default;
        EditorGuiRenderer(const EditorGuiRenderer&) = delete;
        EditorGuiRenderer& operator=(
            const EditorGuiRenderer&) = delete;

        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        [[nodiscard]] virtual bool
            IsInitialized() const noexcept = 0;

        virtual void Initialize(GraphicsDevice& graphics) = 0;
        virtual void NewFrame() = 0;
        virtual void RenderDrawData(ImDrawData* drawData) = 0;
        virtual void Shutdown() noexcept = 0;
    };

    // activeApiにはGraphicsDeviceで解決済みの実効APIを渡します。
    // 現段階で生成できるEditor GUI rendererはDirectX 11だけです。
    [[nodiscard]] std::unique_ptr<EditorGuiRenderer>
        CreateEditorGuiRenderer(RenderingApi activeApi);
}
