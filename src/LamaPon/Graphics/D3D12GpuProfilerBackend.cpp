#include "LamaPon/Graphics/D3D12GpuProfilerBackend.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D12Backend.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    // CPU読取用ヒープの仕様を返す。
    [[nodiscard]] D3D12_HEAP_PROPERTIES ReadbackHeap() noexcept
    {
        // 読取用ヒープの仕様
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    // 読取バッファーの仕様を返す(bytes: バッファーのバイト数)。
    [[nodiscard]] D3D12_RESOURCE_DESC Buffer(
        const std::uint64_t bytes) noexcept
    {
        // バッファー資源の仕様
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

    // 失敗したHRESULTを例外に変える(result: 実行結果, operation: 操作名)。
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
        // 操作するコマンド一覧
        auto* const commands = m_backend->BeginFrameCommands();
        m_activeFrame = m_backend->CurrentBackBufferIndex();
        // 操作するフレームの状態
        auto& frame = m_frames[m_activeFrame];
        if (frame.open)
        {
            return;
        }

        // BeginFrameCommandsが同じスロットのフェンス完了を待った後に、結果を読んで再利用する。
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
        // 操作するフレームの状態
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open || frame.usedSections >= MaximumSections)
        {
            return false;
        }
        if (frame.sections.size() <= frame.usedSections)
        {
            frame.sections.emplace_back();
        }
        // 区間の情報または番号
        auto& section = frame.sections[frame.usedSections];
        section.name = name;
        section.depth = depth;
        frame.sectionStack.push_back(frame.usedSections);

        // 送信する時刻クエリ番号
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
        // 操作するフレームの状態
        auto& frame = m_frames[m_activeFrame];
        if (!frame.open || frame.sectionStack.empty())
        {
            return;
        }
        // 区間の情報または番号
        const auto section = frame.sectionStack.back();
        frame.sectionStack.pop_back();
        try
        {
            // 送信する時刻クエリ番号
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
        // 操作するフレームの状態
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

        // 操作するコマンド一覧
        auto* const commands = m_backend->BeginFrameCommands();
        // フレーム先頭のクエリ番号
        const auto base = TimestampBase(m_activeFrame);
        commands->EndQuery(
            m_timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(base + 1u));
        // 描画統計を読み出せる
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
        // 使用した時刻クエリ数
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
        // 操作するコマンド一覧
        auto* const commands = m_backend->RecordingFrameCommands();
        if (commands == nullptr)
        {
            return false;
        }
        try
        {
            // PIX互換のmetadata 0としてUTF-16のマーカー名を渡す。
            // UTF-16のマーカー名
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
        // この一覧でマーカー開始済み
        const bool open = m_markerStack.back();
        m_markerStack.pop_back();
        if (!open || m_backend == nullptr)
        {
            return;
        }
        // 操作するコマンド一覧
        if (auto* const commands = m_backend->RecordingFrameCommands())
        {
            commands->EndEvent();
        }
    }

    void D3D12GpuProfilerBackend::BeforeCommandListClose(
        ID3D12GraphicsCommandList* const commands) noexcept
    {
        // 閉じるコマンド一覧の中でマーカーの入れ子を完結させる。
        if (commands != nullptr)
        {
            // 逆順に閉じるマーカー位置
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
        // 操作するフレームの状態
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

        // 時刻クエリヒープの仕様
        D3D12_QUERY_HEAP_DESC timestampDescription{};
        timestampDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        timestampDescription.Count = static_cast<UINT>(
            FrameCount * TimestampCountPerFrame);
        ThrowIfFailed(
            m_backend->Device()->CreateQueryHeap(
                &timestampDescription,
                IID_PPV_ARGS(m_timestampHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateQueryHeap(timestamp)");

        // CPU読取用ヒープの仕様
        const auto heap = ReadbackHeap();
        // 時刻読取バッファーの仕様
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

        // 描画統計は任意機能とし、生成失敗で時間計測を無効にしない。
        try
        {
            // 描画統計ヒープの仕様
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
            // 描画統計読取バッファーの仕様
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
        // 操作するフレームの状態
        auto& frame = m_frames[frameIndex];
        if (!frame.pending || m_mappedTimestamps == nullptr)
        {
            return;
        }
        frame.pending = false;
        // フレーム先頭のクエリ番号
        const auto base = TimestampBase(frameIndex);
        // フレーム開始の時刻値
        const auto begin = m_mappedTimestamps[base];
        // フレーム終了の時刻値
        const auto end = m_mappedTimestamps[base + 1u];
        // 一刻みの時間、ミリ秒
        const double millisecondsPerTick = 1000.0
            / static_cast<double>(m_frequency);
        m_latestFrameMilliseconds = end >= begin
            ? static_cast<float>(
                static_cast<double>(end - begin) * millisecondsPerTick)
            : 0.0f;

        m_latestSections.clear();
        m_latestSections.reserve(frame.usedSections);
        // 読み出す区間番号
        for (std::size_t index{}; index < frame.usedSections; ++index)
        {
            // 区間開始の時刻値
            const auto sectionBegin = m_mappedTimestamps[
                base + 2u + index * 2u];
            // 区間終了の時刻値
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
            // 読み出した描画統計
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
