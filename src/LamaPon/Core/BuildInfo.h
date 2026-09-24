#pragma once

#include <string>
#include <string_view>

namespace LamaPon
{
    // どのソースからビルドしたエンジンかを示す情報です。
    //
    // 人に見せるバージョンは「ブランチ名 @ コミット」です。
    // Version.hのMAJOR.MINOR.PATCHはパッケージの必要エンジン版や
    // プロジェクト移行の互換判定にだけ使います。
    //
    // 値はビルドのたびにGitから取り直すため（cmake/GenerateBuildInfo.cmake）、
    // CMakeを再構成しなくても最新のコミットが表示されます。
    // Gitが使えない環境では各値が "unknown" になります。
    struct BuildInfo final
    {
        std::string_view branch;
        // 12桁の短縮ハッシュ。
        std::string_view commit;
        std::string_view commitFull;
        // コミットメッセージの1行目。
        std::string_view commitSubject;
        // 未コミットの変更を含むビルドか。
        bool dirty{};
    };

    // このエンジンバイナリに埋め込まれたビルド情報を返します。
    [[nodiscard]] const BuildInfo& GetBuildInfo() noexcept;

    // Git情報を持っているか（ブランチかコミットが分かるか）を返します。
    [[nodiscard]] bool HasBuildSourceInfo(
        const BuildInfo& info) noexcept;

    // "community/main @ 932b08c3a1b2" のような1行表示です。
    // 未コミットの変更があれば "-dirty" を付けます。Git情報が無い
    // ビルドでは互換バージョンを "v0.1.0" の形で返します。
    [[nodiscard]] std::string FormatBuildLabel(
        const BuildInfo& info,
        std::string_view compatibilityVersion);
    [[nodiscard]] std::string FormatBuildLabel();

    // サポート情報やクラッシュレポート向けの複数行表示です。
    // 各行は "Key: value" 形式で、末尾に改行を含みません。
    [[nodiscard]] std::string FormatBuildDetails(
        const BuildInfo& info,
        std::string_view compatibilityVersion);
    [[nodiscard]] std::string FormatBuildDetails();
}
