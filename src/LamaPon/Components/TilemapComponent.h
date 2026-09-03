#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    class TilemapComponent final : public Component
    {
    public:
        using CellCoordinate = std::pair<int, int>;
        using CellMap = std::map<CellCoordinate, std::uint32_t>;

        // 隣接セルをまとめた矩形（Tilemapのローカル座標、セルの中心基準）。
        struct CollisionRect final
        {
            DirectX::XMFLOAT2 center;
            DirectX::XMFLOAT2 size;
        };

        explicit TilemapComponent(
            DirectX::XMFLOAT2 tileSize = { 32.0f, 32.0f },
            std::uint32_t atlasColumns = 1,
            std::uint32_t atlasRows = 1,
            DirectX::XMFLOAT4 color =
                { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path texturePath = {}) noexcept;

        void SetTileSize(
            const DirectX::XMFLOAT2& tileSize) noexcept;
        void SetAtlasGrid(
            std::uint32_t columns,
            std::uint32_t rows) noexcept;
        void SetColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_color = color;
        }
        void SetTexturePath(
            std::filesystem::path texturePath);
        // 背景／地形／前景など、複数のTilemapを重ねる際の描画順です
        // （数値が大きいほど手前）。Sprite RendererのSortOrderと同じ
        // 尺度で比較されます。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        [[nodiscard]] const DirectX::XMFLOAT2&
            TileSize() const noexcept
        {
            return m_tileSize;
        }
        [[nodiscard]] std::uint32_t
            AtlasColumns() const noexcept
        {
            return m_atlasColumns;
        }
        [[nodiscard]] std::uint32_t
            AtlasRows() const noexcept
        {
            return m_atlasRows;
        }
        [[nodiscard]] std::uint32_t
            TileCount() const noexcept
        {
            return m_atlasColumns * m_atlasRows;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            Color() const noexcept
        {
            return m_color;
        }
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }
        [[nodiscard]] const CellMap&
            Cells() const noexcept
        {
            return m_cells;
        }

        [[nodiscard]] int TileAt(
            int x,
            int y) const noexcept;
        bool SetCell(
            int x,
            int y,
            std::uint32_t tileIndex);
        bool EraseCell(int x, int y) noexcept;
        void Clear() noexcept { m_cells.clear(); }

        // 配置済みセルをすべて衝突対象として、隣接セルをまとめた最小限の
        // 矩形集合を計算します（貪欲な矩形マージ）。当たり判定を持たない
        // 装飾タイルは別のTilemap GameObjectへ分けてください。
        [[nodiscard]] std::vector<CollisionRect>
            ComputeCollisionRects() const;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "Tilemap";
        }
        [[nodiscard]] int RenderSortOrder() const noexcept override
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
        DirectX::XMFLOAT2 m_tileSize;
        std::uint32_t m_atlasColumns;
        std::uint32_t m_atlasRows;
        DirectX::XMFLOAT4 m_color;
        std::filesystem::path m_texturePath;
        int m_sortOrder{};
        CellMap m_cells;
        std::shared_ptr<const TextureAsset> m_texture;
        AssetManager* m_assets{};
        GraphicsDevice* m_graphics{};
    };
}
