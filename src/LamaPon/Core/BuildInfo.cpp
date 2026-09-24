#include "LamaPon/Core/BuildInfo.h"

#include "LamaPon/Core/Version.h"

namespace LamaPon
{
    namespace
    {
        [[nodiscard]] bool IsKnown(const std::string_view value) noexcept
        {
            return !value.empty() && value != "unknown";
        }
    }

    bool HasBuildSourceInfo(const BuildInfo& info) noexcept
    {
        return IsKnown(info.branch) || IsKnown(info.commit);
    }

    std::string FormatBuildLabel(
        const BuildInfo& info,
        const std::string_view compatibilityVersion)
    {
        if (!HasBuildSourceInfo(info))
        {
            std::string label{ "v" };
            label += compatibilityVersion;
            return label;
        }

        std::string label{
            IsKnown(info.branch) ? info.branch : "unknown" };
        label += " @ ";
        label += IsKnown(info.commit) ? info.commit : "unknown";
        if (info.dirty)
        {
            label += "-dirty";
        }
        return label;
    }

    std::string FormatBuildLabel()
    {
        return FormatBuildLabel(GetBuildInfo(), VersionString);
    }

    std::string FormatBuildDetails(
        const BuildInfo& info,
        const std::string_view compatibilityVersion)
    {
        const auto valueOrUnknown =
            [](const std::string_view value)
            {
                return IsKnown(value) ? value : "unknown";
            };

        std::string details{ "Branch: " };
        details += valueOrUnknown(info.branch);
        details += "\nCommit: ";
        details += valueOrUnknown(info.commitFull);
        if (info.dirty)
        {
            details += " (uncommitted changes)";
        }
        details += "\nCommit subject: ";
        details += info.commitSubject.empty()
            ? std::string_view{ "(none)" }
            : info.commitSubject;
        details += "\nCompatibility version: ";
        details += compatibilityVersion;
        return details;
    }

    std::string FormatBuildDetails()
    {
        return FormatBuildDetails(GetBuildInfo(), VersionString);
    }
}
