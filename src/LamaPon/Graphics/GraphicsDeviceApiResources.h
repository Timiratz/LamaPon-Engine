#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

namespace LamaPon
{
    class GraphicsBackend;
    class GraphicsRenderServices;
    class ShadowMap;

    namespace Detail
    {
        class GraphicsDeviceApiResources
        {
            // ワーカー停止、高レベル資源の解放、Resetの順で処理し、バックエンドは最後に終了する。
        public:
            // 派生側を含むAPI固有資源の所有状態を解放する。
            virtual ~GraphicsDeviceApiResources() noexcept = default;

            // API資源のコピーを禁止する。
            GraphicsDeviceApiResources(
                const GraphicsDeviceApiResources&) = delete;
            // API資源のコピー代入を禁止する。
            GraphicsDeviceApiResources& operator=(
                const GraphicsDeviceApiResources&) = delete;

            // 資源が属する描画APIを返す。
            [[nodiscard]] virtual RenderingApi Api() const noexcept = 0;

            // バックエンドとアセット管理の生存中に、両者を借用するワーカーを停止する。
            virtual void QuiesceResourceWork() noexcept = 0;

            // デバイス世代に属する効果とキャッシュなどを解放する。
            virtual void ResetHighLevelResources() noexcept = 0;

            // 部分初期化の巻戻しと通常終了で所有資源を初期化前の状態へ戻す。
            virtual void Reset() noexcept = 0;

            // 所有する描画サービスを借用し、利用不能ならヌルを返す。
            [[nodiscard]] virtual GraphicsRenderServices*
                TryRenderServices() noexcept = 0;

            // 品質設定に応じて影マップを再生成する(backend: 資源作成バックエンド, settings: 描画品質設定)。
            virtual void RecreateShadowMaps(
                GraphicsBackend& backend,
                const GraphicsSettings& settings) = 0;
            // 所有する方向影マップを借用し、不在ならヌルを返す。
            [[nodiscard]] virtual ShadowMap*
                TryDirectionalShadowMap() const noexcept = 0;
            // 所有するスポット影マップを借用し、不在ならヌルを返す。
            [[nodiscard]] virtual ShadowMap*
                TrySpotShadowMap() const noexcept = 0;
            // 所有する点ライト影マップを借用し、不在ならヌルを返す。
            [[nodiscard]] virtual ShadowMap*
                TryPointShadowMap() const noexcept = 0;

        protected:
            // 空のAPI資源の所有状態を作成する。
            GraphicsDeviceApiResources() = default;
        };
    }
}
