#include "LamaPon/Core/Log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // 条件不成立ならmessageで例外にします(condition: 判定, message: 失敗文)
    void Require(
        const bool condition,
        const char* message)
    {
        // 条件が偽ならlog testを失敗させます。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

// Loggerのring・並行書込・file sinkを検証します。
int main()
{
    // 検証例外をprocess failureへ変換します。
    try
    {
        // singleton loggerです。
        auto& logger =
            LamaPon::Logger::Instance();
        logger.CloseFile();
        logger.Clear();
        logger.SetCapacity(64);

        // capacityを超えるentryで古い値を置き換えます。
        for (int index{}; index < 80; ++index)
        {
            logger.Info(
                "Ring entry "
                    + std::to_string(index),
                index == 79 ? 42u : 0u);
        }
        // 現在ringに保持されたentry一覧です。
        auto entries = logger.Snapshot();
        Require(
            entries.size() == 64,
            "Logger did not enforce its ring capacity.");
        Require(
            entries.back().message
                    == "Ring entry 79"
                && entries.back().
                    gameObjectId == 42
                && entries.back().
                    sourceLine != 0
                && !entries.back().
                    sourceFile.empty(),
            "Logger entry metadata is incomplete.");

        logger.Clear();
        logger.SetCapacity(128);
        // 並行書込threadを保持します。
        std::vector<std::thread> workers;
        // 4 workerで同時にwarningを追加します。
        for (int worker{}; worker < 4; ++worker)
        {
            workers.emplace_back(
                [worker, &logger]
                {
                    // 各workerから25 entryずつ追加します。
                    for (int index{};
                        index < 25;
                        ++index)
                    {
                        logger.Warning(
                            "Worker "
                            + std::to_string(worker)
                            + " / "
                            + std::to_string(index));
                    }
                });
        }
        // 全writerの終了を待ちます。
        for (auto& worker : workers)
        {
            worker.join();
        }
        // 並行書込後に保持されたentry一覧です。
        entries = logger.Snapshot();
        Require(
            entries.size() == 100,
            "Concurrent log writes were lost.");
        // 全entryがwarning levelを保つことを確認します。
        for (const auto& entry : entries)
        {
            Require(
                entry.level
                    == LamaPon::LogLevel::Warning,
                "Logger changed a log level.");
        }

        // test-output内のlog file pathです。
        const auto logPath =
            std::filesystem::current_path()
            / "test-output"
            / "logger-test.log";
        Require(
            logger.SetFilePath(logPath),
            "Logger could not open its file sink.");
        logger.Error(
            "日本語エラーログ",
            77);
        logger.CloseFile();

        // file sinkから出力を読み戻します。
        std::ifstream input(
            logPath,
            std::ios::binary);
        // file sinkから読み込んだ全textです。
        const std::string fileText{
            std::istreambuf_iterator<char>{
                input },
            std::istreambuf_iterator<char>{}
        };
        Require(
            fileText.find(
                "日本語エラーログ")
                    != std::string::npos
                && fileText.find("[Error]")
                    != std::string::npos
                && fileText.find(
                    "GameObject 77")
                    != std::string::npos,
            "Logger file output is incomplete.");

        logger.Clear();
        std::cout
            << "Logger tests passed.\n";
        return 0;
    }
    // 検証例外を失敗診断へ変換します(exception: failure)
    catch (const std::exception& exception)
    {
        std::cerr
            << exception.what()
            << '\n';
        return 1;
    }
}
