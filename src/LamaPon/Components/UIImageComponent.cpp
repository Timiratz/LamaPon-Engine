#include "LamaPon/Components/UIImageComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace
{
    // RGBへアルファを乗算した描画色を返します(color: アルファ乗算前のRGBA色)。
    DirectX::XMFLOAT4 Premultiply(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
    }
}

namespace LamaPon
{
    UIImageComponent::UIImageComponent(
        std::filesystem::path texturePath,
        const DirectX::XMFLOAT4 color)
        : m_texturePath(std::move(texturePath))
        , m_color(color)
    {
    }

    void UIImageComponent::SetTexturePath(
        std::filesystem::path path)
    {
        // 読み込みに成功した置換画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_texturePath = std::move(path);
        m_texture = std::move(texture);
    }

    void UIImageComponent::SetBorder(
        const DirectX::XMFLOAT4& border) noexcept
    {
        m_border = {
            std::max(border.x, 0.0f),
            std::max(border.y, 0.0f),
            std::max(border.z, 0.0f),
            std::max(border.w, 0.0f)
        };
    }

    void UIImageComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    void UIImageComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        if (!m_texturePath.empty())
        {
            m_texture =
                m_assets->LoadTexture(m_texturePath);
        }
    }

    void UIImageComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        using namespace DirectX;

        // 所有物体のワールド変換
        XMFLOAT4X4 world{};
        XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // 所有物体のZ回転角ラジアン
        const float rotation =
            std::atan2(world._12, world._11);
        // 表示にUI矩形を使う指定
        bool usesUIRect = false;
        // 表示する矩形ピクセル
        UIRect rect;
        // 所有物体のUI矩形設定
        if (const auto* transform =
            Owner().GetComponent<
                UIRectTransformComponent>();
            transform != nullptr
            && m_graphics != nullptr)
        {
            rect = transform->Resolve(
                static_cast<float>(
                    m_graphics->UIWidth()),
                static_cast<float>(
                    m_graphics->UIHeight()));
            usesUIRect = true;
        }
        else
        {
            rect = {
                { world._41, world._42 },
                {
                    world._41 + m_fallbackSize.x,
                    world._42 + m_fallbackSize.y
                }
            };
        }
        // 表示幅・高さピクセル
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        // アルファ乗算済みの描画色
        const auto color = Premultiply(m_color);
        // 選択した画像の保持ビュー
        GraphicsViewHandle view;
        if (m_texture)
        {
            // 通常画像のGPU資源の保持
            if (const auto resources =
                    m_texture->resources.Acquire())
            {
                view = resources->shaderResourceView;
            }
        }
        // 元画像の幅ピクセル
        float textureWidth = m_texture
            ? static_cast<float>(m_texture->width)
            : 1.0f;
        // 元画像の高ピクセル
        float textureHeight = m_texture
            ? static_cast<float>(m_texture->height)
            : 1.0f;
        // Cameraが描いたレンダーテクスチャがあれば優先します（9-sliceは適用しません）。
        // 有効なカメラ出力画像を使用
        bool usingRenderTexture = false;
        if (!m_renderTexture.empty()
            && m_graphics != nullptr)
        {
            // 取得したカメラ画像のビュー
            const auto renderTextureView =
                m_graphics->RenderTextureViewHandle(
                    m_renderTexture);
            // 名前に対応するカメラ出力
            if (const auto* target =
                    m_graphics->FindRenderTexture(
                        m_renderTexture);
                target != nullptr
                && target->IsValid()
                && renderTextureView)
            {
                view = renderTextureView;
                textureWidth =
                    static_cast<float>(target->Width());
                textureHeight =
                    static_cast<float>(target->Height());
                usingRenderTexture = true;
            }
        }

        // 通常画像を9分割する指定
        const bool sliced =
            !usingRenderTexture
            && m_texture
            && (m_border.x > 0.0f
                || m_border.y > 0.0f
                || m_border.z > 0.0f
                || m_border.w > 0.0f);
        // UIで適用する回転角ラジアン
        const float drawRotation =
            usesUIRect ? rotation : 0.0f;
        if (!sliced)
        {
            // 全体描画の基準位置
            const XMFLOAT2 position = usesUIRect
                ? XMFLOAT2{
                    rect.minimum.x + size.x * 0.5f,
                    rect.minimum.y + size.y * 0.5f }
                : rect.minimum;
            // 元画像の基準位置ピクセル
            const XMFLOAT2 origin = usesUIRect
                ? XMFLOAT2{
                    textureWidth * 0.5f,
                    textureHeight * 0.5f }
                : XMFLOAT2{};
            // 画像・姿勢・色の描画要求
            SpriteDrawRequest request;
            request.texture = view;
            request.position = position;
            request.tint = color;
            request.rotation = drawRotation;
            request.origin = origin;
            request.scale = {
                size.x / textureWidth,
                size.y / textureHeight
            };
            static_cast<void>(sprites.Draw(request));
            return;
        }

        // 元画像の角の寸法を保って辺と中央を伸縮し、小さすぎる表示先では角も縮めます。
        // 中心列を残す横分割幅上限
        const float maximumBorderX =
            std::max(textureWidth - 1.0f, 0.0f);
        // 中心行を残す縦分割幅上限
        const float maximumBorderY =
            std::max(textureHeight - 1.0f, 0.0f);
        // 制限済みの元画像左幅
        float left = std::min(m_border.x, maximumBorderX);
        // 制限済みの元画像上幅
        float top = std::min(m_border.y, maximumBorderY);
        // 制限済みの元画像右幅
        float right =
            std::min(m_border.z, maximumBorderX - left);
        // 制限済みの元画像下幅
        float bottom =
            std::min(m_border.w, maximumBorderY - top);
        // 描画先が小さい場合は角がはみ出さないよう縮めます。
        // 表示先に収める横境界倍率
        const float borderScaleX = std::min(
            1.0f,
            size.x / std::max(left + right, 1.0f));
        // 表示先に収める縦境界倍率
        const float borderScaleY = std::min(
            1.0f,
            size.y / std::max(top + bottom, 1.0f));
        // 表示先の左境界幅ピクセル
        const float destLeft = left * borderScaleX;
        // 表示先の上境界幅ピクセル
        const float destTop = top * borderScaleY;
        // 表示先の右境界幅ピクセル
        const float destRight = right * borderScaleX;
        // 表示先の下境界幅ピクセル
        const float destBottom = bottom * borderScaleY;

        // 元画像の横分割境界ピクセル
        const std::array<float, 4> sourceX{
            0.0f,
            left,
            textureWidth - right,
            textureWidth
        };
        // 元画像の縦分割境界ピクセル
        const std::array<float, 4> sourceY{
            0.0f,
            top,
            textureHeight - bottom,
            textureHeight
        };
        // 表示先の横分割境界ピクセル
        const std::array<float, 4> destX{
            rect.minimum.x,
            rect.minimum.x + destLeft,
            rect.maximum.x - destRight,
            rect.maximum.x
        };
        // 表示先の縦分割境界ピクセル
        const std::array<float, 4> destY{
            rect.minimum.y,
            rect.minimum.y + destTop,
            rect.maximum.y - destBottom,
            rect.maximum.y
        };
        // 表示矩形の中心ピクセル
        const XMFLOAT2 rectCenter{
            rect.minimum.x + size.x * 0.5f,
            rect.minimum.y + size.y * 0.5f };
        // 回転角の余弦
        const float cosine = std::cos(rotation);
        // 回転角の正弦
        const float sine = std::sin(rotation);

        // 9分割の行番号
        for (int row = 0; row < 3; ++row)
        {
            // 9分割の列番号
            for (int column = 0; column < 3; ++column)
            {
                // 分割の元画像の幅ピクセル
                const float sourceWidth =
                    sourceX[column + 1]
                    - sourceX[column];
                // 分割の元画像の高ピクセル
                const float sourceHeight =
                    sourceY[row + 1] - sourceY[row];
                // 分割の表示先の幅ピクセル
                const float destWidth =
                    destX[column + 1] - destX[column];
                // 分割の表示先の高ピクセル
                const float destHeight =
                    destY[row + 1] - destY[row];
                if (sourceWidth <= 0.0f
                    || sourceHeight <= 0.0f
                    || destWidth <= 0.0f
                    || destHeight <= 0.0f)
                {
                    continue;
                }
                // 分割の元画像ピクセル矩形
                const SpriteSourceRectangle source{
                    static_cast<std::int32_t>(sourceX[column]),
                    static_cast<std::int32_t>(sourceY[row]),
                    static_cast<std::int32_t>(
                        sourceX[column + 1]),
                    static_cast<std::int32_t>(sourceY[row + 1])
                };
                // 分割描画の基準位置
                XMFLOAT2 cellPosition{
                    destX[column],
                    destY[row] };
                if (usesUIRect)
                {
                    // 回転前の分割中心ピクセル
                    const XMFLOAT2 cellCenter{
                        destX[column] + destWidth * 0.5f,
                        destY[row] + destHeight * 0.5f };
                    // 表示矩形中心からのX変位
                    const float offsetX =
                        cellCenter.x - rectCenter.x;
                    // 表示矩形中心からのY変位
                    const float offsetY =
                        cellCenter.y - rectCenter.y;
                    cellPosition = {
                        rectCenter.x
                            + offsetX * cosine
                            - offsetY * sine,
                        rectCenter.y
                            + offsetX * sine
                            + offsetY * cosine };
                }
                // 分割元画像の基準位置
                const XMFLOAT2 cellOrigin = usesUIRect
                    ? XMFLOAT2{
                        sourceWidth * 0.5f,
                        sourceHeight * 0.5f }
                    : XMFLOAT2{};
                // 画像・姿勢・色の描画要求
                SpriteDrawRequest request;
                request.texture = view;
                request.position = cellPosition;
                request.hasSourceRectangle = true;
                request.sourceRectangle = source;
                request.tint = color;
                request.rotation = drawRotation;
                request.origin = cellOrigin;
                request.scale = {
                    destWidth / sourceWidth,
                    destHeight / sourceHeight
                };
                static_cast<void>(sprites.Draw(request));
            }
        }
    }

    bool UIImageComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        static_cast<void>(Detail::DescribeTexturedDraw(
            description,
            "UI画像",
            TexturePath(),
            RenderSortOrder()));
        if (!RenderTexture().empty())
        {
            description.material = "RenderTexture: " + RenderTexture();
        }
        return true;
    }
}
