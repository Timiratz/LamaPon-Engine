#include "LamaPon/Editor/MemoryProfilerPanel.h"

#include "LamaPon/Core/PathUtils.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 比較枠の表示名
        constexpr std::array<const char*, 2> SlotNames{
            "A（前）",
            "B（後）"
        };

        enum EntryColumn : ImGuiID
        {
            ColumnCategory,
            ColumnName,
            ColumnDetail,
            ColumnGpu,
            ColumnCpu,
            ColumnTotal
        };

        // 静的に保持された分類名を返します(category: メモリ分類)。
        [[nodiscard]] const char* CategoryLabel(
            const MemoryCategory category)
        {
            // MemoryCategoryNameは静的な文字列を返すため、c_strの代わりにdataをそのまま使えます。
            return MemoryCategoryName(category).data();
        }

        // 大文字小文字を区別して部分一致を判定します(filter: 検索文字列・空なら全件, name: 検索対象の資源名)。
        [[nodiscard]] bool MatchesFilter(
            const std::string& filter,
            const std::string& name)
        {
            return filter.empty()
                || name.find(filter) != std::string::npos;
        }

        // 全資源量に対する割合を描画します(value: 分類の資源量・byte, total: 全分類の資源量・byte)。
        void DrawShareBar(const std::uint64_t value, const std::uint64_t total)
        {
            // 全資源量に対する比率
            const float fraction = total == 0
                ? 0.0f
                : static_cast<float>(
                    static_cast<double>(value)
                    / static_cast<double>(total));
            ImGui::ProgressBar(
                fraction,
                ImVec2{ -FLT_MIN, 0.0f },
                FormatMemoryBytes(value).c_str());
        }

        // プロセスと取得可能なVRAMの量を表示します(process: プロセスのメモリ統計)。
        void DrawProcessTotals(const MemoryProcessTotals& process)
        {
            ImGui::Text(
                "プロセス: working set %s  private %s",
                FormatMemoryBytes(process.processWorkingSetBytes).c_str(),
                FormatMemoryBytes(process.processPrivateBytes).c_str());
            if (process.videoMemoryAvailable)
            {
                ImGui::Text(
                    "VRAM: local %s  non-local %s",
                    FormatMemoryBytes(
                        process.localVideoMemoryUsageBytes).c_str(),
                    FormatMemoryBytes(
                        process.nonLocalVideoMemoryUsageBytes).c_str());
            }
        }
    }

    // 取得・通知・ファイル選択処理を保持します。
    MemoryProfilerPanel::MemoryProfilerPanel(
        CaptureFunction capture,
        StatusSink status,
        DebugCaptureFiles::OpenFileDialog openFile)
        : m_capture(std::move(capture))
        , m_status(std::move(status))
        , m_openFile(std::move(openFile))
    {
    }

    // 登録された状態通知へ内容を渡します。
    void MemoryProfilerPanel::SetStatus(
        std::string message,
        const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    // 取得結果と比較枠を更新します。
    void MemoryProfilerPanel::TakeSnapshot()
    {
        if (!m_capture)
        {
            return;
        }
        // 新たに取得したメモリ内訳
        auto snapshot = m_capture();
        snapshot.capturedAt = DebugCaptureFiles::LocalTimestamp();
        snapshot.label = m_label.empty()
            ? snapshot.capturedAt
            : m_label;
        // 新しい取得結果をB、直前の現在をAへ移して比較を更新します。
        if (m_current)
        {
            m_slots[0].snapshot = std::move(m_current);
        }
        m_current = std::move(snapshot);
        m_slots[1].snapshot = m_current;
        m_comparisonDirty = true;
    }

    // 現在の取得結果を重複しない保存パスへ書き出します。
    bool MemoryProfilerPanel::SaveCurrent(
        const std::filesystem::path& captureDirectory)
    {
        if (!m_current || captureDirectory.empty())
        {
            return false;
        }
        if (!DebugCaptureFiles::EnsureCaptureDirectory(captureDirectory))
        {
            SetStatus(
                "メモリスナップショットの保存先を作成できませんでした",
                true);
            return false;
        }
        // 保存先または選択ファイル
        const auto path = DebugCaptureFiles::UniqueCapturePath(
            captureDirectory,
            "memory",
            ".json");
        if (!WriteMemorySnapshotJson(path, *m_current))
        {
            SetStatus("メモリスナップショットを保存できませんでした", true);
            return false;
        }
        SetStatus(
            "メモリスナップショットを保存しました: " + PathToUtf8(path));
        return true;
    }

    // JSONの読込に成功した場合だけ比較枠を更新します。
    void MemoryProfilerPanel::LoadSlot(
        const std::size_t slot,
        const std::filesystem::path& path)
    {
        // 読込先の取得結果
        MemorySnapshot snapshot;
        // 読込の失敗理由
        std::string error;
        if (!LoadMemorySnapshotJson(path, snapshot, &error))
        {
            SetStatus(
                "メモリスナップショットを読み込めませんでした: "
                    + PathToUtf8(path.filename()) + " (" + error + ")",
                true);
            return;
        }
        if (snapshot.label.empty())
        {
            snapshot.label = PathToUtf8(path.filename());
        }
        m_slots[slot].snapshot = std::move(snapshot);
        m_comparisonDirty = true;
    }

    // 資源別の内訳と比較タブを描画します。
    void MemoryProfilerPanel::Draw(
        const char* const title,
        bool& open,
        const std::filesystem::path& captureDirectory)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 860.0f, 620.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        DrawToolbar(captureDirectory);
        if (ImGui::BeginTabBar("MemoryProfilerTabs"))
        {
            if (ImGui::BeginTabItem("内訳"))
            {
                DrawCurrentView();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("比較"))
            {
                DrawComparisonView(captureDirectory);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // 取得名の入力と取得・保存操作を描画します。
    void MemoryProfilerPanel::DrawToolbar(
        const std::filesystem::path& captureDirectory)
    {
        if (ImGui::Button("スナップショットを取る"))
        {
            TakeSnapshot();
        }
        ImGui::SetItemTooltip(
            "現在読み込まれている資源の内訳を記録します。"
            "取り直すと直前の記録と比較できます。");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        // 取得名の入力バッファ
        char label[64]{};
        m_label.copy(label, sizeof(label) - 1);
        if (ImGui::InputTextWithHint(
                "##MemoryLabel",
                "名前（例: ステージ1読込後）",
                label,
                sizeof(label)))
        {
            m_label = label;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_current || captureDirectory.empty());
        if (ImGui::Button("保存"))
        {
            static_cast<void>(SaveCurrent(captureDirectory));
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "現在のスナップショットを.lamapon/memoryへJSONで保存します。");
    }

    // 資源量を集計し、分類と名前による絞り込みを表示します。
    void MemoryProfilerPanel::DrawCurrentView()
    {
        if (!m_current)
        {
            ImGui::TextDisabled(
                "「スナップショットを取る」で、テクスチャ・モデル・音声などの"
                "内訳を記録します。");
            return;
        }
        // 現在の取得結果
        const auto& snapshot = *m_current;
        ImGui::Text(
            "%s（%s）",
            snapshot.label.c_str(),
            snapshot.capturedAt.c_str());
        DrawProcessTotals(snapshot.process);

        // 分類別の集計結果
        const auto totals = SummarizeMemorySnapshot(snapshot);
        // 全資源のCPUとGPU合計・byte
        std::uint64_t grandTotal{};
        // 分類別の資源量
        for (const auto& total : totals)
        {
            grandTotal += total.gpuBytes + total.cpuBytes;
        }

        ImGui::SeparatorText("分類ごとの合計");
        if (ImGui::BeginTable(
                "MemoryCategories",
                5,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn(
                "分類",
                ImGuiTableColumnFlags_WidthFixed,
                130.0f);
            ImGui::TableSetupColumn(
                "件数",
                ImGuiTableColumnFlags_WidthFixed,
                50.0f);
            ImGui::TableSetupColumn(
                "GPU",
                ImGuiTableColumnFlags_WidthFixed,
                90.0f);
            ImGui::TableSetupColumn(
                "CPU",
                ImGuiTableColumnFlags_WidthFixed,
                90.0f);
            ImGui::TableSetupColumn(
                "合計（割合）",
                ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            // メモリ分類の添字
            for (std::size_t index{}; index < totals.size(); ++index)
            {
                // 分類別の資源量
                const auto& total = totals[index];
                if (total.count == 0)
                {
                    continue;
                }
                // 表示するメモリ分類
                const auto category = static_cast<MemoryCategory>(index);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                // 分類名を押すと下の一覧をその分類に絞り込みます。
                if (ImGui::Selectable(
                        CategoryLabel(category),
                        m_categoryFilter == static_cast<int>(index),
                        ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_categoryFilter =
                        m_categoryFilter == static_cast<int>(index)
                            ? -1
                            : static_cast<int>(index);
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", total.count);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(
                    FormatMemoryBytes(total.gpuBytes).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    FormatMemoryBytes(total.cpuBytes).c_str());
                ImGui::TableSetColumnIndex(4);
                DrawShareBar(total.gpuBytes + total.cpuBytes, grandTotal);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled(
            "GPU量は形式と寸法からの見積もりです。"
            "ドライバーの配置による余白は含みません。");

        ImGui::SeparatorText("資源");
        ImGui::SetNextItemWidth(220.0f);
        // 資源名の入力バッファ
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##MemoryFilter",
                "名前で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }
        if (m_categoryFilter >= 0)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("分類の絞り込みを解除"))
            {
                m_categoryFilter = -1;
            }
        }

        // 絞り込み後の資源への借用参照
        std::vector<const MemorySnapshotEntry*> rows;
        rows.reserve(snapshot.entries.size());
        // 表示する資源の内訳
        for (const auto& entry : snapshot.entries)
        {
            if ((m_categoryFilter < 0
                    || static_cast<int>(entry.category)
                        == m_categoryFilter)
                && MatchesFilter(m_filter, entry.name))
            {
                rows.push_back(&entry);
            }
        }

        if (!ImGui::BeginTable(
                "MemoryEntries",
                6,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY
                    | ImGuiTableFlags_Sortable,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "分類",
            ImGuiTableColumnFlags_WidthFixed,
            110.0f,
            ColumnCategory);
        ImGui::TableSetupColumn(
            "名前",
            ImGuiTableColumnFlags_WidthStretch,
            0.0f,
            ColumnName);
        ImGui::TableSetupColumn(
            "詳細",
            ImGuiTableColumnFlags_WidthFixed,
            180.0f,
            ColumnDetail);
        ImGui::TableSetupColumn(
            "GPU",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnGpu);
        ImGui::TableSetupColumn(
            "CPU",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnCpu);
        ImGui::TableSetupColumn(
            "合計",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_DefaultSort
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnTotal);
        ImGui::TableHeadersRow();

        // 表で指定された並べ替え条件
        if (const auto* specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0)
        {
            // 先頭列の並べ替え条件
            const auto& spec = specs->Specs[0];
            // 昇順で並べるか
            const bool ascending =
                spec.SortDirection == ImGuiSortDirection_Ascending;
            // 先頭の指定列で資源を安定整列します(left: 左の資源, right: 右の資源)。
            std::ranges::stable_sort(
                rows,
                [&spec, ascending](
                    const MemorySnapshotEntry* left,
                    const MemorySnapshotEntry* right)
                {
                    // 指定方向で値を比較します(a: 左の比較値, b: 右の比較値)。
                    const auto compare = [ascending](
                        const auto& a,
                        const auto& b)
                    {
                        return ascending ? a < b : b < a;
                    };
                    // 指定列に対応する比較値を選びます。
                    switch (spec.ColumnUserID)
                    {
                    case ColumnCategory:
                        return compare(left->category, right->category);
                    case ColumnName:
                        return compare(left->name, right->name);
                    case ColumnDetail:
                        return compare(left->detail, right->detail);
                    case ColumnGpu:
                        return compare(left->gpuBytes, right->gpuBytes);
                    case ColumnCpu:
                        return compare(left->cpuBytes, right->cpuBytes);
                    default:
                        return compare(
                            left->TotalBytes(),
                            right->TotalBytes());
                    }
                });
        }

        // 可視行だけを描画する範囲
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step())
        {
            // 表示対象の行添字
            for (int row = clipper.DisplayStart;
                row < clipper.DisplayEnd;
                ++row)
            {
                // 表示する資源の内訳
                const auto& entry = *rows[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(CategoryLabel(entry.category));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::SetItemTooltip("%s", entry.name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", entry.detail.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    FormatMemoryBytes(entry.gpuBytes).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(
                    FormatMemoryBytes(entry.cpuBytes).c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(
                    FormatMemoryBytes(entry.TotalBytes()).c_str());
            }
        }
        ImGui::EndTable();
    }

    // 現在または保存済みの取得結果を比較枠へ設定します。
    void MemoryProfilerPanel::DrawSlotSelector(
        const std::size_t slot,
        const std::filesystem::path& captureDirectory)
    {
        // 選択中の比較枠
        auto& target = m_slots[slot];
        ImGui::PushID(static_cast<int>(slot));
        ImGui::Text(
            "%s: %s",
            SlotNames[slot],
            target.snapshot
                ? target.snapshot->label.c_str()
                : "（未選択）");
        ImGui::BeginDisabled(!m_current);
        if (ImGui::SmallButton("現在を使う"))
        {
            target.snapshot = m_current;
            m_comparisonDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::BeginCombo(
                "##SavedSnapshots",
                "保存済み",
                ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
            {
                m_captureFiles = DebugCaptureFiles::ListCaptures(
                    captureDirectory,
                    ".json");
            }
            if (m_captureFiles.empty())
            {
                ImGui::TextDisabled("保存したスナップショットがありません。");
            }
            // 保存済み取得結果のパス
            for (const auto& file : m_captureFiles)
            {
                // 保存ファイルの表示名
                const auto name = PathToUtf8(file.filename());
                if (ImGui::Selectable(name.c_str()))
                {
                    LoadSlot(slot, file);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_openFile);
        if (ImGui::SmallButton("参照..."))
        {
            // 保存先または選択ファイル
            if (const auto path = m_openFile(captureDirectory))
            {
                LoadSlot(slot, *path);
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // 枠が変わった場合だけ再計算し、AからBへの増減を表示します。
    void MemoryProfilerPanel::DrawComparisonView(
        const std::filesystem::path& captureDirectory)
    {
        if (ImGui::BeginTable(
                "MemoryComparisonSlots",
                2,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_SizingStretchSame))
        {
            // 比較枠の添字・0はA、1はB
            for (std::size_t slot{}; slot < m_slots.size(); ++slot)
            {
                ImGui::TableNextColumn();
                DrawSlotSelector(slot, captureDirectory);
            }
            ImGui::EndTable();
        }

        if (!m_slots[0].snapshot || !m_slots[1].snapshot)
        {
            ImGui::TextDisabled(
                "AとBにスナップショットを選ぶと、増えた資源と減った資源を"
                "一覧にします。");
            return;
        }
        if (m_comparisonDirty || !m_comparison)
        {
            m_comparison = CompareMemorySnapshots(
                *m_slots[0].snapshot,
                *m_slots[1].snapshot);
            m_comparisonDirty = false;
        }
        // 再計算済みの比較結果
        const auto& comparison = *m_comparison;
        ImGui::Text(
            "プロセス private %s  working set %s  VRAM %s",
            FormatMemoryDelta(comparison.processPrivateDelta).c_str(),
            FormatMemoryDelta(comparison.processWorkingSetDelta).c_str(),
            FormatMemoryDelta(comparison.localVideoMemoryDelta).c_str());

        if (ImGui::BeginTable(
                "MemoryCategoryDelta",
                4,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("分類");
            ImGui::TableSetupColumn("A");
            ImGui::TableSetupColumn("B");
            ImGui::TableSetupColumn("差");
            ImGui::TableHeadersRow();
            // メモリ分類の添字
            for (std::size_t index{};
                index < comparison.before.size();
                ++index)
            {
                // 変更前の分類別資源量
                const auto& before = comparison.before[index];
                // 変更後の分類別資源量
                const auto& after = comparison.after[index];
                if (before.count == 0 && after.count == 0)
                {
                    continue;
                }
                // 変更前のCPUとGPU合計・byte
                const auto beforeBytes = before.gpuBytes + before.cpuBytes;
                // 変更後のCPUとGPU合計・byte
                const auto afterBytes = after.gpuBytes + after.cpuBytes;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(
                    CategoryLabel(static_cast<MemoryCategory>(index)));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text(
                    "%s（%zu件）",
                    FormatMemoryBytes(beforeBytes).c_str(),
                    before.count);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text(
                    "%s（%zu件）",
                    FormatMemoryBytes(afterBytes).c_str(),
                    after.count);
                ImGui::TableSetColumnIndex(3);
                // 変更後から変更前を引いた量・byte
                const auto delta =
                    static_cast<std::int64_t>(afterBytes)
                    - static_cast<std::int64_t>(beforeBytes);
                ImGui::TextColored(
                    delta > 0
                        ? ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f }
                        : (delta < 0
                            ? ImVec4{ 0.5f, 0.9f, 0.6f, 1.0f }
                            : ImGui::GetStyleColorVec4(
                                ImGuiCol_TextDisabled)),
                    "%s",
                    FormatMemoryDelta(delta).c_str());
            }
            ImGui::EndTable();
        }

        ImGui::SetNextItemWidth(220.0f);
        // 資源名の入力バッファ
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##MemoryDiffFilter",
                "名前で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }

        if (!ImGui::BeginTable(
                "MemoryDifferences",
                5,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "変化",
            ImGuiTableColumnFlags_WidthFixed,
            50.0f);
        ImGui::TableSetupColumn(
            "分類",
            ImGuiTableColumnFlags_WidthFixed,
            110.0f);
        ImGui::TableSetupColumn(
            "名前",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "A → B",
            ImGuiTableColumnFlags_WidthFixed,
            170.0f);
        ImGui::TableSetupColumn(
            "差",
            ImGuiTableColumnFlags_WidthFixed,
            90.0f);
        ImGui::TableHeadersRow();
        // 表示する資源の増減
        for (const auto& entry : comparison.entries)
        {
            if (!MatchesFilter(m_filter, entry.name))
            {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // 資源の追加・解放・変更を区別して表示します。
            switch (entry.change)
            {
            case MemoryEntryChange::Added:
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.6f, 0.4f, 1.0f },
                    "追加");
                break;
            case MemoryEntryChange::Removed:
                ImGui::TextColored(
                    ImVec4{ 0.5f, 0.9f, 0.6f, 1.0f },
                    "解放");
                break;
            case MemoryEntryChange::Changed:
                ImGui::TextUnformatted("変化");
                break;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(CategoryLabel(entry.category));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::SetItemTooltip(
                "%s\n%s",
                entry.name.c_str(),
                entry.detail.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text(
                "%s → %s",
                FormatMemoryBytes(entry.beforeBytes).c_str(),
                FormatMemoryBytes(entry.afterBytes).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(
                FormatMemoryDelta(entry.DeltaBytes()).c_str());
        }
        ImGui::EndTable();
    }
}
