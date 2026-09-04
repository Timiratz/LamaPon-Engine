#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    bool Overlaps(const LamaPon::Bounds3D& a, const LamaPon::Bounds3D& b)
    {
        return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x
            && a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y
            && a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
    }

    void CheckQueries(const LamaPon::RenderSpatialIndex& index)
    {
        // 線形走査を参照に、BVHの枝刈りで可視候補が欠落しないことを検査。
        for (int sample = -5; sample < 80; ++sample)
        {
            const float x = static_cast<float>(sample);
            const LamaPon::Bounds3D query{ { x, -1, -1 }, { x + 1, 1, 1 } };
            const auto result = index.Query([&](const auto& bounds) { return Overlaps(bounds, query); });
            Require(result.candidates.size() == index.Entries().size(), "Candidate indices must match input order");
            for (std::size_t entry = 0; entry < index.Entries().size(); ++entry)
            {
                if (Overlaps(index.Entries()[entry].cullingBounds, query))
                {
                    Require(result.candidates[entry] != 0, "BVH omitted an intersecting renderer");
                }
            }
        }
    }
}

int main()
{
    try
    {
        LamaPon::RenderSpatialIndex index;
        std::vector<LamaPon::RenderSpatialIndex::Entry> entries;
        for (int item = 0; item < 64; ++item)
        {
            const float x = static_cast<float>(item);
            const LamaPon::Bounds3D bounds{ { x, 0, 0 }, { x + 0.5f, 0.5f, 0.5f } };
            entries.push_back({ nullptr, bounds, bounds });
        }
        Require(!index.Update(entries), "First population must build the tree");
        Require(index.Update(entries), "Unchanged input must reuse the tree");
        Require(index.NodeCount() > 1, "Fixture must exercise internal nodes");
        CheckQueries(index);
        entries[0].cullingBounds = { { 70, 0, 0 }, { 71, 1, 1 } };
        Require(!index.Update(entries), "Changed culling bounds must rebuild");
        CheckQueries(index);
        std::reverse(entries.begin(), entries.end());
        Require(!index.Update(entries), "Changed input order must rebuild index mapping");
        CheckQueries(index);
        const auto outside = index.Query([](const auto&) { return false; });
        Require(outside.nodeTests == 1, "A rejected root must stop traversal");
        Require(std::ranges::none_of(outside.candidates, [](auto value) { return value != 0; }),
            "Rejected query must have no candidates");
        index.Clear();
        const auto empty = index.Query([](const auto&) { return true; });
        Require(empty.candidates.empty() && empty.nodeTests == 0 && index.NodeCount() == 0,
            "Clear must release the tree and borrowed entries");
        Require(!index.Update(entries), "Cleared index must rebuild on reuse");
        Require(!index.Update({}), "Removing all renderers must rebuild to empty");
        Require(index.Entries().empty() && index.NodeCount() == 0, "Empty update must clear nodes");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
