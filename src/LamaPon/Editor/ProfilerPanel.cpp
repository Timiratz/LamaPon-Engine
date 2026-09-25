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
        constexpr std::array<std::size_t, 5> HistoryCapacities{
            240, 600, 1200, 3000, 6000
        };

        // 最上位区間の色です。名前から決めるので、フレームや実行を
        // またいでも同じ区間は同じ色になります。
        [[nodiscard]] ImU32 CategoryColor(const std::string_view name)
        {
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
            std::uint32_t hash = 2166136261u;
            for (const char character : name)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 16777619u;
            }
            return palette[hash % palette.size()];
        }

        [[nodiscard]] ImU32 UnmeasuredColor()
        {
            return IM_COL32(110, 110, 110, 255);
        }

        [[nodiscard]] const void* NodeId(const std::string& name)
        {
            // 表示名に"##"が含まれてもIDの区切りとして解釈されないよう、
            // 名前のハッシュを識別子に使います。TreeNodeのIDスタックで
            // 親の経路と組み合わさるため、同名の別経路とも衝突しません。
            return reinterpret_cast<const void*>(
                std::hash<std::string>{}(name) | 1u);
        }

        void DrawTreeNode(
            const ProfileFrame& frame,
            const ProfileFrameTree& tree,
            const std::uint32_t index,
            const double frameMilliseconds)
        {
            const auto& sample = frame.samples[index];
            const auto& children = tree.children[index];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
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
        auto& profiler = Profiler::Instance();
        if (profiler.LatestFrameIndex() < m_lastPulledIndex)
        {
            // Profiler::Clearでindexが1から振り直されました。
            m_history.clear();
            m_lastPulledIndex = 0;
        }
        auto frames = profiler.SnapshotSince(m_lastPulledIndex);
        for (auto& frame : frames)
        {
            m_lastPulledIndex = frame.index;
            m_history.push_back(std::move(frame));
        }
        TrimHistory();
    }

    void ProfilerPanel::TrimHistory() noexcept
    {
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
        const auto* current = SelectedFrame();
        if (current == nullptr)
        {
            return;
        }
        const auto position = std::distance(
            m_history.begin(),
            std::ranges::find(
                m_history,
                current->index,
                &ProfileFrame::index));
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
        const std::vector<ProfileFrame> frames(
            m_history.begin(),
            m_history.end());
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
        auto& profiler = Profiler::Instance();
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
        const auto capacity = profiler.FrameCapacity();
        char capacityLabel[32]{};
        std::snprintf(
            capacityLabel,
            sizeof(capacityLabel),
            "%zu フレーム",
            capacity);
        if (ImGui::BeginCombo("履歴", capacityLabel))
        {
            for (const auto option : HistoryCapacities)
            {
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
        const float width = std::max(
            ImGui::GetContentRegionAvail().x,
            100.0f);
        constexpr float height = 110.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(
            "##ProfilerTimeline",
            ImVec2{ width, height });
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        auto* const drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            origin,
            ImVec2{ origin.x + width, origin.y + height },
            ImGui::GetColorU32(ImGuiCol_FrameBg));

        const auto slotCount = std::max<std::size_t>(
            m_history.size(),
            120);
        const float slotWidth =
            width / static_cast<float>(slotCount);
        // 最新フレームを右端に揃え、履歴が少ないときは左側を空けます。
        const float firstX =
            origin.x
            + slotWidth
                * static_cast<float>(slotCount - m_history.size());

        double maximum = std::max(
            static_cast<double>(frameBudgetMilliseconds) * 2.0,
            1.0);
        for (const auto& frame : m_history)
        {
            maximum = std::max(maximum, frame.milliseconds * 1.1);
        }
        const auto toY = [&](const double milliseconds)
        {
            return origin.y + height
                - static_cast<float>(
                    std::min(milliseconds / maximum, 1.0))
                    * height;
        };

        const auto* selected = SelectedFrame();
        for (std::size_t position{};
            position < m_history.size();
            ++position)
        {
            const auto& frame = m_history[position];
            const float left =
                firstX + slotWidth * static_cast<float>(position);
            const float right =
                left + std::max(slotWidth - 1.0f, 1.0f);

            // 最上位区間を積み上げ、残りを未計測として灰色で描きます。
            double stacked{};
            for (const auto& sample : frame.samples)
            {
                if (sample.parent != ProfileSample::NoParent)
                {
                    continue;
                }
                const float top = toY(stacked + sample.milliseconds);
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
            const float budgetY =
                toY(static_cast<double>(frameBudgetMilliseconds));
            drawList->AddLine(
                ImVec2{ origin.x, budgetY },
                ImVec2{ origin.x + width, budgetY },
                IM_COL32(255, 220, 90, 180));
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
            const float mouseX = ImGui::GetIO().MousePos.x;
            const auto slot = static_cast<std::ptrdiff_t>(
                std::floor((mouseX - firstX) / slotWidth));
            if (slot >= 0
                && slot < static_cast<std::ptrdiff_t>(m_history.size()))
            {
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
            bool first = true;
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
        for (const auto root : tree.roots)
        {
            DrawTreeNode(frame, tree, root, frame.milliseconds);
        }
        ImGui::EndTable();
    }

    void ProfilerPanel::DrawSelfTimeTable(const ProfileFrame& frame)
    {
        ImGui::SetNextItemWidth(220.0f);
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
