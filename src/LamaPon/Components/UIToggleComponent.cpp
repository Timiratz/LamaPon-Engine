#include "LamaPon/Components/UIToggleComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <utility>

namespace
{
    // 描画用にRGBへアルファを掛ける(color: アルファ乗算前のRGBA)。
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

    // UI矩形またはワールドXYと代替サイズから表示範囲を求める(owner: 所有オブジェクト, graphics: 描画機器かnullptr, fallbackSize: 代替の幅と高さ)。
    LamaPon::UIRect ResolveWidgetRect(
        const LamaPon::GameObject& owner,
        const LamaPon::GraphicsDevice* graphics,
        const DirectX::XMFLOAT2& fallbackSize) noexcept
    {
        // UI矩形の配置情報
        if (const auto* transform =
            owner.GetComponent<
                LamaPon::UIRectTransformComponent>();
            transform != nullptr && graphics != nullptr)
        {
            return transform->Resolve(
                static_cast<float>(graphics->UIWidth()),
                static_cast<float>(
                    graphics->UIHeight()));
        }
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            owner.WorldMatrix());
        return {
            { world._41, world._42 },
            {
                world._41 + fallbackSize.x,
                world._42 + fallbackSize.y
            }
        };
    }
}

namespace LamaPon
{
    UIToggleComponent::UIToggleComponent(
        std::string label,
        const bool isOn)
        : m_label(std::move(label))
        , m_isOn(isOn)
    {
    }

    void UIToggleComponent::SetIsOn(
        const bool isOn) noexcept
    {
        if (m_isOn == isOn)
        {
            return;
        }
        m_isOn = isOn;
        m_valueChanged = true;
    }

    void UIToggleComponent::SetLabel(std::string label)
    {
        m_label = std::move(label);
        RefreshText();
    }

    void UIToggleComponent::SetFontFamily(
        std::string family)
    {
        m_fontFamily = std::move(family);
        RefreshText();
    }

    void UIToggleComponent::SetFontSize(const float size)
    {
        m_fontSize = std::clamp(size, 1.0f, 256.0f);
        RefreshText();
    }

    void UIToggleComponent::SetInteractable(
        const bool interactable) noexcept
    {
        m_interactable = interactable;
        if (!interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
        }
    }

    void UIToggleComponent::SetTextColor(
        const DirectX::XMFLOAT4& color)
    {
        m_textColor = color;
        RefreshText();
    }

    void UIToggleComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    void UIToggleComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        RefreshText();
    }

    void UIToggleComponent::OnUpdate(float)
    {
        if (m_graphics == nullptr
            || !m_graphics->IsInitialized()
            || !m_interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
            return;
        }

        // ポインターの状態
        const auto& pointer =
            m_graphics->Input().Pointer();
        // トグルの表示矩形
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        m_hovered =
            pointer.valid
            && rect.Contains(pointer.position);
        if (pointer.pressed)
        {
            m_pressedInside = m_hovered;
        }
        if (pointer.released)
        {
            if (m_pressedInside && m_hovered)
            {
                m_isOn = !m_isOn;
                m_valueChanged = true;
            }
            m_pressedInside = false;
        }
        if (!pointer.down && !pointer.released)
        {
            m_pressedInside = false;
        }
    }

    void UIToggleComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        using namespace DirectX;

        // トグルの表示矩形
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        // トグルの表示幅と高さ
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }


        // 表示高さに合わせた枠の一辺
        const float boxSize = size.y;
        // 状態に応じた枠背景のRGBA
        auto boxColor = m_boxColor;
        if (!m_interactable)
        {
            boxColor.w *= 0.5f;
        }
        else if (m_hovered)
        {
            boxColor.x *= 1.25f;
            boxColor.y *= 1.25f;
            boxColor.z *= 1.25f;
        }
        // アルファ乗算済み枠背景色
        const auto premultipliedBox =
            Premultiply(boxColor);
        // チェック枠の描画指定
        SpriteDrawRequest boxRequest;
        boxRequest.position = rect.minimum;
        boxRequest.tint = premultipliedBox;
        boxRequest.scale = { boxSize, boxSize };
        static_cast<void>(sprites.Draw(boxRequest));

        if (m_isOn)
        {
            // オン表示の枠内余白
            const float inset = boxSize * 0.25f;
            // 状態に応じたオン表示色
            auto checkColor = m_checkColor;
            if (!m_interactable)
            {
                checkColor.w *= 0.5f;
            }
            // アルファ乗算済みオン表示色
            const auto premultipliedCheck =
                Premultiply(checkColor);
            // オン表示の描画指定
            SpriteDrawRequest checkRequest;
            checkRequest.position = {
                    rect.minimum.x + inset,
                    rect.minimum.y + inset };
            checkRequest.tint = premultipliedCheck;
            checkRequest.scale = {
                boxSize - inset * 2.0f,
                boxSize - inset * 2.0f };
            static_cast<void>(sprites.Draw(checkRequest));
        }

        if (m_textTexture)
        {
            // 文字画像のGPU資源の借用
            const auto textResources =
                m_textTexture->resources.Acquire();
            // 文字画像の描画ビュー
            const auto textTextureView = textResources
                ? textResources->shaderResourceView
                : GraphicsViewHandle{};
            // ラベル表示の左端X座標
            const float labelLeft =
                rect.minimum.x + boxSize
                + boxSize * 0.25f;
            // ラベルの表示幅
            const float labelWidth =
                rect.maximum.x - labelLeft;
            if (textTextureView
                && labelWidth > 0.0f)
            {
                // ラベル画像の描画指定
                SpriteDrawRequest textRequest;
                textRequest.texture = textTextureView;
                textRequest.position = {
                    labelLeft,
                    rect.minimum.y };

                XMStoreFloat4(
                    &textRequest.tint,
                    PremultipliedTextColor(m_textColor));
                textRequest.scale = {
                    labelWidth
                        / static_cast<float>(
                            m_textTexture->width),
                    size.y
                        / static_cast<float>(
                            m_textTexture->height)
                };
                static_cast<void>(sprites.Draw(textRequest));
            }
        }
    }

    void UIToggleComponent::RefreshText()
    {
        if (m_assets == nullptr || m_label.empty())
        {
            m_textTexture.reset();
            return;
        }
        m_textTexture = m_assets->LoadTextTexture(
            m_label,
            m_fontFamily,
            m_fontSize,
            TextLayoutOptions{
                { 512.0f, 128.0f },
                TextHorizontalAlignment::Left,
                TextVerticalAlignment::Center,
                false });
    }

    bool UIToggleComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        description.geometry = "UIトグル";
        description.material =
            "\"" + Label() + "\" " + (IsOn() ? "オン" : "オフ");
        description.state = "並び順 " + std::to_string(RenderSortOrder());
        return true;
    }
}
