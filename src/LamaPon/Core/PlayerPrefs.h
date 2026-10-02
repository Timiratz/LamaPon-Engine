#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class PersistenceProfiles;
    namespace Detail
    {
        struct LocalPersistenceDocument;
        class LocalPersistenceDocuments;
        class OnlinePersistenceCoordinator;
    }

    enum class PlayerPrefType
    {
        // 64ビット整数の設定値
        Integer,
        // 倍精度実数の設定値
        Number,
        // 真偽値の設定値
        Boolean,
        // 文字列の設定値
        String
    };

    // 値の更新と保存先の変更は、ローカル保存の通知先と同じメインスレッドで行います。
    // キーは1～128バイトのUTF-8とし、半角空白・タブ・改行だけのキーは拒否します。
    class PlayerPrefs final
    {
    public:
        // 空の設定と保存先を準備します(filePath: 設定ファイルのパス)。
        explicit PlayerPrefs(std::filesystem::path filePath);
        // メモリー内の設定を解放します。
        // 自動保存は行わないため、必要な保存は破棄前に呼び出し側で行います。
        ~PlayerPrefs();

        // 設定管理のコピーを禁止します。
        PlayerPrefs(const PlayerPrefs&) = delete;
        // 設定管理のコピー代入を禁止します。
        PlayerPrefs& operator=(const PlayerPrefs&) = delete;

        // 現在の設定ファイルを読み込み、成功時にメモリーの値を置き換えます。
        // ファイルがなければ空の設定とし、失敗時は従来の値を保って上書き保存を禁止します。
        void Load();
        // 設定ファイルを読み直し、成功時に未保存の値を置き換えます。
        // 読み込み失敗後は通常の再読み込みを拒否するため、明示的な復旧操作を使います。
        void Reload();

        // 読み込み失敗によって再読込と上書き保存を禁止しているかを返します。
        [[nodiscard]] bool HasLoadFailure() const noexcept;
        // 修復された設定ファイルを読み直し、成功時に読み書きの禁止を解除します。
        // 読み込み失敗の状態でない場合はlogic_errorを送出します。
        void RecoverAfterLoadFailure();
        // 読み込み失敗後のメモリー内の設定を破棄し、上書き保存を許可します。
        // ディスクの内容は次回のSaveで置換し、読み込み失敗の状態でなければlogic_errorを送出します。
        void ResetAfterLoadFailure();

        // 新しい保存先の設定を読み込んで切り替えます(filePath: 新しい設定ファイルのパス)。
        // 保存先が固定中または未保存の変更があれば拒否し、新しい文書の読み込み失敗では元の状態を保ちます。
        void Rebind(std::filesystem::path filePath);
        // 現在の設定を永続化し、成功時に変更フラグを解除して保存を通知します。
        // 読み込み失敗後は上書きを拒否し、明示的な復旧または破棄を要求します。
        void Save();
        // 未保存の設定変更があるかを返します。
        [[nodiscard]] bool IsDirty() const noexcept;
        // 現在の設定ファイルのパスへの参照を返します。
        // 参照はLoad・Reload・Rebindなどで内部状態を交換すると無効になります。
        [[nodiscard]] const std::filesystem::path&
            FilePath() const noexcept;

        // 設定キーが登録済みかを返します(key: 設定キー)。
        [[nodiscard]] bool HasKey(std::string_view key) const;
        // 登録済みの設定キーを辞書順で返します。
        [[nodiscard]] std::vector<std::string> Keys() const;
        // 登録された値の型を返します(key: 設定キー)。
        // 未登録のキーにはout_of_rangeを送出します。
        [[nodiscard]] PlayerPrefType TypeOf(std::string_view key) const;
        // 指定キーをメモリーから削除します(key: 設定キー)。
        void DeleteKey(std::string_view key);
        // メモリー内の設定を全て消去し、未保存の変更として記録します。
        void DeleteAll() noexcept;

        // 整数値を設定して変更を記録します(key: 設定キー, value: 整数値)。
        void SetInteger(std::string key, std::int64_t value);
        // 整数値を返します(key: 設定キー, defaultValue: 未登録・型不一致時の既定値)。
        [[nodiscard]] std::int64_t GetInteger(
            std::string_view key,
            std::int64_t defaultValue = 0) const;
        // 有限の実数を設定して変更を記録します(key: 設定キー, value: 実数値)。
        void SetNumber(std::string key, double value);
        // 実数値を返します(key: 設定キー, defaultValue: 未登録・型不一致時の既定値)。
        [[nodiscard]] double GetNumber(
            std::string_view key,
            double defaultValue = 0.0) const;
        // 真偽値を設定して変更を記録します(key: 設定キー, value: 真偽値)。
        void SetBoolean(std::string key, bool value);
        // 真偽値を返します(key: 設定キー, defaultValue: 未登録・型不一致時の既定値)。
        [[nodiscard]] bool GetBoolean(
            std::string_view key,
            bool defaultValue = false) const;
        // 文字列を設定して変更を記録します(key: 設定キー, value: UTF-8の設定文字列)。
        void SetString(std::string key, std::string value);
        // 文字列を返します(key: 設定キー, defaultValue: 未登録・型不一致時の既定値)。
        [[nodiscard]] std::string GetString(
            std::string_view key,
            std::string defaultValue = {}) const;

        // 現在の値を型名付きの保存文書JSONへ変換します。
        [[nodiscard]] std::string SerializeToJson() const;

    private:
        friend class PersistenceProfiles;
        friend class Detail::LocalPersistenceDocuments;
        friend class Detail::OnlinePersistenceCoordinator;

        // 値を保ったまま保存先のパスを交換します(filePath: 検証・公開済みの保存先)。
        // 保存先の固定権が取得中なら交換を省略します。
        void RelocateBinding(
            std::filesystem::path filePath) noexcept;
        // 保存先・値・保護状態を交換します(other: 準備済みの設定管理)。
        void SwapLoadedState(PlayerPrefs& other) noexcept;
        // 保存先の固定権を取得します(owner: nullではない所有者識別子)。
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
        // ファイルを再読込せず検証済み文書を設定へ変換します(fullDocument: 保存文書の全文, missing: ファイル未存在か)。
        // missingがtrueなら全文は空とし、それ以外では厳格な文書検証を行います。
        void LoadValidatedSnapshot(
            std::string_view fullDocument,
            bool missing);
        // 検証した文書を永続化してからメモリーを交換します(fullDocument: 同期する保存文書の全文)。
        void ApplyRemoteDocumentAtomically(std::string_view fullDocument);
        // 設定ファイルを永続的に削除してからメモリーの値を空にします。
        void DeleteRemoteDocumentAtomically();
        // 観測後の変更がない場合だけ文書を保存して交換します(fullDocument: 保存文書の全文, observed: 直前に観測した保存状態)。
        // 観測との不一致はfalseとし、保存や検証の失敗は例外とします。
        [[nodiscard]] bool ApplyRemoteDocumentAtomicallyIfUnchanged(
            std::string_view fullDocument,
            const Detail::LocalPersistenceDocument& observed);
        // 観測後の変更がない場合だけ設定を削除します(observed: 直前に観測した保存状態)。
        [[nodiscard]] bool DeleteRemoteDocumentAtomicallyIfUnchanged(
            const Detail::LocalPersistenceDocument& observed);

        struct Implementation;
        // 保存先・値・保護状態の実体
        std::unique_ptr<Implementation> m_implementation;
    };

    // ゲーム用保存先のパスを返します(applicationName: 保存先名の基になるゲーム名)。
    // LOCALAPPDATAを優先し、取得できない場合はOSの一時領域を使います。
    [[nodiscard]] std::filesystem::path
        UserDataDirectory(std::string_view applicationName);

    // スクリプト用に登録された設定管理への借用参照を返します。
    // 未登録ならnullを返し、共有登録の実体はCPPに1つだけ置いてEXEとDLL間の分離を避けます。
    [[nodiscard]] PlayerPrefs* ActivePlayerPrefs() noexcept;
    // スクリプト用の設定管理を登録します(prefs: 生存中の借用先、解除ならnull)。
    void SetActivePlayerPrefs(PlayerPrefs* prefs) noexcept;
}
