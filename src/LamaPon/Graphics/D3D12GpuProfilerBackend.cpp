#include "LamaPon/Graphics/D3D12GpuProfilerBackend.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D12Backend.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    [[nodiscard]] D3D12_HEAP_PROPERTIES ReadbackHeap() noexcept
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    [[nodiscard]] D3D12_RESOURCE_DESC Buffer(
        const std::uint64_t bytes) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return description;
    }

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(std::string(operation) + " failed.");
        }
    }
}

namespace LamaPon
{
    D3D12GpuProfilerBackend::D3D12GpuProfilerBackend(
        D3D12Backend& backend) noexcept
        : m_backend(&backend)
    {
        try
        {
            Initialize();
            m_supported = true;
        }
        catch (...)
        {
            m_supported = false;
            if (m_timestampReadback != nullptr
                && m_mappedTimestamps != nullptr)
            {
                m_timestampReadback->Unmap(0, nullptr);
            }
            if (m_pipelineStatisticsReadback != nullptr
                && m_mappedPipelineStatistics != nullptr)
            {
                m_pipelineStatisticsReadback->Unmap(0, nullptr);
            }
            m_mappedTimestamps = nullptr;
            m_mappedPipelineStatistics = nullptr;
            m_pipelineStatisticsReadback.Reset();
            m_timestampReadback.Reset();
            m_pipelineStatisticsHeap.Reset();
            m_timestampHeap.Reset();
        }
    }

    D3D12GpuProfilerBackend::~D3D12GpuProfilerBackend()
    {
        if (m_timestampReadback != nullptr && m_mappedTimestamps != nullptr)
        {
            m_timestampReadback->Unmap(0, nullptr);
        }
        if (m_pipelineStatisticsReadback != nullptr
            && m_mappedPipelineStatistics != nullptr)
        {
            m_pipelineStatisticsReadback->Unmap(0, nullptr);
        }
    }

    bool D3D12GpuProfilerBackend::IsSupported() const noexcept
    {
        return m_supported;
    }

    void D3D12GpuProfilerBackend::OpenFrame()
    {
        if (!m_supported || m_backend == nullptr)
        {
            return;
        }
        auto* const commands = m_backend->BeginFrameCommands();
        m_activeFrame = m_backend->CurrentBackBufferIndex();
        auto& frame = m_frames[m_activeFrame];
        if (frame.open)
        {
            return;
        }

        // BeginFrameCommandsはこのback bufferを以前使ったGPU workの
        // fence完了を待つため、同じslotのreadbackとqueryを安全に再利用できます。
        ReadCompletedFrame(m_activeFrame);
        frame.usedSections = 0;
        frame.sectionStack.clear();
        frame.open = true;
        frame.pipelineStatisticsOpen = m_pipelineStatisticsSupported;
        frame.pipelineStatisticsValid = m_pipelineStatisticsSupported;
        commands->EndQuery(
            m_timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(TimestampBase(m_activeFrame)));
        if (frame.pipelineStatisticsOpen)
        {
            commands->BeginQuery(
                m_pipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                static_cast<UINT>(m_activeFrame));
        }
    }

    bool D3D12GpuProfilerBackend::BeginSection(
        const std::string_view name,
        const std::uint32_t depth)
    {
        if (!m_supported)
        {
            return false;
        }
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open || frame.usedSections >= MaximumSections)
        {
            return false;
        }
        if (frame.sections.size() <= frame.usedSections)
        {
            frame.sections.emplace_back();
        }
        auto& section = frame.sections[frame.usedSections];
        section.name = name;
        section.depth = depth;
        frame.sectionStack.push_back(frame.usedSections);

        const auto query = TimestampBase(m_activeFrame)
            + 2u + frame.usedSections * 2u;
        m_backend->CurrentFrameCommands()->EndQuery(
            m_timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(query));
        ++frame.usedSections;
        return true;
    }

    void D3D12GpuProfilerBackend::EndSection() noexcept
    {
        if (!m_supported)
        {
            return;
        }
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open || frame.sectionStack.empty())
        {
            return;
        }
        const auto section = frame.sectionStack.back();
        frame.sectionStack.pop_back();
        try
        {
            const auto query = TimestampBase(m_activeFrame)
                + 3u + section * 2u;
            m_backend->BeginFrameCommands()->EndQuery(
                m_timestampHeap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                static_cast<UINT>(query));
        }
        catch (...)
        {
            m_supported = false;
            frame.sectionStack.clear();
        }
    }

    void D3D12GpuProfilerBackend::CloseFrame()
    {
        if (!m_supported)
        {
            return;
        }
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open)
        {
            return;
        }
        while (!frame.sectionStack.empty())
        {
            EndSection();
        }
        if (!m_supported)
        {
            frame.open = false;
            return;
        }

        auto* const commands = m_backend->BeginFrameCommands();
        const auto base = TimestampBase(m_activeFrame);
        commands->EndQuery(
            m_timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(base + 1u));
        const bool resolvePipelineStatistics =
            frame.pipelineStatisticsOpen
            && frame.pipelineStatisticsValid;
        if (resolvePipelineStatistics)
        {
            commands->EndQuery(
                m_pipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                static_cast<UINT>(m_activeFrame));
            frame.pipelineStatisticsOpen = false;
        }
        const auto usedTimestampCount = 2u + frame.usedSections * 2u;
        commands->ResolveQueryData(
            m_timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(base),
            static_cast<UINT>(usedTimestampCount),
            m_timestampReadback.Get(),
            static_cast<UINT64>(base * sizeof(std::uint64_t)));
        if (resolvePipelineStatistics)
        {
            commands->ResolveQueryData(
                m_pipelineStatisticsHeap.Get(),
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                static_cast<UINT>(m_activeFrame),
                1,
                m_pipelineStatisticsReadback.Get(),
                static_cast<UINT64>(m_activeFrame
                    * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)));
        }
        frame.open = false;
        frame.pending = true;
    }

    const std::vector<GpuSectionTime>&
        D3D12GpuProfilerBackend::LatestSections() const noexcept
    {
        return m_latestSections;
    }

    float D3D12GpuProfilerBackend::LatestFrameMilliseconds() const noexcept
    {
        return m_latestFrameMilliseconds;
    }

    const GpuPipelineStatistics&
        D3D12GpuProfilerBackend::LatestPipelineStatistics() const noexcept
    {
        return m_latestPipelineStatistics;
    }

    bool D3D12GpuProfilerBackend::BeginMarker(
        const std::string_view name) noexcept
    {
        if (m_backend == nullptr)
        {
            return false;
        }
        auto* const commands = m_backend->RecordingFrameCommands();
        if (commands == nullptr)
        {
            return false;
        }
        try
        {
            // PIXの旧形式（metadata 0 = UTF-16文字列）です。PIXとRenderDocの
            // どちらも、追加のruntime無しでこの形式を読めます。
            const auto wideName = Utf8ToWide(name);
            m_markerStack.reserve(m_markerStack.size() + 1);
            commands->BeginEvent(
                0,
                wideName.c_str(),
                static_cast<UINT>(
                    (wideName.size() + 1) * sizeof(wchar_t)));
            m_markerStack.push_back(true);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void D3D12GpuProfilerBackend::EndMarker() noexcept
    {
        if (m_markerStack.empty())
        {
            return;
        }
        const bool open = m_markerStack.back();
        m_markerStack.pop_back();
        if (!open || m_backend == nullptr)
        {
            return;
        }
        if (auto* const commands = m_backend->RecordingFrameCommands())
        {
            commands->EndEvent();
        }
    }

    void D3D12GpuProfilerBackend::BeforeCommandListClose(
        ID3D12GraphicsCommandList* const commands) noexcept
    {
        // 閉じるcommand listの中でmarkerの入れ子を完結させます。
        if (commands != nullptr)
        {
            for (auto marker = m_markerStack.rbegin();
                marker != m_markerStack.rend();
                ++marker)
            {
                if (*marker)
                {
                    commands->EndEvent();
                    *marker = false;
                }
            }
        }
        if (!m_supported || commands == nullptr)
        {
            return;
        }
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open || !frame.pipelineStatisticsOpen)
        {
            return;
        }
        commands->EndQuery(
            m_pipelineStatisticsHeap.Get(),
            D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
            static_cast<UINT>(m_activeFrame));
        frame.pipelineStatisticsOpen = false;
        frame.pipelineStatisticsValid = false;
    }

    void D3D12GpuProfilerBackend::Initialize()
    {
        if (m_backend == nullptr || !m_backend->IsInitialized()
            || m_backend->Device() == nullptr
            || m_backend->CommandQueue() == nullptr)
        {
            throw std::logic_error(
                "D3D12 GPU profiling requires an initialized backend.");
        }
        ThrowIfFailed(
            m_backend->CommandQueue()->GetTimestampFrequency(&m_frequency),
            "ID3D12CommandQueue::GetTimestampFrequency");
        if (m_frequency == 0)
        {
            throw std::runtime_error(
                "The D3D12 timestamp frequency is zero.");
        }

        D3D12_QUERY_HEAP_DESC timestampDescription{};
        timestampDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        timestampDescription.Count = static_cast<UINT>(
            FrameCount * TimestampCountPerFrame);
        ThrowIfFailed(
            m_backend->Device()->CreateQueryHeap(
                &timestampDescription,
                IID_PPV_ARGS(m_timestampHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateQueryHeap(timestamp)");

        const auto heap = ReadbackHeap();
        const auto timestampBuffer = Buffer(
            FrameCount * TimestampCountPerFrame
            * sizeof(std::uint64_t));
        ThrowIfFailed(
            m_backend->Device()->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &timestampBuffer,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(m_timestampReadback.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(timestamp readback)");
        ThrowIfFailed(
            m_timestampReadback->Map(
                0, nullptr,
                reinterpret_cast<void**>(&m_mappedTimestamps)),
            "ID3D12Resource::Map(timestamp readback)");

        // Pipeline statisticsは一部adapterで利用できないため任意機能です。
        try
        {
            D3D12_QUERY_HEAP_DESC pipelineDescription{};
            pipelineDescription.Type =
                D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
            pipelineDescription.Count = static_cast<UINT>(FrameCount);
            ThrowIfFailed(
                m_backend->Device()->CreateQueryHeap(
                    &pipelineDescription,
                    IID_PPV_ARGS(m_pipelineStatisticsHeap
                        .ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateQueryHeap(pipeline statistics)");
            const auto pipelineBuffer = Buffer(
                FrameCount
                * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
            ThrowIfFailed(
                m_backend->Device()->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &pipelineBuffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(m_pipelineStatisticsReadback
                        .ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateCommittedResource(pipeline readback)");
            ThrowIfFailed(
                m_pipelineStatisticsReadback->Map(
                    0, nullptr,
                    reinterpret_cast<void**>(
                        &m_mappedPipelineStatistics)),
                "ID3D12Resource::Map(pipeline readback)");
            m_pipelineStatisticsSupported = true;
        }
        catch (...)
        {
            if (m_pipelineStatisticsReadback != nullptr
                && m_mappedPipelineStatistics != nullptr)
            {
                m_pipelineStatisticsReadback->Unmap(0, nullptr);
            }
            m_mappedPipelineStatistics = nullptr;
            m_pipelineStatisticsReadback.Reset();
            m_pipelineStatisticsHeap.Reset();
            m_pipelineStatisticsSupported = false;
        }
    }

    void D3D12GpuProfilerBackend::ReadCompletedFrame(
        const std::size_t frameIndex) noexcept
    {
        auto& frame = m_frames[frameIndex];
        if (!frame.pending || m_mappedTimestamps == nullptr)
        {
            return;
        }
        frame.pending = false;
        const auto base = TimestampBase(frameIndex);
        const auto begin = m_mappedTimestamps[base];
        const auto end = m_mappedTimestamps[base + 1u];
        const double millisecondsPerTick = 1000.0
            / static_cast<double>(m_frequency);
        m_latestFrameMilliseconds = end >= begin
            ? static_cast<float>(
                static_cast<double>(end - begin) * millisecondsPerTick)
            : 0.0f;

        m_latestSections.clear();
        m_latestSections.reserve(frame.usedSections);
        for (std::size_t index{}; index < frame.usedSections; ++index)
        {
            const auto sectionBegin = m_mappedTimestamps[
                base + 2u + index * 2u];
            const auto sectionEnd = m_mappedTimestamps[
                base + 3u + index * 2u];
            if (sectionEnd < sectionBegin)
            {
                continue;
            }
            m_latestSections.push_back({
                frame.sections[index].name,
                static_cast<float>(
                    static_cast<double>(sectionEnd - sectionBegin)
                    * millisecondsPerTick),
                frame.sections[index].depth
            });
        }

        if (frame.pipelineStatisticsValid
            && m_mappedPipelineStatistics != nullptr)
        {
            const auto& statistics =
                m_mappedPipelineStatistics[frameIndex];
            m_latestPipelineStatistics = {
                statistics.IAVertices,
                statistics.IAPrimitives,
                statistics.VSInvocations,
                statistics.PSInvocations,
                statistics.HSInvocations,
                statistics.DSInvocations,
                statistics.GSInvocations,
                statistics.CSInvocations,
                true
            };
        }
        else
        {
            m_latestPipelineStatistics = {};
        }
    }

    std::size_t D3D12GpuProfilerBackend::TimestampBase(
        const std::size_t frameIndex) const noexcept
    {
        return frameIndex * TimestampCountPerFrame;
    }
}
