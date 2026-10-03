#include "LamaPon/Components/TextRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon
{
    TextRendererComponent::TextRendererComponent(
        std::string text,
        std::string fontFamily,
        const float fontSize,
        const DirectX::XMFLOAT4 color,
        const DirectX::XMFLOAT2 layoutSize,
        const bool wordWrap,
        const TextHorizontalAlignment horizontalAlignment,
        const TextVerticalAlignment verticalAlignment)
        : m_text(std::move(text))
        , m_fontFamily(std::move(fontFamily))
        , m_fontSize(std::max(fontSize, 1.0f))
        , m_color(color)
        , m_layout{
            {
                std::clamp(layoutSize.x, 0.0f, 4096.0f),
                std::clamp(layoutSize.y, 0.0f, 4096.0f)
            },
            horizontalAlignment,
            verticalAlignment,
            wordWrap
        }
    {
    }

    // 内容・書体・サイズの同値更新は画像生成を省き、保留中だけ再試行します。
    void TextRendererComponent::SetText(std::string text)
    {
        if (m_text == text && !m_textureRefreshPending)
        {
            return;
        }
        m_text = std::move(text);
        RefreshTexture();
    }

    void TextRendererComponent::SetFontFamily(std::string fontFamily)
    {
        if (m_fontFamily == fontFamily && !m_textureRefreshPending)
        {
            return;
        }
        m_fontFamily = std::move(fontFamily);
        RefreshTexture();
    }

    void TextRendererComponent::SetFontSize(const float fontSize)
    {
        // 1以上に制限した文字サイズ
        const float clamped = std::max(fontSize, 1.0f);
        if (m_fontSize == clamped && !m_textureRefreshPending)
        {
            return;
        }
        m_fontSize = clamped;
        RefreshTexture();
    }

    void TextRendererComponent::SetColor(const DirectX::XMFLOAT4& color)
    {
        // 色は画像に焼かないため、生成が保留中の場合だけ再試行します。
        m_color = color;
        if (m_textureRefreshPending)
        {
            RefreshTexture();
        }
    }

    void TextRendererComponent::SetLayoutSize(const DirectX::XMFLOAT2& size)
    {
        m_layout.size = {
            std::clamp(size.x, 0.0f, 4096.0f),
            std::clamp(size.y, 0.0f, 4096.0f)
        };
        RefreshTexture();
    }

    void TextRendererComponent::SetWordWrap(const bool wordWrap)
    {
        m_layout.wordWrap = wordWrap;
        RefreshTexture();
    }

    void TextRendererComponent::SetHorizontalAlignment(
        const TextHorizontalAlignment alignment)
    {
        m_layout.horizontalAlignment = alignment;
        RefreshTexture();
    }

    void TextRendererComponent::SetVerticalAlignment(
        const TextVerticalAlignment alignment)
    {
        m_layout.verticalAlignment = alignment;
        RefreshTexture();
    }

    void TextRendererComponent::OnInitialize(GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        RefreshTexture();
    }

    void TextRendererComponent::RefreshTexture()
    {
        m_textureRefreshPending = true;
        if (m_assets == nullptr || m_text.empty())
        {
            m_texture.reset();
            m_textureRefreshPending = false;
            return;
        }

        m_texture = m_assets->LoadTextTexture(
            m_text,
            m_fontFamily,
            m_fontSize,
            m_layout);
        m_textureRefreshPending = false;
    }

    void TextRendererComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        if (!m_texture)
        {
            return;
        }

        using namespace DirectX;

        // 所有物体のワールド変換
        XMFLOAT4X4 world{};
        XMStoreFloat4x4(&world, Owner().WorldMatrix());

        // 所有物体がUI矩形を持つ指定
        const bool usesUIRect =
            Owner().GetComponent<UIRectTransformComponent>() != nullptr;
        // 表示の基準位置ピクセル
        XMFLOAT2 position{ world._41, world._42 };
        if (!usesUIRect && m_graphics != nullptr)
        {
            // ワールド2Dの画面補正量
            const auto& offset = m_graphics->Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
        }
        // 文字画像の基準位置ピクセル
        XMFLOAT2 origin{};
        // 文字画像から表示へのXY倍率
        XMFLOAT2 scale{
            std::sqrt(world._11 * world._11 + world._12 * world._12),
            std::sqrt(world._21 * world._21 + world._22 * world._22)
        };
        // Z回転角ラジアン
        float rotation = std::atan2(world._12, world._11);
        // 所有物体のUI矩形設定
        if (const auto* rectTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
            rectTransform != nullptr
            && m_graphics != nullptr)
        {
            // UI表示矩形ピクセル
            const auto rect =
                rectTransform->Resolve(
                    static_cast<float>(
                        m_graphics->UIWidth()),
                    static_cast<float>(
                        m_graphics->UIHeight()));
            // UI表示幅・高さピクセル
            const auto rectSize =
                rect.Size();
            position = {
                rect.minimum.x + rectSize.x * 0.5f,
                rect.minimum.y + rectSize.y * 0.5f };
            origin = {
                static_cast<float>(m_texture->width) * 0.5f,
                static_cast<float>(m_texture->height) * 0.5f };
            if (m_layout.size.x > 0.0f)
            {
                scale.x *=
                    rectSize.x
                    / m_layout.size.x;
            }
            if (m_layout.size.y > 0.0f)
            {
                scale.y *=
                    rectSize.y
                    / m_layout.size.y;
            }
        }

        // 白い文字画像へアルファ乗算済みの色を掛けます。
        // 文字画像のGPU資源の保持
        const auto resources = m_texture->resources.Acquire();
        // 文字画像の保持ビュー
        const auto textureView = resources
            ? resources->shaderResourceView
            : GraphicsViewHandle{};
        if (!textureView)
        {
            return;
        }
        // 文字画像・姿勢・色の描画要求
        SpriteDrawRequest request;
        request.texture = textureView;
        request.position = position;
        XMStoreFloat4(
            &request.tint,
            PremultipliedTextColor(m_color));
        request.rotation = rotation;
        request.origin = origin;
        request.scale = scale;
        static_cast<void>(sprites.Draw(request));
    }

    bool TextRendererComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        description.geometry = "文字列";
        description.material =
            "\"" + Text() + "\" " + Detail::FrameDebugNumber(FontSize()) + "pt";
        description.state = "並び順 " + std::to_string(RenderSortOrder());
        return true;
    }
}
