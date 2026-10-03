#include "LamaPon/Editor/ProfileAnalyzerPanel.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Profiler.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 比較する2つの記録の表示名
        constexpr std::array<const char*, 2> SlotNames{ "A", "B" };

        // 並べ替えの対象列を識別するID。
        enum MarkerColumn : ImGuiID
        {
            ColumnName,
            ColumnMedian,
            ColumnMean,
            ColumnMaximum,
            ColumnSelfMean,
            ColumnCalls,
            ColumnFrames,
            ColumnMedianB,
            ColumnDifference,
            ColumnRelative,
            ColumnCallsB
        };

        // 記録の有効範囲内で両端を含む解析対象を借用する(dataset: 範囲指定を持つ計測記録)。
        [[nodiscard]] std::span<const ProfileFrame> RangeOf(
            const ProfileAnalyzerPanel::Dataset& dataset)
        {
            if (!dataset.IsLoaded())
            {
                return {};
            }
            // 解析範囲の0始まり開始位置
            const auto first = static_cast<std::size_t>(
                std::max(dataset.rangeFirst, 0));
            // 履歴範囲へ制限した終了位置
            const auto last = std::min(
                static_cast<std::size_t>(
                    std::max(dataset.rangeLast, 0)),
                dataset.frames.size() - 1);
            if (first > last)
            {
                return {};
            }
            return std::span<const ProfileFrame>(dataset.frames)
                .subspan(first, last - first + 1);
        }

        // 中央・平均・最小・最大・95百分位の時間を表示する(label: 記録の表示名, statistics: 表示する時間統計・ms)。
        void DrawValueStatistics(
            const char* label,
            const ProfileValueStatistics& statistics)
        {
            ImGui::Text(
                "%s 中央値 %.3f  平均 %.3f  最小 %.3f  最大 %.3f  95%% %.3f ms",
                label,
                statistics.median,
                statistics.mean,
                statistics.minimum,
                statistics.maximum,
                statistics.percentile95);
        }

        // 経路の大小文字を区別する部分一致で絞り込む(filter: 検索文字列・空なら全件, path: 検査する区間経路)。
        [[nodiscard]] bool MatchesFilter(
            const std::string& filter,
            const std::string& path)
        {
            return filter.empty()
                || path.find(filter) != std::string::npos;
        }

        // 区間が存在するフレーム当たりの平均呼出数を返す(marker: 呼出数と出現数を持つ区間統計)。
        [[nodiscard]] double CallsPerFrame(
            const ProfileMarkerStatistics& marker)
        {
            return marker.presentFrameCount == 0
                ? 0.0
                : static_cast<double>(marker.totalCalls)
                    / static_cast<double>(marker.presentFrameCount);
        }

        // 階層の深さで字下げし経路をtooltipに表示して選択を返す(path: 識別とtooltip用の経路, name: 表示する区間名, depth: 字下げする階層の深さ, selected: 当該区間を選択中か)。
        [[nodiscard]] bool DrawMarkerNameCell(
            const std::string& path,
            const std::string& name,
            const std::uint32_t depth,
            const bool selected)
        {
            ImGui::Indent(static_cast<float>(depth) * 12.0f);
            ImGui::PushID(path.c_str());
            // 区間名の行を選択したか
            const bool clicked = ImGui::Selectable(
                name.c_str(),
                selected,
                ImGuiSelectableFlags_SpanAllColumns);
            ImGui::PopID();
            ImGui::SetItemTooltip("%s", path.c_str());
            ImGui::Unindent(static_cast<float>(depth) * 12.0f);
            return clicked;
        }
    }

    ProfileAnalyzerPanel::ProfileAnalyzerPanel(
        StatusSink status,
        DebugCaptureFiles::OpenFileDialog openFile)
        : m_status(std::move(status))
        , m_openFile(std::move(openFile))
    {
    }

    void ProfileAnalyzerPanel::SetStatus(
        std::string message,
        const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    void ProfileAnalyzerPanel::SetDataset(
        const std::size_t slot,
        std::string label,
        std::vector<ProfileFrame> frames)
    {
        if (slot >= m_datasets.size() || frames.empty())
        {
            return;
        }
        // 解析範囲を編集する記録の借用
        auto& dataset = m_datasets[slot];
        dataset.label = std::move(label);
        dataset.frames = std::move(frames);
        dataset.rangeFirst = 0;
        dataset.rangeLast =
            static_cast<int>(dataset.frames.size()) - 1;
        dataset.dirty = true;
        m_comparisonDirty = true;
        m_seriesDirty = true;
    }

    bool ProfileAnalyzerPanel::LoadDataset(
        const std::size_t slot,
        const std::filesystem::path& path)
    {
        // 新規に読み込む計測フレーム一覧
        std::vector<ProfileFrame> frames;
        // 記録文書を読み込めなかった理由
        std::string error;
        if (!LoadProfileJson(path, frames, &error))
        {
            SetStatus(
                "プロファイルを読み込めませんでした: "
                    + PathToUtf8(path.filename()) + " (" + error + ")",
                true);
            return false;
        }
        if (frames.empty())
        {
            SetStatus(
                "フレームが記録されていないプロファイルです: "
                    + PathToUtf8(path.filename()),
                true);
            return false;
        }
        SetDataset(slot, PathToUtf8(path.filename()), std::move(frames));
        return true;
    }

    void ProfileAnalyzerPanel::RefreshAnalysis()
    {
        // 解析範囲を編集する記録の借用
        for (auto& dataset : m_datasets)
        {
            if (!dataset.dirty)
            {
                continue;
            }
            // 範囲指定できる最後の0始まり位置
            const int lastFrame =
                std::max(static_cast<int>(dataset.frames.size()) - 1, 0);
            dataset.rangeFirst =
                std::clamp(dataset.rangeFirst, 0, lastFrame);
            dataset.rangeLast = std::clamp(
                dataset.rangeLast,
                dataset.rangeFirst,
                lastFrame);
            dataset.analysis = AnalyzeProfile(RangeOf(dataset));
            dataset.dirty = false;
            m_comparisonDirty = true;
            m_seriesDirty = true;
        }
        if (m_comparisonDirty
            && m_datasets[0].IsLoaded()
            && m_datasets[1].IsLoaded())
        {
            m_comparison = CompareProfiles(
                m_datasets[0].analysis,
                m_datasets[1].analysis);
        }
        m_comparisonDirty = false;
    }

    void ProfileAnalyzerPanel::Draw(
        const char* const title,
        bool& open,
        const std::filesystem::path& captureDirectory)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 900.0f, 640.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        if (ImGui::BeginTable(
                "ProfileAnalyzerDatasets",
                2,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_SizingStretchSame))
        {
            // Aは0・Bは1の記録番号
            for (std::size_t slot{}; slot < m_datasets.size(); ++slot)
            {
                ImGui::TableNextColumn();
                DrawDatasetControls(slot, captureDirectory);
            }
            ImGui::EndTable();
        }
        RefreshAnalysis();

        ImGui::Separator();
        ImGui::RadioButton("単一（Aを集計）", &m_view, 0);
        ImGui::SameLine();
        ImGui::BeginDisabled(
            !m_datasets[0].IsLoaded() || !m_datasets[1].IsLoaded());
        ImGui::RadioButton("比較（A → B）", &m_view, 1);
        ImGui::EndDisabled();
        if (m_view == 1
            && (!m_datasets[0].IsLoaded() || !m_datasets[1].IsLoaded()))
        {
            m_view = 0;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        // 区間経路の検索入力バッファ
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##ProfileAnalyzerFilter",
                "経路で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }
        ImGui::SameLine();
        ImGui::Checkbox("最上位のみ", &m_topLevelOnly);

        if (!m_datasets[0].IsLoaded())
        {
            ImGui::TextDisabled(
                "Aに「現在の記録を取り込む」か、保存したプロファイルを"
                "読み込んでください。");
            ImGui::End();
            return;
        }

        DrawMarkerDistribution();
        if (m_view == 0)
        {
            DrawSingleView();
        }
        else
        {
            DrawComparisonView();
        }
        ImGui::End();
    }

    void ProfileAnalyzerPanel::DrawDatasetControls(
        const std::size_t slot,
        const std::filesystem::path& captureDirectory)
    {
        // 解析範囲を編集する記録の借用
        auto& dataset = m_datasets[slot];
        ImGui::PushID(static_cast<int>(slot));
        ImGui::Text(
            "%s: %s",
            SlotNames[slot],
            dataset.IsLoaded() ? dataset.label.c_str() : "（未読み込み）");

        if (ImGui::Button("現在の記録を取り込む"))
        {
            // 新規に読み込む計測フレーム一覧
            auto frames = Profiler::Instance().Snapshot();
            if (frames.empty())
            {
                SetStatus(
                    "取り込めるフレームがありません。"
                    "プロファイラーの「記録」を有効にしてください。",
                    true);
            }
            else
            {
                // 現在の記録の表示名の文字バッファ
                char label[64]{};
                std::snprintf(
                    label,
                    sizeof(label),
                    "現在の記録（%zuフレーム）",
                    frames.size());
                SetDataset(slot, label, std::move(frames));
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo(
                "##SavedCaptures",
                "保存済みの記録",
                ImGuiComboFlags_HeightLarge))
        {
            // 開くたびに一覧を読み直し、別の場所で保存した記録も選べます。
            if (ImGui::IsWindowAppearing())
            {
                m_captureFiles = DebugCaptureFiles::ListCaptures(
                    captureDirectory,
                    ".json");
            }
            if (m_captureFiles.empty())
            {
                ImGui::TextDisabled(
                    "プロファイラーの「記録を保存」で作成できます。");
            }
            // 選択する保存済み計測文書
            for (const auto& file : m_captureFiles)
            {
                // 保存済み計測文書の表示名
                const auto name = PathToUtf8(file.filename());
                if (ImGui::Selectable(name.c_str()))
                {
                    static_cast<void>(LoadDataset(slot, file));
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_openFile);
        if (ImGui::Button("参照..."))
        {
            // dialogで選択した計測文書のパス
            if (const auto path = m_openFile(captureDirectory))
            {
                static_cast<void>(LoadDataset(slot, *path));
            }
        }
        ImGui::EndDisabled();

        if (dataset.IsLoaded())
        {
            // 範囲指定できる最後の0始まり位置
            const int lastFrame =
                static_cast<int>(dataset.frames.size()) - 1;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragIntRange2(
                    "##Range",
                    &dataset.rangeFirst,
                    &dataset.rangeLast,
                    0.5f,
                    0,
                    lastFrame,
                    "開始 %d",
                    "終了 %d",
                    ImGuiSliderFlags_AlwaysClamp))
            {
                dataset.dirty = true;
            }
            // 両端を含む解析用フレームの借用
            const auto range = RangeOf(dataset);
            if (!range.empty())
            {
                ImGui::TextDisabled(
                    "フレーム #%llu 〜 #%llu（%zuフレーム）",
                    static_cast<unsigned long long>(range.front().index),
                    static_cast<unsigned long long>(range.back().index),
                    range.size());
            }
            DrawValueStatistics(
                "フレーム",
                dataset.analysis.frameMilliseconds);
        }
        ImGui::PopID();
    }

    void ProfileAnalyzerPanel::DrawMarkerDistribution()
    {
        if (m_selectedPath.empty())
        {
            ImGui::TextDisabled(
                "表の区間を選ぶと、フレームごとの時間をグラフで表示します。");
            return;
        }
        if (m_seriesDirty || m_seriesPath != m_selectedPath)
        {
            // Aは0・Bは1の記録番号
            for (std::size_t slot{}; slot < m_datasets.size(); ++slot)
            {
                // 区間のフレームごとの時間・ms
                const auto values = MarkerMillisecondsPerFrame(
                    RangeOf(m_datasets[slot]),
                    m_selectedPath);
                m_series[slot].assign(values.begin(), values.end());
            }
            m_seriesPath = m_selectedPath;
            m_seriesDirty = false;
        }

        // AとBの時間グラフの縦軸上限・ms
        float maximum = 0.0f;
        // AまたはBのフレームごとの時間
        for (const auto& series : m_series)
        {
            // 縦軸の最大値を調べる時間・ms
            for (const float value : series)
            {
                maximum = std::max(maximum, value);
            }
        }
        maximum = std::max(maximum * 1.1f, 0.01f);
        ImGui::Text("%s", m_selectedPath.c_str());
        // Aは0・Bは1の記録番号
        for (std::size_t slot{}; slot < m_series.size(); ++slot)
        {
            if (m_series[slot].empty())
            {
                continue;
            }
            ImGui::PlotLines(
                SlotNames[slot],
                m_series[slot].data(),
                static_cast<int>(m_series[slot].size()),
                0,
                nullptr,
                0.0f,
                maximum,
                ImVec2{ ImGui::GetContentRegionAvail().x - 24.0f, 48.0f });
        }
    }

    void ProfileAnalyzerPanel::DrawSingleView()
    {
        // 記録Aの集計結果の借用
        const auto& analysis = m_datasets[0].analysis;
        // 検索条件に合う区間の参照一覧
        std::vector<const ProfileMarkerStatistics*> rows;
        rows.reserve(analysis.markers.size());
        // 表示または比較する計測区間
        for (const auto& marker : analysis.markers)
        {
            if ((!m_topLevelOnly || marker.depth == 0)
                && MatchesFilter(m_filter, marker.path))
            {
                rows.push_back(&marker);
            }
        }

        if (!ImGui::BeginTable(
                "ProfileAnalyzerMarkers",
                7,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY
                    | ImGuiTableFlags_Sortable
                    | ImGuiTableFlags_SortTristate,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "区間",
            ImGuiTableColumnFlags_WidthStretch
                | ImGuiTableColumnFlags_NoSortDescending,
            0.0f,
            ColumnName);
        ImGui::TableSetupColumn(
            "中央値 ms",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnMedian);
        ImGui::TableSetupColumn(
            "平均 ms",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnMean);
        ImGui::TableSetupColumn(
            "最大 ms",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            80.0f,
            ColumnMaximum);
        ImGui::TableSetupColumn(
            "自己平均 ms",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            90.0f,
            ColumnSelfMean);
        ImGui::TableSetupColumn(
            "呼出/フレーム",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            90.0f,
            ColumnCalls);
        ImGui::TableSetupColumn(
            "出現",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            60.0f,
            ColumnFrames);
        ImGui::TableHeadersRow();

        // 並べ替え指定が無い（三状態の解除）ときは、親の直後に子が並ぶ解析順のまま表示します。
        // 表に指定された並べ替え条件
        if (const auto* specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0)
        {
            // 最優先の並べ替え対象列
            const auto& spec = specs->Specs[0];
            // 並べ替えを昇順にするか
            const bool ascending =
                spec.SortDirection == ImGuiSortDirection_Ascending;
            // 指定列の区間統計を返す(marker: 比較する区間の統計)。
            const auto value =
                [&spec](const ProfileMarkerStatistics& marker)
                {
                    // 選択列に対応する比較値を返す。
                    switch (spec.ColumnUserID)
                    {
                    case ColumnMedian:
                        return marker.milliseconds.median;
                    case ColumnMean:
                        return marker.milliseconds.mean;
                    case ColumnMaximum:
                        return marker.milliseconds.maximum;
                    case ColumnSelfMean:
                        return marker.selfMilliseconds.mean;
                    case ColumnCalls:
                        return CallsPerFrame(marker);
                    case ColumnFrames:
                        return static_cast<double>(
                            marker.presentFrameCount);
                    default:
                        return 0.0;
                    }
                };
            // 指定列の値で安定に並べ替える(left: 比較する区間の借用, right: 比較対象の区間の借用)。
            std::ranges::stable_sort(
                rows,
                [&](const ProfileMarkerStatistics* left,
                    const ProfileMarkerStatistics* right)
                {
                    if (spec.ColumnUserID == ColumnName)
                    {
                        return ascending
                            ? left->path < right->path
                            : left->path > right->path;
                    }
                    return ascending
                        ? value(*left) < value(*right)
                        : value(*left) > value(*right);
                });
        }

        // 表に並べ替えの指定があるか
        const bool sorted =
            ImGui::TableGetSortSpecs() != nullptr
            && ImGui::TableGetSortSpecs()->SpecsCount > 0;
        // 表示または比較する計測区間
        for (const auto* marker : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (DrawMarkerNameCell(
                    marker->path,
                    sorted ? marker->path : marker->name,
                    sorted ? 0u : marker->depth,
                    m_selectedPath == marker->path))
            {
                m_selectedPath = marker->path;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", marker->milliseconds.median);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", marker->milliseconds.mean);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", marker->milliseconds.maximum);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "最大のフレーム: #%llu",
                    static_cast<unsigned long long>(
                        marker->milliseconds.maximumFrameIndex));
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", marker->selfMilliseconds.mean);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f", CallsPerFrame(*marker));
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%zu", marker->presentFrameCount);
        }
        ImGui::EndTable();
    }

    void ProfileAnalyzerPanel::DrawComparisonView()
    {
        ImGui::Text(
            "フレーム中央値 %.3f → %.3f ms（%+.3f ms）  平均 %+.3f ms",
            m_comparison.a.frameMilliseconds.median,
            m_comparison.b.frameMilliseconds.median,
            m_comparison.frameMedianDifference,
            m_comparison.frameMeanDifference);

        // 検索条件に合う区間の参照一覧
        std::vector<const ProfileMarkerComparison*> rows;
        // 棒の幅の基準になる最大の時間差
        double largestDifference = 0.0;
        // 表示または比較する計測区間
        for (const auto& marker : m_comparison.markers)
        {
            if ((!m_topLevelOnly || marker.depth == 0)
                && MatchesFilter(m_filter, marker.path))
            {
                rows.push_back(&marker);
                largestDifference = std::max(
                    largestDifference,
                    std::abs(marker.medianDifference));
            }
        }

        if (!ImGui::BeginTable(
                "ProfileAnalyzerComparison",
                7,
                ImGuiTableFlags_BordersInnerV
                    | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_Resizable
                    | ImGuiTableFlags_ScrollY
                    | ImGuiTableFlags_Sortable
                    | ImGuiTableFlags_SortTristate,
                ImVec2{ 0.0f, 0.0f }))
        {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "区間",
            ImGuiTableColumnFlags_WidthStretch,
            0.0f,
            ColumnName);
        ImGui::TableSetupColumn(
            "A 中央値",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            75.0f,
            ColumnMedian);
        ImGui::TableSetupColumn(
            "B 中央値",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            75.0f,
            ColumnMedianB);
        ImGui::TableSetupColumn(
            "差 (B-A)",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            140.0f,
            ColumnDifference);
        ImGui::TableSetupColumn(
            "変化率",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            70.0f,
            ColumnRelative);
        ImGui::TableSetupColumn(
            "A 呼出",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            60.0f,
            ColumnCalls);
        ImGui::TableSetupColumn(
            "B 呼出",
            ImGuiTableColumnFlags_WidthFixed
                | ImGuiTableColumnFlags_PreferSortDescending,
            60.0f,
            ColumnCallsB);
        ImGui::TableHeadersRow();

        // 並べ替え指定が無いときは、CompareProfilesの差の大きい順です。
        // 表に指定された並べ替え条件
        if (const auto* specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0)
        {
            // 最優先の並べ替え対象列
            const auto& spec = specs->Specs[0];
            // 並べ替えを昇順にするか
            const bool ascending =
                spec.SortDirection == ImGuiSortDirection_Ascending;
            // 指定列の比較統計を返す(marker: 比較するAとBの区間統計)。
            const auto value =
                [&spec](const ProfileMarkerComparison& marker)
                {
                    // 選択列に対応する比較値を返す。
                    switch (spec.ColumnUserID)
                    {
                    case ColumnMedian:
                        return marker.medianA;
                    case ColumnMedianB:
                        return marker.medianB;
                    case ColumnDifference:
                        return marker.medianDifference;
                    case ColumnRelative:
                        return marker.medianRelativeChange;
                    case ColumnCalls:
                        return marker.callsPerFrameA;
                    case ColumnCallsB:
                        return marker.callsPerFrameB;
                    default:
                        return 0.0;
                    }
                };
            // 指定列の値で安定に並べ替える(left: 比較する区間の借用, right: 比較対象の区間の借用)。
            std::ranges::stable_sort(
                rows,
                [&](const ProfileMarkerComparison* left,
                    const ProfileMarkerComparison* right)
                {
                    if (spec.ColumnUserID == ColumnName)
                    {
                        return ascending
                            ? left->path < right->path
                            : left->path > right->path;
                    }
                    return ascending
                        ? value(*left) < value(*right)
                        : value(*left) > value(*right);
                });
        }

        // 表示または比較する計測区間
        for (const auto* marker : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (DrawMarkerNameCell(
                    marker->path,
                    marker->path,
                    0u,
                    m_selectedPath == marker->path))
            {
                m_selectedPath = marker->path;
            }
            ImGui::TableSetColumnIndex(1);
            if (marker->presentInA)
            {
                ImGui::Text("%.3f", marker->medianA);
            }
            else
            {
                ImGui::TextDisabled("なし");
            }
            ImGui::TableSetColumnIndex(2);
            if (marker->presentInB)
            {
                ImGui::Text("%.3f", marker->medianB);
            }
            else
            {
                ImGui::TextDisabled("なし");
            }

            // 時間差を棒で示しBが遅い正の差は赤、速い負の差は緑にする。
            ImGui::TableSetColumnIndex(3);
            // 差分を描く表のcellの左上座標
            const ImVec2 cell = ImGui::GetCursorScreenPos();
            // 差分cellの幅・ピクセル
            const float cellWidth = ImGui::GetContentRegionAvail().x;
            // 差分cellの高さ・ピクセル
            const float lineHeight = ImGui::GetTextLineHeight();
            if (largestDifference > 0.0)
            {
                // 最大時間差に対する比率
                const float ratio = static_cast<float>(
                    std::abs(marker->medianDifference)
                    / largestDifference);
                // 正負の差分を分ける中央x座標
                const float center = cell.x + cellWidth * 0.5f;
                // 比率に対応する差分の幅
                const float extent = ratio * cellWidth * 0.5f;
                // Bの時間がAより長くなったか
                const bool slower = marker->medianDifference > 0.0;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2{
                        slower ? center : center - extent,
                        cell.y + 2.0f },
                    ImVec2{
                        slower ? center + extent : center,
                        cell.y + lineHeight - 2.0f },
                    slower
                        ? IM_COL32(220, 90, 80, 150)
                        : IM_COL32(90, 190, 110, 150));
            }
            ImGui::Text("%+.3f", marker->medianDifference);

            ImGui::TableSetColumnIndex(4);
            if (!marker->presentInA)
            {
                ImGui::TextDisabled("新規");
            }
            else if (!marker->presentInB)
            {
                ImGui::TextDisabled("消失");
            }
            else
            {
                ImGui::Text(
                    "%+.1f%%",
                    marker->medianRelativeChange * 100.0);
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f", marker->callsPerFrameA);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.1f", marker->callsPerFrameB);
        }
        ImGui::EndTable();
    }
}
