#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace LamaPon
{
    class GraphicsDevice;

    namespace Detail
    {
        class SpriteRenderPassState;
    }

    // b1に格納する2Dライトの最大数
    inline constexpr std::size_t MaximumSprite2DLights = 16;

    struct Sprite2DLight final
    {
        // xy=画面位置、z=半径、w=強度
        DirectX::XMFLOAT4 positionRadiusIntensity{};
        // rgb=色、w=予約
        DirectX::XMFLOAT4 color{};
    };

    // 2D照明シェーダーのb1用で、未宣言の自作シェーダーには影響しません。
    struct Sprite2DLighting final
    {
        // x=灯数、yzw=予約
        DirectX::XMUINT4 counts{};
        // 灯数分の有効なライト列
        std::array<Sprite2DLight, MaximumSprite2DLights>
            lights{};
    };

    struct SpriteSourceRectangle final
    {
        // 切り出し左端（画素）
        std::int32_t left{};
        // 切り出し上端（画素）
        std::int32_t top{};
        // 切り出し右端（画素）
        std::int32_t right{};
        // 切り出し下端（画素）
        std::int32_t bottom{};
    };

    struct SpriteClipRectangle final
    {
        // クリップ左端（画素）
        float minimumX{};
        // クリップ上端（画素）
        float minimumY{};
        // クリップ右端（画素）
        float maximumX{};
        // クリップ下端（画素）
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

    // 空のtextureは白へ置換し、tintは事前乗算せずに使います。
    struct SpriteDrawRequest final
    {
        // 描画画像の読み取りビュー
        GraphicsViewHandle texture;
        // 画面位置（ピクセル）
        DirectX::XMFLOAT2 position{};
        // 切り出し範囲の指定有無
        bool hasSourceRectangle{};
        // 画像の切り出し範囲
        SpriteSourceRectangle sourceRectangle{};
        // RGBAの乗算色
        DirectX::XMFLOAT4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 回転角（ラジアン）
        float rotation{};
        // 回転・拡縮の原点
        DirectX::XMFLOAT2 origin{};
        // 各軸の拡大率
        DirectX::XMFLOAT2 scale{ 1.0f, 1.0f };
        // 画像の反転方向
        SpriteFlip flip{ SpriteFlip::None };
        // 描画深度
        float layerDepth{};
    };

    struct SpriteMeshVertex final
    {
        // 画面位置（ピクセル）
        DirectX::XMFLOAT2 position{};
        // 正規化した画像UV
        DirectX::XMFLOAT2 textureCoordinate{};
        // RGBAの乗算色
        DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    // 2Dメッシュの頂点数の上限
    inline constexpr std::size_t MaximumSpriteMeshVertices = 65535u;
    // 2Dメッシュの索引数の上限
    inline constexpr std::size_t MaximumSpriteMeshIndices = 393216u;

    // 三角形リストで描く2Dメッシュです。空のtextureは白へ置換し、裏面も描きます。
    // 頂点と索引は描画の呼び出し中だけ参照し、色はSpriteDrawRequestのtintと同じ扱いです。
    struct SpriteMeshDrawRequest final
    {
        // 描画画像の読み取りビュー
        GraphicsViewHandle texture;
        // 頂点列
        std::span<const SpriteMeshVertex> vertices;
        // 3個ずつで三角形を作る頂点番号列
        std::span<const std::uint16_t> indices;
        // 描画深度
        float layerDepth{};
    };

    struct SpritePassDescription final
    {
        // 画像の合成方式
        SpriteBlendMode blend{
            SpriteBlendMode::NonPremultiplied };
        // 画素シェーダーのパス
        std::filesystem::path pixelShader;
        // b0の自作シェーダー引数
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
        // b1の2Dライト情報
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
        // 使用シェーダーの世代
        std::uint64_t generation{};
        // 元シェーダーの失敗理由
        std::string error;
        // 代替描画の種類
        SpriteShaderFallback fallback{
            SpriteShaderFallback::None };
    };

    // コピーしてもパス寿命を延長せず、パス終了・デバイス破棄後の描画はfalseです。
    class SpriteDrawContext final
    {
    public:
        // 空の描画窓口を作ります。
        SpriteDrawContext() noexcept = default;

        // 描画パスが現在も有効か返します。
        [[nodiscard]] explicit operator bool() const noexcept;

        // 画像を送信できたか返します(request: 画像と位置・色・変形の指定)。
        bool Draw(const SpriteDrawRequest& request) const;
        // 三角形メッシュを送信できたか返します(request: 画像と頂点・索引)。
        // 頂点数・索引数が上限外、索引が3の倍数でない、範囲外の索引や非有限値を含む場合はfalseです。
        bool DrawMesh(const SpriteMeshDrawRequest& request) const;
        // クリップ範囲を積めたか返します(rectangle: ピクセル座標の矩形)。
        bool PushScissor(
            const SpriteClipRectangle& rectangle) const;
        // 直前のクリップ範囲へ戻せたか返します。
        bool PopScissor() const;

    private:
        // 描画窓口を作ります(state: 寿命を延ばさないパス参照)。
        explicit SpriteDrawContext(
            std::weak_ptr<Detail::SpriteRenderPassState>
                state) noexcept;

        // パス寿命を延ばさない参照
        std::weak_ptr<Detail::SpriteRenderPassState> m_state;

        friend class SpriteRenderPass;
    };

    // 描画を開始したスレッドで操作し、デバイス破棄後は無効になります。
    class SpriteRenderPass final
    {
    public:
        // 空の描画パスを作ります。
        SpriteRenderPass() noexcept = default;
        // 例外を外へ出さず描画パスを終了します。
        ~SpriteRenderPass() noexcept;

        // パスの所有権を移します(other: 移動元のパス)。
        SpriteRenderPass(SpriteRenderPass&& other) noexcept;
        // 現在のパスを終了して所有権を移します(other: 移動元のパス)。
        SpriteRenderPass& operator=(
            SpriteRenderPass&& other) noexcept;

        // 描画パスのコピーを禁止します。
        SpriteRenderPass(const SpriteRenderPass&) = delete;
        // 描画パスのコピー代入を禁止します。
        SpriteRenderPass& operator=(
            const SpriteRenderPass&) = delete;

        // 所有する描画パスが現在も有効か返します。
        [[nodiscard]] explicit operator bool() const noexcept;
        // パス寿命を延ばさない描画窓口を返します。
        [[nodiscard]] SpriteDrawContext Context() const noexcept;
        // 使用中シェーダーの世代・失敗理由・代替状態を返します。
        [[nodiscard]] SpriteShaderStatus ShaderStatus() const;

        // 画像を送信できたか返します(request: 画像と位置・色・変形の指定)。
        bool Draw(const SpriteDrawRequest& request) const;
        // 三角形メッシュを送信できたか返します(request: 画像と頂点・索引)。
        bool DrawMesh(const SpriteMeshDrawRequest& request) const;
        // クリップ範囲を積めたか返します(rectangle: ピクセル座標の矩形)。
        bool PushScissor(
            const SpriteClipRectangle& rectangle) const;
        // 直前のクリップ範囲へ戻せたか返します。
        bool PopScissor() const;

        // 描画を終了し、送信エラーを伝えます。
        void End();
        // 開始スレッドなら描画を終了し、例外を外へ出しません。
        void Abort() noexcept;

    private:
        // 描画パスを所有します(state: 開始したパスの共有状態)。
        explicit SpriteRenderPass(
            std::shared_ptr<Detail::SpriteRenderPassState>
                state) noexcept;

        // 所有する描画パスの状態
        std::shared_ptr<Detail::SpriteRenderPassState> m_state;

        friend class GraphicsDevice;
    };
}
