#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class PlayerPrefs;
    class SaveDataStore;
}

namespace LamaPon::Detail
{
    // 時刻はWin32の100ナノ秒単位で、識別情報は条件付き更新の再照合に使います。
    struct LocalPersistenceDocumentIdentity final
    {
        // ファイルを持つボリューム番号
        std::uint64_t volumeSerial{};
        // ファイル固有の16バイトID
        std::array<std::uint8_t, 16u> fileId{};
        // ファイル内容のバイト数
        std::uint64_t byteLength{};
        // 最終書き込み時刻
        std::int64_t lastWriteTime{};
        // メタデータ変更時刻
        std::int64_t changeTime{};
        // 識別情報の取得成功有無
        bool valid{};
        // ファイル全体の保持有無
        bool completeBytes{};

        // 保存文書の識別情報を全フィールドで比較します。
        friend bool operator==(
            const LocalPersistenceDocumentIdentity&,
            const LocalPersistenceDocumentIdentity&) = default;
    };

    // ディスクの未存在・利用不能・破損を区別し、同期時に安全側で判断します。
    enum class LocalPersistenceDocumentState : std::uint8_t
    {
        // 検証済み文書を読み込み済み
        Loaded,
        // 対象が存在しない状態
        Missing,
        // 安全に読み込めない状態
        Unavailable,
        // 内容または一覧が不正
        Corrupt
    };

    // Loadedのbytesは検証済みで、Corruptのbytesは同一性確認だけに使います。
    // completeBytesがfalseの過大ファイルでは、内容を保持せず識別情報だけを残します。
    struct LocalPersistenceDocument final
    {
        // 厳密な文書読み取りの状態
        LocalPersistenceDocumentState state{
            LocalPersistenceDocumentState::Unavailable
        };
        // 読み取った文書のバイト列
        std::vector<std::uint8_t> bytes;
        // 照合用のファイル識別情報
        LocalPersistenceDocumentIdentity identity;
    };

    // LoadedのslotsだけがWindows ordinal-ignore-caseで一意なスロット一覧です。
    struct LocalPersistenceSlotListing final
    {
        // 厳密な列挙結果の状態
        LocalPersistenceDocumentState state{
            LocalPersistenceDocumentState::Unavailable
        };
        // 検証済みスロット名の配列
        std::vector<std::string> slots;
    };

    enum class LocalPersistenceConditionalApplyResult : std::uint8_t
    {
        // 要求を反映済みの状態
        Applied,
        // 観測後にローカルが変化
        LocalChanged
    };

    enum class LocalPersistenceResourceKind : std::uint8_t
    {
        // 設定値の保存文書
        PlayerPrefs,
        // スロットの保存文書
        SaveData
    };

    // source・filePath・slotは通知中だけ有効な非所有参照で、通知経路では割り当てません。
    struct LocalPersistenceCommitEvent final
    {
        // コミット対象の文書種別
        LocalPersistenceResourceKind kind{
            LocalPersistenceResourceKind::PlayerPrefs
        };
        // 通知元オブジェクトの参照
        const void* source{};
        // 対象ファイルパスの参照
        const std::filesystem::path* filePath{};
        // 対象のスロット名
        std::string_view slot;
        // 削除コミットの有無
        bool deleted{};
    };

    // コミットを通知します(context: 登録側の文脈, profileEpoch: プロファイル世代, event: コミット情報)。
    // 全通知と登録操作は主スレッド限定で、falseでも保存は成功し後続処理へ失敗を伝えます。
    using LocalPersistenceCommitCallback = bool(*)(
        void* context,
        std::uint64_t profileEpoch,
        const LocalPersistenceCommitEvent& event) noexcept;
    // 削除の実行可否を返します(context: 登録側の文脈, profileEpoch: プロファイル世代, event: 削除対象)。
    // 同期用の事前記録を確定できずfalseを返した場合は、既存ファイルを維持します。
    using LocalPersistencePreDeleteCallback = bool(*)(
        void* context,
        std::uint64_t profileEpoch,
        const LocalPersistenceCommitEvent& event) noexcept;

    // 通知登録を識別する番号
    using LocalPersistenceObserverToken = std::uint64_t;

    // 通知登録を差し替えます(callback: コミット通知, context: 非所有の文脈, profileEpoch: 世代, preDeleteCallback: 削除前通知)。
    // callbackがnullなら0を返し、非所有の通知先と文脈は登録解除まで呼び出し側が保持します。
    [[nodiscard]] LocalPersistenceObserverToken
        AttachLocalPersistenceCommitObserver(
        LocalPersistenceCommitCallback callback,
        void* context,
        std::uint64_t profileEpoch,
        LocalPersistencePreDeleteCallback preDeleteCallback = nullptr) noexcept;
    // 現在の登録が一致した場合に解除します(token: 登録番号)。
    [[nodiscard]] bool DetachLocalPersistenceCommitObserver(
        LocalPersistenceObserverToken token) noexcept;
    // 通知失敗の有無を取得し、失敗フラグを解除します。
    [[nodiscard]] bool ConsumeLocalPersistenceObserverFailure() noexcept;

    class ScopedLocalPersistenceObserverSuppression final
    {
    public:
        // このスレッドの保存通知と削除前通知を抑制します。
        ScopedLocalPersistenceObserverSuppression() noexcept;
        // このスコープ分の通知抑制を解除します。
        ~ScopedLocalPersistenceObserverSuppression();

        // 抑制回数の二重解放を防ぐためコピーを禁止します。
        ScopedLocalPersistenceObserverSuppression(
            const ScopedLocalPersistenceObserverSuppression&) = delete;
        // 抑制回数の二重解放を防ぐためコピー代入を禁止します。
        ScopedLocalPersistenceObserverSuppression& operator=(
            const ScopedLocalPersistenceObserverSuppression&) = delete;
    };

    enum class LocalPersistenceTestFailPoint : std::uint8_t
    {
        // 失敗を注入しない設定
        None,
        // 書き込み確定前の失敗
        BeforeFlush,
        // 書き込み確定後の公開前失敗
        AfterFlushBeforePublish
    };

    // 次に一致する保存段階へ一度だけ失敗を注入します(failPoint: 失敗段階)。
    void SetLocalPersistenceTestFailPoint(
        LocalPersistenceTestFailPoint failPoint) noexcept;

    // 二重取得でロックの排他性を検査します(targetPath: 検査する保存先)。
    // 回帰テスト専用で、親フォルダーとロックファイルを作成する場合があります。
    [[nodiscard]] bool IsLocalPersistenceLockExclusiveForTesting(
        const std::filesystem::path& targetPath);

    // 同期向けの厳密な文書操作で、更新前に文書全体を検証し失敗時は例外を送出します。
    // 条件付き操作は同じ保存先ロック内で観測を再照合し、先行更新があればLocalChangedです。
    class LocalPersistenceDocuments final
    {
    public:
        // ディスク上の設定文書を厳密に読みます(playerPrefs: 保存先を持つ設定値)。
        [[nodiscard]] static LocalPersistenceDocument ReadPlayerPrefs(
            const PlayerPrefs& playerPrefs);
        // ディスク上の保存文書を厳密に読みます(saveData: 保存先, slot: スロット名)。
        [[nodiscard]] static LocalPersistenceDocument ReadSaveData(
            const SaveDataStore& saveData,
            std::string_view slot);
        // スロット名の一意性と上限を検査して列挙します(saveData: 列挙する保存先)。
        // 各文書の本文は読み込まず、名前の配列を昇順で返します。
        [[nodiscard]] static LocalPersistenceSlotListing ListSaveData(
            const SaveDataStore& saveData);

        // リモート設定文書を反映します(playerPrefs: 更新する設定値, fullDocument: 文書全体)。
        // メモリーの置換状態も公開前に準備し、PlayerPrefs本体のアドレスを維持します。
        static void ApplyPlayerPrefs(
            PlayerPrefs& playerPrefs,
            std::span<const std::uint8_t> fullDocument);
        // 設定文書を削除しメモリーを空にします(playerPrefs: 更新する設定値)。
        static void DeletePlayerPrefs(PlayerPrefs& playerPrefs);
        // 観測と一致する設定文書だけを更新します(playerPrefs: 更新先, observed: 厳密な読み取り結果, fullDocument: 文書全体)。
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            ApplyPlayerPrefsIfUnchanged(
            PlayerPrefs& playerPrefs,
            const LocalPersistenceDocument& observed,
            std::span<const std::uint8_t> fullDocument);
        // 観測と一致する設定文書だけを削除します(playerPrefs: 更新先, observed: 厳密な読み取り結果)。
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            DeletePlayerPrefsIfUnchanged(
            PlayerPrefs& playerPrefs,
            const LocalPersistenceDocument& observed);
        // 読み込み済みの設定状態を交換します(target: 切り替える設定値, prepared: 準備済み設定値)。
        // プロファイル切り替え用で、通知登録は移動しません。
        static void SwapPlayerPrefsLoadedState(
            PlayerPrefs& target,
            PlayerPrefs& prepared) noexcept;
        // 読み取り結果から設定メモリーを復元します(target: 復元先, snapshot: 厳密な読み取り結果)。
        // ディスクを再読み込み・変更せず、LoadedとMissing以外は拒否します。
        static void LoadPlayerPrefsSnapshot(
            PlayerPrefs& target,
            const LocalPersistenceDocument& snapshot);
        // リモート保存文書を検証して公開します(saveData: 保存先, slot: スロット名, fullDocument: 文書全体)。
        static void ApplySaveData(
            SaveDataStore& saveData,
            std::string_view slot,
            std::span<const std::uint8_t> fullDocument);
        // 指定スロットの文書を削除します(saveData: 保存先, slot: スロット名)。
        static void DeleteSaveData(
            SaveDataStore& saveData,
            std::string_view slot);
        // 観測と一致する保存文書だけを更新します(saveData: 保存先, slot: スロット名, observed: 厳密な読み取り結果, fullDocument: 文書全体)。
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            ApplySaveDataIfUnchanged(
            SaveDataStore& saveData,
            std::string_view slot,
            const LocalPersistenceDocument& observed,
            std::span<const std::uint8_t> fullDocument);
        // 観測と一致する保存文書だけを削除します(saveData: 保存先, slot: スロット名, observed: 厳密な読み取り結果)。
        [[nodiscard]] static LocalPersistenceConditionalApplyResult
            DeleteSaveDataIfUnchanged(
            SaveDataStore& saveData,
            std::string_view slot,
            const LocalPersistenceDocument& observed);
    };

    // 文書を確定して公開します(targetPath: 保存先, bytes: 文書全体)。
    // 保存側と同期側でロックを共有し、.writingへのFlushFileBuffers成功後だけWRITE_THROUGHで改名します。
    void DurablePublishLocalDocument(
        const std::filesystem::path& targetPath,
        std::string_view bytes);
    // 文書を確定削除し、未存在ならfalseを返します(targetPath: 保存先)。
    [[nodiscard]] bool DurableDeleteLocalDocument(
        const std::filesystem::path& targetPath);
    // 観測と一致する文書だけを公開します(targetPath: 保存先, observed: 観測結果, bytes: 文書全体, maximumBytes: 読み取り上限, saveSlot: 空なら設定値)。
    // 同じロックを使うLamaPon保存側との比較更新で、ロックを無視する外部書き込みとの原子性は保証しません。
    // Missingの観測には置換なしで公開し、後から現れたファイルを上書きしません。
    [[nodiscard]] LocalPersistenceConditionalApplyResult
        DurablePublishLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        std::string_view bytes,
        std::size_t maximumBytes,
        std::string_view saveSlot = {});
    // 観測と一致する文書だけを削除します(targetPath: 保存先, observed: 観測結果, maximumBytes: 読み取り上限, saveSlot: 空なら設定値)。
    [[nodiscard]] LocalPersistenceConditionalApplyResult
        DurableDeleteLocalDocumentIfUnchanged(
        const std::filesystem::path& targetPath,
        const LocalPersistenceDocument& observed,
        std::size_t maximumBytes,
        std::string_view saveSlot = {});
    // 保存コミットを登録先へ通知します(event: コミット情報)。
    void NotifyLocalPersistenceCommit(
        const LocalPersistenceCommitEvent& event) noexcept;
    // 削除前通知の許可を取得します(event: 削除対象)。
    [[nodiscard]] bool PrepareLocalPersistenceDelete(
        const LocalPersistenceCommitEvent& event) noexcept;

    // 設定文書の構造・型・上限を検証します(bytes: 文書全体)。
    // 重複キー、未知の文書キー、深さ64超、要素数65536超などを拒否し、違反は例外です。
    void ValidatePlayerPrefsFullDocument(std::string_view bytes);
    // 保存文書の構造・名前・上限を検証します(slot: スロット名, bytes: 文書全体)。
    // 文書内の名前はWindows ordinal-ignore-caseで照合し、違反は例外です。
    void ValidateSaveDataFullDocument(
        std::string_view slot,
        std::string_view bytes);
}
