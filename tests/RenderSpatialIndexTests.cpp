#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const char* message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition) throw std::runtime_error(message);
    }

    // 境界を含むAABBの3軸の重なりを判定する(a: 比較する左境界, b: 比較する右境界)。
    bool Overlaps(const LamaPon::Bounds3D& a, const LamaPon::Bounds3D& b)
    {
        return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x
            && a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y
            && a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
    }

    // 線形走査と比較してBVH問合せの候補欠落と入力順を検査する(index: 検査する空間Index)。
    void CheckQueries(const LamaPon::RenderSpatialIndex& index)
    {
        // 問合せを移動させるX位置の番号
        for (int sample = -5; sample < 80; ++sample)
        {
            // 問合せ境界の左端X
            const float x = static_cast<float>(sample);
            // X幅1の参照用問合せ境界
            const LamaPon::Bounds3D query{ { x, -1, -1 }, { x + 1, 1, 1 } };
            // 参照境界と重なるBVH候補を得る(bounds: 判定する枝または要素の境界)。
            const auto result = index.Query([&](const auto& bounds) { return Overlaps(bounds, query); });
            Require(result.candidates.size() == index.Entries().size(), "Candidate indices must match input order");
            // 元の入力順で比較する要素番号
            for (std::size_t entry = 0; entry < index.Entries().size(); ++entry)
            {
                // 交差する要素がBVH候補に残るか検証します。
                if (Overlaps(index.Entries()[entry].cullingBounds, query))
                {
                    Require(result.candidates[entry] != 0, "BVH omitted an intersecting renderer");
                }
            }
        }
    }
}

// BVHの初回構築・再利用・境界と順序変更・拒否・消去後の再構築を検査する。
int main()
{
    // BVH検査の例外を終了コードへ変換します。
    try
    {
        // 構築と問合せの検査対象BVH
        LamaPon::RenderSpatialIndex index;
        // 入力順と境界を変更する検査要素列
        std::vector<LamaPon::RenderSpatialIndex::Entry> entries;
        // 横一列へ生成する検査要素の番号
        for (int item = 0; item < 64; ++item)
        {
            // 検査要素の境界の左端X
            const float x = static_cast<float>(item);
            // 検査要素の半幅0.5の境界
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
        // ルートの判定を常に拒否する問合せ結果
        const auto outside = index.Query([](const auto&) { return false; });
        Require(outside.nodeTests == 1, "A rejected root must stop traversal");
        // 候補が1つも残らないことを検査する(value: 各候補の有効フラグ)。
        Require(std::ranges::none_of(outside.candidates, [](auto value) { return value != 0; }),
            "Rejected query must have no candidates");
        index.Clear();
        // 全判定を許可する空Indexの結果
        const auto empty = index.Query([](const auto&) { return true; });
        Require(empty.candidates.empty() && empty.nodeTests == 0 && index.NodeCount() == 0,
            "Clear must release the tree and borrowed entries");
        Require(!index.Update(entries), "Cleared index must rebuild on reuse");
        Require(!index.Update({}), "Removing all renderers must rebuild to empty");
        Require(index.Entries().empty() && index.NodeCount() == 0, "Empty update must clear nodes");
    }
    // 検査失敗を標準エラーへ出す(error: 捕捉した検査エラー)。
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
