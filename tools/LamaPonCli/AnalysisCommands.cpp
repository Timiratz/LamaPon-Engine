#include "AnalysisCommands.h"

#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/ProfileAnalysis.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace LamaPon::Cli
{
    namespace
    {
        // 上位表示件数の既定値
        constexpr std::size_t DefaultTopCount = 20;

        // pathを区切り文字/のUTF-8文字列にします。
        // ToUtf8(path: 変換するパス)
        [[nodiscard]] std::string ToUtf8(const std::filesystem::path& path)
        {
            // 汎用形式のUTF-8パス
            const auto text = path.generic_u8string();
            return { text.begin(), text.end() };
        }

        // textをUTF-8文字列にします。
        // ToUtf8(text: 変換するワイド文字列)
        [[nodiscard]] std::string ToUtf8(const std::wstring_view text)
        {
            return ToUtf8(std::filesystem::path{ std::wstring{ text } });
        }

        struct ParsedArguments final
        {
            // 入力ファイル一覧
            std::vector<std::filesystem::path> files;
            // 解析範囲の先頭位置
            std::optional<std::size_t> first;
            // 解析範囲の末尾位置
            std::optional<std::size_t> last;
            // 結果の上位表示件数
            std::size_t top{ DefaultTopCount };
        };

        // 数値オプションを非負整数として解析します。
        // ParseCount(option: オプション名, value: 入力値)
        [[nodiscard]] std::size_t ParseCount(
            const std::wstring_view option,
            const std::wstring_view value)
        {
            // 数値変換失敗は共通の入力エラーへ変換する
            try
            {
                // 数値変換で消費した文字数
                std::size_t consumed{};
                // 入力全体が非負整数なら採用
                const auto parsed =
                    std::stoll(std::wstring{ value }, &consumed);
                // 数値以外の末尾文字を認めない
                if (consumed == value.size() && parsed >= 0)
                {
                    return static_cast<std::size_t>(parsed);
                }
            }
            // 数値変換例外は共通の入力エラーへまとめる
            catch (const std::exception&)
            {
            }
            // option名を含む形式で入力エラーを返す
            throw std::invalid_argument(
                ToUtf8(option) + " requires a non-negative integer.");
        }

        // CLI引数を範囲・件数・ファイルに分解します。
        // ParseArguments(arguments: コマンドと引数一覧)
        [[nodiscard]] ParsedArguments ParseArguments(
            const std::span<const std::wstring_view> arguments)
        {
            // 解析結果
            ParsedArguments parsed;
            // index: コマンド名以降の引数位置
            for (std::size_t index = 1; index < arguments.size(); ++index)
            {
                // 現在の引数
                const auto argument = arguments[index];
                // 値付きオプションの次の引数を取得
                const auto value = [&]() -> std::wstring_view
                {
                    // 値が後続しないオプションを拒否
                    if (index + 1 >= arguments.size())
                    {
                        throw std::invalid_argument(
                            ToUtf8(argument) + " requires a value.");
                    }
                    return arguments[++index];
                };
                // 解析開始位置を設定
                if (argument == L"--first")
                {
                    parsed.first = ParseCount(argument, value());
                }
                // 解析終了位置を設定
                else if (argument == L"--last")
                {
                    parsed.last = ParseCount(argument, value());
                }
                // 出力上位件数を設定
                else if (argument == L"--top")
                {
                    parsed.top = ParseCount(argument, value());
                }
                // 未知のオプションを拒否
                else if (argument.starts_with(L"--"))
                {
                    throw std::invalid_argument(
                        "Unknown option: " + ToUtf8(argument));
                }
                // オプション以外は入力ファイルとして扱う
                else
                {
                    parsed.files.emplace_back(std::wstring{ argument });
                }
            }
            return parsed;
        }

        // 入力ファイル数がusage条件と一致するか確認します。
        // RequireFileCount(parsed: 引数解析結果, count: 必要数, usage: エラー説明)
        void RequireFileCount(
            const ParsedArguments& parsed,
            const std::size_t count,
            const char* usage)
        {
            // 想定外の入力数をエラーにする
            if (parsed.files.size() != count)
            {
                throw std::invalid_argument(usage);
            }
        }

        // 指定されたプロファイル記録を読み込みます。
        // LoadFrames(path: 読み込む記録ファイル)
        [[nodiscard]] std::vector<ProfileFrame> LoadFrames(
            const std::filesystem::path& path)
        {
            // 読み込んだフレーム一覧
            std::vector<ProfileFrame> frames;
            // 読み込み失敗時の説明
            std::string error;
            // 失敗理由をパスと共に返す
            if (!LoadProfileJson(path, frames, &error))
            {
                throw std::runtime_error(
                    "Could not read profile " + ToUtf8(path) + ": " + error);
            }
            return frames;
        }

        // 指定されたメモリスナップショットを読み込みます。
        // LoadSnapshot(path: 読み込むスナップショット)
        [[nodiscard]] MemorySnapshot LoadSnapshot(
            const std::filesystem::path& path)
        {
            // 読み込んだスナップショット
            MemorySnapshot snapshot;
            // 読み込み失敗時の説明
            std::string error;
            // 失敗理由をパスと共に返す
            if (!LoadMemorySnapshotJson(path, snapshot, &error))
            {
                throw std::runtime_error(
                    "Could not read memory snapshot " + ToUtf8(path)
                    + ": " + error);
            }
            return snapshot;
        }

        // 統計値をCLI応答用JSONにします。
        // ValueJson(statistics: 集計済み統計値)
        [[nodiscard]] nlohmann::json ValueJson(
            const ProfileValueStatistics& statistics)
        {
            return {
                { "count", statistics.count },
                { "minimum", statistics.minimum },
                { "maximum", statistics.maximum },
                { "mean", statistics.mean },
                { "median", statistics.median },
                { "percentile95", statistics.percentile95 },
                { "maximumFrameIndex", statistics.maximumFrameIndex },
            };
        }

        // メモリ差分の種別名を返します。
        // ChangeName(change: 差分種別)
        [[nodiscard]] std::string_view ChangeName(
            const MemoryEntryChange change) noexcept
        {
            // 差分種別に対応する出力名
            switch (change)
            {
            // 新規項目
            case MemoryEntryChange::Added:
                return "added";
            // 削除項目
            case MemoryEntryChange::Removed:
                return "removed";
            // 変更項目は既定の戻り値へ進む
            case MemoryEntryChange::Changed:
                break;
            }
            return "changed";
        }

        // メモリ分類ごとの合計をJSON配列にします。
        // CategoryTotalsJson(totals: 分類別合計)
        [[nodiscard]] nlohmann::json CategoryTotalsJson(
            const MemoryCategoryTotals& totals)
        {
            // 出力対象の分類
            auto categories = nlohmann::json::array();
            // index: メモリ分類の位置
            for (std::size_t index{}; index < totals.size(); ++index)
            {
                // 現在の分類合計
                const auto& total = totals[index];
                // 0件の分類は出力を省く
                if (total.count == 0)
                {
                    continue;
                }
                categories.push_back({
                    { "category",
                        std::string(MemoryCategoryKey(
                            static_cast<MemoryCategory>(index))) },
                    { "count", total.count },
                    { "gpuBytes", total.gpuBytes },
                    { "cpuBytes", total.cpuBytes },
                });
            }
            return categories;
        }
    }

    // 重い順に並べ、先頭topCount件だけを返します。
    // ProfileAnalysisJson(analysis: 解析結果, topCount: 表示上限)
    nlohmann::json ProfileAnalysisJson(
        const ProfileAnalysis& analysis,
        const std::size_t topCount)
    {
        // 中央値の降順に並べるマーカー参照
        std::vector<const ProfileMarkerStatistics*> markers;
        markers.reserve(analysis.markers.size());
        // marker: 解析結果内の各計測マーカー
        for (const auto& marker : analysis.markers)
        {
            markers.push_back(&marker);
        }
        // left/right: 中央値の降順比較
        std::ranges::stable_sort(
            markers,
            [](const ProfileMarkerStatistics* left,
                const ProfileMarkerStatistics* right)
            {
                return left->milliseconds.median
                    > right->milliseconds.median;
            });
        // JSON出力するマーカー一覧
        auto markerJson = nlohmann::json::array();
        // index: 表示上限内のマーカー位置
        for (std::size_t index{};
            index < std::min(topCount, markers.size());
            ++index)
        {
            // 出力対象の計測マーカー
            const auto& marker = *markers[index];
            markerJson.push_back({
                { "path", marker.path },
                { "depth", marker.depth },
                { "milliseconds", ValueJson(marker.milliseconds) },
                { "selfMeanMilliseconds", marker.selfMilliseconds.mean },
                { "callsPerFrame",
                    marker.presentFrameCount == 0
                        ? 0.0
                        : static_cast<double>(marker.totalCalls)
                            / static_cast<double>(
                                marker.presentFrameCount) },
                { "presentFrames", marker.presentFrameCount },
            });
        }
        return {
            { "frameCount", analysis.frameCount },
            { "firstFrameIndex", analysis.firstFrameIndex },
            { "lastFrameIndex", analysis.lastFrameIndex },
            { "frameMilliseconds", ValueJson(analysis.frameMilliseconds) },
            { "markerCount", analysis.markers.size() },
            { "markers", std::move(markerJson) },
        };
    }

    // 2記録のマーカー差分をJSONにします。
    // ProfileComparisonJson(comparison: 比較結果, topCount: 表示上限)
    nlohmann::json ProfileComparisonJson(
        const ProfileComparison& comparison,
        const std::size_t topCount)
    {
        // JSON出力する比較結果
        auto markers = nlohmann::json::array();
        // index: 表示上限内の差分位置
        for (std::size_t index{};
            index < std::min(topCount, comparison.markers.size());
            ++index)
        {
            // 出力対象のマーカー差分
            const auto& marker = comparison.markers[index];
            markers.push_back({
                { "path", marker.path },
                { "status",
                    !marker.presentInA
                        ? "added"
                        : (!marker.presentInB ? "removed" : "changed") },
                { "medianA", marker.medianA },
                { "medianB", marker.medianB },
                { "medianDifference", marker.medianDifference },
                { "medianRelativeChange", marker.medianRelativeChange },
                { "meanDifference", marker.meanDifference },
                { "callsPerFrameA", marker.callsPerFrameA },
                { "callsPerFrameB", marker.callsPerFrameB },
            });
        }
        return {
            { "frameCountA", comparison.a.frameCount },
            { "frameCountB", comparison.b.frameCount },
            { "frameMedianA", comparison.a.frameMilliseconds.median },
            { "frameMedianB", comparison.b.frameMilliseconds.median },
            { "frameMedianDifference", comparison.frameMedianDifference },
            { "frameMeanDifference", comparison.frameMeanDifference },
            { "markers", std::move(markers) },
        };
    }

    // メモリ使用状況をJSONにまとめます。
    // MemorySummaryJson(snapshot: 記録, topCount: 表示上限)
    nlohmann::json MemorySummaryJson(
        const MemorySnapshot& snapshot,
        const std::size_t topCount)
    {
        // 合計バイト数で並べる記録項目
        std::vector<const MemorySnapshotEntry*> entries;
        entries.reserve(snapshot.entries.size());
        // entry: スナップショット内の各項目
        for (const auto& entry : snapshot.entries)
        {
            entries.push_back(&entry);
        }
        // 合計バイト数の降順で安定ソート
        std::ranges::stable_sort(
            entries,
            [](const MemorySnapshotEntry* left,
                const MemorySnapshotEntry* right)
            {
                return left->TotalBytes() > right->TotalBytes();
            });
        // JSON出力するメモリ項目
        auto entryJson = nlohmann::json::array();
        // index: 表示上限内の項目位置
        for (std::size_t index{};
            index < std::min(topCount, entries.size());
            ++index)
        {
            // 出力対象のメモリ項目
            const auto& entry = *entries[index];
            entryJson.push_back({
                { "category",
                    std::string(MemoryCategoryKey(entry.category)) },
                { "name", entry.name },
                { "detail", entry.detail },
                { "gpuBytes", entry.gpuBytes },
                { "cpuBytes", entry.cpuBytes },
            });
        }
        return {
            { "label", snapshot.label },
            { "capturedAt", snapshot.capturedAt },
            { "process", {
                { "workingSetBytes",
                    snapshot.process.processWorkingSetBytes },
                { "privateBytes", snapshot.process.processPrivateBytes },
                { "localVideoMemoryBytes",
                    snapshot.process.localVideoMemoryUsageBytes },
            } },
            { "entryCount", snapshot.entries.size() },
            { "categories",
                CategoryTotalsJson(SummarizeMemorySnapshot(snapshot)) },
            { "entries", std::move(entryJson) },
        };
    }

    // 2スナップショットの差分をJSONにまとめます。
    // MemoryComparisonJson(comparison: 比較結果, topCount: 表示上限)
    nlohmann::json MemoryComparisonJson(
        const MemorySnapshotComparison& comparison,
        const std::size_t topCount)
    {
        // JSON出力する分類別差分
        auto categories = nlohmann::json::array();
        // index: 分類の位置
        for (std::size_t index{}; index < comparison.before.size(); ++index)
        {
            // 比較前の分類合計
            const auto& before = comparison.before[index];
            // 比較後の分類合計
            const auto& after = comparison.after[index];
            // 両側とも0件の分類は出力しない
            if (before.count == 0 && after.count == 0)
            {
                continue;
            }
            // 比較前のCPU/GPU合計バイト数
            const auto beforeBytes = before.gpuBytes + before.cpuBytes;
            // 比較後のCPU/GPU合計バイト数
            const auto afterBytes = after.gpuBytes + after.cpuBytes;
            categories.push_back({
                { "category",
                    std::string(MemoryCategoryKey(
                        static_cast<MemoryCategory>(index))) },
                { "beforeBytes", beforeBytes },
                { "afterBytes", afterBytes },
                { "deltaBytes",
                    static_cast<std::int64_t>(afterBytes)
                        - static_cast<std::int64_t>(beforeBytes) },
            });
        }
        // JSON出力する項目別差分
        auto entries = nlohmann::json::array();
        // index: 表示上限内の差分位置
        for (std::size_t index{};
            index < std::min(topCount, comparison.entries.size());
            ++index)
        {
            // 出力対象の項目差分
            const auto& entry = comparison.entries[index];
            entries.push_back({
                { "category",
                    std::string(MemoryCategoryKey(entry.category)) },
                { "name", entry.name },
                { "change", std::string(ChangeName(entry.change)) },
                { "beforeBytes", entry.beforeBytes },
                { "afterBytes", entry.afterBytes },
                { "deltaBytes", entry.DeltaBytes() },
            });
        }
        return {
            { "privateBytesDelta", comparison.processPrivateDelta },
            { "workingSetBytesDelta", comparison.processWorkingSetDelta },
            { "localVideoMemoryBytesDelta",
                comparison.localVideoMemoryDelta },
            { "changedEntryCount", comparison.entries.size() },
            { "categories", std::move(categories) },
            { "entries", std::move(entries) },
        };
    }

    // profile/memoryの解析・比較結果を返します。
    // RunAnalysisCommand(command: コマンド, arguments: 引数一覧)
    nlohmann::json RunAnalysisCommand(
        const std::wstring_view command,
        const std::span<const std::wstring_view> arguments)
    {
        // サブコマンド未指定では処理を選べない
        if (arguments.empty())
        {
            throw std::invalid_argument(
                command == L"profile"
                    ? "profile requires analyze or compare."
                    : "memory requires summary or compare.");
        }
        // 先頭引数のサブコマンド名
        const auto action = arguments.front();
        // オプションとファイルを分離した引数
        const auto parsed = ParseArguments(arguments);
        // コマンド名を含む応答の共通部分
        nlohmann::json response{
            { "ok", true },
            { "command", ToUtf8(command) + " " + ToUtf8(action) },
        };

        // profile analyzeは1件の記録を解析する
        if (command == L"profile" && action == L"analyze")
        {
            RequireFileCount(
                parsed,
                1,
                "profile analyze requires one capture file.");
            // 読み込んだ記録フレーム
            const auto frames = LoadFrames(parsed.files.front());
            // 空記録から範囲を作れない
            if (frames.empty())
            {
                throw std::runtime_error(
                    "The profile capture has no frames.");
            }
            // --first/--lastは記録内の位置（0始まり、両端を含む）です。
            const auto first = std::min(
                parsed.first.value_or(0),
                frames.size() - 1);
            // 最終フレーム位置
            const auto last = std::clamp(
                parsed.last.value_or(frames.size() - 1),
                first,
                frames.size() - 1);
            response["file"] = ToUtf8(parsed.files.front());
            response["range"] = { { "first", first }, { "last", last } };
            response["analysis"] = ProfileAnalysisJson(
                AnalyzeProfile(
                    std::span<const ProfileFrame>(frames)
                        .subspan(first, last - first + 1)),
                parsed.top);
            return response;
        }
        // profile compareは2件の記録を比較する
        if (command == L"profile" && action == L"compare")
        {
            RequireFileCount(
                parsed,
                2,
                "profile compare requires two capture files.");
            // 比較元と比較先のフレーム
            const auto a = LoadFrames(parsed.files[0]);
            // 比較先のフレーム
            const auto b = LoadFrames(parsed.files[1]);
            response["fileA"] = ToUtf8(parsed.files[0]);
            response["fileB"] = ToUtf8(parsed.files[1]);
            response["comparison"] = ProfileComparisonJson(
                CompareProfiles(AnalyzeProfile(a), AnalyzeProfile(b)),
                parsed.top);
            return response;
        }
        // memory summaryは1件の記録を集計する
        if (command == L"memory" && action == L"summary")
        {
            RequireFileCount(
                parsed,
                1,
                "memory summary requires one snapshot file.");
            response["file"] = ToUtf8(parsed.files.front());
            response["summary"] = MemorySummaryJson(
                LoadSnapshot(parsed.files.front()),
                parsed.top);
            return response;
        }
        // memory compareは2件の記録を比較する
        if (command == L"memory" && action == L"compare")
        {
            RequireFileCount(
                parsed,
                2,
                "memory compare requires two snapshot files.");
            response["fileA"] = ToUtf8(parsed.files[0]);
            response["fileB"] = ToUtf8(parsed.files[1]);
            response["comparison"] = MemoryComparisonJson(
                CompareMemorySnapshots(
                    LoadSnapshot(parsed.files[0]),
                    LoadSnapshot(parsed.files[1])),
                parsed.top);
            return response;
        }
        throw std::invalid_argument(
            "Unknown " + ToUtf8(command) + " action: " + ToUtf8(action));
    }
}
