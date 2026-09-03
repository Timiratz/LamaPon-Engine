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
    // エディターは編集を止めないよう**非同期**（ShellExecuteEx）で、
    // LamaPonCliは結果をJSONで返すため**同期**（CreateProcess＋待機）で
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

    // Mapped drives and UNC/WebDAV projects should keep CMake/NMake
    // intermediates on a local disk. Project loading can canonicalize a
    // mapped drive (for example Z:) to a UNC path before this builder is
    // called, so both path forms need to be recognized.
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

    // WebDAV（Z:等のネットワークドライブ）のmtimeキャッシュ対策。
    // ホスト側で編集したソースの更新時刻が古いまま見えることがあり、
    // NMakeが「変更なし」と誤判定して古いDLLのまま成功を返す
    // （手掛かりはbuild結果のmoduleUpdated:falseだけ。2026-08-13に
    // CarGameで実際に発生し、旧DLLで回帰テストを通してしまった）。
    // 前回ビルド時の内容ハッシュをbuildDirectory内のマニフェストと
    // 比較し、内容が変わっているソースの更新時刻を現在時刻へ進めて
    // NMakeに確実に拾わせます。戻り値は進めたファイル数。
    // 失敗は無害側（何もしない）へ倒します。
    int RefreshStaleGameModuleSources(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& buildDirectory) noexcept;

    // ビルドされたDLLが**実際に名乗るAPI版数**を返します
    // （LamaPonGetGameModuleを呼んでdescriptorを見るだけ）。
    //
    // なぜ更新時刻の比較では足りないのか: NMakeはヘッダ依存の
    // 追跡が不完全で、GameModule.hの GameModuleApiVersion を上げても
    // それを埋め込んだobjを再コンパイルせず、他のobjだけを作り
    // 直してリンクします。するとDLLの更新時刻だけ新しくなり、
    // 中身は古い版数のままになります。2026-08-18にCarGameで発生し、
    // buildは runtimeCompatible:true を返したのにエディターの再生が
    // 背景だけになりました。
    //
    // 読めないとき（ファイルが無い、ロードできない、エクスポートが
    // 無い）は nullopt です。
    [[nodiscard]] std::optional<std::uint32_t>
        ReadGameModuleApiVersion(
            const std::filesystem::path& modulePath) noexcept;
}
