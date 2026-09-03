#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;
    struct TextureAsset;

    class UIButtonComponent final : public Component
    {
    public:
        explicit UIButtonComponent(
            std::string label = "ボタン",
            DirectX::XMFLOAT2 fallbackSize =
                { 220.0f, 56.0f },
            std::filesystem::path texturePath = {});

        void SetLabel(std::string label);
        void SetFontFamily(std::string family);
        void SetFontSize(float size);
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetNormalColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetHoveredColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetPressedColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetDisabledColor(
            const DirectX::XMFLOAT4& color) noexcept;
        void SetTextColor(
            const DirectX::XMFLOAT4& color);
        void SetInteractable(bool interactable) noexcept;
        void SetTexturePath(
            std::filesystem::path path);
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        void SetTargetScene(
            std::filesystem::path path)
        {
            m_targetScene = std::move(path);
        }
        void SetReloadCurrentScene(
            bool reload) noexcept
        {
            m_reloadCurrentScene = reload;
        }
        // trueなら、Target Sceneを切り替えではなく追加読み込み
        // （Additive）で開きます。設定画面やポーズ画面を今の
        // シーンへ重ねたいときに使います。
        void SetLoadTargetAdditive(
            bool additive) noexcept
        {
            m_loadTargetAdditive = additive;
        }

        [[nodiscard]] const std::string&
            Label() const noexcept { return m_label; }
        [[nodiscard]] const std::string&
            FontFamily() const noexcept
        {
            return m_fontFamily;
        }
        [[nodiscard]] float FontSize() const noexcept
        {
            return m_fontSize;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            NormalColor() const noexcept
        {
            return m_normalColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            HoveredColor() const noexcept
        {
            return m_hoveredColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            PressedColor() const noexcept
        {
            return m_pressedColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            DisabledColor() const noexcept
        {
            return m_disabledColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
        }
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // trueなら当たり判定を矩形ではなく内接する楕円にします
        // （円形ボタンの四隅の透明部分がクリックに反応しなくなります）。
        void SetCircularHitArea(
            const bool circular) noexcept
        {
            m_circularHitArea = circular;
        }
        [[nodiscard]] bool
            CircularHitArea() const noexcept
        {
            return m_circularHitArea;
        }
        [[nodiscard]] bool IsHovered() const noexcept
        {
            return m_hovered;
        }
        [[nodiscard]] bool IsPressed() const noexcept
        {
            return m_pressedInside;
        }
        [[nodiscard]] bool WasClicked() const noexcept
        {
            return m_clicked;
        }
        [[nodiscard]] bool ConsumeClick() noexcept
        {
            const bool clicked = m_clicked;
            m_clicked = false;
            return clicked;
        }
        // クリック時にSceneのEventBusへ発行するイベント名
        // （空なら発行しません。Script::Onで受信できます）。
        void SetClickEventName(std::string name)
        {
            m_clickEventName = std::move(name);
        }
        [[nodiscard]] const std::string&
            ClickEventName() const noexcept
        {
            return m_clickEventName;
        }
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] const std::filesystem::path&
            TargetScene() const noexcept
        {
            return m_targetScene;
        }
        [[nodiscard]] bool
            ReloadCurrentScene() const noexcept
        {
            return m_reloadCurrentScene;
        }
        [[nodiscard]] bool
            LoadTargetAdditive() const noexcept
        {
            return m_loadTargetAdditive;
        }
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIButton";
        }
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView*
                whiteTexture) override;

    private:
        void RefreshText();

        std::string m_label;
        std::string m_fontFamily{
            "Yu Gothic UI" };
        float m_fontSize{ 24.0f };
        DirectX::XMFLOAT2 m_fallbackSize;
        DirectX::XMFLOAT4 m_normalColor{
            0.08f, 0.28f, 0.52f, 0.96f };
        DirectX::XMFLOAT4 m_hoveredColor{
            0.12f, 0.42f, 0.76f, 1.0f };
        DirectX::XMFLOAT4 m_pressedColor{
            0.04f, 0.20f, 0.40f, 1.0f };
        DirectX::XMFLOAT4 m_disabledColor{
            0.18f, 0.20f, 0.24f, 0.65f };
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        bool m_interactable{ true };
        bool m_circularHitArea{};
        bool m_hovered{};
        bool m_pressedInside{};
        bool m_clicked{};
        std::string m_clickEventName;
        std::filesystem::path m_texturePath;
        std::filesystem::path m_targetScene;
        bool m_reloadCurrentScene{};
        bool m_loadTargetAdditive{};
        int m_sortOrder{};
        std::shared_ptr<const TextureAsset>
            m_texture;
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
    };
}
