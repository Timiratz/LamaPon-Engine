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
    constexpr std::uint32_t MaximumGridAxis = 128;

    struct OpenNode final
    {
        std::size_t index{};
        int score{};

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
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const float originX =
            world._41
            - static_cast<float>(
                GridWidth())
                * m_cellSize
                * 0.5f;
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
        const auto width = GridWidth();
        const auto depth = GridDepth();
        m_blocked.assign(
            static_cast<std::size_t>(width)
                * depth,
            0);

        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const float surfaceY = world._42;
        for (std::uint32_t z{};
            z < depth;
            ++z)
        {
            for (std::uint32_t x{};
                x < width;
                ++x)
            {
                const auto center =
                    CellCenter(x, z);
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
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const float originX =
            world._41
            - static_cast<float>(
                GridWidth())
                * m_cellSize
                * 0.5f;
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
        std::uint32_t bestX{};
        std::uint32_t bestZ{};
        int bestDistance =
            std::numeric_limits<int>::max();
        for (std::uint32_t candidateZ{};
            candidateZ < GridDepth();
            ++candidateZ)
        {
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
                const int deltaX =
                    static_cast<int>(
                        candidateX) - x;
                const int deltaZ =
                    static_cast<int>(
                        candidateZ) - z;
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

        int startX{};
        int startZ{};
        int destinationX{};
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
        const auto startCell =
            NearestWalkable(
                startX,
                startZ);
        const auto destinationCell =
            NearestWalkable(
                destinationX,
                destinationZ);

        const auto width = GridWidth();
        const auto cellCount =
            static_cast<std::size_t>(width)
            * GridDepth();
        const auto startIndex =
            CellIndex(
                startCell.first,
                startCell.second);
        const auto destinationIndex =
            CellIndex(
                destinationCell.first,
                destinationCell.second);
        std::vector<int> scores(
            cellCount,
            std::numeric_limits<int>::max());
        std::vector<std::size_t> parents(
            cellCount,
            std::numeric_limits<
                std::size_t>::max());
        std::vector<std::uint8_t> closed(
            cellCount,
            0);
        std::priority_queue<OpenNode> open;

        const auto heuristic =
            [&destinationCell](
                const int xValue,
                const int zValue)
            {
                const int deltaX =
                    std::abs(
                        static_cast<int>(
                            destinationCell.first)
                        - xValue);
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

            const int currentX =
                static_cast<int>(
                    current.index % width);
            const int currentZ =
                static_cast<int>(
                    current.index / width);
            for (const auto& [offsetX, offsetZ] :
                Directions)
            {
                const int nextX =
                    currentX + offsetX;
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

                const auto nextIndex =
                    CellIndex(
                        static_cast<
                            std::uint32_t>(
                                nextX),
                        static_cast<
                            std::uint32_t>(
                                nextZ));
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

        std::vector<CellCoordinate> cells;
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

        // 視線が通る限り中継セルを飛ばして経路を平滑化します
        // （string pulling）。グリッドA*特有のジグザグを解消します。
        std::vector<CellCoordinate> pulled;
        pulled.reserve(cells.size());
        pulled.push_back(cells.front());
        std::size_t anchor = 0;
        while (anchor + 1 < cells.size())
        {
            std::size_t farthest = anchor + 1;
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

        std::vector<DirectX::XMFLOAT3> path;
        path.reserve(pulled.size() + 1);
        path.push_back(start);
        // 終端セルの中心は実際の目的地で置き換えます。
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
        // セル中心間の線分が通過する全セルを走査します
        // （Amanatides & Wooのボクセル走査）。
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

        int x = static_cast<int>(from.first);
        int z = static_cast<int>(from.second);
        const int targetX = static_cast<int>(to.first);
        const int targetZ = static_cast<int>(to.second);
        const float deltaX =
            static_cast<float>(targetX - x);
        const float deltaZ =
            static_cast<float>(targetZ - z);
        const int stepX =
            deltaX > 0.0f ? 1 : (deltaX < 0.0f ? -1 : 0);
        const int stepZ =
            deltaZ > 0.0f ? 1 : (deltaZ < 0.0f ? -1 : 0);
        const float infinity =
            std::numeric_limits<float>::max();
        // 開始点はセル中心なので次の境界まで0.5セルです。
        float tMaxX = stepX != 0
            ? 0.5f / std::abs(deltaX)
            : infinity;
        float tMaxZ = stepZ != 0
            ? 0.5f / std::abs(deltaZ)
            : infinity;
        const float tDeltaX = stepX != 0
            ? 1.0f / std::abs(deltaX)
            : infinity;
        const float tDeltaZ = stepZ != 0
            ? 1.0f / std::abs(deltaZ)
            : infinity;

        constexpr float cornerEpsilon = 0.000001f;
        while (x != targetX || z != targetZ)
        {
            if (stepX != 0
                && stepZ != 0
                && std::abs(tMaxX - tMaxZ)
                    < cornerEpsilon)
            {
                // 角を正確に通過する場合は、斜め移動と同じく
                // 両隣のセルも通行可能である必要があります。
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
        int x{};
        int z{};
        return WorldToCell(point, x, z);
    }

    float NavMeshComponent::SurfaceHeight() const noexcept
    {
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
        const auto width = GridWidth();
        const auto depth = GridDepth();
        std::vector<DirectX::XMFLOAT3>
            walkableLines;
        std::vector<DirectX::XMFLOAT3>
            blockedLines;
        walkableLines.reserve(
            static_cast<std::size_t>(
                width) * depth * 8);
        blockedLines.reserve(
            walkableLines.capacity());

        for (std::uint32_t z{};
            z < depth;
            ++z)
        {
            for (std::uint32_t x{};
                x < width;
                ++x)
            {
                const auto center =
                    CellCenter(x, z);
                const float half =
                    m_cellSize * 0.47f;
                const float y =
                    center.y + 0.025f;
                auto& lines =
                    IsBlocked(x, z)
                    ? blockedLines
                    : walkableLines;
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
