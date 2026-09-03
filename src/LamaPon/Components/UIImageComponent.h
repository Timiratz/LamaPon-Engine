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
    struct TextureAsset;

    // UI用の画像。borderを設定すると9-slice（角を伸縮させない
    // 分割描画）になります。
    class UIImageComponent final : public Component
    {
    public:
        explicit UIImageComponent(
            std::filesystem::path texturePath = {},
            DirectX::XMFLOAT4 color =
                { 1.0f, 1.0f, 1.0f, 1.0f });

        void SetTexturePath(std::filesystem::path path);
        void SetColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_color = color;
        }
        // 左・上・右・下の順のピクセル境界。すべて0で通常描画。
        void SetBorder(
            const DirectX::XMFLOAT4& border) noexcept;
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // Cameraが描いたレンダーテクスチャを表示します（名前は
        // CameraのTarget Textureと合わせます）。設定するとTexture
        // より優先され、9-sliceは無効になります。ミニマップや
        // 装備プレビューをUIへ出すのに使います。
        void SetRenderTexture(std::string name)
        {
            m_renderTexture = std::move(name);
        }
        [[nodiscard]] const std::string&
            RenderTexture() const noexcept
        {
            return m_renderTexture;
        }

        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            Color() const noexcept
        {
            return m_color;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            Border() const noexcept
        {
            return m_border;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIImage";
        }
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView*
                whiteTexture) override;

    private:
        std::filesystem::path m_texturePath;
        std::string m_renderTexture;
        DirectX::XMFLOAT4 m_color;
        DirectX::XMFLOAT4 m_border{};
        DirectX::XMFLOAT2 m_fallbackSize{
            100.0f, 100.0f };
        int m_sortOrder{};
        std::shared_ptr<const TextureAsset> m_texture;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
    };
}
