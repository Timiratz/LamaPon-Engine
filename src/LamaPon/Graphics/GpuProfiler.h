#pragma once

#include "LamaPon/Core/Profiler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // LOD・カリング・インスタンス処理後にGPUが実行した処理量。
    struct GpuPipelineStatistics final
    {
        // 入力アセンブラーの頂点数
        std::uint64_t inputAssemblerVertices{};
        // 入力アセンブラーのプリミティブ数
        std::uint64_t inputAssemblerPrimitives{};
        // 頂点シェーダーの実行数
        std::uint64_t vertexShaderInvocations{};
        // 画素シェーダーの実行数
        std::uint64_t pixelShaderInvocations{};
        // ハルシェーダーの実行数
        std::uint64_t hullShaderInvocations{};
        // ドメインシェーダーの実行数
        std::uint64_t domainShaderInvocations{};
        // ジオメトリーシェーダーの実行数
        std::uint64_t geometryShaderInvocations{};
        // 計算シェーダーの実行数
        std::uint64_t computeShaderInvocations{};
        // 計測結果の有効有無
        bool valid{};
    };

    // 入れ子の時間は重複するため、全体との比較はdepthが0の区間だけを加算する。
    struct GpuSectionTime final
    {
        // 計測した区間名
        std::string name;
        // 区間の経過ミリ秒
        float milliseconds{};
        // 入れ子の深さ、0は最上位
        std::uint32_t depth{};
    };

    // バックエンドが所有し、計測窓口が借用する描画API固有の計測実装。
    class GpuProfilerBackend
    {
    public:
        // 派生側を含むGPU計測の所有状態を解放する。
        virtual ~GpuProfilerBackend() = default;

        // 計測実装の基底を作成する。
        GpuProfilerBackend() = default;
        // 計測実装のコピーを禁止する。
        GpuProfilerBackend(const GpuProfilerBackend&) = delete;
        // 計測実装のコピー代入を禁止する。
        GpuProfilerBackend& operator=(
            const GpuProfilerBackend&) = delete;

        // GPU区間の計測に対応しているか判定する。
        [[nodiscard]] virtual bool
            IsSupported() const noexcept = 0;
        // 計測フレームを開始し、開始済みなら無処理とする。
        virtual void OpenFrame() = 0;
        // GPU計測を開始して成功有無を返す(name: 区間名, depth: 計測済み区間の入れ子深さ)。
        [[nodiscard]] virtual bool BeginSection(
            std::string_view name,
            std::uint32_t depth) = 0;
        // 成功した直近のGPU計測区間を終了する。
        virtual void EndSection() noexcept = 0;
        // フレームを閉じて、準備済みの過去フレームの計測を取り込む。
        virtual void CloseFrame() = 0;

        // 直近に確定した区間一覧を借用する。
        [[nodiscard]] virtual const std::vector<GpuSectionTime>&
            LatestSections() const noexcept = 0;
        // 直近の確定フレームのGPU経過ミリ秒を返す。
        [[nodiscard]] virtual float
            LatestFrameMilliseconds() const noexcept = 0;
        // 直近の確定フレームのGPU処理量を借用する。
        [[nodiscard]] virtual const GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept = 0;

        // デバッグマーカーの開始成功有無を返し、既定実装は開始しない(name: マーカー名)。
        [[nodiscard]] virtual bool BeginMarker(
            std::string_view name) noexcept
        {
            static_cast<void>(name);
            return false;
        }
        // 開始済みマーカーを閉じ、既定実装は無処理とする。
        virtual void EndMarker() noexcept {}
    };

    // 区間と同じ順の入れ子で、例外を送出せずに描画パスの開始・終了を受け取る通知先。
    class GpuSectionListener
    {
    public:
        // 派生側を含む区間通知先を解放する。
        virtual ~GpuSectionListener() = default;

        // 区間通知先の基底を作成する。
        GpuSectionListener() = default;
        // 区間通知先のコピーを禁止する。
        GpuSectionListener(const GpuSectionListener&) = delete;
        // 区間通知先のコピー代入を禁止する。
        GpuSectionListener& operator=(
            const GpuSectionListener&) = delete;

        // 区間の開始を通知する(name: 開始する区間名)。
        virtual void OnGpuSectionBegin(
            std::string_view name) noexcept = 0;
        // 対応する直近の区間の終了を通知する。
        virtual void OnGpuSectionEnd() noexcept = 0;
    };

    // GPU区間をCPU計測・デバッグマーカー・区間通知先にも伝える共通窓口。
    // GPU計測が未接続・非対応ならGPU結果は空とするが、CPU計測と通知は継続する。
    class GpuProfiler final
    {
    public:
        // 計測結果の公開型名を維持する。
        using PipelineStatistics = GpuPipelineStatistics;
        using SectionTime = GpuSectionTime;

        // 構築後に開始した全区間を閉じ、接続世代が変わった区間には触れないスコープ。
        class SectionScope final
        {
        public:
            // 復帰する深さと接続世代を保存して区間を開始する(profiler: 借用する計測窓口, name: 区間名)。
            // 計測窓口はスコープより長く保持する。
            SectionScope(
                GpuProfiler& profiler,
                std::string_view name);
            // 構築時の深さまで区間を閉じる。
            ~SectionScope() noexcept;

            // 同じ接続世代の区間を構築時の深さまで閉じ、再呼出は無処理とする。
            void End() noexcept;
            // 構築前に開いていた全区間の数を返す。
            [[nodiscard]] std::size_t InitialDepth() const noexcept
            {
                return m_initialDepth;
            }

            // 区間スコープのコピーを禁止する。
            SectionScope(const SectionScope&) = delete;
            // 区間スコープのコピー代入を禁止する。
            SectionScope& operator=(const SectionScope&) = delete;

        private:
            // スコープ中に借用する計測窓口
            GpuProfiler* m_profiler{};
            // 構築前に開いていた全区間数
            std::size_t m_initialDepth{};
            // 構築時のバックエンド接続世代
            std::uint64_t m_generation{};
        };


        // 現在の接続を解除して計測実装を借用する(backend: 計測実装、ヌルなら未接続)。
        // バックエンドは本体より長く保持し、破棄前にDetachする。
        void Attach(GpuProfilerBackend* backend) noexcept;
        // 開いている全区間を終了してバックエンド参照を解除し、接続世代を進める。
        void Detach() noexcept;

        // 対応するバックエンドの計測フレームを開始する。
        void OpenFrame();
        // GPU・CPU計測とマーカー・通知の区間を開始する(name: 区間名)。
        // 保存領域の確保やバックエンドの開始失敗は例外で返す。
        void BeginSection(std::string_view name);
        // 直近の区間を終了し、開始区間がなければ無処理とする。
        void EndSection();

        // 区間の通知先を借用し、ヌルなら解除する(listener: 開始・終了の通知先)。
        // 開いた区間を全て閉じてから変更し、通知先は本体より長く保持するか破棄前に解除する。
        void SetSectionListener(
            GpuSectionListener* listener) noexcept;
        // 開いている全区間を閉じ、対応するGPUフレームの計測を確定する。
        void CloseFrame();

        // 接続済みの計測実装がGPU計測に対応しているか判定する。
        [[nodiscard]] bool IsSupported() const noexcept;
        // 直近の確定区間一覧を借用し、未接続・非対応なら空の一覧を返す。
        [[nodiscard]] const std::vector<SectionTime>&
            LatestSections() const noexcept;
        // 直近のGPU経過ミリ秒を返し、未接続・非対応なら0を返す。
        [[nodiscard]] float
            LatestFrameMilliseconds() const noexcept;
        // 直近のGPU処理量を借用し、未接続・非対応なら無効な値を返す。
        [[nodiscard]] const PipelineStatistics&
            LatestPipelineStatistics() const noexcept;

    private:
        // 開始できた通知先だけを終了するための区間状態。
        struct OpenSection final
        {
            // CPU計測の開始トークン
            ProfileScopeToken cpuScope;
            // CPU計測の開始時刻
            std::chrono::steady_clock::time_point cpuStart;
            // GPU計測の開始成功有無
            bool timed{};
            // マーカーの開始成功有無
            bool marker{};
            // 区間開始の通知済み有無
            bool listened{};
        };

        // 指定した全区間数になるまで末尾の区間を終了する(depth: 復帰する区間数)。
        void EndSectionsToDepth(std::size_t depth) noexcept;
        // 開始済みの通知先だけを閉じ、末尾の一区間を解除する。
        void EndTopSection() noexcept;

        // 借用するGPU計測実装
        GpuProfilerBackend* m_backend{};
        // 借用する区間通知先
        GpuSectionListener* m_listener{};
        // 開いている全区間の状態
        std::vector<OpenSection> m_sections;
        // バックエンドの深さは計測開始に成功した区間だけを数え、スコープの復帰は全区間数で行う。
        // 計測開始に成功した区間の深さ
        std::size_t m_timedDepth{};
        // 古いスコープによる新しい区間の終了を防ぐ。
        // 接続変更を識別する世代番号
        std::uint64_t m_generation{};
    };
}
