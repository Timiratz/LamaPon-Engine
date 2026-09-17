#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace LamaPon
{
    class GraphicsDevice;

    namespace Detail
    {
        class SpriteRenderPassState;
    }

    // 画面へ同時に効かせられるLight2Dの数。b1の定数バッファに載せる
    // ので、CustomParameters（b0・8本しかなく、自作Shaderの持ち物）を
    // 圧迫しません。
    inline constexpr std::size_t MaximumSprite2DLights = 16;

    struct Sprite2DLight final
    {
        // xy=画面ピクセル座標, z=届く半径, w=強さ。
        DirectX::XMFLOAT4 positionRadiusIntensity{};
        // rgb=色, w=予約。
        DirectX::XMFLOAT4 color{};
    };

    // 組み込みの2D照明Shader（LamaPonSpriteLit.hlsl）が読む灯り一覧。
    // 自作Shaderは宣言しなければ何の影響も受けません。
    struct Sprite2DLighting final
    {
        // x=灯数, yzw=予約。
        DirectX::XMUINT4 counts{};
        std::array<Sprite2DLight, MaximumSprite2DLights>
            lights{};
    };

    struct SpriteSourceRectangle final
    {
        std::int32_t left{};
        std::int32_t top{};
        std::int32_t right{};
        std::int32_t bottom{};
    };

    struct SpriteClipRectangle final
    {
        float minimumX{};
        float minimumY{};
        float maximumX{};
        float maximumY{};
    };

    enum class SpriteFlip : std::uint8_t
    {
        None,
        Horizontal,
        Vertical,
        Both
    };

    enum class SpriteBlendMode : std::uint8_t
    {
        NonPremultiplied,
        AlphaBlend,
        Additive,
        Opaque
    };

    // 1枚のSpriteを描くAPI非依存requestです。textureがemptyなら
    // white textureへfallbackします。tintは呼び出し側の
    // 値をそのまま使い、暗黙のpremultiplyは行いません。
    struct SpriteDrawRequest final
    {
        GraphicsViewHandle texture;
        DirectX::XMFLOAT2 position{};
        bool hasSourceRectangle{};
        SpriteSourceRectangle sourceRectangle{};
        DirectX::XMFLOAT4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
        float rotation{};
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT2 scale{ 1.0f, 1.0f };
        SpriteFlip flip{ SpriteFlip::None };
        float layerDepth{};
    };

    struct SpritePassDescription final
    {
        SpriteBlendMode blend{
            SpriteBlendMode::NonPremultiplied };
        std::filesystem::path pixelShader;
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
        Sprite2DLighting lighting{};
    };

    enum class SpriteShaderFallback : std::uint8_t
    {
        None,
        ErrorPlaceholder,
        DefaultPipeline
    };

    struct SpriteShaderStatus final
    {
        std::uint64_t generation{};
        std::string error;
        SpriteShaderFallback fallback{
            SpriteShaderFallback::None };
    };

    // SpriteRenderPassの間だけ有効な、描画API非依存の送信窓口です。
    // copyしてもpassの寿命は延長しません。pass終了後、またはDevice破棄後
    // に保存済みcontextを呼んだ場合は、GPUへ触れずfalseを返します。
    class SpriteDrawContext final
    {
    public:
        SpriteDrawContext() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;

        // requestを現在のpassへ積めた場合にtrueを返します。別Backend世代や
        // ShaderResource以外のhandleは安全に無視してfalseを返します。
        bool Draw(const SpriteDrawRequest& request) const;
        bool PushScissor(
            const SpriteClipRectangle& rectangle) const;
        bool PopScissor() const;

    private:
        explicit SpriteDrawContext(
            std::weak_ptr<Detail::SpriteRenderPassState>
                state) noexcept;

        std::weak_ptr<Detail::SpriteRenderPassState> m_state;

        friend class SpriteRenderPass;
    };

    // BeginSpritePassで開始したSprite描画を必ず閉じるmove-only scopeです。
    // DirectXTKには真のcancelが無いため、AbortとdestructorはEndを
    // best-effortで実行し、例外だけを外へ出しません。GraphicsDeviceが
    // 先に破棄された場合は無効化されます。同じrender threadから操作して
    // ください。
    class SpriteRenderPass final
    {
    public:
        SpriteRenderPass() noexcept = default;
        ~SpriteRenderPass() noexcept;

        SpriteRenderPass(SpriteRenderPass&& other) noexcept;
        SpriteRenderPass& operator=(
            SpriteRenderPass&& other) noexcept;

        SpriteRenderPass(const SpriteRenderPass&) = delete;
        SpriteRenderPass& operator=(
            const SpriteRenderPass&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] SpriteDrawContext Context() const noexcept;
        [[nodiscard]] SpriteShaderStatus ShaderStatus() const;

        bool Draw(const SpriteDrawRequest& request) const;
        bool PushScissor(
            const SpriteClipRectangle& rectangle) const;
        bool PopScissor() const;

        // 明示終了ではGPU送信エラーを呼び出し側へ伝えます。同じpassを
        // 複数回終了しても何もしません。
        void End();
        void Abort() noexcept;

    private:
        explicit SpriteRenderPass(
            std::shared_ptr<Detail::SpriteRenderPassState>
                state) noexcept;

        std::shared_ptr<Detail::SpriteRenderPassState> m_state;

        friend class GraphicsDevice;
    };
}
