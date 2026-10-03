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
        // 最上位区間の添字一覧
        std::vector<std::uint32_t> roots;
        // 各区間の子の添字、出現順
        std::vector<std::vector<std::uint32_t>> children;
        // 子区間を除いた時間、ミリ秒
        std::vector<double> selfMilliseconds;
        // 最上位区間の合計時間、ミリ秒
        double rootMilliseconds{};
    };

    // 同名の区間を木全体で合算した1行です。
    // 自己時間の大きい順の一覧で、どこから呼ばれたかに関係なく重い処理を探すために使います。
    struct ProfileFlatEntry final
    {
        // 集計する計測区間名
        std::string name;
        // 子を含む累積時間、ミリ秒
        double totalMilliseconds{};
        // 子を除く累積時間、ミリ秒
        double selfMilliseconds{};
        // 同名区間の合計呼出数
        std::uint64_t calls{};
    };

    // フレームの親子関係と自己時間を求めます(frame: 計測フレーム)。
    // 無効な親は最上位として扱い、子区間を引いた自己時間は最小0に制限します。
    [[nodiscard]] LAMAPON_API ProfileFrameTree BuildProfileFrameTree(
        const ProfileFrame& frame);
    // 同名区間を合算して自己時間の降順で返します(frame: 計測フレーム)。
    // 同名区間が自身の中で入れ子になっている場合、合計時間は重複して数えます（自己時間は重複しません）。
    [[nodiscard]] LAMAPON_API std::vector<ProfileFlatEntry>
        FlattenProfileFrame(const ProfileFrame& frame);

    // 複数フレームにわたる1つの値の分布です。
    // 時間値の単位はミリ秒とし、countが0の場合は他の値も全て0です。
    struct ProfileValueStatistics final
    {
        // 分布のサンプル数
        std::size_t count{};
        // 分布の最小値
        double minimum{};
        // 分布の最大値
        double maximum{};
        // 分布の算術平均値
        double mean{};
        // 分布の中央値
        double median{};
        // 95パーセンタイル値
        double percentile95{};
        // 全サンプルの合計値
        double total{};
        // 最小値を記録したフレーム番号
        std::uint64_t minimumFrameIndex{};
        // 最大値を記録したフレーム番号
        std::uint64_t maximumFrameIndex{};
    };

    // 1つの呼び出し経路（例: Render/Scene.Render）の統計です。
    // 同じ名前でも親が違う区間は別の経路として集計します。
    struct ProfileMarkerStatistics final
    {
        // 親から区間名を連結した経路
        std::string path;
        // 経路末尾の計測区間名
        std::string name;
        // 区間の深さ、最上位は0
        std::uint32_t depth{};
        // 子を含む時間分布、ミリ秒
        ProfileValueStatistics milliseconds;
        // 子を除く時間分布、ミリ秒
        ProfileValueStatistics selfMilliseconds;
        // 経路内の合計呼出数
        std::uint64_t totalCalls{};
        // 区間が現れたフレーム数
        std::size_t presentFrameCount{};
    };

    struct ProfileAnalysis final
    {
        // 解析したフレーム数
        std::size_t frameCount{};
        // 入力の先頭フレーム番号
        std::uint64_t firstFrameIndex{};
        // 入力の末尾フレーム番号
        std::uint64_t lastFrameIndex{};
        // フレーム時間分布、ミリ秒
        ProfileValueStatistics frameMilliseconds;
        // 初出順の区間経路ごとの統計
        std::vector<ProfileMarkerStatistics> markers;

        // 指定経路の統計を返し、存在しなければnullを返します(path: 親から連結した区間経路)。
        [[nodiscard]] LAMAPON_API const ProfileMarkerStatistics*
            FindMarker(std::string_view path) const noexcept;
    };

    // 同じ経路の区間をAとBで比べた結果です。
    // 時間差はミリ秒単位のB－Aで、正の値はBの方が遅いことを示します。
    struct ProfileMarkerComparison final
    {
        // 比較する計測区間の経路
        std::string path;
        // 経路末尾の計測区間名
        std::string name;
        // 区間の深さ、最上位は0
        std::uint32_t depth{};
        // 基準側に区間が存在するか
        bool presentInA{};
        // 比較側に区間が存在するか
        bool presentInB{};
        // 基準側の時間中央値、ミリ秒
        double medianA{};
        // 比較側の時間中央値、ミリ秒
        double medianB{};
        // 基準側の平均時間、ミリ秒
        double meanA{};
        // 比較側の平均時間、ミリ秒
        double meanB{};
        // 比較側－基準側の中央値差
        double medianDifference{};
        // 比較側－基準側の平均値差
        double meanDifference{};
        // 基準の中央値に対する増減比
        double medianRelativeChange{};
        // 基準側の出現フレーム当たり呼出数
        double callsPerFrameA{};
        // 比較側の出現フレーム当たり呼出数
        double callsPerFrameB{};
    };

    struct ProfileComparison final
    {
        // 比較基準の解析結果
        ProfileAnalysis a;
        // 比較対象の解析結果
        ProfileAnalysis b;
        // フレーム中央値差、比較－基準
        double frameMedianDifference{};
        // フレーム平均値差、比較－基準
        double frameMeanDifference{};
        // 中央値差の絶対値降順の区間比較
        std::vector<ProfileMarkerComparison> markers;
    };

    // 全フレームと区間経路ごとの時間分布を解析します(frames: 計測フレーム列)。
    // 空の列には空の結果を返し、区間の分布はその区間が存在するフレームだけで求めます。
    [[nodiscard]] LAMAPON_API ProfileAnalysis AnalyzeProfile(
        std::span<const ProfileFrame> frames);
    // 2つの解析結果で同じ経路の区間を比較します(a: 比較基準の解析結果, b: 比較対象の解析結果)。
    // 差はb－aで、相対変化率はaの中央値が正の場合だけ求め、それ以外は0とします。
    [[nodiscard]] LAMAPON_API ProfileComparison CompareProfiles(
        const ProfileAnalysis& a,
        const ProfileAnalysis& b);

    // 指定経路の合計時間をフレーム順で返します(frames: 計測フレーム列, path: 区間経路)。
    // 区間が現れないフレームの値は0です。
    [[nodiscard]] LAMAPON_API std::vector<double>
        MarkerMillisecondsPerFrame(
            std::span<const ProfileFrame> frames,
            std::string_view path);

    // 計測JSONをファイルから読み込みます(path: 読み込み元, frames: 成功時の出力先, error: 診断出力、不要ならnull)。
    // version 1は階層情報を持たないため、全区間を最上位として扱います。
    // 失敗時はfalseを返し、errorへ理由を入れます（framesは変更しません）。
    [[nodiscard]] LAMAPON_API bool LoadProfileJson(
        const std::filesystem::path& path,
        std::vector<ProfileFrame>& frames,
        std::string* error = nullptr);
    // 計測JSONの文字列を解釈します(text: JSON本文, frames: 成功時の出力先, error: 診断出力、不要ならnull)。
    // 版番号1と2を読み、失敗時はfalseを返してframesを変更しません。
    [[nodiscard]] LAMAPON_API bool ParseProfileJson(
        std::string_view text,
        std::vector<ProfileFrame>& frames,
        std::string* error = nullptr);
}
