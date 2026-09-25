#pragma once

#include "LamaPon/Graphics/GpuProfiler.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace LamaPon
{
    class D3D12Backend;

    class D3D12GpuProfilerBackend final : public GpuProfilerBackend
    {
    public:
        explicit D3D12GpuProfilerBackend(D3D12Backend& backend) noexcept;
        ~D3D12GpuProfilerBackend() override;

        [[nodiscard]] bool IsSupported() const noexcept override;
        void OpenFrame() override;
        [[nodiscard]] bool BeginSection(
            std::string_view name,
            std::uint32_t depth) override;
        void EndSection() noexcept override;
        void CloseFrame() override;
        [[nodiscard]] const std::vector<GpuSectionTime>&
            LatestSections() const noexcept override;
        [[nodiscard]] float LatestFrameMilliseconds() const noexcept override;
        [[nodiscard]] const GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept override;
        [[nodiscard]] bool BeginMarker(
            std::string_view name) noexcept override;
        void EndMarker() noexcept override;

        // Capture等がフレーム途中でcommand listを閉じる前に、開いている
        // pipeline queryを閉じます。そのフレームの統計だけは無効です。
        void BeforeCommandListClose(
            ID3D12GraphicsCommandList* commands) noexcept;

    private:
        static constexpr std::size_t FrameCount = 2;
        static constexpr std::size_t MaximumSections = 128;
        static constexpr std::size_t TimestampCountPerFrame =
            2u + MaximumSections * 2u;

        struct Section final
        {
            std::string name;
            std::uint32_t depth{};
        };

        struct Frame final
        {
            std::vector<Section> sections;
            std::vector<std::size_t> sectionStack;
            std::size_t usedSections{};
            bool open{};
            bool pending{};
            bool pipelineStatisticsOpen{};
            bool pipelineStatisticsValid{};
        };

        void Initialize();
        void ReadCompletedFrame(std::size_t frameIndex) noexcept;
        [[nodiscard]] std::size_t TimestampBase(
            std::size_t frameIndex) const noexcept;

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_pipelineStatisticsHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;
        Microsoft::WRL::ComPtr<ID3D12Resource>
            m_pipelineStatisticsReadback;
        std::uint64_t* m_mappedTimestamps{};
        D3D12_QUERY_DATA_PIPELINE_STATISTICS*
            m_mappedPipelineStatistics{};
        std::array<Frame, FrameCount> m_frames;
        std::size_t m_activeFrame{};
        std::uint64_t m_frequency{};
        bool m_supported{};
        bool m_pipelineStatisticsSupported{};
        std::vector<GpuSectionTime> m_latestSections;
        float m_latestFrameMilliseconds{};
        GpuPipelineStatistics m_latestPipelineStatistics;
        // 開いているdebug markerごとに、現在のcommand listでEndEventが
        // 必要かどうかです。途中でcommand listを閉じるときに全て閉じ、
        // 以降の対応するEndMarkerでは何もしません。
        std::vector<bool> m_markerStack;
    };
}
