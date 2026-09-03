#include "LamaPon/Components/UIImageComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace
{
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
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        XMFLOAT4X4 world{};
        XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const float rotation =
            std::atan2(world._12, world._11);
        bool usesUIRect = false;
        UIRect rect;
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
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        const auto premultiplied =
            Premultiply(m_color);
        const auto color =
            XMLoadFloat4(&premultiplied);
        auto* view = m_texture
            ? m_texture->view.Get()
            : whiteTexture;
        float textureWidth = m_texture
            ? static_cast<float>(m_texture->width)
            : 1.0f;
        float textureHeight = m_texture
            ? static_cast<float>(m_texture->height)
            : 1.0f;
        // Cameraが描いたレンダーテクスチャがあれば優先します
        // （9-sliceは適用しません）。
        bool usingRenderTexture = false;
        if (!m_renderTexture.empty()
            && m_graphics != nullptr)
        {
            if (const auto* target =
                    m_graphics->FindRenderTexture(
                        m_renderTexture);
                target != nullptr
                && target->IsValid())
            {
                view =
                    target->DisplayShaderResourceView();
                textureWidth =
                    static_cast<float>(target->Width());
                textureHeight =
                    static_cast<float>(target->Height());
                usingRenderTexture = true;
            }
        }

        const bool sliced =
            !usingRenderTexture
            && m_texture
            && (m_border.x > 0.0f
                || m_border.y > 0.0f
                || m_border.z > 0.0f
                || m_border.w > 0.0f);
        const float drawRotation =
            usesUIRect ? rotation : 0.0f;
        if (!sliced)
        {
            const XMFLOAT2 position = usesUIRect
                ? XMFLOAT2{
                    rect.minimum.x + size.x * 0.5f,
                    rect.minimum.y + size.y * 0.5f }
                : rect.minimum;
            const XMFLOAT2 origin = usesUIRect
                ? XMFLOAT2{
                    textureWidth * 0.5f,
                    textureHeight * 0.5f }
                : XMFLOAT2{};
            spriteBatch.Draw(
                view,
                position,
                nullptr,
                color,
                drawRotation,
                origin,
                {
                    size.x / textureWidth,
                    size.y / textureHeight
                });
            return;
        }

        // 9-slice: 角は原寸を維持し、辺と中央だけを伸縮させます。
        // borderは左・上・右・下（テクスチャピクセル単位）。
        const float maximumBorderX =
            std::max(textureWidth - 1.0f, 0.0f);
        const float maximumBorderY =
            std::max(textureHeight - 1.0f, 0.0f);
        float left = std::min(m_border.x, maximumBorderX);
        float top = std::min(m_border.y, maximumBorderY);
        float right =
            std::min(m_border.z, maximumBorderX - left);
        float bottom =
            std::min(m_border.w, maximumBorderY - top);
        // 描画先が小さい場合は角がはみ出さないよう縮めます。
        const float borderScaleX = std::min(
            1.0f,
            size.x / std::max(left + right, 1.0f));
        const float borderScaleY = std::min(
            1.0f,
            size.y / std::max(top + bottom, 1.0f));
        const float destLeft = left * borderScaleX;
        const float destTop = top * borderScaleY;
        const float destRight = right * borderScaleX;
        const float destBottom = bottom * borderScaleY;

        const std::array<float, 4> sourceX{
            0.0f,
            left,
            textureWidth - right,
            textureWidth
        };
        const std::array<float, 4> sourceY{
            0.0f,
            top,
            textureHeight - bottom,
            textureHeight
        };
        const std::array<float, 4> destX{
            rect.minimum.x,
            rect.minimum.x + destLeft,
            rect.maximum.x - destRight,
            rect.maximum.x
        };
        const std::array<float, 4> destY{
            rect.minimum.y,
            rect.minimum.y + destTop,
            rect.maximum.y - destBottom,
            rect.maximum.y
        };
        const XMFLOAT2 rectCenter{
            rect.minimum.x + size.x * 0.5f,
            rect.minimum.y + size.y * 0.5f };
        const float cosine = std::cos(rotation);
        const float sine = std::sin(rotation);

        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const float sourceWidth =
                    sourceX[column + 1]
                    - sourceX[column];
                const float sourceHeight =
                    sourceY[row + 1] - sourceY[row];
                const float destWidth =
                    destX[column + 1] - destX[column];
                const float destHeight =
                    destY[row + 1] - destY[row];
                if (sourceWidth <= 0.0f
                    || sourceHeight <= 0.0f
                    || destWidth <= 0.0f
                    || destHeight <= 0.0f)
                {
                    continue;
                }
                const RECT source{
                    static_cast<LONG>(sourceX[column]),
                    static_cast<LONG>(sourceY[row]),
                    static_cast<LONG>(
                        sourceX[column + 1]),
                    static_cast<LONG>(sourceY[row + 1])
                };
                XMFLOAT2 cellPosition{
                    destX[column],
                    destY[row] };
                if (usesUIRect)
                {
                    const XMFLOAT2 cellCenter{
                        destX[column] + destWidth * 0.5f,
                        destY[row] + destHeight * 0.5f };
                    const float offsetX =
                        cellCenter.x - rectCenter.x;
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
                const XMFLOAT2 cellOrigin = usesUIRect
                    ? XMFLOAT2{
                        sourceWidth * 0.5f,
                        sourceHeight * 0.5f }
                    : XMFLOAT2{};
                spriteBatch.Draw(
                    view,
                    cellPosition,
                    &source,
                    color,
                    drawRotation,
                    cellOrigin,
                    XMFLOAT2{
                        destWidth / sourceWidth,
                        destHeight / sourceHeight
                    });
            }
        }
    }
}
