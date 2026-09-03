#include "LamaPon/Core/VersionCompare.h"

#include <algorithm>

namespace LamaPon
{
    std::vector<std::uint32_t> ParseVersionNumbers(
        std::string_view version)
    {
        if (!version.empty()
            && (version.front() == 'v'
                || version.front() == 'V'))
        {
            version.remove_prefix(1);
        }
        if (version.empty())
        {
            return {};
        }

        std::vector<std::uint32_t> numbers;
        std::uint64_t current = 0;
        bool hasDigit = false;
        for (const char character : version)
        {
            if (character >= '0' && character <= '9')
            {
                current = current * 10
                    + static_cast<std::uint64_t>(
                        character - '0');
                if (current > 0xFFFFFFFFull)
                {
                    return {};
                }
                hasDigit = true;
            }
            else if (character == '.')
            {
                if (!hasDigit)
                {
                    return {};
                }
                numbers.push_back(
                    static_cast<std::uint32_t>(current));
                current = 0;
                hasDigit = false;
            }
            else
            {
                return {};
            }
        }
        if (!hasDigit)
        {
            return {};
        }
        numbers.push_back(
            static_cast<std::uint32_t>(current));
        return numbers;
    }

    bool IsNewerVersion(
        const std::string_view current,
        const std::string_view latest)
    {
        const auto currentNumbers =
            ParseVersionNumbers(current);
        const auto latestNumbers =
            ParseVersionNumbers(latest);
        if (currentNumbers.empty() || latestNumbers.empty())
        {
            return false;
        }

        const std::size_t count = std::max(
            currentNumbers.size(),
            latestNumbers.size());
        for (std::size_t index = 0; index < count; ++index)
        {
            const std::uint32_t currentValue =
                index < currentNumbers.size()
                    ? currentNumbers[index]
                    : 0;
            const std::uint32_t latestValue =
                index < latestNumbers.size()
                    ? latestNumbers[index]
                    : 0;
            if (latestValue != currentValue)
            {
                return latestValue > currentValue;
            }
        }
        return false;
    }
}
