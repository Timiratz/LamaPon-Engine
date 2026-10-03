#pragma once

#include <filesystem>

namespace LamaPon
{
    class ProjectInstanceLock final
    {
    public:
        // プロジェクトの起動目印を確保します(projectRoot: プロジェクトのルート)。
        // 既に目印があれば未取得とし、作成エラーは例外を送出します。
        explicit ProjectInstanceLock(
            const std::filesystem::path& projectRoot);
        // プロジェクトの起動目印を解放します。
        ~ProjectInstanceLock();

        // 起動目印の所有権のコピーを禁止します。
        ProjectInstanceLock(const ProjectInstanceLock&) = delete;
        // 起動目印のコピー代入を禁止します。
        ProjectInstanceLock& operator=(
            const ProjectInstanceLock&) = delete;

        // 今回の起動でプロジェクトの目印を確保したかを返します。
        [[nodiscard]] bool Acquired() const noexcept
        {
            return m_handle != nullptr;
        }

        // 同じプロジェクトを開き直せるよう起動目印を解放します。
        void Release() noexcept;

    private:
        // 所有する名前付きミューテックス
        void* m_handle{};
    };

    // プロジェクトの起動目印が存在するかを返します(projectRoot: プロジェクトのルート)。
    // 目印は同じWindowsセッション内で共有し、確認エラーは例外を送出します。
    [[nodiscard]] bool IsProjectEditorOpen(
        const std::filesystem::path& projectRoot);
}
