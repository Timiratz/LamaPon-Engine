#include "LamaPon/Editor/FrameDebuggerPanel.h"

#include "LamaPon/Graphics/FrameDebugger.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace LamaPon
{
    namespace
    {
        [[nodiscard]] const char* PassLabel(const FrameDebugPass pass)
        {
            switch (pass)
            {
            case FrameDebugPass::ShadowDepth:
                return "影（深度のみ）";
            case FrameDebugPass::DepthPrepass:
                return "深度プリパス";
            case FrameDebugPass::Color:
                break;
            }
            return "カラー";
        }

        [[nodiscard]] const char* KindLabel(const FrameDebugEventKind kind)
        {
            switch (kind)
            {
            case FrameDebugEventKind::PreRender3D:
                return "事前パス";
            case FrameDebugEventKind::Draw2D:
                return "2D / UI";
            case FrameDebugEventKind::InstancedBatch:
                return "インスタンス描画";
            case FrameDebugEventKind::Draw3D:
                break;
            }
            return "3D描画";
        }

        [[nodiscard]] bool MatchesFilter(
            const std::string& filter,
            const FrameDebugEvent& event)
        {
            return filter.empty()
                || event.objectName.find(filter) != std::string::npos
                || event.componentType.find(filter) != std::string::npos
                || event.sectionPath.find(filter) != std::string::npos
                || event.description.geometry.find(filter)
                    != std::string::npos;
        }

        void DetailRow(const char* label, const std::string& value)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped(
                "%s",
                value.empty() ? "（不明）" : value.c_str());
        }
    }

    FrameDebuggerPanel::FrameDebuggerPanel(
        FrameDebugger& debugger,
        SelectObject select,
        PauseGame pause)
        : m_debugger(debugger)
        , m_select(std::move(select))
        , m_pause(std::move(pause))
    {
    }

    void FrameDebuggerPanel::SynchronizeEnabled(const bool panelOpen)
    {
        const bool enabled = panelOpen && m_active;
        if (m_debugger.IsEnabled() != enabled)
        {
            m_debugger.SetEnabled(enabled);
        }
        m_debugger.SetEventLimit(
            enabled ? m_selected : std::nullopt);
    }

    void FrameDebuggerPanel::SelectEvent(
        const std::optional<std::uint32_t> index)
    {
        m_selected = index;
        m_scrollToSelection = true;
        m_debugger.SetEventLimit(m_selected);
        if (m_selected && m_pause)
        {
            m_pause();
        }
    }

    void FrameDebuggerPanel::Draw(const char* const title, bool& open)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(
            ImVec2{ 760.0f, 600.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, &open))
        {
            ImGui::End();
            return;
        }

        bool active = m_active;
        if (ImGui::Checkbox("フレームデバッガーを有効にする", &active))
        {
            m_active = active;
            if (m_active)
            {
                if (m_pause)
                {
                    m_pause();
                }
            }
            else
            {
                m_selected.reset();
            }
            SynchronizeEnabled(true);
        }
        if (!m_active)
        {
            ImGui::TextWrapped(
                "有効にすると、フレームを構成する描画を1件ずつ一覧にします。"
                "イベントを選ぶと、そこまでで描画を止めた途中の絵が"
                "Scene View / Game Viewに表示されます（再生中は一時停止します）。");
            ImGui::End();
            return;
        }

        const auto& events = m_debugger.LastFrameEvents();
        const auto count = static_cast<int>(events.size());
        if (m_selected && *m_selected >= events.size() && count > 0)
        {
            // シーンの変化でイベントが減った場合は最後のイベントへ寄せます。
            SelectEvent(static_cast<std::uint32_t>(count - 1));
        }

        ImGui::BeginDisabled(count == 0);
        int position = m_selected
            ? static_cast<int>(*m_selected) + 1
            : count;
        ImGui::SetNextItemWidth(
            std::max(ImGui::GetContentRegionAvail().x - 190.0f, 120.0f));
        if (ImGui::SliderInt(
                "##FrameDebuggerEvent",
                &position,
                1,
                std::max(count, 1),
                "%d 件目まで描画",
                ImGuiSliderFlags_AlwaysClamp))
        {
            SelectEvent(static_cast<std::uint32_t>(position - 1));
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##FrameDebuggerPrevious", ImGuiDir_Left)
            && position > 1)
        {
            SelectEvent(static_cast<std::uint32_t>(position - 2));
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##FrameDebuggerNext", ImGuiDir_Right)
            && position < count)
        {
            SelectEvent(static_cast<std::uint32_t>(position));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_selected);
        if (ImGui::Button("すべて描画"))
        {
            SelectEvent(std::nullopt);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::TextDisabled(
            "描画イベント %d 件。空・ポスト処理・エディター表示は常に描かれます。",
            count);

        const float detailsHeight = 190.0f;
        if (ImGui::BeginChild(
                "FrameDebuggerEvents",
                ImVec2{
                    0.0f,
                    std::max(
                        ImGui::GetContentRegionAvail().y - detailsHeight,
                        120.0f) },
                ImGuiChildFlags_Borders))
        {
            DrawEventList();
        }
        ImGui::EndChild();
        DrawEventDetails();
        ImGui::End();
    }

    void FrameDebuggerPanel::DrawEventList()
    {
        ImGui::SetNextItemWidth(220.0f);
        char filter[128]{};
        m_filter.copy(filter, sizeof(filter) - 1);
        if (ImGui::InputTextWithHint(
                "##FrameDebuggerFilter",
                "GameObject・種類・区間で絞り込み",
                filter,
                sizeof(filter)))
        {
            m_filter = filter;
        }

        const auto& events = m_debugger.LastFrameEvents();
        if (events.empty())
        {
            ImGui::TextDisabled(
                "描画イベントがありません。Scene ViewかGame Viewを"
                "表示してください。");
            return;
        }
        if (!ImGui::BeginTable(
                "FrameDebuggerEventTable",
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
            "#",
            ImGuiTableColumnFlags_WidthFixed,
            40.0f);
        ImGui::TableSetupColumn(
            "GameObject",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "Component",
            ImGuiTableColumnFlags_WidthFixed,
            120.0f);
        ImGui::TableSetupColumn(
            "種類",
            ImGuiTableColumnFlags_WidthFixed,
            110.0f);
        ImGui::TableSetupColumn(
            "形状",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const std::string* previousSection = nullptr;
        for (const auto& event : events)
        {
            if (!MatchesFilter(m_filter, event))
            {
                continue;
            }
            // GPU区間が変わるところに見出し行を入れ、パスの境目を示します。
            if (previousSection == nullptr
                || *previousSection != event.sectionPath)
            {
                ImGui::TableNextRow(ImGuiTableRowFlags_None);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(
                    ImVec4{ 0.55f, 0.75f, 1.0f, 1.0f },
                    "%s",
                    event.sectionPath.empty()
                        ? "（区間外）"
                        : event.sectionPath.c_str());
                previousSection = &event.sectionPath;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(event.index));
            const bool selected =
                m_selected && *m_selected == event.index;
            if (event.skipped)
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            char label[16]{};
            std::snprintf(label, sizeof(label), "%u", event.index + 1);
            if (ImGui::Selectable(
                    label,
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns))
            {
                SelectEvent(event.index);
            }
            if (selected && m_scrollToSelection)
            {
                ImGui::SetScrollHereY(0.5f);
                m_scrollToSelection = false;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(event.objectName.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(event.componentType.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(KindLabel(event.kind));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(event.description.geometry.c_str());
            if (event.skipped)
            {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void FrameDebuggerPanel::DrawEventDetails()
    {
        const auto& events = m_debugger.LastFrameEvents();
        if (!m_selected || *m_selected >= events.size())
        {
            ImGui::TextDisabled(
                "イベントを選ぶと、描画内容とパイプラインの状態を表示します。");
            return;
        }
        const auto& event = events[*m_selected];
        ImGui::SeparatorText("選択中のイベント");
        if (ImGui::BeginTable(
                "FrameDebuggerDetails",
                2,
                ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn(
                "項目",
                ImGuiTableColumnFlags_WidthFixed,
                110.0f);
            ImGui::TableSetupColumn(
                "値",
                ImGuiTableColumnFlags_WidthStretch);
            DetailRow(
                "イベント",
                std::to_string(event.index + 1) + " / "
                    + std::to_string(events.size()) + "（"
                    + KindLabel(event.kind) + "）");
            DetailRow("描画パス", PassLabel(event.pass));
            DetailRow("GPU区間", event.sectionPath);
            DetailRow(
                "GameObject",
                event.objectName + "（ID "
                    + std::to_string(event.objectId) + "）");
            DetailRow("Component", event.componentType);
            DetailRow("形状", event.description.geometry);
            DetailRow("マテリアル", event.description.material);
            DetailRow("状態", event.description.state);
            std::string counts;
            if (event.description.vertexCount > 0)
            {
                counts += std::to_string(event.description.vertexCount)
                    + "頂点 ";
            }
            if (event.description.triangleCount > 0)
            {
                counts += std::to_string(event.description.triangleCount)
                    + "三角形 ";
            }
            if (event.description.instanceCount > 1)
            {
                counts += "x"
                    + std::to_string(event.description.instanceCount);
            }
            DetailRow("数", counts);
            ImGui::EndTable();
        }
        if (event.objectId != 0 && m_select)
        {
            if (ImGui::Button("このGameObjectを選択"))
            {
                m_select(event.objectId);
            }
        }
    }
}
