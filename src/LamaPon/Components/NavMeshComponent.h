#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace LamaPon
{
    // 所有者のワールド位置だけを使う平坦なXZグリッドで、回転・拡縮は適用しない。
    // 数値は有限でセル座標と二乗距離がintに収まることを前提とし、移動や障害物変更後は再ベイクする。
    class NavMeshComponent final : public Component
    {
    public:
        // XとZの非負セル座標
        using CellCoordinate = std::pair<
            std::uint32_t,
            std::uint32_t>;

        // 平坦なXZグリッドの経路探索サーフェスを作る(surfaceSize: XとZ方向の範囲, cellSize: セルの一辺の長さ, agentRadius: 障害物を広げる半径, agentHeight: 障害物を調べる高さ)。
        explicit NavMeshComponent(
            DirectX::XMFLOAT2 surfaceSize =
                { 20.0f, 20.0f },
            float cellSize = 1.0f,
            float agentRadius = 0.4f,
            float agentHeight = 1.8f) noexcept;

        // 絶対値を各軸1〜4096に収め、最大128セルに調整してベイクを解除する(size: XとZ方向の範囲)。
        void SetSurfaceSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 絶対値を0.1〜64に収め、各軸128セル以下に調整してベイクを解除する(size: セルの一辺の長さ)。
        void SetCellSize(float size) noexcept;
        // 半径の絶対値を0〜32に収めてベイクを解除する(radius: 障害物をXZへ広げる半径)。
        void SetAgentRadius(float radius) noexcept;
        // 高さの絶対値を0.1〜64に収めてベイクを解除する(height: サーフェス上の調査高さ)。
        void SetAgentHeight(float height) noexcept;

        // グリッド軸のセル数を決めるXとZの範囲を取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            SurfaceSize() const noexcept
        {
            return m_surfaceSize;
        }
        // グリッドセルの一辺の長さを取得する。
        [[nodiscard]] float
            CellSize() const noexcept
        {
            return m_cellSize;
        }
        // ベイク時に障害物をXZ方向へ広げる半径を取得する。
        [[nodiscard]] float
            AgentRadius() const noexcept
        {
            return m_agentRadius;
        }
        // ベイク時に障害物を調べる高さを取得する。
        [[nodiscard]] float
            AgentHeight() const noexcept
        {
            return m_agentHeight;
        }
        // X範囲をセルサイズで切り上げた1〜128の列数を取得する。
        [[nodiscard]] std::uint32_t
            GridWidth() const noexcept;
        // Z範囲をセルサイズで切り上げた1〜128の行数を取得する。
        [[nodiscard]] std::uint32_t
            GridDepth() const noexcept;
        // セルの通行状態が確保されているか確認する。
        [[nodiscard]] bool
            IsBaked() const noexcept
        {
            return !m_blocked.empty();
        }
        // 通行禁止として記録されたセル数を取得する。
        [[nodiscard]] std::size_t
            BlockedCellCount() const noexcept;
        // 未ベイク・範囲外・通行禁止のいずれかならtrueを返す(x: セルのX座標, z: セルのZ座標)。
        [[nodiscard]] bool IsBlocked(
            std::uint32_t x,
            std::uint32_t z) const noexcept;
        // X優先で並ぶ通行状態の配列を取得する。
        [[nodiscard]] const std::vector<
            std::uint8_t>&
            BakedCells() const noexcept
        {
            return m_blocked;
        }

        // 高さ範囲と半径で広げた障害物AABBからセル中心の通行可否を作る(obstacles: ワールド座標の障害物AABB)。
        // サーフェスから0.05以下の床は無視し、セル面積全体ではなく中心点で障害物を判定する。
        void Bake(
            std::span<const Bounds3D>
                obstacles);
        // 通行状態を解除して全セルを通行禁止扱いにする。
        void ClearBake() noexcept
        {
            m_blocked.clear();
        }
        // 全セルを通行可として再確保し、指定の範囲内セルを通行禁止にする(blockedCells: 通行禁止セルの座標一覧)。
        void RestoreBake(
            std::span<
                const CellCoordinate>
                    blockedCells);

        // 近傍の通行可セルで8近傍探索と平滑化を行い、未ベイクや経路なしなら空を返す(start: ワールド座標の始点, destination: ワールド座標の目的地)。
        // 通行可セルが皆無なら例外となり、返す始点と目的地は入力座標のままで範囲内や通行可を保証しない。
        [[nodiscard]] std::vector<
            DirectX::XMFLOAT3> FindPath(
                const DirectX::XMFLOAT3&
                    start,
                const DirectX::XMFLOAT3&
                    destination) const;
        // 所有者のワールド位置を中心とするグリッドのセル中心を求める(x: セルのX座標, z: セルのZ座標)。
        [[nodiscard]] DirectX::XMFLOAT3
            CellCenter(
                std::uint32_t x,
                std::uint32_t z) const noexcept;
        // Y座標やベイク状態を問わず、点のXZがグリッド範囲内か確認する(point: 調べるワールド座標)。
        [[nodiscard]] bool ContainsPoint(
            const DirectX::XMFLOAT3& point)
            const noexcept;
        // 所有者のワールドY位置をサーフェスの高さとして取得する。
        [[nodiscard]] float
            SurfaceHeight() const noexcept;

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "NavMesh";
        }

    protected:
        // 通行可否で色分けしてグリッドを描く(graphics: デバッグ描画機器, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        // ワールドXZからセル座標を出力し、グリッド範囲内か返す(point: ワールド座標, x: セルX座標の出力先, z: セルZ座標の出力先)。
        [[nodiscard]] bool WorldToCell(
            const DirectX::XMFLOAT3&
                point,
            int& x,
            int& z) const noexcept;
        // セルから二乗距離が最小の通行可セルを探し、なければ例外を返す(x: 基準セルのX座標, z: 基準セルのZ座標)。
        [[nodiscard]] CellCoordinate
            NearestWalkable(
                int x,
                int z) const;
        // 通行可の端点セル間を通る全セルと斜めの両隣セルの通行可否を調べる(from: 始点の通行可セル, to: 終点の通行可セル)。
        [[nodiscard]] bool HasLineOfSight(
            const CellCoordinate& from,
            const CellCoordinate& to) const noexcept;
        // セル座標をX優先の通行状態配列の添字へ変換する(x: セルのX座標, z: セルのZ座標)。
        [[nodiscard]] std::size_t
            CellIndex(
                std::uint32_t x,
                std::uint32_t z) const noexcept
        {
            return static_cast<std::size_t>(z)
                    * GridWidth()
                + x;
        }

        // XとZのグリッド範囲
        DirectX::XMFLOAT2 m_surfaceSize;
        // セルの一辺の長さ
        float m_cellSize;
        // 障害物を広げるXZ半径
        float m_agentRadius;
        // サーフェス上の障害物調査高さ
        float m_agentHeight;
        // X優先の通行禁止フラグ
        std::vector<std::uint8_t> m_blocked;
    };
}
