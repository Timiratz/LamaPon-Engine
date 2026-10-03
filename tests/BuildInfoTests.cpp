#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/Version.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const char* const message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // 文字列が指定部分を含むかを調べる(text: 検査する文字列, part: 探す部分文字列)。
    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }
}

// Git情報の有無・未コミット状態・互換版を含むBuild表示を検査する。
int main()
{
    // Build情報検査の例外を終了コードへ変換します。
    try
    {
        // 未変更のGit情報の検査値
        const LamaPon::BuildInfo clean{
            "community/main",
            "932b08c3a1b2",
            "932b08c3a1b2c3d4e5f60718293a4b5c6d7e8f90",
            "Harden exported game assets",
            false,
        };
        Require(LamaPon::HasBuildSourceInfo(clean),
            "A branch and commit must count as source info");
        Require(LamaPon::FormatBuildLabel(clean, "0.1.0")
                == "community/main @ 932b08c3a1b2",
            "The label must be 'branch @ commit'");

        // 未コミット変更ありへ変える検査値
        auto dirty = clean;
        dirty.dirty = true;
        Require(LamaPon::FormatBuildLabel(dirty, "0.1.0")
                == "community/main @ 932b08c3a1b2-dirty",
            "Uncommitted changes must be marked in the label");

        // Git情報が不明な場合の検査値
        const LamaPon::BuildInfo unknown{
            "unknown",
            "unknown",
            "unknown",
            "",
            false,
        };
        Require(!LamaPon::HasBuildSourceInfo(unknown),
            "Unknown values must not count as source info");
        Require(LamaPon::FormatBuildLabel(unknown, "0.1.0") == "v0.1.0",
            "Builds without Git must fall back to the compatibility version");

        // Commitだけが判明する検査値
        const LamaPon::BuildInfo detachedCommitOnly{
            "",
            "932b08c3a1b2",
            "932b08c3a1b2c3d4e5f60718293a4b5c6d7e8f90",
            "",
            false,
        };
        Require(LamaPon::FormatBuildLabel(detachedCommitOnly, "0.1.0")
                == "unknown @ 932b08c3a1b2",
            "A missing branch must still show the commit");

        // Git情報と互換版を含む詳細表示
        const auto details =
            LamaPon::FormatBuildDetails(dirty, "0.1.0");
        Require(Contains(details, "Branch: community/main"),
            "Details must include the branch");
        Require(Contains(details,
                "Commit: 932b08c3a1b2c3d4e5f60718293a4b5c6d7e8f90"
                " (uncommitted changes)"),
            "Details must include the full commit and dirty state");
        Require(Contains(details,
                "Commit subject: Harden exported game assets"),
            "Details must include the commit subject");
        Require(Contains(details, "Compatibility version: 0.1.0"),
            "Details must keep the compatibility version");
        Require(details.back() != '\n',
            "Details must not end with a newline");
        Require(Contains(
                LamaPon::FormatBuildDetails(unknown, "0.1.0"),
                "Commit subject: (none)"),
            "An empty subject must be shown as (none)");

        // 実際に埋め込まれたBuild情報の借用
        const auto& embedded = LamaPon::GetBuildInfo();
        Require(!embedded.branch.empty() && !embedded.commit.empty(),
            "The embedded build info must not be empty");
        Require(!LamaPon::FormatBuildLabel().empty(),
            "The embedded build label must not be empty");
        Require(Contains(LamaPon::FormatBuildDetails(),
                std::string("Compatibility version: ")
                    + std::string(LamaPon::VersionString)),
            "Embedded details must use the engine compatibility version");
    }
    // 検査失敗を標準エラーへ出す(error: 捕捉した検査エラー)。
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
