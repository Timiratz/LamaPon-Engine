#include "LamaPon/Core/ProjectInstance.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonProjectInstanceTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };
}

int main()
{
    try
    {
        TemporaryDirectory temporary;
        const auto project = temporary.Path() / L"Project";
        const auto otherProject = temporary.Path() / L"Other";
        std::filesystem::create_directories(project);
        std::filesystem::create_directories(otherProject);

        Require(
            !LamaPon::IsProjectEditorOpen(project),
            "A project without a lock was reported as open.");

        {
            LamaPon::ProjectInstanceLock first(project);
            Require(first.Acquired(), "The first project lock failed.");
            Require(
                LamaPon::IsProjectEditorOpen(project / L"."),
                "An equivalent project path did not find the lock.");

            LamaPon::ProjectInstanceLock duplicate(
                project / L"folder" / L"..");
            Require(
                !duplicate.Acquired(),
                "A duplicate project lock was incorrectly acquired.");

            LamaPon::ProjectInstanceLock other(otherProject);
            Require(
                other.Acquired(),
                "A different project should be allowed to open.");
        }

        Require(
            !LamaPon::IsProjectEditorOpen(project),
            "The project lock was not released on shutdown.");

        std::cout << "Project instance lock tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
