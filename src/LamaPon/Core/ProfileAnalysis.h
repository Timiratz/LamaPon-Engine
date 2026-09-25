#pragma once

#include "LamaPon/Core/Api.h"
#include "LamaPon/Core/Profiler.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 1フレームの区間を呼び出し木として辿るための補助情報です。
    // 添字はProfileFrame::samplesの位置です。
    struct ProfileFrameTree final
    {
        std::vector<std::uint32_t> roots;
        // children[i]はsamples[i]の子の添字を出現順に持ちます。
        std::vector<std::vector<std::uint32_t>> children;
        // 子区間の合計を除いた時間です（0未満は0に丸めます）。
        std::vector<double> selfMilliseconds;
        // 最上位区間の合計です。フレーム時間との差が未計測の時間です。
        double rootMilliseconds{};
    };

    // 同名の区間を木全体で合算した1行です。自己時間の大きい順の一覧で、
    // どこから呼ばれたかに関係なく重い処理を探すために使います。
    struct ProfileFlatEntry final
    {
        std::string name;
        double totalMilliseconds{};
        double selfMilliseconds{};
        std::uint64_t calls{};
    };

    [[nodiscard]] LAMAPON_API ProfileFrameTree BuildProfileFrameTree(
        const ProfileFrame& frame);
    // 自己時間の大きい順に返します。同名区間が自身の中で入れ子になって
    // いる場合、合計時間は重複して数えます（自己時間は重複しません）。
    [[nodiscard]] LAMAPON_API std::vector<ProfileFlatEntry>
        FlattenProfileFrame(const ProfileFrame& frame);

    // 複数フレームにわたる1つの値の分布です。countが0の場合は
    // 他の値もすべて0です。
    struct ProfileValueStatistics final
    {
        std::size_t count{};
        double minimum{};
        double maximum{};
        double mean{};
        double median{};
        // 上位5%を除いた最大値です。ヒッチの影響を受けにくい「ほぼ最悪」
        // の目安として使います。
        double percentile95{};
        double total{};
        std::uint64_t minimumFrameIndex{};
        std::uint64_t maximumFrameIndex{};
    };

    // 1つの呼び出し経路（例: Render/Scene.Render）の統計です。
    // 同じ名前でも親が違う区間は別の経路として集計します。
    struct ProfileMarkerStatistics final
    {
        // 親から順に区間名を"/"で連結した識別子です。
        std::string path;
        std::string name;
        std::uint32_t depth{};
        // 区間が現れたフレームでの合計時間（子を含む）の分布です。
        ProfileValueStatistics milliseconds;
        // 子区間を除いた自身の時間の分布です。
        ProfileValueStatistics selfMilliseconds;
        std::uint64_t totalCalls{};
        // 解析したフレームのうち区間が現れたフレーム数です。
        std::size_t presentFrameCount{};
    };

    struct ProfileAnalysis final
    {
        std::size_t frameCount{};
        std::uint64_t firstFrameIndex{};
        std::uint64_t lastFrameIndex{};
        ProfileValueStatistics frameMilliseconds;
        // 最初に現れた順（親は子より前）に並びます。
        std::vector<ProfileMarkerStatistics> markers;

        [[nodiscard]] LAMAPON_API const ProfileMarkerStatistics*
            FindMarker(std::string_view path) const noexcept;
    };

    // 同じ経路の区間をAとBで比べた結果です。differenceはB - Aで、
    // 正の値はBの方が遅いことを示します。
    struct ProfileMarkerComparison final
    {
        std::string path;
        std::string name;
        std::uint32_t depth{};
        bool presentInA{};
        bool presentInB{};
        double medianA{};
        double medianB{};
        double meanA{};
        double meanB{};
        double medianDifference{};
        double meanDifference{};
        // Aの中央値に対する変化率です。Aに無い区間は0で、
        // presentInAを見て「新規」と表示してください。
        double medianRelativeChange{};
        double callsPerFrameA{};
        double callsPerFrameB{};
    };

    struct ProfileComparison final
    {
        ProfileAnalysis a;
        ProfileAnalysis b;
        double frameMedianDifference{};
        double frameMeanDifference{};
        // 中央値の差の絶対値が大きい順に並びます。
        std::vector<ProfileMarkerComparison> markers;
    };

    // framesの全フレームを解析します。空の場合は空の結果を返します。
    [[nodiscard]] LAMAPON_API ProfileAnalysis AnalyzeProfile(
        std::span<const ProfileFrame> frames);
    [[nodiscard]] LAMAPON_API ProfileComparison CompareProfiles(
        const ProfileAnalysis& a,
        const ProfileAnalysis& b);

    // 経路pathの区間の、フレームごとの合計時間です。区間が現れない
    // フレームは0です。分布のグラフ表示に使います。
    [[nodiscard]] LAMAPON_API std::vector<double>
        MarkerMillisecondsPerFrame(
            std::span<const ProfileFrame> frames,
            std::string_view path);

    // WriteProfileJsonが書いたJSON（version 1と2）を読みます。
    // version 1は階層情報を持たないため、全区間を最上位として扱います。
    // 失敗時はfalseを返し、errorへ理由を入れます（framesは変更しません）。
    [[nodiscard]] LAMAPON_API bool LoadProfileJson(
        const std::filesystem::path& path,
        std::vector<ProfileFrame>& frames,
        std::string* error = nullptr);
    // 文字列から読みます。LoadProfileJsonの本体で、テストからも使います。
    [[nodiscard]] LAMAPON_API bool ParseProfileJson(
        std::string_view text,
        std::vector<ProfileFrame>& frames,
        std::string* error = nullptr);
}
