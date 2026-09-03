#pragma once

#include <filesystem>

namespace LamaPon
{
    class ProjectInstanceLock final
    {
    public:
        explicit ProjectInstanceLock(
            const std::filesystem::path& projectRoot);
        ~ProjectInstanceLock();

        ProjectInstanceLock(const ProjectInstanceLock&) = delete;
        ProjectInstanceLock& operator=(
            const ProjectInstanceLock&) = delete;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return m_handle != nullptr;
        }

        // ロックを明示的に解放します（同じプロジェクトを開き直す
        // 前に呼びます）。デストラクタでも解放されます。
        void Release() noexcept;

    private:
        void* m_handle{};
    };

    [[nodiscard]] bool IsProjectEditorOpen(
        const std::filesystem::path& projectRoot);
}
