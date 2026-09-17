#pragma once

// GraphicsDeviceの公開layoutから描画APIごとの所有資源を隠すRuntime内部
// interfaceです。具象D3D11 stateは別headerに置き、SDKには入れません。
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
        public:
            virtual ~GraphicsDeviceApiResources() noexcept = default;

            GraphicsDeviceApiResources(
                const GraphicsDeviceApiResources&) = delete;
            GraphicsDeviceApiResources& operator=(
                const GraphicsDeviceApiResources&) = delete;

            [[nodiscard]] virtual RenderingApi Api() const noexcept = 0;
            // Shader worker等、BackendとAssetManagerを借用する処理を
            // 両方が生存している間に停止します。
            virtual void QuiesceResourceWork() noexcept = 0;
            // Device世代に属するEffect/cache等を先に解放します。
            virtual void ResetHighLevelResources() noexcept = 0;
            // 部分初期化の巻き戻しと通常終了の両方から呼べます。
            virtual void Reset() noexcept = 0;

            [[nodiscard]] virtual GraphicsRenderServices*
                TryRenderServices() noexcept = 0;
            // ShadowMapのnative実装はまだAPI固有ですが、所有と再生成は
            // この境界へ閉じ、GraphicsDevice本体は具象stateを知りません。
            virtual void RecreateShadowMaps(
                GraphicsBackend& backend,
                const GraphicsSettings& settings) = 0;
            [[nodiscard]] virtual ShadowMap*
                TryDirectionalShadowMap() const noexcept = 0;
            [[nodiscard]] virtual ShadowMap*
                TrySpotShadowMap() const noexcept = 0;
            [[nodiscard]] virtual ShadowMap*
                TryPointShadowMap() const noexcept = 0;

        protected:
            GraphicsDeviceApiResources() = default;
        };
    }
}
