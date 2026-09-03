#include "LamaPon/Components/TextRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

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

    // 以下のSetterは「値が変わっていなければ何もしない」ようにして
    // います。文字テクスチャの作り直しはDirectWriteでの描画とGPU
    // テクスチャ生成を伴うため、HUDのように毎フレーム同じ値を入れ直す
    // 書き方でも無駄が出ないようにするためです。
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
        // 色は文字テクスチャに焼かれていないので、作り直しは要りません
        // （毎フレーム色を変えるフェードも軽く書けます）。作り直しが
        // 保留になっている場合だけ、ここで拾います。
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
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView*)
    {
        if (!m_texture)
        {
            return;
        }

        using namespace DirectX;

        XMFLOAT4X4 world{};
        XMStoreFloat4x4(&world, Owner().WorldMatrix());

        const bool usesUIRect =
            Owner().GetComponent<UIRectTransformComponent>() != nullptr;
        XMFLOAT2 position{ world._41, world._42 };
        if (!usesUIRect && m_graphics != nullptr)
        {
            const auto& offset = m_graphics->Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
        }
        XMFLOAT2 origin{};
        XMFLOAT2 scale{
            std::sqrt(world._11 * world._11 + world._12 * world._12),
            std::sqrt(world._21 * world._21 + world._22 * world._22)
        };
        float rotation = std::atan2(world._12, world._11);
        if (const auto* rectTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
            rectTransform != nullptr
            && m_graphics != nullptr)
        {
            const auto rect =
                rectTransform->Resolve(
                    static_cast<float>(
                        m_graphics->UIWidth()),
                    static_cast<float>(
                        m_graphics->UIHeight()));
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

        // 文字テクスチャは白で焼いてあるので、色はここで掛けます
        // （こうすると色を変えてもテクスチャは作り直しになりません＝
        // フェードのような演出ができます）。
        spriteBatch.Draw(
            m_texture->view.Get(),
            position,
            nullptr,
            PremultipliedTextColor(m_color),
            rotation,
            origin,
            scale);
    }
}
