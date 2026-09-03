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
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    try
    {
        auto& logger =
            LamaPon::Logger::Instance();
        logger.CloseFile();
        logger.Clear();
        logger.SetCapacity(64);

        for (int index{}; index < 80; ++index)
        {
            logger.Info(
                "Ring entry "
                    + std::to_string(index),
                index == 79 ? 42u : 0u);
        }
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
        std::vector<std::thread> workers;
        for (int worker{}; worker < 4; ++worker)
        {
            workers.emplace_back(
                [worker, &logger]
                {
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
        for (auto& worker : workers)
        {
            worker.join();
        }
        entries = logger.Snapshot();
        Require(
            entries.size() == 100,
            "Concurrent log writes were lost.");
        for (const auto& entry : entries)
        {
            Require(
                entry.level
                    == LamaPon::LogLevel::Warning,
                "Logger changed a log level.");
        }

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

        std::ifstream input(
            logPath,
            std::ios::binary);
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
    catch (const std::exception& exception)
    {
        std::cerr
            << exception.what()
            << '\n';
        return 1;
    }
}
