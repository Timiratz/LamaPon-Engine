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
        double value{};
        std::uint64_t frameIndex{};
    };

    // valuesは並べ替えるため値で受け取ります。
    LamaPon::ProfileValueStatistics Summarize(
        std::vector<ValueSample> values)
    {
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
        const std::size_t middle = values.size() / 2;
        statistics.median = values.size() % 2 == 0
            ? (values[middle - 1].value + values[middle].value)
                * 0.5
            : values[middle].value;
        // nearest-rank法です。件数が少ないときも実在する値を返します。
        const auto rank = static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(values.size())));
        statistics.percentile95 =
            values[std::clamp<std::size_t>(rank, 1, values.size()) - 1]
                .value;
        return statistics;
    }

    struct MarkerAccumulator final
    {
        std::string name;
        std::uint32_t depth{};
        std::vector<ValueSample> milliseconds;
        std::vector<ValueSample> selfMilliseconds;
        std::uint64_t totalCalls{};
    };

    struct SamplePath final
    {
        std::string path;
        std::uint32_t depth{};
    };

    [[nodiscard]] bool HasValidParent(
        const LamaPon::ProfileSample& sample,
        const std::size_t index) noexcept
    {
        return sample.parent != LamaPon::ProfileSample::NoParent
            && sample.parent < index;
    }

    // 各サンプルの経路を、親の経路へ自身の名前を足して作ります。
    // 親は子より前に並ぶ契約なので1回の走査で済みます。範囲外の親は
    // 壊れたファイル由来として最上位へ倒します。
    std::vector<SamplePath> BuildPaths(
        const std::vector<LamaPon::ProfileSample>& samples)
    {
        std::vector<SamplePath> paths;
        paths.reserve(samples.size());
        for (std::size_t index{}; index < samples.size(); ++index)
        {
            const auto& sample = samples[index];
            if (HasValidParent(sample, index))
            {
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
        ProfileFrameTree tree;
        const auto count = frame.samples.size();
        tree.children.resize(count);
        tree.selfMilliseconds.resize(count);
        for (std::size_t index{}; index < count; ++index)
        {
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
        for (auto& self : tree.selfMilliseconds)
        {
            self = std::max(self, 0.0);
        }
        return tree;
    }

    std::vector<ProfileFlatEntry> FlattenProfileFrame(
        const ProfileFrame& frame)
    {
        const auto tree = BuildProfileFrameTree(frame);
        std::vector<ProfileFlatEntry> entries;
        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t index{}; index < frame.samples.size(); ++index)
        {
            const auto& sample = frame.samples[index];
            const auto [entry, inserted] =
                indices.try_emplace(sample.name, entries.size());
            if (inserted)
            {
                entries.push_back({ sample.name });
            }
            auto& flat = entries[entry->second];
            flat.totalMilliseconds += sample.milliseconds;
            flat.selfMilliseconds += tree.selfMilliseconds[index];
            flat.calls += sample.callCount;
        }
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
        ProfileAnalysis analysis;
        if (frames.empty())
        {
            return analysis;
        }

        analysis.frameCount = frames.size();
        analysis.firstFrameIndex = frames.front().index;
        analysis.lastFrameIndex = frames.back().index;

        std::vector<ValueSample> frameTimes;
        frameTimes.reserve(frames.size());
        std::vector<MarkerAccumulator> accumulators;
        std::unordered_map<std::string, std::size_t> markerIndices;
        for (const auto& frame : frames)
        {
            frameTimes.push_back({ frame.milliseconds, frame.index });

            // 同じ経路が1フレームに複数回現れることは通常ありませんが、
            // 古い形式や壊れた親添字でも1フレーム1値になるよう合算します。
            const auto paths = BuildPaths(frame.samples);
            std::vector<double> childMilliseconds(
                frame.samples.size(),
                0.0);
            for (std::size_t index{};
                index < frame.samples.size();
                ++index)
            {
                const auto& sample = frame.samples[index];
                if (HasValidParent(sample, index))
                {
                    childMilliseconds[sample.parent] +=
                        sample.milliseconds;
                }
            }

            std::unordered_map<std::string, std::pair<double, double>>
                frameValues;
            std::vector<std::string> frameOrder;
            for (std::size_t index{};
                index < frame.samples.size();
                ++index)
            {
                const auto& sample = frame.samples[index];
                const auto& path = paths[index].path;
                auto [markerIndex, inserted] = markerIndices.try_emplace(
                    path,
                    accumulators.size());
                if (inserted)
                {
                    MarkerAccumulator accumulator;
                    accumulator.name = sample.name;
                    accumulator.depth = paths[index].depth;
                    accumulators.push_back(std::move(accumulator));
                }
                accumulators[markerIndex->second].totalCalls +=
                    sample.callCount;

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
            for (const auto& path : frameOrder)
            {
                const auto& [total, self] = frameValues[path];
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
        std::vector<std::string> pathsByIndex(accumulators.size());
        for (const auto& [path, index] : markerIndices)
        {
            pathsByIndex[index] = path;
        }
        for (std::size_t index{}; index < accumulators.size(); ++index)
        {
            auto& accumulator = accumulators[index];
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
        ProfileComparison comparison;
        comparison.a = a;
        comparison.b = b;
        comparison.frameMedianDifference =
            b.frameMilliseconds.median - a.frameMilliseconds.median;
        comparison.frameMeanDifference =
            b.frameMilliseconds.mean - a.frameMilliseconds.mean;

        const auto callsPerFrame =
            [](const ProfileMarkerStatistics& marker)
            {
                return marker.presentFrameCount == 0
                    ? 0.0
                    : static_cast<double>(marker.totalCalls)
                        / static_cast<double>(
                            marker.presentFrameCount);
            };

        const auto addMarker =
            [&comparison, &callsPerFrame](
                const ProfileMarkerStatistics* markerA,
                const ProfileMarkerStatistics* markerB)
            {
                const auto& source =
                    markerA != nullptr ? *markerA : *markerB;
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

        for (const auto& markerA : a.markers)
        {
            addMarker(&markerA, b.FindMarker(markerA.path));
        }
        for (const auto& markerB : b.markers)
        {
            if (a.FindMarker(markerB.path) == nullptr)
            {
                addMarker(nullptr, &markerB);
            }
        }

        // 同じ差なら経路名順にして、表示順を実行ごとに安定させます。
        std::ranges::stable_sort(
            comparison.markers,
            [](const ProfileMarkerComparison& left,
                const ProfileMarkerComparison& right)
            {
                const double leftMagnitude =
                    std::abs(left.medianDifference);
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
        std::vector<double> values;
        values.reserve(frames.size());
        for (const auto& frame : frames)
        {
            const auto paths = BuildPaths(frame.samples);
            double total{};
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

        // value()は型が合わないと例外を送出するため、形式の確認から
        // まとめて捕捉し、呼び出し側には失敗として返します。
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
            const auto version = document.value("version", 0);
            if (version < 1 || version > 2)
            {
                SetError(
                    error,
                    "対応していないプロファイル形式のバージョンです: "
                        + std::to_string(version));
                return false;
            }
            const auto frameArray = document.find("frames");
            if (frameArray == document.end()
                || !frameArray->is_array())
            {
                SetError(error, "framesがありません。");
                return false;
            }

            std::vector<ProfileFrame> loaded;
            loaded.reserve(frameArray->size());
            for (const auto& frameJson : *frameArray)
            {
                if (!frameJson.is_object())
                {
                    SetError(error, "フレームの形式が不正です。");
                    return false;
                }
                ProfileFrame frame;
                frame.index =
                    frameJson.value("index", std::uint64_t{});
                frame.milliseconds =
                    frameJson.value("milliseconds", 0.0);
                const auto samples = frameJson.find("samples");
                if (samples != frameJson.end() && samples->is_array())
                {
                    for (const auto& sampleJson : *samples)
                    {
                        if (!sampleJson.is_object())
                        {
                            continue;
                        }
                        ProfileSample sample;
                        sample.name =
                            sampleJson.value("name", std::string{});
                        sample.milliseconds =
                            sampleJson.value("milliseconds", 0.0);
                        sample.callCount = sampleJson.value(
                            "calls",
                            std::uint32_t{});
                        // 親は自分より前にある場合だけ採用し、木が
                        // 循環しないことを保証します。
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
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            SetError(error, "ファイルを開けませんでした。");
            return false;
        }
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        return ParseProfileJson(text, frames, error);
    }
}
