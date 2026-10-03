#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class PersistenceProfiles;
    namespace Detail
    {
        class OnlinePersistenceCoordinator;
    }

    // 保存・削除と保存先の変更は、ローカル保存の通知先と同じメインスレッドで行います。
    class SaveDataStore final
    {
    public:
        // 保存スロットの格納先を設定します(directory: 保存先ディレクトリ)。
        explicit SaveDataStore(
            std::filesystem::path directory);

        // 保存先管理のコピーを禁止します。
        SaveDataStore(const SaveDataStore&) = delete;
        // 保存先管理のコピー代入を禁止します。
        SaveDataStore& operator=(const SaveDataStore&) = delete;
        // 外部の借用参照を維持するためムーブを禁止します。
        SaveDataStore(SaveDataStore&&) = delete;
        // 外部の借用参照を維持するためムーブ代入を禁止します。
        SaveDataStore& operator=(SaveDataStore&&) = delete;

        // 以後の保存先を切り替えます(directory: 新しい保存先ディレクトリ)。
        // 保存先の固定権が取得中なら切り替えを省略し、既存ファイルの移動は行いません。
        void Rebind(std::filesystem::path directory) noexcept;

        // JSON本文をスロットへ永続化します(slot: UTF-8の保存名, json: 保存するJSON本文)。
        // 不正な名前・JSON・保存失敗は例外とし、保存成功後にローカル保存を通知します。
        void SaveJson(
            std::string_view slot,
            std::string_view json);
        // スロットのJSON本文を読み込みます(slot: UTF-8の保存名)。
        // ファイルを開けない場合はnullopt、不正な文書は例外とし、旧形式の移行はメモリー内だけで行います。
        [[nodiscard]] std::optional<std::string>
            LoadJson(std::string_view slot) const;
        // スロットの保存先が通常ファイルかを返します(slot: UTF-8の保存名)。
        [[nodiscard]] bool HasSlot(
            std::string_view slot) const;
        // 保存スロットを削除し実際の削除可否を返します(slot: UTF-8の保存名)。
        // 削除前通知が拒否した場合やファイルが存在しない場合はfalseを返します。
        bool DeleteSlot(std::string_view slot);
        // 保存先のスロット名を昇順で返します。
        // .save.jsonで終わる通常ファイルを列挙するだけで、名前や本文の有効性は検証しません。
        [[nodiscard]] std::vector<std::string>
            ListSlots() const;
        // 名前を検証して保存ファイルのパスを返します(slot: UTF-8の保存名)。
        [[nodiscard]] std::filesystem::path
            SlotPath(std::string_view slot) const;
        // 現在の保存先ディレクトリへの参照を返します。
        [[nodiscard]] const std::filesystem::path&
            Directory() const noexcept
        {
            return m_directory;
        }

    private:
        friend class PersistenceProfiles;
        friend class Detail::OnlinePersistenceCoordinator;

        // 保存先の固定権を取得します(owner: nullではない所有者の識別子)。
        [[nodiscard]] bool AcquireBindingLease(
            const void* owner) noexcept;
        // 所有者が一致する場合だけ固定権を解放します(owner: 取得時の所有者識別子)。
        [[nodiscard]] bool ReleaseBindingLease(
            const void* owner) noexcept;
        // 保存先の固定権が取得中かを返します。
        [[nodiscard]] bool IsBindingLeased() const noexcept;
        // 固定権の所有者が一致するかを返します(owner: 照合する所有者識別子)。
        [[nodiscard]] bool BindingLeaseOwnedBy(
            const void* owner) const noexcept;
        // 固定権の所有者が一致する場合だけ保存先を切り替えます(directory: 新しい保存先, owner: 取得時の所有者識別子)。
        void RelocateBinding(
            std::filesystem::path directory,
            const void* owner) noexcept;

        // スロットファイルの保存先
        std::filesystem::path m_directory;
        // 保存先の固定権の所有者識別子
        const void* m_bindingLeaseOwner{};
    };
}
