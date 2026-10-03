#include "LamaPon/Core/ProjectInstance.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const std::string& message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        // 検査用の一時領域を作り、このObjectが削除を担当する。
        TemporaryDirectory()
        {
            // 領域名の重複を避ける時計カウント
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonProjectInstanceTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        // 保持した検査用領域を削除し、削除失敗は例外を出さず無視する。
        ~TemporaryDirectory()
        {
            // 破棄時に無視する削除エラー
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // このObjectの寿命内で有効な検査用領域のパスを借用で返す。
        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        // 破棄時に削除する検査領域のパス
        std::filesystem::path m_path;
    };
}

// 等価パスの重複ロックと異なるProjectの同時ロック・破棄時の解除を検査する。
int main()
{
    // Project lock検査の例外を終了コードへ変換します。
    try
    {
        // Scope終了時に削除する検査領域
        TemporaryDirectory temporary;
        // 同一Projectとして比較するパス
        const auto project = temporary.Path() / L"Project";
        // 独立に開ける別Projectのパス
        const auto otherProject = temporary.Path() / L"Other";
        std::filesystem::create_directories(project);
        std::filesystem::create_directories(otherProject);

        Require(
            !LamaPon::IsProjectEditorOpen(project),
            "A project without a lock was reported as open.");

        {
            // 最初に取得するProjectロック
            LamaPon::ProjectInstanceLock first(project);
            Require(first.Acquired(), "The first project lock failed.");
            Require(
                LamaPon::IsProjectEditorOpen(project / L"."),
                "An equivalent project path did not find the lock.");

            // 等価パスで取得を試す重複ロック
            LamaPon::ProjectInstanceLock duplicate(
                project / L"folder" / L"..");
            Require(
                !duplicate.Acquired(),
                "A duplicate project lock was incorrectly acquired.");

            // 別Projectで取得するロック
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
    // 検査失敗を標準エラーへ出す(exception: 捕捉した検査エラー)。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
