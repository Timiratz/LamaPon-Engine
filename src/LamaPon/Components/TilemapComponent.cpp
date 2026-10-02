#include "LamaPon/Components/TilemapComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

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
        // 読み込み後に差し替える画像
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
        // 検索した配置セル
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

        // 更新前の配置セル
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
        // 生成する衝突矩形の一覧
        std::vector<CollisionRect> rects;
        if (m_cells.empty())
        {
            return rects;
        }

        // 矩形にまとめていないセル集合
        std::set<CellCoordinate> remaining;
        // 配置座標を集合に追加する(coordinate: セル座標, tileIndex: 未使用のタイル番号)。
        for (const auto& [coordinate, tileIndex] : m_cells)
        {
            remaining.insert(coordinate);
        }

        // X、Yの順で最小の未処理セルから横幅を決め、全幅が埋まる行だけ縦に広げる。
        while (!remaining.empty())
        {
            // 次の矩形の開始セル
            const auto start = *remaining.begin();
            // 矩形の開始X座標
            const int startX = start.first;
            // 矩形の開始Y座標
            const int startY = start.second;

            // 矩形の横方向のセル数
            int width = 1;
            while (remaining.contains(
                { startX + width, startY }))
            {
                ++width;
            }

            // 矩形の縦方向のセル数
            int height = 1;
            // 次の行まで矩形を伸ばせるか
            bool canExpand = true;
            while (canExpand)
            {
                // 検査または削除するセルX座標
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

            // 削除するセルY座標
            for (int y = startY; y < startY + height; ++y)
            {
                // 検査または削除するセルX座標
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
        const SpriteDrawContext& sprites)
    {
        using namespace DirectX;

        // 所有者のワールド行列
        XMFLOAT4X4 world{};
        XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());

        // ワールドX軸のXY上の拡縮率
        const float worldScaleX = std::sqrt(
            world._11 * world._11
            + world._12 * world._12);
        // ワールドY軸のXY上の拡縮率
        const float worldScaleY = std::sqrt(
            world._21 * world._21
            + world._22 * world._22);
        // ワールドX軸からのZ回転角
        const float rotation =
            std::atan2(world._12, world._11);
        // アルファ乗算済みの描画色
        const XMFLOAT4 premultipliedColor{
            m_color.x * m_color.w,
            m_color.y * m_color.w,
            m_color.z * m_color.w,
            m_color.w
        };

        // アトラスの1セルの画像幅
        const float sourceWidth = m_texture
            ? static_cast<float>(m_texture->width)
                / static_cast<float>(m_atlasColumns)
            : 1.0f;
        // アトラスの1セルの画像高さ
        const float sourceHeight = m_texture
            ? static_cast<float>(m_texture->height)
                / static_cast<float>(m_atlasRows)
            : 1.0f;
        // タイル画像から表示への拡縮率
        const XMFLOAT2 scale{
            m_tileSize.x / sourceWidth
                * worldScaleX,
            m_tileSize.y / sourceHeight
                * worldScaleY
        };

        // タイル画像の描画ビュー
        GraphicsViewHandle textureView;
        if (m_texture)
        {
            // 画像のGPU資源の借用
            const auto resources =
                m_texture->resources.Acquire();
            textureView = resources
                ? resources->shaderResourceView
                : GraphicsViewHandle{};
        }

        // 配置セルを描く(coordinate: セル座標, tileIndex: アトラスのタイル番号)。
        for (const auto& [coordinate, tileIndex] :
            m_cells)
        {
            if (tileIndex >= TileCount())
            {
                continue;
            }

            // セル左上のローカルX座標
            const float localX =
                static_cast<float>(coordinate.first)
                * m_tileSize.x;
            // セル左上のローカルY座標
            const float localY =
                static_cast<float>(coordinate.second)
                * m_tileSize.y;
            // 表示オフセット適用後の位置
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

            // セルのスプライト描画指定
            SpriteDrawRequest request;
            request.texture = textureView;
            request.position = position;
            request.tint = premultipliedColor;
            request.rotation = rotation;
            request.scale = scale;
            if (m_texture)
            {
                // タイル番号のアトラス列
                const std::uint32_t column =
                    tileIndex % m_atlasColumns;
                // タイル番号のアトラス行
                const std::uint32_t row =
                    tileIndex / m_atlasColumns;
                request.hasSourceRectangle = true;
                request.sourceRectangle.left =
                    static_cast<std::int32_t>(
                        column * m_texture->width
                        / m_atlasColumns);
                request.sourceRectangle.top =
                    static_cast<std::int32_t>(
                        row * m_texture->height
                        / m_atlasRows);
                request.sourceRectangle.right =
                    static_cast<std::int32_t>(
                        (column + 1)
                            * m_texture->width
                        / m_atlasColumns);
                request.sourceRectangle.bottom =
                    static_cast<std::int32_t>(
                        (row + 1)
                            * m_texture->height
                        / m_atlasRows);
            }

            static_cast<void>(sprites.Draw(request));
        }
    }

    bool TilemapComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        return Detail::DescribeTexturedDraw(
            description,
            "タイル " + std::to_string(Cells().size()) + "枚",
            TexturePath(),
            RenderSortOrder());
    }
}
