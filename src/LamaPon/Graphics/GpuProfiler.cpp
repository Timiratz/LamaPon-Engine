#include "LamaPon/Graphics/GpuProfiler.h"

namespace LamaPon
{
    namespace
    {
        // 未接続・非対応時に返す空の区間一覧を借用する。
        [[nodiscard]] const std::vector<GpuSectionTime>&
            EmptySections() noexcept
        {
            // 返却する空のGPU区間一覧
            static const std::vector<GpuSectionTime> sections;
            return sections;
        }

        // 未接続・非対応時に返す無効な処理量を借用する。
        [[nodiscard]] const GpuPipelineStatistics&
            EmptyPipelineStatistics() noexcept
        {
            // 返却する無効なGPU処理量
            static const GpuPipelineStatistics statistics;
            return statistics;
        }

    }

    GpuProfiler::SectionScope::SectionScope(
        GpuProfiler& profiler,
        const std::string_view name)
        : m_profiler(&profiler)
        , m_initialDepth(profiler.m_sections.size())
        , m_generation(profiler.m_generation)
    {
        profiler.BeginSection(name);
    }

    // 構築時の深さまで区間を閉じる。
    GpuProfiler::SectionScope::~SectionScope() noexcept
    {
        End();
    }

    void GpuProfiler::SectionScope::End() noexcept
    {
        if (m_profiler == nullptr)
        {
            return;
        }
        if (m_profiler->m_generation == m_generation)
        {
            m_profiler->EndSectionsToDepth(m_initialDepth);
        }
        m_profiler = nullptr;
    }

    void GpuProfiler::Attach(
        GpuProfilerBackend* const backend) noexcept
    {
        Detach();
        m_backend = backend;
    }

    void GpuProfiler::Detach() noexcept
    {
        EndSectionsToDepth(0);
        m_backend = nullptr;
        ++m_generation;
    }

    void GpuProfiler::OpenFrame()
    {
        if (IsSupported())
        {
            m_backend->OpenFrame();
        }
    }

    void GpuProfiler::SetSectionListener(
        GpuSectionListener* const listener) noexcept
    {
        m_listener = listener;
    }

    void GpuProfiler::BeginSection(
        const std::string_view name)
    {
        // 通知前に保存領域を確保し、登録できない区間の開始だけが残ることを防ぐ。
        m_sections.reserve(m_sections.size() + 1);

        // 開始・終了する区間の状態
        OpenSection section;
        if (IsSupported())
        {
            m_backend->OpenFrame();
            section.timed = m_backend->BeginSection(
                name,
                static_cast<std::uint32_t>(m_timedDepth));
            if (section.timed)
            {
                ++m_timedDepth;
            }
        }
        // 開始に成功した通知先を区間状態に記録し、終了時に対応させる。
        if (m_backend != nullptr)
        {
            section.marker = m_backend->BeginMarker(name);
        }
        if (m_listener != nullptr)
        {
            m_listener->OnGpuSectionBegin(name);
            section.listened = true;
        }
        try
        {
            section.cpuScope =
                Profiler::Instance().BeginScope(name);
            if (section.cpuScope.IsValid())
            {
                section.cpuStart = std::chrono::steady_clock::now();
            }
        }
        catch (...)
        {
            // CPU計測の失敗は描画パスへ伝播させない。
            section.cpuScope = {};
        }
        m_sections.push_back(section);
    }

    void GpuProfiler::EndSection()
    {
        if (m_sections.empty())
        {
            return;
        }
        EndTopSection();
    }

    void GpuProfiler::EndTopSection() noexcept
    {
        // 開始・終了する区間の状態
        const auto section = m_sections.back();
        m_sections.pop_back();
        if (section.cpuScope.IsValid())
        {
            Profiler::Instance().EndScope(
                section.cpuScope,
                std::chrono::steady_clock::now()
                    - section.cpuStart);
        }
        if (section.listened && m_listener != nullptr)
        {
            m_listener->OnGpuSectionEnd();
        }
        if (section.marker && m_backend != nullptr)
        {
            m_backend->EndMarker();
        }
        if (section.timed)
        {
            if (m_backend != nullptr)
            {
                m_backend->EndSection();
            }
            --m_timedDepth;
        }
    }

    void GpuProfiler::EndSectionsToDepth(
        const std::size_t depth) noexcept
    {
        while (m_sections.size() > depth)
        {
            EndTopSection();
        }
    }

    void GpuProfiler::CloseFrame()
    {
        EndSectionsToDepth(0);
        if (IsSupported())
        {
            m_backend->CloseFrame();
        }
    }

    bool GpuProfiler::IsSupported() const noexcept
    {
        return m_backend != nullptr
            && m_backend->IsSupported();
    }

    const std::vector<GpuProfiler::SectionTime>&
        GpuProfiler::LatestSections() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestSections()
            : EmptySections();
    }

    float GpuProfiler::LatestFrameMilliseconds() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestFrameMilliseconds()
            : 0.0f;
    }

    const GpuProfiler::PipelineStatistics&
        GpuProfiler::LatestPipelineStatistics() const noexcept
    {
        return IsSupported()
            ? m_backend->LatestPipelineStatistics()
            : EmptyPipelineStatistics();
    }

}
