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

    // ImGuiの入力処理を含まない描画契約で、初期化したcontextをShutdownまで存続させ、その間GraphicsDeviceのresource leaseを保持する。
    class EditorGuiRenderer
    {
    public:
        // 派生rendererの資源を解放する。
        virtual ~EditorGuiRenderer() = default;

        // GUI描画の基底を作る。
        EditorGuiRenderer() = default;
        // 描画資源の共有を禁止する。
        EditorGuiRenderer(const EditorGuiRenderer&) = delete;
        // 描画資源の共有を禁止する。
        EditorGuiRenderer& operator=(
            const EditorGuiRenderer&) = delete;

        // 対応する描画APIを返す。
        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        // 描画資源が初期化済みかを返す。
        [[nodiscard]] virtual bool
            IsInitialized() const noexcept = 0;

        // 現在のImGui contextに描画資源を初期化する(graphics: Shutdownまで存続する描画装置)。
        virtual void Initialize(GraphicsDevice& graphics) = 0;
        // 初期化済みの描画資源で次のImGuiフレームを準備する。
        virtual void NewFrame() = 0;
        // 初期化時のcontextがcurrentのときだけ取得し、frameごとのdescriptorは保持・再利用しない。
        // 現在のImGuiフレームのRenderDrawData終了まで有効な参照を返す(texture: フレーム内で描画するtexture)。
        [[nodiscard]] virtual ImTextureRef TextureReference(
            const TextureAsset& texture) = 0;
        // 描画先を表示するため同じフレームの描画完了まで有効な参照を返す(target: 表示する描画先の借用)。
        [[nodiscard]] virtual ImTextureRef DisplayTextureReference(
            const RenderTarget& target) = 0;
        // 初期化時と同じcontextで現在の描画先へGUIを描く(drawData: 当該フレームの描画命令の借用)。
        virtual void RenderDrawData(ImDrawData* drawData) = 0;
        // 初期化時のcontextが生存している間に描画資源とleaseを解放する。
        virtual void Shutdown() noexcept = 0;
    };

    // 実効APIに対応するGUI rendererを作り未対応なら例外を出す(activeApi: 解決済みの実効API)。
    [[nodiscard]] std::unique_ptr<EditorGuiRenderer>
        CreateEditorGuiRenderer(RenderingApi activeApi);
}
