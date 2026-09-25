#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/Version.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const char* const message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }
}

int main()
{
    try
    {
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

        auto dirty = clean;
        dirty.dirty = true;
        Require(LamaPon::FormatBuildLabel(dirty, "0.1.0")
                == "community/main @ 932b08c3a1b2-dirty",
            "Uncommitted changes must be marked in the label");

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

        // 実際に埋め込まれた値でも、表示が空にならないことを確かめます。
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
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
