#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // D3D11のタイムスタンプクエリでGPUの区間時間を計測します。
    // 結果の読み出しはGPUに追いつかれないよう数フレーム遅らせる
    // ため、表示される値は直近の「確定した」フレームのものです。
    // クエリ作成に失敗した環境では自動的に無効になります。
    class GpuProfiler final
    {
    public:
        // CPU側のオブジェクト数ではなく、LOD・カリング・
        // インスタンシングを通過してGPUへ届いた実仕事量です。
        struct PipelineStatistics final
        {
            std::uint64_t inputAssemblerVertices{};
            std::uint64_t inputAssemblerPrimitives{};
            std::uint64_t vertexShaderInvocations{};
            std::uint64_t pixelShaderInvocations{};
            std::uint64_t hullShaderInvocations{};
            std::uint64_t domainShaderInvocations{};
            std::uint64_t geometryShaderInvocations{};
            std::uint64_t computeShaderInvocations{};
            bool valid{};
        };

        struct SectionTime final
        {
            std::string name;
            float milliseconds{};
            // 入れ子の深さ（0が最上位）。区間は入れ子にできるので、
            // 一覧をそのまま足すと内側が二重に数えられます。
            // GPU全体との比較は depth==0 だけを足してください。
            std::uint32_t depth{};
        };

        void Initialize(
            ID3D11Device* device,
            ID3D11DeviceContext* context);

        // フレームの計測を開始します（開始済みなら何もしません）。
        void OpenFrame();
        // 区間の計測を開始/終了します（入れ子可）。
        void BeginSection(std::string_view name);
        void EndSection();
        // フレームを閉じ、準備できた過去フレームの結果を
        // 取り込みます。
        void CloseFrame();

        [[nodiscard]] bool IsSupported() const noexcept
        {
            return m_supported;
        }
        // 直近の確定フレームの区間一覧。
        [[nodiscard]] const std::vector<SectionTime>&
            LatestSections() const noexcept
        {
            return m_latestSections;
        }
        // 直近の確定フレームのGPU全体時間（ミリ秒）。
        [[nodiscard]] float
            LatestFrameMilliseconds() const noexcept
        {
            return m_latestFrameMilliseconds;
        }
        [[nodiscard]] const PipelineStatistics&
            LatestPipelineStatistics() const noexcept
        {
            return m_latestPipelineStatistics;
        }

    private:
        struct SectionQueries final
        {
            std::string name;
            Microsoft::WRL::ComPtr<ID3D11Query> begin;
            Microsoft::WRL::ComPtr<ID3D11Query> end;
            std::uint32_t depth{};
        };

        struct FrameQueries final
        {
            Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
            Microsoft::WRL::ComPtr<ID3D11Query>
                frameBegin;
            Microsoft::WRL::ComPtr<ID3D11Query> frameEnd;
            Microsoft::WRL::ComPtr<ID3D11Query>
                pipelineStatistics;
            std::vector<SectionQueries> sections;
            // 今フレームで使った区間数（クエリはプール再利用）。
            std::size_t usedSections{};
            bool open{};
            bool pending{};
        };

        [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Query>
            CreateTimestampQuery() const;
        void PollPendingFrames();

        // 読み出し遅延用のリングバッファ。
        static constexpr std::size_t FrameCount = 4;
        std::array<FrameQueries, FrameCount> m_frames;
        std::size_t m_writeIndex{};
        std::vector<std::size_t> m_sectionStack;

        ID3D11Device* m_device{};
        ID3D11DeviceContext* m_context{};
        bool m_supported{};

        std::vector<SectionTime> m_latestSections;
        float m_latestFrameMilliseconds{};
        PipelineStatistics m_latestPipelineStatistics;
    };
}
