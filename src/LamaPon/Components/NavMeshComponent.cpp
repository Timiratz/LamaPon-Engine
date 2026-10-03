#include "LamaPon/Components/NavMeshComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace
{
    // グリッド各軸の最大セル数
    constexpr std::uint32_t MaximumGridAxis = 128;

    struct OpenNode final
    {
        // 探索候補セルの配列添字
        // 経路に追加する中継点の番号
        std::size_t index{};
        // 始点コストと残り距離の和
        int score{};

        // 優先度キューで小さい探索スコアを先に取り出すよう比較する(other: 比較相手の探索候補)。
        bool operator<(
            const OpenNode& other) const noexcept
        {
            return score > other.score;
        }
    };
}

namespace LamaPon
{
    NavMeshComponent::NavMeshComponent(
        const DirectX::XMFLOAT2 surfaceSize,
        const float cellSize,
        const float agentRadius,
        const float agentHeight) noexcept
        : m_surfaceSize(surfaceSize)
        , m_cellSize(cellSize)
        , m_agentRadius(agentRadius)
        , m_agentHeight(agentHeight)
    {
        SetSurfaceSize(surfaceSize);
        SetCellSize(cellSize);
        SetAgentRadius(agentRadius);
        SetAgentHeight(agentHeight);
    }

    void NavMeshComponent::SetSurfaceSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_surfaceSize.x =
            std::clamp(
                std::abs(size.x),
                1.0f,
                4096.0f);
        m_surfaceSize.y =
            std::clamp(
                std::abs(size.y),
                1.0f,
                4096.0f);
        m_cellSize = std::max(
            m_cellSize,
            std::max(
                m_surfaceSize.x,
                m_surfaceSize.y)
                / static_cast<float>(
                    MaximumGridAxis));
        ClearBake();
    }

    void NavMeshComponent::SetCellSize(
        const float size) noexcept
    {
        m_cellSize =
            std::clamp(
                std::abs(size),
                0.1f,
                64.0f);
        m_cellSize = std::max(
            m_cellSize,
            std::max(
                m_surfaceSize.x,
                m_surfaceSize.y)
                / static_cast<float>(
                    MaximumGridAxis));
        ClearBake();
    }

    void NavMeshComponent::SetAgentRadius(
        const float radius) noexcept
    {
        m_agentRadius =
            std::clamp(
                std::abs(radius),
                0.0f,
                32.0f);
        ClearBake();
    }

    void NavMeshComponent::SetAgentHeight(
        const float height) noexcept
    {
        m_agentHeight =
            std::clamp(
                std::abs(height),
                0.1f,
                64.0f);
        ClearBake();
    }

    std::uint32_t
        NavMeshComponent::GridWidth() const noexcept
    {
        return std::clamp(
            static_cast<std::uint32_t>(
                std::ceil(
                    m_surfaceSize.x
                    / m_cellSize)),
            1u,
            MaximumGridAxis);
    }

    std::uint32_t
        NavMeshComponent::GridDepth() const noexcept
    {
        return std::clamp(
            static_cast<std::uint32_t>(
                std::ceil(
                    m_surfaceSize.y
                    / m_cellSize)),
            1u,
            MaximumGridAxis);
    }

    std::size_t
        NavMeshComponent::BlockedCellCount()
            const noexcept
    {
        return static_cast<std::size_t>(
            std::ranges::count(
                m_blocked,
                std::uint8_t{ 1 }));
    }

    bool NavMeshComponent::IsBlocked(
        const std::uint32_t x,
        const std::uint32_t z) const noexcept
    {
        return !IsBaked()
            || x >= GridWidth()
            || z >= GridDepth()
            || m_blocked[CellIndex(x, z)] != 0;
    }

    DirectX::XMFLOAT3
        NavMeshComponent::CellCenter(
            const std::uint32_t x,
            const std::uint32_t z) const noexcept
    {
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // グリッド最小端のワールドX
        const float originX =
            world._41
            - static_cast<float>(
                GridWidth())
                * m_cellSize
                * 0.5f;
        // グリッド最小端のワールドZ
        const float originZ =
            world._43
            - static_cast<float>(
                GridDepth())
                * m_cellSize
                * 0.5f;
        return {
            originX
                + (static_cast<float>(x)
                    + 0.5f)
                    * m_cellSize,
            world._42,
            originZ
                + (static_cast<float>(z)
                    + 0.5f)
                    * m_cellSize
        };
    }

    void NavMeshComponent::Bake(
        const std::span<
            const Bounds3D> obstacles)
    {
        // グリッドのX方向のセル数
        const auto width = GridWidth();
        // グリッドのZ方向のセル数
        const auto depth = GridDepth();
        m_blocked.assign(
            static_cast<std::size_t>(width)
                * depth,
            0);

        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // サーフェスのワールド高さ
        const float surfaceY = world._42;
        // 対象セルのZ座標
        for (std::uint32_t z{};
            // グリッドのZ方向のセル数
            z < depth;
            ++z)
        {
            // 対象セルのX座標
            for (std::uint32_t x{};
                // グリッドのX方向のセル数
                x < width;
                ++x)
            {
                // セル中心のワールド座標
                const auto center =
                    CellCenter(x, z);
                // 通行可否を調べる障害物AABB
                for (const auto& obstacle :
                    obstacles)
                {
                    if (obstacle.maximum.y
                            <= surfaceY + 0.05f
                        || obstacle.minimum.y
                            >= surfaceY
                                + m_agentHeight)
                    {
                        continue;
                    }
                    if (center.x
                            >= obstacle.minimum.x
                                - m_agentRadius
                        && center.x
                            <= obstacle.maximum.x
                                + m_agentRadius
                        && center.z
                            >= obstacle.minimum.z
                                - m_agentRadius
                        && center.z
                            <= obstacle.maximum.z
                                + m_agentRadius)
                    {
                        m_blocked[
                            CellIndex(x, z)] = 1;
                        break;
                    }
                }
            }
        }
    }

    void NavMeshComponent::RestoreBake(
        const std::span<
            const CellCoordinate>
                blockedCells)
    {
        m_blocked.assign(
            static_cast<std::size_t>(
                GridWidth())
                * GridDepth(),
            0);
        // 通行禁止セルを復元する(x: セルのX座標, z: セルのZ座標)。
        for (const auto& [x, z] :
            blockedCells)
        {
            if (x < GridWidth()
                && z < GridDepth())
            {
                m_blocked[
                    CellIndex(x, z)] = 1;
            }
        }
    }

    bool NavMeshComponent::WorldToCell(
        const DirectX::XMFLOAT3& point,
        int& x,
        int& z) const noexcept
    {
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // グリッド最小端のワールドX
        const float originX =
            world._41
            - static_cast<float>(
                GridWidth())
                * m_cellSize
                * 0.5f;
        // グリッド最小端のワールドZ
        const float originZ =
            world._43
            - static_cast<float>(
                GridDepth())
                * m_cellSize
                * 0.5f;
        x = static_cast<int>(
            std::floor(
                (point.x - originX)
                / m_cellSize));
        z = static_cast<int>(
            std::floor(
                (point.z - originZ)
                / m_cellSize));
        return x >= 0
            && z >= 0
            && x < static_cast<int>(
                GridWidth())
            && z < static_cast<int>(
                GridDepth());
    }

    NavMeshComponent::CellCoordinate
        NavMeshComponent::NearestWalkable(
            const int x,
            const int z) const
    {
        // 最近傍の通行可セルX座標
        std::uint32_t bestX{};
        // 最近傍の通行可セルZ座標
        std::uint32_t bestZ{};
        // 最良セルまでの二乗距離
        int bestDistance =
            std::numeric_limits<int>::max();
        // 近傍検索中のセルZ座標
        for (std::uint32_t candidateZ{};
            candidateZ < GridDepth();
            ++candidateZ)
        {
            // 近傍検索中のセルX座標
            for (std::uint32_t candidateX{};
                candidateX < GridWidth();
                ++candidateX)
            {
                if (IsBlocked(
                        candidateX,
                        candidateZ))
                {
                    continue;
                }
                // 対象セル間のX座標差
                const int deltaX =
                    static_cast<int>(
                        candidateX) - x;
                // 対象セル間のZ座標差
                const int deltaZ =
                    static_cast<int>(
                        candidateZ) - z;
                // 候補セルまでの二乗距離
                const int distance =
                    deltaX * deltaX
                    + deltaZ * deltaZ;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestX = candidateX;
                    bestZ = candidateZ;
                }
            }
        }
        if (bestDistance
            == std::numeric_limits<
                int>::max())
        {
            throw std::runtime_error(
                "NavMesh has no walkable cells.");
        }
        return { bestX, bestZ };
    }

    std::vector<DirectX::XMFLOAT3>
        NavMeshComponent::FindPath(
            const DirectX::XMFLOAT3& start,
            const DirectX::XMFLOAT3&
                destination) const
    {
        if (!IsBaked())
        {
            return {};
        }

        // 入力始点のセルX座標
        int startX{};
        // 入力始点のセルZ座標
        int startZ{};
        // 入力目的地のセルX座標
        int destinationX{};
        // 入力目的地のセルZ座標
        int destinationZ{};
        static_cast<void>(
            WorldToCell(
                start,
                startX,
                startZ));
        static_cast<void>(
            WorldToCell(
                destination,
                destinationX,
                destinationZ));
        // 始点に最も近い通行可セル
        const auto startCell =
            NearestWalkable(
                startX,
                startZ);
        // 目的地に最も近い通行可セル
        const auto destinationCell =
            NearestWalkable(
                destinationX,
                destinationZ);

        // グリッドのX方向のセル数
        const auto width = GridWidth();
        // グリッド全体のセル数
        const auto cellCount =
            static_cast<std::size_t>(width)
            * GridDepth();
        // 探索始点の配列添字
        const auto startIndex =
            CellIndex(
                startCell.first,
                startCell.second);
        // 探索目的地の配列添字
        const auto destinationIndex =
            CellIndex(
                destinationCell.first,
                destinationCell.second);
        // 始点から各セルへの最良コスト
        std::vector<int> scores(
            cellCount,
            std::numeric_limits<int>::max());
        // 経路を復元する直前セルの添字
        std::vector<std::size_t> parents(
            cellCount,
            std::numeric_limits<
                std::size_t>::max());
        // 最良経路を確定したセルのフラグ
        std::vector<std::uint8_t> closed(
            cellCount,
            0);
        // 探索スコア順の候補キュー
        std::priority_queue<OpenNode> open;

        // 直進10・斜め14の目的地までの推定コストを返す(xValue: 対象セルのX座標, zValue: 対象セルのZ座標)。
        const auto heuristic =
            [&destinationCell](
                const int xValue,
                const int zValue)
            {
                // 対象セル間のX座標差
                const int deltaX =
                    std::abs(
                        static_cast<int>(
                            destinationCell.first)
                        - xValue);
                // 対象セル間のZ座標差
                const int deltaZ =
                    std::abs(
                        static_cast<int>(
                            destinationCell.second)
                        - zValue);
                return 10
                        * std::max(
                            deltaX,
                            deltaZ)
                    + 4
                        * std::min(
                            deltaX,
                            deltaZ);
            };

        scores[startIndex] = 0;
        open.push({
            startIndex,
            heuristic(
                static_cast<int>(
                    startCell.first),
                static_cast<int>(
                    startCell.second))
        });
        // 直進と斜めの8近傍の座標差
        constexpr std::array<
            std::pair<int, int>,
            8> Directions{
                std::pair{ -1, 0 },
                std::pair{ 1, 0 },
                std::pair{ 0, -1 },
                std::pair{ 0, 1 },
                std::pair{ -1, -1 },
                std::pair{ 1, -1 },
                std::pair{ -1, 1 },
                std::pair{ 1, 1 }
            };

        while (!open.empty())
        {
            // 最小スコアで取り出した探索候補
            const auto current =
                open.top();
            open.pop();
            if (closed[current.index])
            {
                continue;
            }
            closed[current.index] = 1;
            if (current.index
                == destinationIndex)
            {
                break;
            }

            // 探索中のセルX座標
            const int currentX =
                static_cast<int>(
                    current.index % width);
            // 探索中のセルZ座標
            const int currentZ =
                static_cast<int>(
                    current.index / width);
            // 隣接セルを調べる(offsetX: X方向の座標差, offsetZ: Z方向の座標差)。
            for (const auto& [offsetX, offsetZ] :
                Directions)
            {
                // 隣接候補のセルX座標
                const int nextX =
                    currentX + offsetX;
                // 隣接候補のセルZ座標
                const int nextZ =
                    currentZ + offsetZ;
                if (nextX < 0
                    || nextZ < 0
                    || nextX
                        >= static_cast<int>(
                            width)
                    || nextZ
                        >= static_cast<int>(
                            GridDepth())
                    || IsBlocked(
                        static_cast<
                            std::uint32_t>(
                                nextX),
                        static_cast<
                            std::uint32_t>(
                                nextZ)))
                {
                    continue;
                }
                if (offsetX != 0
                    && offsetZ != 0
                    && (IsBlocked(
                            static_cast<
                                std::uint32_t>(
                                    currentX
                                    + offsetX),
                            static_cast<
                                std::uint32_t>(
                                    currentZ))
                        || IsBlocked(
                            static_cast<
                                std::uint32_t>(
                                    currentX),
                            static_cast<
                                std::uint32_t>(
                                    currentZ
                                    + offsetZ))))
                {
                    continue;
                }

                // 隣接候補の配列添字
                const auto nextIndex =
                    CellIndex(
                        static_cast<
                            std::uint32_t>(
                                nextX),
                        static_cast<
                            std::uint32_t>(
                                nextZ));
                // 始点から隣接候補へのコスト
                const int candidateScore =
                    scores[current.index]
                    + (offsetX != 0
                        && offsetZ != 0
                        ? 14
                        : 10);
                if (candidateScore
                    >= scores[nextIndex])
                {
                    continue;
                }
                scores[nextIndex] =
                    candidateScore;
                parents[nextIndex] =
                    current.index;
                open.push({
                    nextIndex,
                    candidateScore
                        + heuristic(
                            nextX,
                            nextZ)
                });
            }
        }

        if (destinationIndex
                != startIndex
            && parents[destinationIndex]
                == std::numeric_limits<
                    std::size_t>::max())
        {
            return {};
        }

        // 始点から目的地へのセル経路
        std::vector<CellCoordinate> cells;
        // 経路を逆にたどるセルの配列添字
        for (std::size_t current =
                destinationIndex;;
            current = parents[current])
        {
            cells.emplace_back(
                static_cast<std::uint32_t>(
                    current % width),
                static_cast<std::uint32_t>(
                    current / width));
            if (current == startIndex)
            {
                break;
            }
        }
        std::ranges::reverse(cells);

        // 通行可の直線で結べる最遠の中継セルへ飛ばして経路を平滑化する。
        // 直線で中継を省いたセル経路
        std::vector<CellCoordinate> pulled;
        pulled.reserve(cells.size());
        pulled.push_back(cells.front());
        // 平滑化する線分の始点番号
        std::size_t anchor = 0;
        while (anchor + 1 < cells.size())
        {
            // 直線で到達できる最遠点の番号
            std::size_t farthest = anchor + 1;
            // 最遠から試す中継点の番号
            for (std::size_t candidate =
                    cells.size() - 1;
                candidate > anchor + 1;
                --candidate)
            {
                if (HasLineOfSight(
                        cells[anchor],
                        cells[candidate]))
                {
                    farthest = candidate;
                    break;
                }
            }
            pulled.push_back(cells[farthest]);
            anchor = farthest;
        }

        // 返すワールド座標の経路
        std::vector<DirectX::XMFLOAT3> path;
        path.reserve(pulled.size() + 1);
        path.push_back(start);
        // 両端はセル中心へ補正せず、入力された始点と目的地をそのまま返す。
        // 経路に追加する中継点の番号
        for (std::size_t index = 1;
            index + 1 < pulled.size();
            ++index)
        {
            path.push_back(
                CellCenter(
                    pulled[index].first,
                    pulled[index].second));
        }
        path.push_back(destination);
        return path;
    }

    bool NavMeshComponent::HasLineOfSight(
        const CellCoordinate& from,
        const CellCoordinate& to) const noexcept
    {

        // 符号付き座標の範囲外も通行禁止として判定する(x: セルのX座標, z: セルのZ座標)。
        const auto isBlockedSafe =
            [this](const int x, const int z) noexcept
        {
            return x < 0
                || z < 0
                || x >= static_cast<int>(GridWidth())
                || z >= static_cast<int>(GridDepth())
                || IsBlocked(
                    static_cast<std::uint32_t>(x),
                    static_cast<std::uint32_t>(z));
        };

        // 対象セルのX座標
        int x = static_cast<int>(from.first);
        // 対象セルのZ座標
        int z = static_cast<int>(from.second);
        // 線分終点のセルX座標
        const int targetX = static_cast<int>(to.first);
        // 線分終点のセルZ座標
        const int targetZ = static_cast<int>(to.second);
        // 対象セル間のX座標差
        const float deltaX =
            static_cast<float>(targetX - x);
        // 対象セル間のZ座標差
        const float deltaZ =
            static_cast<float>(targetZ - z);
        // 線分が進むX方向の符号
        const int stepX =
            deltaX > 0.0f ? 1 : (deltaX < 0.0f ? -1 : 0);
        // 線分が進むZ方向の符号
        const int stepZ =
            deltaZ > 0.0f ? 1 : (deltaZ < 0.0f ? -1 : 0);
        // 進まない軸の境界時刻代替値
        const float infinity =
            std::numeric_limits<float>::max();
        // セル中心から最初の境界まで半セルとして線分の通過順を計算する。
        // 次のX境界までの線分内の割合
        float tMaxX = stepX != 0
            ? 0.5f / std::abs(deltaX)
            : infinity;
        // 次のZ境界までの線分内の割合
        float tMaxZ = stepZ != 0
            ? 0.5f / std::abs(deltaZ)
            : infinity;
        // X境界一つ分の線分内の割合
        const float tDeltaX = stepX != 0
            ? 1.0f / std::abs(deltaX)
            : infinity;
        // Z境界一つ分の線分内の割合
        const float tDeltaZ = stepZ != 0
            ? 1.0f / std::abs(deltaZ)
            : infinity;

        // 角通過とみなす割合差の許容値
        constexpr float cornerEpsilon = 0.000001f;
        while (x != targetX || z != targetZ)
        {
            if (stepX != 0
                && stepZ != 0
                && std::abs(tMaxX - tMaxZ)
                    < cornerEpsilon)
            {
                // 角を通る線分も斜め移動と同じく両隣の通行可を必要とする。
                if (isBlockedSafe(x + stepX, z)
                    || isBlockedSafe(x, z + stepZ))
                {
                    return false;
                }
                x += stepX;
                z += stepZ;
                tMaxX += tDeltaX;
                tMaxZ += tDeltaZ;
            }
            else if (tMaxX < tMaxZ)
            {
                x += stepX;
                tMaxX += tDeltaX;
            }
            else
            {
                z += stepZ;
                tMaxZ += tDeltaZ;
            }
            if (isBlockedSafe(x, z))
            {
                return false;
            }
        }
        return true;
    }

    bool NavMeshComponent::ContainsPoint(
        const DirectX::XMFLOAT3& point) const noexcept
    {
        // 対象セルのX座標
        int x{};
        // 対象セルのZ座標
        int z{};
        return WorldToCell(point, x, z);
    }

    float NavMeshComponent::SurfaceHeight() const noexcept
    {
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        return world._42;
    }

    void NavMeshComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // グリッドのX方向のセル数
        const auto width = GridWidth();
        // グリッドのZ方向のセル数
        const auto depth = GridDepth();
        // 通行可セルの枠線頂点
        std::vector<DirectX::XMFLOAT3>
            walkableLines;
        // 通行禁止セルの枠線頂点
        std::vector<DirectX::XMFLOAT3>
            blockedLines;
        walkableLines.reserve(
            static_cast<std::size_t>(
                width) * depth * 8);
        blockedLines.reserve(
            walkableLines.capacity());

        // 対象セルのZ座標
        for (std::uint32_t z{};
            // グリッドのZ方向のセル数
            z < depth;
            ++z)
        {
            // 対象セルのX座標
            for (std::uint32_t x{};
                // グリッドのX方向のセル数
                x < width;
                ++x)
            {
                // セル中心のワールド座標
                const auto center =
                    CellCenter(x, z);
                // 描くセル枠の半幅
                const float half =
                    m_cellSize * 0.47f;
                // セル枠を描くワールド高さ
                const float y =
                    center.y + 0.025f;
                // 通行可否に対応する線の格納先
                auto& lines =
                    IsBlocked(x, z)
                    ? blockedLines
                    : walkableLines;
                // セル枠のワールド座標の四隅
                const DirectX::XMFLOAT3
                    corners[]{
                        {
                            center.x - half,
                            y,
                            center.z - half
                        },
                        {
                            center.x + half,
                            y,
                            center.z - half
                        },
                        {
                            center.x + half,
                            y,
                            center.z + half
                        },
                        {
                            center.x - half,
                            y,
                            center.z + half
                        }
                    };
                // セル枠の辺番号
                for (std::size_t edge{};
                    edge < 4;
                    ++edge)
                {
                    lines.push_back(
                        corners[edge]);
                    lines.push_back(
                        corners[
                            (edge + 1) % 4]);
                }
            }
        }

        graphics.Debug().DrawLines(
            walkableLines,
            DirectX::XMVectorSet(
                0.08f,
                0.72f,
                1.0f,
                0.75f),
            view,
            projection);
        graphics.Debug().DrawLines(
            blockedLines,
            DirectX::XMVectorSet(
                1.0f,
                0.20f,
                0.12f,
                0.85f),
            view,
            projection);
    }
}
