#include "LamaPon/Graphics/FrameDebugger.h"

#include <utility>

namespace LamaPon
{
    void FrameDebugger::SetEnabled(const bool enabled) noexcept
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        // 切替時は空から記録し、途中有効化では最初のフレームの区間経路が不完全になり得る。
        m_sections.clear();
        m_droppedSections = 0;
        m_currentFrame.clear();
        m_nextIndex = 0;
        if (!enabled)
        {
            m_lastFrame.clear();
        }
    }

    void FrameDebugger::SetEventLimit(
        const std::optional<std::uint32_t> limit) noexcept
    {
        m_limit = limit;
    }

    std::string FrameDebugger::CurrentSectionPath() const
    {
        // 返却するGPU区間の経路
        std::string path;
        // 経路へ加えるGPU区間名
        for (const auto& section : m_sections)
        {
            if (!path.empty())
            {
                path += '/';
            }
            path += section;
        }
        return path;
    }

    bool FrameDebugger::SubmitDrawEvent(
        const FrameDebugEventKind kind,
        const FrameDebugPass pass,
        const std::uint64_t objectId,
        const std::string_view objectName,
        const std::string_view componentType,
        FrameDebugDrawDescription description) noexcept
    {
        if (!m_enabled)
        {
            return true;
        }
        // 記録前に消費するイベント番号
        const auto index = m_nextIndex++;
        // 設定上限内で描画する有無
        const bool draw = !m_limit || index <= *m_limit;
        try
        {
            // 登録する描画イベント
            FrameDebugEvent event;
            event.index = index;
            event.kind = kind;
            event.pass = pass;
            event.sectionPath = CurrentSectionPath();
            event.objectId = objectId;
            event.objectName = std::string(objectName);
            event.componentType = std::string(componentType);
            event.description = std::move(description);
            event.skipped = !draw;
            m_currentFrame.push_back(std::move(event));
        }
        catch (...)
        {
            // 記録失敗でもイベント番号と描画上限の判定は維持する。
        }
        return draw;
    }

    void FrameDebugger::EndFrame() noexcept
    {
        if (!m_enabled)
        {
            return;
        }
        m_lastFrame.swap(m_currentFrame);
        m_currentFrame.clear();
        m_nextIndex = 0;
        m_sections.clear();
        m_droppedSections = 0;
        ++m_completedFrames;
    }

    void FrameDebugger::OnGpuSectionBegin(
        const std::string_view name) noexcept
    {
        if (!m_enabled)
        {
            return;
        }
        try
        {
            m_sections.emplace_back(name);
        }
        catch (...)
        {
            ++m_droppedSections;
        }
    }

    void FrameDebugger::OnGpuSectionEnd() noexcept
    {
        if (m_droppedSections > 0)
        {
            --m_droppedSections;
            return;
        }
        // 対応する開始がない区間の終了は無視する。
        if (!m_sections.empty())
        {
            m_sections.pop_back();
        }
    }
}
