#include "AnalysisCommands.h"

#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/Profiler.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // Require(condition: 条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件違反を検出する
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Frame(index: フレーム番号, render: 描画時間)は描画サンプル付きのフレームを作る。
    LamaPon::ProfileFrame Frame(
        const std::uint64_t index,
        const double render)
    {
        // 生成するプロファイルフレーム
        LamaPon::ProfileFrame frame;
        frame.index = index;
        frame.milliseconds = render + 1.0;
        // 描画サンプル
        LamaPon::ProfileSample sample;
        sample.name = "Render";
        sample.milliseconds = render;
        sample.callCount = 1;
        frame.samples.push_back(sample);
        return frame;
    }

    // Throws(function: 例外確認対象)は例外が発生したときtrueを返す。
    template<typename Function>
    bool Throws(Function&& function)
    {
        // 呼び出し時の例外を判定する
        try
        {
            function();
        }
        // 例外発生を記録する
        catch (const std::exception&)
        {
            return true;
        }
        return false;
    }
}

// CLIのプロファイル・メモリ分析を検証する
int main()
{
    // テスト失敗を終了コードへ変換する
    try
    {
        // テスト生成物の出力先
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "cli-analysis";
        // 出力先の削除結果
        std::error_code error;
        std::filesystem::remove_all(root, error);

        // 比較用プロファイルのパス
        const auto profileA = root / "a.json";
        // 比較用プロファイルのパス
        const auto profileB = root / "b.json";
        // 前半のフレーム群
        const std::vector<LamaPon::ProfileFrame> framesA{
            Frame(1, 2.0), Frame(2, 4.0), Frame(3, 6.0) };
        // 比較対象のフレーム群
        const std::vector<LamaPon::ProfileFrame> framesB{
            Frame(1, 8.0), Frame(2, 8.0) };
        Require(
            LamaPon::WriteProfileJson(profileA, framesA)
                && LamaPon::WriteProfileJson(profileB, framesB),
            "Profile fixtures could not be written.");

        // プロファイルAのコマンド引数
        const std::wstring profileAText = profileA.wstring();
        // プロファイルBのコマンド引数
        const std::wstring profileBText = profileB.wstring();

        // 分析コマンドの入力
        const std::array analyzeArguments{
            std::wstring_view{ L"analyze" },
            std::wstring_view{ profileAText },
            std::wstring_view{ L"--first" },
            std::wstring_view{ L"1" },
            std::wstring_view{ L"--top" },
            std::wstring_view{ L"5" },
        };
        // 分析コマンドの結果
        const auto analyzed = LamaPon::Cli::RunAnalysisCommand(
            L"profile",
            analyzeArguments);
        Require(
            analyzed["ok"].get<bool>()
                && analyzed["command"] == "profile analyze"
                && analyzed["range"]["first"] == 1
                && analyzed["range"]["last"] == 2
                && analyzed["analysis"]["frameCount"] == 2
                && analyzed["analysis"]["markers"][0]["path"] == "Render"
                && analyzed["analysis"]["markers"][0]["milliseconds"]
                    ["median"].get<double>() == 5.0,
            "profile analyze did not honour the frame range.");

        // 比較コマンドの入力
        const std::array compareArguments{
            std::wstring_view{ L"compare" },
            std::wstring_view{ profileAText },
            std::wstring_view{ profileBText },
        };
        // 比較コマンドの結果
        const auto compared = LamaPon::Cli::RunAnalysisCommand(
            L"profile",
            compareArguments);
        Require(
            compared["comparison"]["frameMedianDifference"]
                    .get<double>() == 9.0 - 5.0
                && compared["comparison"]["markers"][0]["status"]
                    == "changed"
                && compared["comparison"]["markers"][0]
                    ["medianDifference"].get<double>() == 4.0,
            "profile compare did not report the slowdown.");

        // 比較前のメモリ記録
        LamaPon::MemorySnapshot before;
        before.label = "before";
        before.entries.push_back({
            LamaPon::MemoryCategory::Texture,
            "a.png",
            "",
            100,
            0 });
        // 比較後のメモリ記録
        auto after = before;
        after.label = "after";
        after.entries.push_back({
            LamaPon::MemoryCategory::Model,
            "hero.glb",
            "",
            300,
            50 });
        // メモリ記録Aのパス
        const auto memoryA = root / "memory-a.json";
        // メモリ記録Bのパス
        const auto memoryB = root / "memory-b.json";
        Require(
            LamaPon::WriteMemorySnapshotJson(memoryA, before)
                && LamaPon::WriteMemorySnapshotJson(memoryB, after),
            "Memory fixtures could not be written.");
        // メモリ記録Aのコマンド引数
        const std::wstring memoryAText = memoryA.wstring();
        // メモリ記録Bのコマンド引数
        const std::wstring memoryBText = memoryB.wstring();

        // 要約コマンドの入力
        const std::array summaryArguments{
            std::wstring_view{ L"summary" },
            std::wstring_view{ memoryBText },
        };
        // 要約コマンドの結果
        const auto summary = LamaPon::Cli::RunAnalysisCommand(
            L"memory",
            summaryArguments);
        Require(
            summary["summary"]["entryCount"] == 2
                && summary["summary"]["entries"][0]["name"] == "hero.glb",
            "memory summary did not order entries by size.");

        // メモリ比較コマンドの入力
        const std::array memoryCompareArguments{
            std::wstring_view{ L"compare" },
            std::wstring_view{ memoryAText },
            std::wstring_view{ memoryBText },
        };
        // メモリ比較コマンドの結果
        const auto memoryCompared = LamaPon::Cli::RunAnalysisCommand(
            L"memory",
            memoryCompareArguments);
        Require(
            memoryCompared["comparison"]["changedEntryCount"] == 1
                && memoryCompared["comparison"]["entries"][0]["change"]
                    == "added"
                && memoryCompared["comparison"]["entries"][0]
                    ["deltaBytes"] == 350,
            "memory compare did not report the added model.");

        // 不正入力の各パターン
        const std::array missingFile{ std::wstring_view{ L"compare" } };
        // 不正な分析オプション
        const std::array badOption{
            std::wstring_view{ L"analyze" },
            std::wstring_view{ profileAText },
            std::wstring_view{ L"--top" },
            std::wstring_view{ L"-1" },
        };
        // 未対応の分析操作
        const std::array unknownAction{ std::wstring_view{ L"explode" } };
        // 種別不一致の入力ファイル
        const std::array brokenFile{
            std::wstring_view{ L"summary" },
            std::wstring_view{ profileAText },
        };
        // 不正な分析コマンドを拒否する
        Require(
            Throws([&] {
                static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                    L"profile", missingFile));
            })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"profile", badOption));
                })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"memory", unknownAction));
                })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"memory", brokenFile));
                }),
            "Invalid analysis commands must be rejected.");

        std::cout << "CLI analysis command tests passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返す
        return 1;
    }
}
