#pragma once

#include <string>
#include <string_view>

namespace LamaPon
{
    // Git情報は表示専用とし、互換判定にはVersion.hの数値を使う。
    struct BuildInfo final
    {
        // ビルド元のブランチ名
        std::string_view branch;
        // 12桁のコミットハッシュ
        std::string_view commit;
        // 完全なコミットハッシュ
        std::string_view commitFull;
        // コミットメッセージの先頭行
        std::string_view commitSubject;
        // 未コミット変更の有無
        bool dirty{};
    };

    // このエンジンバイナリに埋め込まれたビルド情報を返します。
    [[nodiscard]] const BuildInfo& GetBuildInfo() noexcept;

    // ブランチかコミットが判明しているかを返します(info: 判定するビルド情報)。
    [[nodiscard]] bool HasBuildSourceInfo(
        const BuildInfo& info) noexcept;

    // Git不明時は互換バージョンを使い、未コミット時は-dirtyを付ける。
    // ビルド元を1行表示に整形する(info: ビルド情報, compatibilityVersion: Git不明時の互換バージョン)。
    [[nodiscard]] std::string FormatBuildLabel(
        const BuildInfo& info,
        std::string_view compatibilityVersion);
    // 実行中のエンジンのビルド元を1行表示に整形します。
    [[nodiscard]] std::string FormatBuildLabel();

    // サポート用のビルド情報を整形します(info: ビルド情報, compatibilityVersion: 互換判定用の版)。
    // 各行は "Key: value" 形式で、末尾に改行を含みません。
    [[nodiscard]] std::string FormatBuildDetails(
        const BuildInfo& info,
        std::string_view compatibilityVersion);
    // 実行中のエンジンのサポート用ビルド情報を整形します。
    [[nodiscard]] std::string FormatBuildDetails();
}
