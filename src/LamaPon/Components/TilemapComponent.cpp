#include "LamaPon/Components/TilemapComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace LamaPon
{
    TilemapComponent::TilemapComponent(
        const DirectX::XMFLOAT2 tileSize,
        const std::uint32_t atlasColumns,
        const std::uint32_t atlasRows,
        const DirectX::XMFLOAT4 color,
        std::filesystem::path texturePath) noexcept
        : m_tileSize(tileSize)
        , m_atlasColumns(atlasColumns)
        , m_atlasRows(atlasRows)
        , m_color(color)
        , m_texturePath(std::move(texturePath))
    {
        SetTileSize(tileSize);
        SetAtlasGrid(atlasColumns, atlasRows);
    }

    void TilemapComponent::SetTileSize(
        const DirectX::XMFLOAT2& tileSize) noexcept
    {
        m_tileSize.x = std::max(tileSize.x, 1.0f);
        m_tileSize.y = std::max(tileSize.y, 1.0f);
    }

    void TilemapComponent::SetAtlasGrid(
        const std::uint32_t columns,
        const std::uint32_t rows) noexcept
    {
        m_atlasColumns =
            std::clamp(columns, 1u, 64u);
        m_atlasRows =
            std::clamp(rows, 1u, 64u);
    }

    void TilemapComponent::OnInitialize(
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

    void TilemapComponent::SetTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr
            && !texturePath.empty())
        {
            texture =
                m_assets->LoadTexture(texturePath);
        }

        m_texturePath = std::move(texturePath);
        m_texture = std::move(texture);
    }

    int TilemapComponent::TileAt(
        const int x,
        const int y) const noexcept
    {
        const auto cell = m_cells.find({ x, y });
        return cell == m_cells.end()
            ? -1
            : static_cast<int>(cell->second);
    }

    bool TilemapComponent::SetCell(
        const int x,
        const int y,
        const std::uint32_t tileIndex)
    {
        if (tileIndex >= TileCount())
        {
            throw std::out_of_range(
                "Tile index is outside the atlas grid.");
        }

        const auto existing =
            m_cells.find({ x, y });
        if (existing != m_cells.end()
            && existing->second == tileIndex)
        {
            return false;
        }
        m_cells.insert_or_assign(
            { x, y },
            tileIndex);
        return true;
    }

    bool TilemapComponent::EraseCell(
        const int x,
        const int y) noexcept
    {
        return m_cells.erase({ x, y }) != 0;
    }

    std::vector<TilemapComponent::CollisionRect>
        TilemapComponent::ComputeCollisionRects() const
    {
        std::vector<CollisionRect> rects;
        if (m_cells.empty())
        {
            return rects;
        }

        std::set<CellCoordinate> remaining;
        for (const auto& [coordinate, tileIndex] : m_cells)
        {
            remaining.insert(coordinate);
        }

        // 未処理セルを1つ選び、右方向・下方向へ矩形が崩れない範囲まで
        // 貪欲に拡張してから未処理集合から取り除く、を繰り返します。
        while (!remaining.empty())
        {
            const auto start = *remaining.begin();
            const int startX = start.first;
            const int startY = start.second;

            int width = 1;
            while (remaining.contains(
                { startX + width, startY }))
            {
                ++width;
            }

            int height = 1;
            bool canExpand = true;
            while (canExpand)
            {
                for (int x = startX; x < startX + width; ++x)
                {
                    if (!remaining.contains(
                        { x, startY + height }))
                    {
                        canExpand = false;
                        break;
                    }
                }
                if (canExpand)
                {
                    ++height;
                }
            }

            for (int y = startY; y < startY + height; ++y)
            {
                for (int x = startX; x < startX + width; ++x)
                {
                    remaining.erase({ x, y });
                }
            }

            rects.push_back({
                {
                    (static_cast<float>(startX)
                        + static_cast<float>(width) * 0.5f)
                        * m_tileSize.x,
                    (static_cast<float>(startY)
                        + static_cast<float>(height) * 0.5f)
                        * m_tileSize.y
                },
                {
                    static_cast<float>(width) * m_tileSize.x,
                    static_cast<float>(height) * m_tileSize.y
                }
            });
        }
        return rects;
    }

    void TilemapComponent::OnRender2D(
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        XMFLOAT4X4 world{};
        XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());

        const float worldScaleX = std::sqrt(
            world._11 * world._11
            + world._12 * world._12);
        const float worldScaleY = std::sqrt(
            world._21 * world._21
            + world._22 * world._22);
        const float rotation =
            std::atan2(world._12, world._11);
        const XMFLOAT4 premultipliedColor{
            m_color.x * m_color.w,
            m_color.y * m_color.w,
            m_color.z * m_color.w,
            m_color.w
        };

        const float sourceWidth = m_texture
            ? static_cast<float>(m_texture->width)
                / static_cast<float>(m_atlasColumns)
            : 1.0f;
        const float sourceHeight = m_texture
            ? static_cast<float>(m_texture->height)
                / static_cast<float>(m_atlasRows)
            : 1.0f;
        const XMFLOAT2 scale{
            m_tileSize.x / sourceWidth
                * worldScaleX,
            m_tileSize.y / sourceHeight
                * worldScaleY
        };

        for (const auto& [coordinate, tileIndex] :
            m_cells)
        {
            if (tileIndex >= TileCount())
            {
                continue;
            }

            const float localX =
                static_cast<float>(coordinate.first)
                * m_tileSize.x;
            const float localY =
                static_cast<float>(coordinate.second)
                * m_tileSize.y;
            const XMFLOAT2 position{
                world._41
                    + localX * world._11
                    + localY * world._21
                    + (m_graphics != nullptr
                        ? m_graphics->Sprite2DOffset().x
                        : 0.0f),
                world._42
                    + localX * world._12
                    + localY * world._22
                    + (m_graphics != nullptr
                        ? m_graphics->Sprite2DOffset().y
                        : 0.0f)
            };

            RECT sourceRectangle{};
            const RECT* source{};
            if (m_texture)
            {
                const std::uint32_t column =
                    tileIndex % m_atlasColumns;
                const std::uint32_t row =
                    tileIndex / m_atlasColumns;
                sourceRectangle.left =
                    static_cast<LONG>(
                        column * m_texture->width
                        / m_atlasColumns);
                sourceRectangle.top =
                    static_cast<LONG>(
                        row * m_texture->height
                        / m_atlasRows);
                sourceRectangle.right =
                    static_cast<LONG>(
                        (column + 1)
                            * m_texture->width
                        / m_atlasColumns);
                sourceRectangle.bottom =
                    static_cast<LONG>(
                        (row + 1)
                            * m_texture->height
                        / m_atlasRows);
                source = &sourceRectangle;
            }

            spriteBatch.Draw(
                m_texture
                    ? m_texture->view.Get()
                    : whiteTexture,
                position,
                source,
                XMLoadFloat4(
                    &premultipliedColor),
                rotation,
                XMFLOAT2{ 0.0f, 0.0f },
                scale);
        }
    }
}
