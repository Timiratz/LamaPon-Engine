#include "LamaPon/Editor/ProfilerPanel.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProfileAnalysis.h"
#include "LamaPon/Editor/DebugCaptureFiles.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace
    {
        // 履歴に保持するフレーム数の選択肢
        constexpr std::array<std::size_t, 5> HistoryCapacities{
            240, 600, 1200, 3000, 6000
        };

        // 区間名のhashから実行やフレームをまたいで同じ色を返す(name: 色を決める最上位区間名)。
        [[nodiscard]] ImU32 CategoryColor(const std::string_view name)
        {
            // 区間名のhashで選ぶ8色
            static constexpr std::array<ImU32, 8> palette{
                IM_COL32(86, 156, 214, 255),
                IM_COL32(106, 190, 120, 255),
                IM_COL32(230, 160, 70, 255),
                IM_COL32(200, 110, 190, 255),
                IM_COL32(220, 200, 90, 255),
                IM_COL32(90, 200, 200, 255),
                IM_COL32(230, 100, 100, 255),
                IM_COL32(150, 130, 230, 255)
            };
            // 区間名から色を決める32bit hash
            std::uint32_t hash = 2166136261u;
            // 色を決める区間名の文字
            for (const char character : name)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 16777619u;
            }
            return palette[hash % palette.size()];
        }

        // 未計測時間を表示する灰色を返す。
        [[nodiscard]] ImU32 UnmeasuredColor()
        {
            return IM_COL32(110, 110, 110, 255);
        }

        // 区間名のhashからImGui階層ノードのIDを作る(name: 階層内で識別する区間名)。
        [[nodiscard]] const void* NodeId(const std::string& name)
        {
            // 表示名の##をImGuiのID区切りとして扱わず、hashを親階層のID stackと組み合わせる。
            return reinterpret_cast<const void*>(
                std::hash<std::string>{}(name) | 1u);
        }

        // 計測区間とその子を再帰的に表示する(frame: 表示するフレーム, tree: 当該フレームの親子解析結果, index: 表示するsampleの番号, frameMilliseconds: 割合の基準となる時間・ms)。
        void DrawTreeNode(
            const ProfileFrame& frame,
            const ProfileFrameTree& tree,
            const std::uint32_t index,
            const double frameMilliseconds)
        {
            // 表示する計測区間
            const auto& sample = frame.samples[index];
            // 階層表示する子区間の番号一覧
            const auto& children = tree.children[index];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // 階層・葉に応じたTreeNode表示設定
            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_SpanAllColumns
                | ImGuiTreeNodeFlags_OpenOnArrow
                | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            if (children.empty())
            {
                flags |= ImGuiTreeNodeFlags_Leaf
                    | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            if (sample.depth == 0)
            {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            // 当該階層ノードを展開したか
            const bool open = ImGui::TreeNodeEx(
                NodeId(sample.name),
                flags,
                "%s",
                sample.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", sample.milliseconds);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", tree.selfMilliseconds[index]);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text(
                "%.1f%%",
                frameMilliseconds > 0.0
                    ? sample.milliseconds / frameMilliseconds * 100.0
                    : 0.0);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", sample.callCount);

            if (open && !children.empty())
            {
                // 再帰表示する子区間の番号
                for (const auto child : children)
                {
                    DrawTreeNode(frame, tree, child, frameMilliseconds);
                }
                ImGui::TreePop();
            }
        }
    }

    ProfilerPanel::ProfilerPanel(StatusSink status)
        : m_status(std::move(status))
    {
    }

    void ProfilerPanel::SetStatus(
        std::string message,
        const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    void ProfilerPanel::PullFrames()
    {
        // 計測データを取り込むProfiler
        auto& profiler = Profiler::Instance();
        if (profiler.LatestFrameIndex() < m_lastPulledIndex)
        {
            // Profilerの番号リセットを検出したら古い履歴を捨てて取り込み直す。
            m_history.clear();
            m_lastPulledIndex = 0;
        }
        // 新規取込または保存するフレーム
        auto frames = profiler.SnapshotSince(m_lastPulledIndex);
        // 取り込むまたは表示するフレーム
        for (auto& frame : frames)
        {
            m_lastPulledIndex = frame.index;
            m_history.push_back(std::move(frame));
        }
        TrimHistory();
    }

    void ProfilerPanel::TrimHistory() noexcept
    {
        // 表示履歴に保持する最大フレーム数
        const auto capacity =
            std::max<std::size_t>(
                Profiler::Instance().FrameCapacity(),
                1);
        while (m_history.size() > capacity)
        {
            m_history.pop_front();
        }
    }

    const ProfileFrame* ProfilerPanel::SelectedFrame() const noexcept
    {
        if (m_history.empty())
        {
            return nullptr;
        }
        if (!m_followLatest)
        {
            // 選択した計測フレームの位置
            const auto selected = std::ranges::find(
                m_history,
                m_selectedFrameIndex,
                &ProfileFrame::index);
            if (selected != m_history.end())
            {
                return &*selected;
            }
        }
        return &m_history.back();
    }

    void ProfilerPanel::SelectFrame(
        const std::uint64_t frameIndex) noexcept
    {
        m_selectedFrameIndex = frameIndex;
        m_followLatest = false;
    }

    void ProfilerPanel::StepSelection(const int offset) noexcept
    {
        // 現在選択しているフレームの借用
        const auto* current = SelectedFrame();
        if (current == nullptr)
        {
            return;
        }
        // 履歴配列の位置
        const auto position = std::distance(
            m_history.begin(),
            std::ranges::find(
                m_history,
                current->index,
                &ProfileFrame::index));
        // 履歴範囲に制限した移動先位置
        const auto target = std::clamp<std::ptrdiff_t>(
            position + offset,
            0,
            static_cast<std::ptrdiff_t>(m_history.size()) - 1);
        SelectFrame(
            m_history[static_cast<std::size_t>(target)].index);
    }

    void ProfilerPanel::FollowLatest() noexcept
    {
        m_followLatest = true;
    }

    void ProfilerPanel::ClearHistory() noexcept
    {
        Profiler::Instance().Clear();
        m_history.clear();
        m_lastPulledIndex = 0;
        m_followLatest = true;
    }

    bool ProfilerPanel::SaveHistory(
        const std::filesystem::path& captureDirectory)
    {
        if (captureDirectory.empty() || m_history.empty())
        {
            return false;
        }
        if (!DebugCaptureFiles::EnsureCaptureDirectory(captureDirectory))
        {
            SetStatus("プロファイルの保存先を作成できませんでした", true);
            return false;
        }
        // 保存する表示履歴のコピー
        const std::vector<ProfileFrame> frames(
            m_history.begin(),
            m_history.end());
        // 今回保存する計測JSONのパス
        const auto path = DebugCaptureFiles::UniqueCapturePath(
            captureDirectory,
            "profile",
            ".json");
        if (!WriteProfileJson(path, frames))
        {
            SetStatus("プロファイルを保存できませんでした", true);
            return false;
        }
        SetStatus(
            "プロファイルを保存しました: " + PathToUtf8(path));
        return true;
    }

    void ProfilerPanel::Draw(
        const char* const title,
        bool& open,
        const std::filesystem::path& captureDirectory,
        const float frameBudgetMilliseconds)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 560.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        PullFrames();
        DrawToolbar(captureDirectory);
        DrawTimeline(frameBudgetMilliseconds);
        // 取り込むまたは表示するフレーム
        if (const auto* frame = SelectedFrame())
        {
            DrawSelectedFrame(*frame);
        }
        else
        {
            ImGui::TextDisabled(
                "記録されたフレームがありません。"
                "「記録」を有効にしてください。");
        }
        ImGui::End();
    }

    void ProfilerPanel::DrawToolbar(
        const std::filesystem::path& captureDirectory)
    {
        // 計測データを取り込むProfiler
        auto& profiler = Profiler::Instance();
        // 計測の有効・無効の編集値
        bool recording = profiler.IsEnabled();
        if (ImGui::Checkbox("記録", &recording))
        {
            profiler.SetEnabled(recording);
        }
        ImGui::SetItemTooltip(
            "止めると、履歴と選択中のフレームを保ったまま調べられます。");

        ImGui::SameLine();
        ImGui::BeginDisabled(m_history.empty());
        if (ImGui::ArrowButton("##ProfilerPrevious", ImGuiDir_Left))
        {
            StepSelection(-1);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##ProfilerNext", ImGuiDir_Right))
        {
            StepSelection(1);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_followLatest);
        if (ImGui::Button("最新へ"))
        {
            FollowLatest();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("消去"))
        {
            ClearHistory();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        // 表示履歴に保持する最大フレーム数
        const auto capacity = profiler.FrameCapacity();
        // 現在の履歴容量を表示する文字列
        char capacityLabel[32]{};
        std::snprintf(
            capacityLabel,
            sizeof(capacityLabel),
            "%zu フレーム",
            capacity);
        if (ImGui::BeginCombo("履歴", capacityLabel))
        {
            // 選べる履歴フレーム数
            for (const auto option : HistoryCapacities)
            {
                // 履歴容量の選択肢の表示文字列
                char optionLabel[32]{};
                std::snprintf(
                    optionLabel,
                    sizeof(optionLabel),
                    "%zu フレーム",
                    option);
                if (ImGui::Selectable(optionLabel, option == capacity))
                {
                    profiler.SetFrameCapacity(option);
                    TrimHistory();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(
            captureDirectory.empty() || m_history.empty());
        if (ImGui::Button("記録を保存"))
        {
            static_cast<void>(SaveHistory(captureDirectory));
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "履歴全体を.lamapon/profilesへJSONで保存します。"
            "「プロファイル分析」で比較に使えます。");
    }

    void ProfilerPanel::DrawTimeline(
        const float frameBudgetMilliseconds)
    {
        // タイムラインの幅・ピクセル
        const float width = std::max(
            ImGui::GetContentRegionAvail().x,
            100.0f);
        // タイムラインの高さ・ピクセル
        constexpr float height = 110.0f;
        // タイムライン左上のscreen座標
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(
            "##ProfilerTimeline",
            ImVec2{ width, height });
        // タイムラインへマウスを置いたか
        const bool hovered = ImGui::IsItemHovered();
        // タイムラインを操作中か
        const bool active = ImGui::IsItemActive();

        // タイムラインを描くImGui命令の借用
        auto* const drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            origin,
            ImVec2{ origin.x + width, origin.y + height },
            ImGui::GetColorU32(ImGuiCol_FrameBg));

        // 描画するフレーム枠数・最少120
        const auto slotCount = std::max<std::size_t>(
            m_history.size(),
            120);
        // 1フレーム枠の幅・ピクセル
        const float slotWidth =
            width / static_cast<float>(slotCount);
        // 最新フレームを右端へ揃え、履歴が少ない分は左側を空ける。
        // 最新を右へ揃えた先頭枠のx座標
        const float firstX =
            origin.x
            + slotWidth
                * static_cast<float>(slotCount - m_history.size());

        // 縦軸上限にするフレーム時間・ms
        double maximum = std::max(
            static_cast<double>(frameBudgetMilliseconds) * 2.0,
            1.0);
        // 取り込むまたは表示するフレーム
        for (const auto& frame : m_history)
        {
            maximum = std::max(maximum, frame.milliseconds * 1.1);
        }
        // 時間をscreen y座標へ変換する(milliseconds: 描画する累積時間・ms)。
        const auto toY = [&](const double milliseconds)
        {
            return origin.y + height
                - static_cast<float>(
                    std::min(milliseconds / maximum, 1.0))
                    * height;
        };

        // 選択した計測フレームの位置
        const auto* selected = SelectedFrame();
        // 履歴配列の位置
        for (std::size_t position{};
            position < m_history.size();
            ++position)
        {
            // 取り込むまたは表示するフレーム
            const auto& frame = m_history[position];
            // 当該フレーム枠の左端screen座標
            const float left =
                firstX + slotWidth * static_cast<float>(position);
            // 当該フレーム枠の右端screen座標
            const float right =
                left + std::max(slotWidth - 1.0f, 1.0f);

            // 最上位の計測時間だけを積み上げ、frame時間との差分を未計測として灰色にする。
            // 最上位区間の累積時間・ms
            double stacked{};
            // 表示する計測区間
            for (const auto& sample : frame.samples)
            {
                if (sample.parent != ProfileSample::NoParent)
                {
                    continue;
                }
                // 計測区間の上端screen座標
                const float top = toY(stacked + sample.milliseconds);
                // 計測区間の下端screen座標
                const float bottom = toY(stacked);
                if (bottom - top >= 0.5f)
                {
                    drawList->AddRectFilled(
                        ImVec2{ left, top },
                        ImVec2{ right, bottom },
                        CategoryColor(sample.name));
                }
                stacked += sample.milliseconds;
            }
            if (frame.milliseconds > stacked)
            {
                drawList->AddRectFilled(
                    ImVec2{ left, toY(frame.milliseconds) },
                    ImVec2{ right, toY(stacked) },
                    UnmeasuredColor());
            }
            if (selected == &frame && !m_followLatest)
            {
                drawList->AddRect(
                    ImVec2{ left - 1.0f, origin.y },
                    ImVec2{ right + 1.0f, origin.y + height },
                    IM_COL32(255, 255, 255, 230),
                    0.0f,
                    0,
                    2.0f);
            }
        }

        if (frameBudgetMilliseconds > 0.0f)
        {
            // 予算線のscreen y座標
            const float budgetY =
                toY(static_cast<double>(frameBudgetMilliseconds));
            drawList->AddLine(
                ImVec2{ origin.x, budgetY },
                ImVec2{ origin.x + width, budgetY },
                IM_COL32(255, 220, 90, 180));
            // 予算時間を表示する文字列
            char budgetLabel[32]{};
            std::snprintf(
                budgetLabel,
                sizeof(budgetLabel),
                "%.1f ms",
                frameBudgetMilliseconds);
            drawList->AddText(
                ImVec2{ origin.x + 4.0f, budgetY - 16.0f },
                IM_COL32(255, 220, 90, 220),
                budgetLabel);
        }

        if ((hovered || active) && !m_history.empty())
        {
            // マウスのscreen x座標
            const float mouseX = ImGui::GetIO().MousePos.x;
            // マウス位置に対応するフレーム枠
            const auto slot = static_cast<std::ptrdiff_t>(
                std::floor((mouseX - firstX) / slotWidth));
            if (slot >= 0
                && slot < static_cast<std::ptrdiff_t>(m_history.size()))
            {
                // 取り込むまたは表示するフレーム
                const auto& frame =
                    m_history[static_cast<std::size_t>(slot)];
                if (hovered)
                {
                    ImGui::SetTooltip(
                        "フレーム #%llu\n%.2f ms",
                        static_cast<unsigned long long>(frame.index),
                        frame.milliseconds);
                }
                // ドラッグでも選択を追従させ、波形をなぞるように探せます。
                if (active)
                {
                    SelectFrame(frame.index);
                }
            }
        }

        // 最新フレームの最上位区間を凡例として並べます。
        if (!m_history.empty())
        {
            // 最初の凡例を表示する位置か
            bool first = true;
            // 表示する計測区間
            for (const auto& sample : m_history.back().samples)
            {
                if (sample.parent != ProfileSample::NoParent)
                {
                    continue;
                }
                if (!first)
                {
                    ImGui::SameLine();
                }
                first = false;
                ImGui::ColorButton(
                    sample.name.c_str(),
                    ImGui::ColorConvertU32ToFloat4(
                        CategoryColor(sample.name)),
                    ImGuiColorEditFlags_NoTooltip
                        | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2{ 10.0f, 10.0f });
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextUnformatted(sample.name.c_str());
            }
            ImGui::SameLine();
            ImGui::ColorButton(
                "未計測",
                ImGui::ColorConvertU32ToFloat4(UnmeasuredColor()),
                ImGuiColorEditFlags_NoTooltip
                    | ImGuiColorEditFlags_NoDragDrop,
                ImVec2{ 10.0f, 10.0f });
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextUnformatted("未計測");
        }
    }

    void ProfilerPanel::DrawSelectedFrame(const ProfileFrame& frame)
    {
        // 自己時間と親子関係の解析結果
        const auto tree = BuildProfileFrameTree(frame);
        ImGui::Separator();
        ImGui::Text(
            "フレーム #%llu  %.3f ms",
            static_cast<unsigned long long>(frame.index),
            frame.milliseconds);
        ImGui::SameLine();
        ImGui::TextDisabled(
            "（計測区間 %.3f ms / 未計測 %.3f ms）",
            tree.rootMilliseconds,
            std::max(frame.milliseconds - tree.rootMilliseconds, 0.0));
        if (m_followLatest)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("最新を表示中");
        }

        // 階層または自己時間の編集選択
        int mode = static_cast<int>(m_viewMode);
        ImGui::RadioButton("階層", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("自己時間順", &mode, 1);
        m_viewMode = static_cast<ViewMode>(mode);

        if (m_viewMode == ViewMode::Hierarchy)
        {
            DrawHierarchy(frame, tree);
        }
        else
        {
            DrawSelfTimeTable(frame);
        }
    }

    void ProfilerPanel::DrawHierarchy(
        const ProfileFrame& frame,
        const ProfileFrameTree& tree)
    {
        if (!ImGui::BeginTable(
                "ProfilerHierarchy",
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
            "区間",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "合計 ms",
            ImGuiTableColumnFlags_WidthFixed,
            80.0f);
        ImGui::TableSetupColumn(
            "自己 ms",
            ImGuiTableColumnFlags_WidthFixed,
            80.0f);
        ImGui::TableSetupColumn(
            "割合",
            ImGuiTableColumnFlags_WidthFixed,
            60.0f);
        ImGui::TableSetupColumn(
            "呼出",
            ImGuiTableColumnFlags_WidthFixed,
            50.0f);
        ImGui::TableHeadersRow();
        // 階層表の最上位区間の番号
        for (const auto root : tree.roots)
        {
            DrawTreeNode(frame, tree, root, frame.milliseconds);
        }
        ImGui::EndTable();
    }

    void ProfilerPanel::DrawSelfTimeTable(const ProfileFrame& frame)
    {
        ImGui::SetNextItemWidth(220.0f);
        // 区間名検索の入力バッファ
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##ProfilerFilter",
                "区間名で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }

        // 自己時間順の集計済み区間
        const auto entries = FlattenProfileFrame(frame);
        if (!ImGui::BeginTable(
                "ProfilerSelfTime",
                4,
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
            "区間",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "自己 ms",
            ImGuiTableColumnFlags_WidthFixed,
            80.0f);
        ImGui::TableSetupColumn(
            "合計 ms",
            ImGuiTableColumnFlags_WidthFixed,
            80.0f);
        ImGui::TableSetupColumn(
            "呼出",
            ImGuiTableColumnFlags_WidthFixed,
            60.0f);
        ImGui::TableHeadersRow();
        // 自己時間表へ表示する集計区間
        for (const auto& entry : entries)
        {
            if (!m_filter.empty()
                && entry.name.find(m_filter) == std::string::npos)
            {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", entry.selfMilliseconds);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", entry.totalMilliseconds);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text(
                "%llu",
                static_cast<unsigned long long>(entry.calls));
        }
        ImGui::EndTable();
    }
}
