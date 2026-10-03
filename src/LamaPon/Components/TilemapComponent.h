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

    // 数値設定は有限値で、セル座標の拡張計算がintの範囲に収まることを前提とする。
    class TilemapComponent final : public Component
    {
    public:
        // 横と縦の整数セル座標
        using CellCoordinate = std::pair<int, int>;
        // セル座標からタイル番号への対応
        using CellMap = std::map<CellCoordinate, std::uint32_t>;

        // セル範囲をまとめたローカル座標の矩形で、セルの左上は座標×タイルサイズとなる。
        struct CollisionRect final
        {
            // ローカル座標の矩形中心
            DirectX::XMFLOAT2 center;
            // ローカル座標の幅と高さ
            DirectX::XMFLOAT2 size;
        };

        // 均等アトラスから指定サイズのタイルを配置する(tileSize: ローカルでのタイル幅と高さ, atlasColumns: アトラスの列数, atlasRows: アトラスの行数, color: アルファ乗算前のRGBA, texturePath: タイル画像のパス)。
        explicit TilemapComponent(
            DirectX::XMFLOAT2 tileSize = { 32.0f, 32.0f },
            std::uint32_t atlasColumns = 1,
            std::uint32_t atlasRows = 1,
            DirectX::XMFLOAT4 color =
                { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path texturePath = {}) noexcept;

        // タイルのローカル幅と高さを各軸1以上に収める(tileSize: タイルの幅と高さ)。
        void SetTileSize(
            const DirectX::XMFLOAT2& tileSize) noexcept;
        // アトラスの列数と行数を各1〜64に収める(columns: アトラスの列数, rows: アトラスの行数)。
        // 既存セルの番号は変更せず、新しいグリッド範囲外のセルは描画時に省く。
        void SetAtlasGrid(
            std::uint32_t columns,
            std::uint32_t rows) noexcept;
        // タイル全体の描画色を設定する(color: アルファ乗算前のRGBA)。
        void SetColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_color = color;
        }
        // タイル画像を読み込んで変更し、空なら単色表示にする(texturePath: タイル画像のパス)。
        // 読み込み失敗時は以前のパスと画像を保持する。
        void SetTexturePath(
            std::filesystem::path texturePath);
        // 他の2D部品と共通の描画順を設定する(sortOrder: 小さいほど先に描く順番)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        // タイルのローカル幅と高さを取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            TileSize() const noexcept
        {
            return m_tileSize;
        }
        // アトラスの列数を取得する。
        [[nodiscard]] std::uint32_t
            AtlasColumns() const noexcept
        {
            return m_atlasColumns;
        }
        // アトラスの行数を取得する。
        [[nodiscard]] std::uint32_t
            AtlasRows() const noexcept
        {
            return m_atlasRows;
        }
        // アトラス内で有効なタイル番号の個数を取得する。
        [[nodiscard]] std::uint32_t
            TileCount() const noexcept
        {
            return m_atlasColumns * m_atlasRows;
        }
        // タイルのアルファ乗算前の描画色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            Color() const noexcept
        {
            return m_color;
        }
        // タイル画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // 設定された2D描画順を取得する。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }
        // セル座標とタイル番号の対応を取得する。
        [[nodiscard]] const CellMap&
            Cells() const noexcept
        {
            return m_cells;
        }

        // セルのタイル番号を取得し、未配置なら-1を返す(x: セルのX座標, y: セルのY座標)。
        [[nodiscard]] int TileAt(
            int x,
            int y) const noexcept;
        // セルを配置して変更の有無を返し、範囲外のタイル番号なら例外を返す(x: セルのX座標, y: セルのY座標, tileIndex: 左上から行優先のタイル番号)。
        bool SetCell(
            int x,
            int y,
            std::uint32_t tileIndex);
        // 指定セルを削除し、削除したかを返す(x: セルのX座標, y: セルのY座標)。
        bool EraseCell(int x, int y) noexcept;
        // 配置済みのセルをすべて削除する。
        void Clear() noexcept { m_cells.clear(); }

        // 全配置セルを横に伸ばしてから縦に広げる貪欲な矩形集合にまとめる。
        // タイル番号や描画可否によらず全セルを含むため、衝突不要の装飾は別Tilemapに配置する。
        [[nodiscard]] std::vector<CollisionRect>
            ComputeCollisionRects() const;

        // 配置枚数と画像などの描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "Tilemap";
        }
        // 2D描画を整列するための順番を取得する。
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画機器とアセットを借用し、指定のタイル画像を読む(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 有効なタイルを所有者のワールド変換と2D表示オフセットで描く(sprites: スプライト描画先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // ローカルでのタイル幅と高さ
        DirectX::XMFLOAT2 m_tileSize;
        // アトラスの列数
        std::uint32_t m_atlasColumns;
        // アトラスの行数
        std::uint32_t m_atlasRows;
        // アルファ乗算前のRGBA
        DirectX::XMFLOAT4 m_color;
        // タイル画像のパス
        std::filesystem::path m_texturePath;
        // 2D描画の並び順
        int m_sortOrder{};
        // セル座標とタイル番号の対応
        CellMap m_cells;
        // 共有するタイル画像
        std::shared_ptr<const TextureAsset> m_texture;
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
    };
}
