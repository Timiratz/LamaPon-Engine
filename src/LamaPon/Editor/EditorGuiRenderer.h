#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <memory>

struct ImDrawData;
struct ImTextureRef;

namespace LamaPon
{
    class GraphicsDevice;
    class RenderTarget;
    struct TextureAsset;

    // Dear ImGuiの描画API固有処理をEditorLayerから分離するための
    // 最小契約です。Win32側の入力・フレーム処理は含みません。
    // InitializeしたImGui contextはShutdown完了まで生存させます。
    // 初期化中はGraphicsDevice resource leaseも保持します。
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
        // 返す参照は現在のImGui frameのRenderDrawData完了時までだけ
        // 有効です。描画APIによってはframeごとにdescriptorを確保する
        // ため、呼び出し側で保持・再利用してはいけません。
        // Initialize済みで、そのときのImGui contextがcurrentの場合だけ
        // 呼び出せます。
        [[nodiscard]] virtual ImTextureRef TextureReference(
            const TextureAsset& texture) = 0;
        [[nodiscard]] virtual ImTextureRef DisplayTextureReference(
            const RenderTarget& target) = 0;
        virtual void RenderDrawData(ImDrawData* drawData) = 0;
        virtual void Shutdown() noexcept = 0;
    };

    // activeApiにはGraphicsDeviceで解決済みの実効APIを渡します。
    // 現段階で生成できるEditor GUI rendererはDirectX 11だけです。
    [[nodiscard]] std::unique_ptr<EditorGuiRenderer>
        CreateEditorGuiRenderer(RenderingApi activeApi);
}
