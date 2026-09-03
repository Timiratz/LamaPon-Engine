#include "LamaPon/Components/UIButtonComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"

#include <SpriteBatch.h>

#include <algorithm>
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
    UIButtonComponent::UIButtonComponent(
        std::string label,
        const DirectX::XMFLOAT2 fallbackSize,
        std::filesystem::path texturePath)
        : m_label(std::move(label))
        , m_fallbackSize{
            std::max(fallbackSize.x, 1.0f),
            std::max(fallbackSize.y, 1.0f) }
        , m_texturePath(std::move(texturePath))
    {
    }

    void UIButtonComponent::SetLabel(
        std::string label)
    {
        m_label = std::move(label);
        RefreshText();
    }

    void UIButtonComponent::SetFontFamily(
        std::string family)
    {
        m_fontFamily = std::move(family);
        RefreshText();
    }

    void UIButtonComponent::SetFontSize(
        const float size)
    {
        m_fontSize =
            std::clamp(size, 1.0f, 256.0f);
        RefreshText();
    }

    void UIButtonComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    void UIButtonComponent::SetNormalColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_normalColor = color;
    }

    void UIButtonComponent::SetHoveredColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_hoveredColor = color;
    }

    void UIButtonComponent::SetPressedColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_pressedColor = color;
    }

    void UIButtonComponent::SetDisabledColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        m_disabledColor = color;
    }

    void UIButtonComponent::SetTextColor(
        const DirectX::XMFLOAT4& color)
    {
        m_textColor = color;
        RefreshText();
    }

    void UIButtonComponent::SetInteractable(
        const bool interactable) noexcept
    {
        m_interactable = interactable;
        if (!interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
        }
    }

    void UIButtonComponent::SetTexturePath(
        std::filesystem::path path)
    {
        std::shared_ptr<const TextureAsset>
            texture;
        if (m_assets != nullptr
            && !path.empty())
        {
            texture =
                m_assets->LoadTexture(path);
        }
        m_texturePath = std::move(path);
        m_texture = std::move(texture);
    }

    void UIButtonComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        if (!m_texturePath.empty())
        {
            m_texture =
                m_assets->LoadTexture(
                    m_texturePath);
        }
        RefreshText();
    }

    void UIButtonComponent::OnUpdate(float)
    {
        m_clicked = false;
        if (m_graphics == nullptr
            || !m_interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
            return;
        }

        const auto& pointer =
            m_graphics->Input().Pointer();
        UIRect rect;
        if (const auto* transform =
            Owner().GetComponent<
                UIRectTransformComponent>())
        {
            rect = transform->Resolve(
                static_cast<float>(
                    m_graphics->UIWidth()),
                static_cast<float>(
                    m_graphics->UIHeight()));
        }
        else
        {
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMStoreFloat4x4(
                &world,
                Owner().WorldMatrix());
            rect = {
                { world._41, world._42 },
                {
                    world._41 + m_fallbackSize.x,
                    world._42 + m_fallbackSize.y
                }
            };
        }

        m_hovered =
            pointer.valid
            && rect.Contains(pointer.position);
        if (m_hovered && m_circularHitArea)
        {
            // 矩形に内接する楕円の内側だけを有効にします。
            const float halfW =
                (rect.maximum.x - rect.minimum.x) * 0.5f;
            const float halfH =
                (rect.maximum.y - rect.minimum.y) * 0.5f;
            if (halfW > 0.0f && halfH > 0.0f)
            {
                const float dx =
                    (pointer.position.x
                        - (rect.minimum.x + halfW))
                    / halfW;
                const float dy =
                    (pointer.position.y
                        - (rect.minimum.y + halfH))
                    / halfH;
                m_hovered = dx * dx + dy * dy <= 1.0f;
            }
        }
        if (pointer.pressed)
        {
            m_pressedInside = m_hovered;
        }
        if (pointer.released)
        {
            m_clicked =
                m_pressedInside && m_hovered;
            m_pressedInside = false;
        }
        if (!pointer.down
            && !pointer.released)
        {
            m_pressedInside = false;
        }
        if (m_clicked)
        {
            if (!m_clickEventName.empty())
            {
                // 名前付きイベントとして発行します
                // （Script::Onで受信できます）。
                EventArgs eventArgs;
                eventArgs.sender = &Owner();
                Owner().GetScene().Events().Publish(
                    m_clickEventName,
                    eventArgs);
            }
            // シーン遷移の失敗を握りつぶすと「押しても何も
            // 起きない」状態になるため、必ず理由をログへ残します。
            auto& scenes = Owner().GetScene().Scenes();
            if (m_reloadCurrentScene)
            {
                if (!scenes.RequestReloadAsync())
                {
                    Logger::Instance().Error(
                        "UI Buttonのシーン再読み込みに失敗しました: "
                        + scenes.LastError());
                }
            }
            else if (!m_targetScene.empty())
            {
                const bool requested =
                    m_loadTargetAdditive
                        ? scenes.
                            RequestLoadAdditiveAsync(
                                m_targetScene)
                        : scenes.RequestLoadAsync(
                            m_targetScene);
                if (!requested)
                {
                    Logger::Instance().Error(
                        m_loadTargetAdditive
                            ? "UI Buttonの追加シーン読み込みに"
                              "失敗しました: "
                                + PathToUtf8(m_targetScene)
                                + " | "
                                + scenes.LastError()
                            : "UI Buttonのシーン遷移に失敗しました: "
                                + PathToUtf8(m_targetScene)
                                + " | "
                                + scenes.LastError());
                }
            }
        }
    }

    void UIButtonComponent::OnRender2D(
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView*
            whiteTexture)
    {
        using namespace DirectX;

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
        }
        else
        {
            XMFLOAT4X4 world{};
            XMStoreFloat4x4(
                &world,
                Owner().WorldMatrix());
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

        const XMFLOAT4* color =
            !m_interactable
                ? &m_disabledColor
                : m_pressedInside
                    ? &m_pressedColor
                    : m_hovered
                        ? &m_hoveredColor
                        : &m_normalColor;
        const auto premultiplied =
            Premultiply(*color);
        const float textureWidth =
            m_texture
                ? static_cast<float>(
                    m_texture->width)
                : 1.0f;
        const float textureHeight =
            m_texture
                ? static_cast<float>(
                    m_texture->height)
                : 1.0f;
        spriteBatch.Draw(
            m_texture
                ? m_texture->view.Get()
                : whiteTexture,
            rect.minimum,
            nullptr,
            XMLoadFloat4(
                &premultiplied),
            0.0f,
            {},
            {
                size.x / textureWidth,
                size.y / textureHeight
            });

        if (m_textTexture)
        {
            const XMFLOAT2 labelScale{
                size.x / static_cast<float>(
                    m_textTexture->width),
                size.y / static_cast<float>(
                    m_textTexture->height)
            };
            // 文字テクスチャは白で焼いてあるので色はここで掛けます。
            spriteBatch.Draw(
                m_textTexture->view.Get(),
                rect.minimum,
                nullptr,
                PremultipliedTextColor(m_textColor),
                0.0f,
                {},
                labelScale);
        }
    }

    void UIButtonComponent::RefreshText()
    {
        if (m_assets == nullptr
            || m_label.empty())
        {
            m_textTexture.reset();
            return;
        }
        m_textTexture =
            m_assets->LoadTextTexture(
                m_label,
                m_fontFamily,
                m_fontSize,
                TextLayoutOptions{
                    { 512.0f, 128.0f },
                    TextHorizontalAlignment::Center,
                    TextVerticalAlignment::Center,
                    false });
    }
}
