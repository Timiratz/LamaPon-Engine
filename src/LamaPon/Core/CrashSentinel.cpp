#include "LamaPon/Core/CrashSentinel.h"

#include <exception>
#include <fstream>
#include <system_error>

namespace LamaPon
{
    CrashSentinel::CrashSentinel(
        const std::filesystem::path& projectRoot)
    {
        try
        {
            if (projectRoot.empty())
            {
                return;
            }
            const auto directory =
                projectRoot / L".lamapon";
            std::error_code createError;
            std::filesystem::create_directories(
                directory,
                createError);
            m_sentinelPath = directory / L"editor-session";

            m_previousRunCrashed =
                std::filesystem::is_regular_file(
                    m_sentinelPath);

            std::ofstream output(
                m_sentinelPath,
                std::ios::binary | std::ios::trunc);
            if (output)
            {
                // 中身は診断の手がかり程度で、存在自体が目印です。
                output << "LamaPonEditor session in progress.\n";
            }
        }
        catch (const std::exception&)
        {
            m_sentinelPath.clear();
            m_previousRunCrashed = false;
        }
    }

    CrashSentinel::~CrashSentinel()
    {
        MarkCleanExit();
    }

    void CrashSentinel::MarkCleanExit() noexcept
    {
        if (m_sentinelPath.empty())
        {
            return;
        }
        std::error_code removeError;
        std::filesystem::remove(
            m_sentinelPath,
            removeError);
        m_sentinelPath.clear();
    }
}
