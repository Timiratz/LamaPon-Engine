#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;
}

namespace LamaPon
{
    // Sprite Mask（SpriteMaskComponent）との関係。Noneはマスクの
    // 影響を受けません。マスクが複数ある場合は最も近い1つだけを
    // 使います。
    enum class SpriteMaskInteraction
    {
        None,
        VisibleInsideMask,
        VisibleOutsideMask
    };

    class SpriteRendererComponent final : public Component
    {
    public:
        static constexpr std::size_t CustomParameterCount = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        explicit SpriteRendererComponent(
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f },
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path texturePath = {}) noexcept;

        void SetSize(const DirectX::XMFLOAT2& size) noexcept { m_size = size; }
        void SetColor(const DirectX::XMFLOAT4& color) noexcept { m_color = color; }
        // 基準点（0〜1の割合）。{0,0}が左上、{0.5,0.5}が中心、
        // {1,1}が右下です。既定値は左上です。
        //
        // Transformの位置が指す場所と回転の中心を、この値で指定します。
        // 中心を使う場合は{0.5,0.5}を設定すると、回転時の位置補正が
        // 不要になります。
        //
        // UI Rect Transformを持つGameObjectでは使いません。UIの位置は
        // Rect Transform側のAnchorとPivotが決め、回転は矩形の中心です。
        void SetPivot(const DirectX::XMFLOAT2& pivot) noexcept
        {
            m_pivot = pivot;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            Pivot() const noexcept
        {
            return m_pivot;
        }
        void SetTexturePath(std::filesystem::path texturePath);
        void SetSortOrder(const int sortOrder) noexcept { m_sortOrder = sortOrder; }
        void SetMaskInteraction(
            const SpriteMaskInteraction interaction) noexcept
        {
            m_maskInteraction = interaction;
        }
        [[nodiscard]] SpriteMaskInteraction
            MaskInteraction() const noexcept
        {
            return m_maskInteraction;
        }
        void SetShaderPath(std::filesystem::path shaderPath)
        {
            m_shaderPath = std::move(shaderPath);
        }
        void SetCustomParameter(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customParameters.size())
            {
                m_customParameters[index] = value;
            }
        }
        void ReloadShader();
        // テクスチャの一部だけを表示する正規化矩形
        // （x, y, 幅, 高さ。すべて0～1）。スプライトシートや
        // アトラスの1コマ表示に使い、SpriteAnimatorが毎フレーム
        // 更新します。{0,0,1,1}で全体表示です。
        void SetSourceRect(
            const DirectX::XMFLOAT4& sourceRect) noexcept
        {
            m_sourceRect = sourceRect;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            SourceRect() const noexcept
        {
            return m_sourceRect;
        }
        // Cameraが描いたレンダーテクスチャを表示します。名前を
        // 設定するとTextureより優先され、ミニマップや防犯カメラの
        // 映像をそのまま貼れます。空にすると通常のテクスチャに
        // 戻ります。名前はCameraのTarget Textureと合わせます。
        void SetRenderTexture(std::string name)
        {
            m_renderTexture = std::move(name);
        }
        [[nodiscard]] const std::string&
            RenderTexture() const noexcept
        {
            return m_renderTexture;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept { return m_size; }
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }
        [[nodiscard]] const std::filesystem::path& TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] int SortOrder() const noexcept { return m_sortOrder; }
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_shaderPath;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(const std::size_t index) const noexcept
        {
            return m_customParameters[
                index < m_customParameters.size() ? index : 0];
        }
        [[nodiscard]] const CustomParameters&
            CustomParameterValues() const noexcept
        {
            return m_customParameters;
        }
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        [[nodiscard]] std::uint64_t
            ShaderGeneration() const noexcept
        {
            return m_shaderGeneration;
        }
        DirectX::SpriteBatch& BeginRenderBatch(
            GraphicsDevice& graphics);
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "SpriteRenderer"; }
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView* whiteTexture) override;

    private:
        DirectX::XMFLOAT2 m_size;
        DirectX::XMFLOAT4 m_color;
        // 既定は左上。ここを変えると既存のシーンで全スプライトが
        // 動いてしまうため、中心にはしていません。
        DirectX::XMFLOAT2 m_pivot{ 0.0f, 0.0f };
        DirectX::XMFLOAT4 m_sourceRect{
            0.0f, 0.0f, 1.0f, 1.0f };
        std::filesystem::path m_texturePath;
        std::filesystem::path m_shaderPath;
        CustomParameters m_customParameters{};
        int m_sortOrder{};
        SpriteMaskInteraction m_maskInteraction{
            SpriteMaskInteraction::None };
        std::string m_renderTexture;
        std::shared_ptr<const TextureAsset> m_texture;
        AssetManager* m_assets{};
        GraphicsDevice* m_graphics{};
        std::uint64_t m_shaderGeneration{};
        std::string m_shaderError;
    };
}
