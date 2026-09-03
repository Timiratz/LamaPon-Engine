#pragma once

#include "LamaPon/Core/Api.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    class LAMAPON_API CrashReporter final
    {
    public:
        static void Install(
            std::filesystem::path outputDirectory,
            std::string applicationName);
        static void Uninstall() noexcept;

        [[nodiscard]] static bool IsInstalled() noexcept;
        [[nodiscard]] static bool WriteDiagnostic(
            std::string_view reason) noexcept;
    };
}
