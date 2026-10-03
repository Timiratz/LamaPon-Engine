#include "LamaPon/Core/ProfileAnalysis.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace
{
    struct ValueSample final
    {
        // 統計対象の時間、ミリ秒
        double value{};
        // 値を計測したフレーム番号
        std::uint64_t frameIndex{};
    };

    // 時間分布の統計を求めます(values: フレーム番号付き時間サンプルの複製)。
    LamaPon::ProfileValueStatistics Summarize(
        std::vector<ValueSample> values)
    {
        // 時間分布の集計結果
        LamaPon::ProfileValueStatistics statistics;
        if (values.empty())
        {
            return statistics;
        }

        statistics.count = values.size();
        statistics.minimum = values.front().value;
        statistics.maximum = values.front().value;
        statistics.minimumFrameIndex = values.front().frameIndex;
        statistics.maximumFrameIndex = values.front().frameIndex;
        // 分布を集計する時間サンプル
        for (const auto& sample : values)
        {
            statistics.total += sample.value;
            if (sample.value < statistics.minimum)
            {
                statistics.minimum = sample.value;
                statistics.minimumFrameIndex = sample.frameIndex;
            }
            if (sample.value > statistics.maximum)
            {
                statistics.maximum = sample.value;
                statistics.maximumFrameIndex = sample.frameIndex;
            }
        }
        statistics.mean =
            statistics.total
            / static_cast<double>(values.size());

        std::ranges::sort(
            values,
            {},
            &ValueSample::value);
        // 昇順サンプル列の中央位置
        const std::size_t middle = values.size() / 2;
        statistics.median = values.size() % 2 == 0
            ? (values[middle - 1].value + values[middle].value)
                * 0.5
            : values[middle].value;
        // 95パーセンタイルの順位
        // nearest-rank法で実在するサンプルを選びます。
        const auto rank = static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(values.size())));
        statistics.percentile95 =
            values[std::clamp<std::size_t>(rank, 1, values.size()) - 1]
                .value;
        return statistics;
    }

    struct MarkerAccumulator final
    {
        // 集計する区間の末尾の名前
        std::string name;
        // 集計する区間の入れ子の深さ
        std::uint32_t depth{};
        // 子を含む時間のサンプル列
        std::vector<ValueSample> milliseconds;
        // 子を除く時間のサンプル列
        std::vector<ValueSample> selfMilliseconds;
        // 全フレームでの累積呼出数
        std::uint64_t totalCalls{};
    };

    struct SamplePath final
    {
        // 親から区間名を連結した経路
        std::string path;
        // 区間の深さ、最上位は0
        std::uint32_t depth{};
    };

    // 親の添字が自身より前かを返します(sample: 計測区間の記録, index: 自身の添字)。
    [[nodiscard]] bool HasValidParent(
        const LamaPon::ProfileSample& sample,
        const std::size_t index) noexcept
    {
        return sample.parent != LamaPon::ProfileSample::NoParent
            && sample.parent < index;
    }

    // 区間名を親から連結した経路を作ります(samples: フレーム内の計測区間列)。
    // 親は子より前の添字だけを採用し、それ以外は最上位として扱います。
    std::vector<SamplePath> BuildPaths(
        const std::vector<LamaPon::ProfileSample>& samples)
    {
        // サンプルの添字に対応する経路列
        std::vector<SamplePath> paths;
        paths.reserve(samples.size());
        // 経路を作成する計測区間の位置
        for (std::size_t index{}; index < samples.size(); ++index)
        {
            // 経路を作成する計測区間
            const auto& sample = samples[index];
            if (HasValidParent(sample, index))
            {
                // 経路を引き継ぐ親区間の情報
                const auto& parent = paths[sample.parent];
                paths.push_back({
                    parent.path + "/" + sample.name,
                    parent.depth + 1 });
            }
            else
            {
                paths.push_back({ sample.name, 0 });
            }
        }
        return paths;
    }

    // 診断を指定先へ格納します(error: 出力先、不要ならnull, message: 診断本文)。
    void SetError(std::string* error, std::string message)
    {
        if (error != nullptr)
        {
            *error = std::move(message);
        }
    }
}

namespace LamaPon
{
    ProfileFrameTree BuildProfileFrameTree(const ProfileFrame& frame)
    {
        // 計測区間の親子関係と自己時間
        ProfileFrameTree tree;
        // フレーム内の計測区間数
        const auto count = frame.samples.size();
        tree.children.resize(count);
        tree.selfMilliseconds.resize(count);
        // 木へ登録する計測区間の位置
        for (std::size_t index{}; index < count; ++index)
        {
            // 集計する計測区間の記録
            const auto& sample = frame.samples[index];
            tree.selfMilliseconds[index] = sample.milliseconds;
            if (HasValidParent(sample, index))
            {
                tree.children[sample.parent].push_back(
                    static_cast<std::uint32_t>(index));
                tree.selfMilliseconds[sample.parent] -=
                    sample.milliseconds;
            }
            else
            {
                tree.roots.push_back(
                    static_cast<std::uint32_t>(index));
                tree.rootMilliseconds += sample.milliseconds;
            }
        }
        // 負数を補正する区間の自己時間
        for (auto& self : tree.selfMilliseconds)
        {
            self = std::max(self, 0.0);
        }
        return tree;
    }

    std::vector<ProfileFlatEntry> FlattenProfileFrame(
        const ProfileFrame& frame)
    {
        // 自己時間を求めた区間の木
        const auto tree = BuildProfileFrameTree(frame);
        // 同名区間を合算した集計一覧
        std::vector<ProfileFlatEntry> entries;
        // 区間名に対応する集計先の位置
        std::unordered_map<std::string, std::size_t> indices;
        // 同名区間へ合算する記録の位置
        for (std::size_t index{}; index < frame.samples.size(); ++index)
        {
            // 集計する計測区間の記録
            const auto& sample = frame.samples[index];
            // entry: 同名区間の登録位置
            // inserted: 初回登録か
            const auto [entry, inserted] =
                indices.try_emplace(sample.name, entries.size());
            if (inserted)
            {
                entries.push_back({ sample.name });
            }
            // 同名区間の集計先
            auto& flat = entries[entry->second];
            flat.totalMilliseconds += sample.milliseconds;
            flat.selfMilliseconds += tree.selfMilliseconds[index];
            flat.calls += sample.callCount;
        }
        // 自己時間の降順を判定します(left: 比較元の区間集計, right: 比較先の区間集計)。
        std::ranges::stable_sort(
            entries,
            [](const ProfileFlatEntry& left,
                const ProfileFlatEntry& right)
            {
                return left.selfMilliseconds > right.selfMilliseconds;
            });
        return entries;
    }

    const ProfileMarkerStatistics* ProfileAnalysis::FindMarker(
        const std::string_view path) const noexcept
    {
        // 指定経路に一致した統計の位置
        // 指定経路と一致するかを判定します(candidate: 登録された区間統計)。
        const auto marker = std::ranges::find_if(
            markers,
            [path](const ProfileMarkerStatistics& candidate)
            {
                return candidate.path == path;
            });
        return marker == markers.end() ? nullptr : &*marker;
    }

    ProfileAnalysis AnalyzeProfile(
        const std::span<const ProfileFrame> frames)
    {
        // フレーム全体と経路ごとの統計
        ProfileAnalysis analysis;
        if (frames.empty())
        {
            return analysis;
        }

        analysis.frameCount = frames.size();
        analysis.firstFrameIndex = frames.front().index;
        analysis.lastFrameIndex = frames.back().index;

        // フレーム番号付き経過時間の列
        std::vector<ValueSample> frameTimes;
        frameTimes.reserve(frames.size());
        // 経路ごとの累積計測記録
        std::vector<MarkerAccumulator> accumulators;
        // 区間経路に対応する集計位置
        std::unordered_map<std::string, std::size_t> markerIndices;
        // 時間を集計する計測フレーム
        for (const auto& frame : frames)
        {
            frameTimes.push_back({ frame.milliseconds, frame.index });

            // 同じ経路の複数記録は1フレームで1値に合算します。
            // 各区間に対応する経路情報
            const auto paths = BuildPaths(frame.samples);
            // 各区間の子の合計時間、ミリ秒
            std::vector<double> childMilliseconds(
                frame.samples.size(),
                0.0);
            // 現在集計する計測区間の位置
            for (std::size_t index{};
                index < frame.samples.size();
                ++index)
            {
                // 現在集計する計測区間の記録
                const auto& sample = frame.samples[index];
                if (HasValidParent(sample, index))
                {
                    childMilliseconds[sample.parent] +=
                        sample.milliseconds;
                }
            }

            // 経路別の合計時間と自己時間
            std::unordered_map<std::string, std::pair<double, double>>
                frameValues;
            // フレーム内での経路の初出順
            std::vector<std::string> frameOrder;
            // 現在集計する計測区間の位置
            for (std::size_t index{};
                index < frame.samples.size();
                ++index)
            {
                // 現在集計する計測区間の記録
                const auto& sample = frame.samples[index];
                // 集計する区間の経路
                const auto& path = paths[index].path;
                // markerIndex: 経路の登録位置
                // inserted: 経路を初回登録したか
                auto [markerIndex, inserted] = markerIndices.try_emplace(
                    path,
                    accumulators.size());
                if (inserted)
                {
                    // 新規経路の累積計測記録
                    MarkerAccumulator accumulator;
                    accumulator.name = sample.name;
                    accumulator.depth = paths[index].depth;
                    accumulators.push_back(std::move(accumulator));
                }
                accumulators[markerIndex->second].totalCalls +=
                    sample.callCount;

                // value: フレーム内の経路別集計先
                // firstInFrame: このフレームで初出か
                auto [value, firstInFrame] =
                    frameValues.try_emplace(path, 0.0, 0.0);
                if (firstInFrame)
                {
                    frameOrder.push_back(path);
                }
                value->second.first += sample.milliseconds;
                // 子の合計が親を上回るのは計測誤差なので0に丸めます。
                value->second.second += std::max(
                    sample.milliseconds - childMilliseconds[index],
                    0.0);
            }
            // 初出順に処理する区間経路
            for (const auto& path : frameOrder)
            {
                // total: 子を含む合計ミリ秒
                // self: 子を除く合計ミリ秒
                const auto& [total, self] = frameValues[path];
                // 現在の経路の累積計測記録
                auto& accumulator =
                    accumulators[markerIndices[path]];
                accumulator.milliseconds.push_back(
                    { total, frame.index });
                accumulator.selfMilliseconds.push_back(
                    { self, frame.index });
            }
        }

        analysis.frameMilliseconds = Summarize(std::move(frameTimes));
        analysis.markers.reserve(accumulators.size());
        // 集計先の添字に対応する経路
        std::vector<std::string> pathsByIndex(accumulators.size());
        // path: 区間経路, index: 集計位置
        for (const auto& [path, index] : markerIndices)
        {
            pathsByIndex[index] = path;
        }
        // 統計へ変換する経路の集計位置
        for (std::size_t index{}; index < accumulators.size(); ++index)
        {
            // 統計へ変換する累積計測記録
            auto& accumulator = accumulators[index];
            // 完成した経路別の時間統計
            ProfileMarkerStatistics marker;
            marker.path = std::move(pathsByIndex[index]);
            marker.name = std::move(accumulator.name);
            marker.depth = accumulator.depth;
            marker.presentFrameCount =
                accumulator.milliseconds.size();
            marker.totalCalls = accumulator.totalCalls;
            marker.milliseconds =
                Summarize(std::move(accumulator.milliseconds));
            marker.selfMilliseconds =
                Summarize(std::move(accumulator.selfMilliseconds));
            analysis.markers.push_back(std::move(marker));
        }
        return analysis;
    }

    ProfileComparison CompareProfiles(
        const ProfileAnalysis& a,
        const ProfileAnalysis& b)
    {
        // 基準側と比較側の統計差分
        ProfileComparison comparison;
        comparison.a = a;
        comparison.b = b;
        comparison.frameMedianDifference =
            b.frameMilliseconds.median - a.frameMilliseconds.median;
        comparison.frameMeanDifference =
            b.frameMilliseconds.mean - a.frameMilliseconds.mean;

        // 区間が現れたフレーム当たりの呼出数を返します(marker: 区間経路の統計)。
        const auto callsPerFrame =
            [](const ProfileMarkerStatistics& marker)
            {
                return marker.presentFrameCount == 0
                    ? 0.0
                    : static_cast<double>(marker.totalCalls)
                        / static_cast<double>(
                            marker.presentFrameCount);
            };

        // 区間の比較結果を追加します(markerA: 基準側の統計、なければnull, markerB: 比較側の統計、なければnull)。
        // 少なくとも一方の統計への参照が必要です。
        const auto addMarker =
            [&comparison, &callsPerFrame](
                const ProfileMarkerStatistics* markerA,
                const ProfileMarkerStatistics* markerB)
            {
                // 名前や経路を転記する既存統計
                const auto& source =
                    markerA != nullptr ? *markerA : *markerB;
                // 現在の区間経路の比較結果
                ProfileMarkerComparison result;
                result.path = source.path;
                result.name = source.name;
                result.depth = source.depth;
                result.presentInA = markerA != nullptr;
                result.presentInB = markerB != nullptr;
                if (markerA != nullptr)
                {
                    result.medianA = markerA->milliseconds.median;
                    result.meanA = markerA->milliseconds.mean;
                    result.callsPerFrameA = callsPerFrame(*markerA);
                }
                if (markerB != nullptr)
                {
                    result.medianB = markerB->milliseconds.median;
                    result.meanB = markerB->milliseconds.mean;
                    result.callsPerFrameB = callsPerFrame(*markerB);
                }
                result.medianDifference =
                    result.medianB - result.medianA;
                result.meanDifference = result.meanB - result.meanA;
                if (result.medianA > 0.0)
                {
                    result.medianRelativeChange =
                        result.medianDifference / result.medianA;
                }
                comparison.markers.push_back(std::move(result));
            };

        // 比較基準側に存在する区間統計
        for (const auto& markerA : a.markers)
        {
            addMarker(&markerA, b.FindMarker(markerA.path));
        }
        // 比較対象側に存在する区間統計
        for (const auto& markerB : b.markers)
        {
            if (a.FindMarker(markerB.path) == nullptr)
            {
                addMarker(nullptr, &markerB);
            }
        }

        // 中央値差の絶対値の降順を判定します(left: 比較元の区間差分, right: 比較先の区間差分)。
        // 同じ差なら経路名順で表示順を安定させます。
        std::ranges::stable_sort(
            comparison.markers,
            [](const ProfileMarkerComparison& left,
                const ProfileMarkerComparison& right)
            {
                // 比較元の中央値差の絶対値
                const double leftMagnitude =
                    std::abs(left.medianDifference);
                // 比較先の中央値差の絶対値
                const double rightMagnitude =
                    std::abs(right.medianDifference);
                if (leftMagnitude != rightMagnitude)
                {
                    return leftMagnitude > rightMagnitude;
                }
                return left.path < right.path;
            });
        return comparison;
    }

    std::vector<double> MarkerMillisecondsPerFrame(
        const std::span<const ProfileFrame> frames,
        const std::string_view path)
    {
        // フレーム順の区間時間、ミリ秒
        std::vector<double> values;
        values.reserve(frames.size());
        // 時間を集計する計測フレーム
        for (const auto& frame : frames)
        {
            // 各区間に対応する経路情報
            const auto paths = BuildPaths(frame.samples);
            // 指定経路のフレーム内合計時間
            double total{};
            // 現在集計する計測区間の位置
            for (std::size_t index{}; index < paths.size(); ++index)
            {
                if (paths[index].path == path)
                {
                    total += frame.samples[index].milliseconds;
                }
            }
            values.push_back(total);
        }
        return values;
    }

    bool ParseProfileJson(
        const std::string_view text,
        std::vector<ProfileFrame>& frames,
        std::string* error)
    {
        // 読み込んだ計測JSONの文書
        const auto document = nlohmann::json::parse(
            text.begin(),
            text.end(),
            nullptr,
            false);
        if (document.is_discarded() || !document.is_object())
        {
            SetError(error, "JSONとして読み込めませんでした。");
            return false;
        }

        // value()は型が合わないと例外を送出するため、形式の確認からまとめて捕捉し、呼び出し側には失敗として返します。
        try
        {
            if (document.value("format", std::string{})
                != "LamaPonProfile")
            {
                SetError(
                    error,
                    "LamaPonProfile形式のJSONではありません。");
                return false;
            }
            // 計測JSONの形式バージョン
            const auto version = document.value("version", 0);
            if (version < 1 || version > 2)
            {
                SetError(
                    error,
                    "対応していないプロファイル形式のバージョンです: "
                        + std::to_string(version));
                return false;
            }
            // JSONに記録されたフレーム配列
            const auto frameArray = document.find("frames");
            if (frameArray == document.end()
                || !frameArray->is_array())
            {
                SetError(error, "framesがありません。");
                return false;
            }

            // 検証後に出力へ移すフレーム列
            std::vector<ProfileFrame> loaded;
            loaded.reserve(frameArray->size());
            // 読み込むフレームのJSON記録
            for (const auto& frameJson : *frameArray)
            {
                if (!frameJson.is_object())
                {
                    SetError(error, "フレームの形式が不正です。");
                    return false;
                }
                // JSONから構築する計測フレーム
                ProfileFrame frame;
                frame.index =
                    frameJson.value("index", std::uint64_t{});
                frame.milliseconds =
                    frameJson.value("milliseconds", 0.0);
                // フレーム内の区間記録のJSON配列
                const auto samples = frameJson.find("samples");
                if (samples != frameJson.end() && samples->is_array())
                {
                    // 読み込む計測区間のJSON記録
                    for (const auto& sampleJson : *samples)
                    {
                        if (!sampleJson.is_object())
                        {
                            continue;
                        }
                        // JSONから構築する計測区間
                        ProfileSample sample;
                        sample.name =
                            sampleJson.value("name", std::string{});
                        sample.milliseconds =
                            sampleJson.value("milliseconds", 0.0);
                        sample.callCount = sampleJson.value(
                            "calls",
                            std::uint32_t{});
                        // 親は自分より前にある場合だけ採用し、木が循環しないことを保証します。
                        // JSON内の親区間の添字
                        const auto parent = sampleJson.value(
                            "parent",
                            static_cast<std::int64_t>(-1));
                        if (parent >= 0
                            && static_cast<std::uint64_t>(parent)
                                < frame.samples.size())
                        {
                            sample.parent =
                                static_cast<std::uint32_t>(parent);
                            sample.depth =
                                frame.samples[sample.parent].depth + 1;
                        }
                        frame.samples.push_back(std::move(sample));
                    }
                }
                loaded.push_back(std::move(frame));
            }
            frames = std::move(loaded);
            return true;
        }
        // 計測JSONの値の検証エラー
        catch (const nlohmann::json::exception& exception)
        {
            SetError(
                error,
                std::string{ "プロファイルの値が不正です: " }
                    + exception.what());
            return false;
        }
    }

    bool LoadProfileJson(
        const std::filesystem::path& path,
        std::vector<ProfileFrame>& frames,
        std::string* error)
    {
        // 計測JSONの入力ファイル
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            SetError(error, "ファイルを開けませんでした。");
            return false;
        }
        // ファイルから読み込んだJSON本文
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        return ParseProfileJson(text, frames, error);
    }
}
