#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace LamaPon
{
    // Game Module（プロジェクトのC++ Script）をビルドするための
    // cmd.exe コマンド一式です。
    //
    // なぜ「実行」ではなく「コマンドの構築」までなのか:
    // エディターは編集を止めないよう非同期（ShellExecuteEx）で、
    // LamaPonCliは結果をJSONで返すため同期（CreateProcess＋待機）で
    // 実行します。実行方法だけが違い、コマンドの中身が食い違うと
    // 「エディターでは通るのにCLIでは失敗する」（またはその逆）が
    // 生まれるので、構築を1箇所に集めています。
    struct GameModuleBuildCommand final
    {
        // cmd.exe へ渡すパラメーター（"/d /c ..."）。
        std::wstring parameters;
        // ビルドログの行き先（<プロジェクト>/.lamapon/game-module-build.log）。
        std::filesystem::path logPath;
        // 成果物（<プロジェクト>/.lamapon/bin/LamaPonGameModule.dll）。
        std::filesystem::path outputModule;
        // CMakeの生成物を置く場所。ネットワークドライブでは
        // %LOCALAPPDATA%配下になり、完成DLLだけをprojectへ戻します。
        std::filesystem::path buildDirectory;
        bool usesLocalBuildCache{};
    };

    struct GameModuleBuildState final
    {
        bool hasSources{};
        bool outputExists{};
        bool buildRequired{};
        // エンジン（LamaPonRuntime.dll）よりDLLが古い。GameModuleHostが
        // 安全のため読み込みを拒否する状態なので、再ビルドが必要
        bool staleAgainstRuntime{};
    };

    // 初回起動時にも「C++ファイルがあるのにDLLが無い／古い」を検出
    // します。保存イベントだけに頼ると、テンプレート同梱Scriptは一度
    // 手で保存しない限り永遠にビルドされないためです。
    [[nodiscard]] GameModuleBuildState InspectGameModuleBuildState(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& outputModule = {}) noexcept;

    // マップドドライブやUNC/WebDAV上のプロジェクトでは、CMake/NMakeの
    // 中間生成物をローカルディスクに置きます。読み込み時にマップド
    // ドライブがUNCパスへ正規化される場合があるため、両方を判定します。
    [[nodiscard]] bool ShouldUseLocalGameModuleBuildCache(
        const std::filesystem::path& projectRoot) noexcept;

    // 前提が満たされていないとき（ビルドツール一式が無い、
    // LamaPonRuntime.libが無い）は std::runtime_error を投げます。
    [[nodiscard]] GameModuleBuildCommand
        MakeGameModuleBuildCommand(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& engineRoot,
            const std::filesystem::path& runtimeDirectory,
            const std::string& configuration);

    // WebDAVなどでは編集後も更新時刻が古いまま見えることがあり、
    // NMakeが変更を見落とすため、内容ハッシュでも更新を検出します。
    // 前回ビルド時の内容ハッシュをbuildDirectory内のマニフェストと
    // 比較し、内容が変わっているソースの更新時刻を現在時刻へ進めて
    // NMakeに確実に拾わせます。戻り値は進めたファイル数。
    // 失敗は無害側（何もしない）へ倒します。
    int RefreshStaleGameModuleSources(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& buildDirectory) noexcept;

    // ビルドされたDLLが公開するAPI版数を返します
    // （LamaPonGetGameModuleを呼んでdescriptorを見るだけ）。
    //
    // NMakeはヘッダ依存の追跡が不完全なため、更新時刻だけではAPI版数の
    // 更新を確認できません。DLL自身が公開する版数を読み取ります。
    //
    // 読めないとき（ファイルが無い、ロードできない、エクスポートが
    // 無い）は nullopt です。
    [[nodiscard]] std::optional<std::uint32_t>
        ReadGameModuleApiVersion(
            const std::filesystem::path& modulePath) noexcept;
}
