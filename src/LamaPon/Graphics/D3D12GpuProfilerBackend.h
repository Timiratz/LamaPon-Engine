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
        // 計測資源を準備し、失敗時は時間計測を無効にする(backend: 寿命が長い描画基盤)。
        explicit D3D12GpuProfilerBackend(D3D12Backend& backend) noexcept;
        // 読取領域のマップを解除して計測資源を解放する。
        ~D3D12GpuProfilerBackend() override;

        // GPU時間計測が利用可能か返す。
        [[nodiscard]] bool IsSupported() const noexcept override;
        // 再利用可能なスロットでフレーム計測を開始する。
        void OpenFrame() override;
        // 計測区間を開始し、成功を返す(name: 区間の表示名, depth: 入れ子の深さ)。
        // 一フレームの128区間を超えた場合は計測を追加せず偽を返す。
        [[nodiscard]] bool BeginSection(
            std::string_view name,
            std::uint32_t depth) override;
        // 最後に開始した計測区間を終了する。
        void EndSection() noexcept override;
        // 未完了区間を閉じてフレーム計測を終了する。
        void CloseFrame() override;
        // 次回更新まで有効な直近の区間一覧を借用する。
        [[nodiscard]] const std::vector<GpuSectionTime>&
            LatestSections() const noexcept override;
        // 直近のフレーム時間をミリ秒で返す。
        [[nodiscard]] float LatestFrameMilliseconds() const noexcept override;
        // 次回更新まで有効な直近の描画統計を借用する。
        [[nodiscard]] const GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept override;
        // 描画マーカーを開始し、成功を返す(name: マーカーの表示名)。
        [[nodiscard]] bool BeginMarker(
            std::string_view name) noexcept override;
        // 最後に開始した描画マーカーを終了する。
        void EndMarker() noexcept override;


        // マーカーと統計クエリを閉じ、今回の統計を無効にする(commands: 閉じる任意のコマンド一覧)。
        void BeforeCommandListClose(
            ID3D12GraphicsCommandList* commands) noexcept;

    private:
        // 計測するバックバッファー数
        static constexpr std::size_t FrameCount = 2;
        // 一フレームの最大計測区間数
        static constexpr std::size_t MaximumSections = 128;
        // 一フレームの時刻クエリ数
        static constexpr std::size_t TimestampCountPerFrame =
            2u + MaximumSections * 2u;

        struct Section final
        {
            // 区間の表示名
            std::string name;
            // 区間の入れ子の深さ
            std::uint32_t depth{};
        };

        struct Frame final
        {
            // 再利用する計測区間
            std::vector<Section> sections;
            // 未完了区間の番号スタック
            std::vector<std::size_t> sectionStack;
            // このフレームの使用区間数
            std::size_t usedSections{};
            // フレーム計測中
            bool open{};
            // 計測結果の読取待ち
            bool pending{};
            // 描画統計のクエリ開始済み
            bool pipelineStatisticsOpen{};
            // このフレームの描画統計が有効
            bool pipelineStatisticsValid{};
        };

        // 初期化済みの描画基盤に計測資源と読取領域を作る。
        void Initialize();
        // GPU完了後にのみ結果を読み、一覧を更新する(frameIndex: 有効なスロット番号)。
        // この非送出処理でも区間一覧の確保が必要なため、確保失敗は呼出し元で回復できない。
        void ReadCompletedFrame(std::size_t frameIndex) noexcept;
        // スロット先頭の時刻クエリ番号を求める(frameIndex: 有効なスロット番号)。
        [[nodiscard]] std::size_t TimestampBase(
            std::size_t frameIndex) const noexcept;

        // 借用するD3D12の描画基盤
        D3D12Backend* m_backend{};
        // 時刻クエリのヒープ
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestampHeap;
        // 描画統計クエリのヒープ
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_pipelineStatisticsHeap;
        // CPU読取用の時刻資源
        Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;
        // CPU読取用の描画統計資源
        Microsoft::WRL::ComPtr<ID3D12Resource>
            m_pipelineStatisticsReadback;
        // 常時マップした時刻領域
        std::uint64_t* m_mappedTimestamps{};
        // 常時マップした描画統計領域
        D3D12_QUERY_DATA_PIPELINE_STATISTICS*
            m_mappedPipelineStatistics{};
        // バックバッファー別の計測状態
        std::array<Frame, FrameCount> m_frames;
        // 計測中のスロット番号
        std::size_t m_activeFrame{};
        // 一秒当たりの時刻刻み数
        std::uint64_t m_frequency{};
        // GPU時間計測が利用可能
        bool m_supported{};
        // GPU描画統計が利用可能
        bool m_pipelineStatisticsSupported{};
        // 直近に取得した区間一覧
        std::vector<GpuSectionTime> m_latestSections;
        // 直近のフレーム時間、ミリ秒
        float m_latestFrameMilliseconds{};
        // 直近に取得した描画統計
        GpuPipelineStatistics m_latestPipelineStatistics;
        // 途中でコマンド一覧を閉じたマーカーは、対応するEndMarkerで再終了させない。
        // 現在の一覧で閉じるマーカー
        std::vector<bool> m_markerStack;
    };
}
