#include "LamaPon/Core/PersistenceProfiles.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    // スロットファイルの拡張子
    constexpr std::string_view SaveSuffix = ".save.json";
    // 保存先キーの用途と形式の識別列
    constexpr std::array<std::uint8_t, 29> ProfileHashDomain{
        'L', 'a', 'm', 'a', 'P', 'o', 'n', '.',
        'P', 'e', 'r', 's', 'i', 's', 't', 'e', 'n', 'c', 'e', '.',
        'P', 'r', 'o', 'f', 'i', 'l', 'e', '.', '1'
    };

    class AlgorithmHandle final
    {
    public:
        // CNGアルゴリズムのハンドルを閉じます。
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        // 所有するCNGアルゴリズム
        BCRYPT_ALG_HANDLE value{};
    };

    class HashHandle final
    {
    public:
        // SHA-256の計算状態を破棄します。
        ~HashHandle()
        {
            if (value != nullptr)
            {
                BCryptDestroyHash(value);
            }
        }

        // 所有するハッシュ計算状態
        BCRYPT_HASH_HANDLE value{};
    };

    // CNGの失敗を例外へ変換します(status: NTSTATUSの結果, operation: 診断に付ける操作名)。
    void RequireNtSuccess(
        const NTSTATUS status,
        const char* operation)
    {
        if (status < 0)
        {
            throw std::runtime_error(
                std::string{ operation } + " failed.");
        }
    }

    // 長さを8バイトのビッグエンディアンへ変換します(length: 入力のバイト数)。
    std::array<std::uint8_t, 8> EncodeLength(
        const std::size_t length)
    {
        // 長さを格納する8バイト列
        std::array<std::uint8_t, 8> bytes{};
        // 下位バイトから取り出す長さ値
        auto value = static_cast<std::uint64_t>(length);
        // 低位から取り出すバイト番号
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[bytes.size() - index - 1] =
                static_cast<std::uint8_t>(value & 0xff);
            value >>= 8;
        }
        return bytes;
    }

    // バイト列をハッシュ計算へ追加します(hash: 計算状態, data: 入力先頭, size: 入力バイト数)。
    void AppendHashData(
        const BCRYPT_HASH_HANDLE hash,
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size == 0)
        {
            return;
        }
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Persistence player id is too long.");
        }
        RequireNtSuccess(
            BCryptHashData(
                hash,
                const_cast<PUCHAR>(data),
                static_cast<ULONG>(size),
                0),
            "BCryptHashData(profile id)");
    }

    // 識別子のUTF-8と長さを検証します(value: 識別子, maximumBytes: 上限バイト数, message: 不正時の例外本文)。
    void ValidateOpaqueId(
        const std::string_view value,
        const std::size_t maximumBytes,
        const char* message)
    {
        if (value.empty()
            || value.size() > maximumBytes
            || LamaPon::Utf8ToWide(value).empty())
        {
            throw std::invalid_argument(message);
        }
    }

    // バイト数と文字列をハッシュ計算へ追加します(hash: 計算状態, value: UTF-8の識別子)。
    void AppendLengthTagged(
        const BCRYPT_HASH_HANDLE hash,
        const std::string_view value)
    {
        // 8バイトへ符号化した識別子長
        const auto length = EncodeLength(value.size());
        AppendHashData(hash, length.data(), length.size());
        AppendHashData(
            hash,
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size());
    }

    // 保存スロットの拡張子かを返します(path: 照合するパス)。
    bool HasSaveSuffix(const std::filesystem::path& path)
    {
        // 照合するファイル名のUTF-8表記
        const auto name = LamaPon::PathToUtf8(path.filename());
        return name.size() >= SaveSuffix.size()
            && name.ends_with(SaveSuffix);
    }

    // インポート失敗の結果を構築します(message: 固定文言の失敗理由)。
    LamaPon::GuestPersistenceImportResult FailedImport(
        std::string message)
    {
        return {
            LamaPon::GuestPersistenceImportStatus::Failed,
            std::move(message)
        };
    }

    // ステージ固有の乱数を小文字16進32桁で生成します。
    std::string RandomStageId()
    {
        // ステージを識別する乱数16バイト
        std::array<std::uint8_t, 16> random{};
        RequireNtSuccess(
            BCryptGenRandom(
                nullptr,
                random.data(),
                static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "BCryptGenRandom(persistence import)");

        // 小文字16進表記の文字一覧
        constexpr char Hex[] = "0123456789abcdef";
        // 16進表記の識別文字列
        std::string encoded;
        encoded.reserve(random.size() * 2);
        // 16進表記へ変換する乱数バイト
        for (const auto byte : random)
        {
            encoded.push_back(Hex[byte >> 4]);
            encoded.push_back(Hex[byte & 0x0f]);
        }
        SecureZeroMemory(random.data(), random.size());
        return encoded;
    }
}

namespace LamaPon
{
    PersistenceProfiles::PersistenceProfiles(
        std::filesystem::path userDataDirectory,
        std::string gameId,
        std::string environmentId)
        : m_userDataDirectory(std::move(userDataDirectory)),
          m_gameId(std::move(gameId)),
          m_environmentId(std::move(environmentId))
    {
        if (m_userDataDirectory.empty())
        {
            throw std::invalid_argument(
                "Persistence user data directory must not be empty.");
        }
        ValidateOpaqueId(
            m_gameId,
            128,
            "Persistence game id must be valid UTF-8 text with 1 to 128 bytes.");
        ValidateOpaqueId(
            m_environmentId,
            64,
            "Persistence environment id must be valid UTF-8 text with 1 to 64 bytes.");
    }

    const std::filesystem::path&
        PersistenceProfiles::UserDataDirectory() const noexcept
    {
        return m_userDataDirectory;
    }

    PersistenceProfilePaths PersistenceProfiles::Guest() const
    {
        return {
            m_userDataDirectory,
            m_userDataDirectory / L"PlayerPrefs.json",
            m_userDataDirectory / L"Saves",
            {},
            true
        };
    }

    std::string PersistenceProfiles::AccountStorageKey(
        const std::string_view playerId)
        const
    {
        ValidateOpaqueId(
            playerId,
            1024,
            "Persistence player id must be valid UTF-8 text with 1 to 1024 bytes.");

        // SHA-256アルゴリズムの所有者
        AlgorithmHandle algorithm;
        RequireNtSuccess(
            BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider(profile id)");
        // 保存先キーのハッシュ計算状態
        HashHandle hash;
        RequireNtSuccess(
            BCryptCreateHash(
                algorithm.value,
                &hash.value,
                nullptr,
                0,
                nullptr,
                0,
                0),
            "BCryptCreateHash(profile id)");

        AppendHashData(
            hash.value,
            ProfileHashDomain.data(),
            ProfileHashDomain.size());
        AppendLengthTagged(hash.value, m_gameId);
        AppendLengthTagged(hash.value, m_environmentId);
        AppendLengthTagged(hash.value, playerId);

        // 保存先を識別するSHA-256値
        std::array<std::uint8_t, 32> digest{};
        RequireNtSuccess(
            BCryptFinishHash(
                hash.value,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0),
            "BCryptFinishHash(profile id)");

        // 小文字16進表記の文字一覧
        constexpr char Hex[] = "0123456789abcdef";
        // 16進表記の識別文字列
        std::string encoded;
        encoded.reserve(digest.size() * 2);
        // 16進表記へ変換するハッシュ値
        for (const auto byte : digest)
        {
            encoded.push_back(Hex[byte >> 4]);
            encoded.push_back(Hex[byte & 0x0f]);
        }
        SecureZeroMemory(digest.data(), digest.size());
        return encoded;
    }

    PersistenceProfilePaths PersistenceProfiles::Account(
        const std::string_view playerId) const
    {
        // アカウントの保存先識別キー
        auto key = AccountStorageKey(playerId);
        // ハッシュ名で分離する保存ルート
        const auto root =
            m_userDataDirectory.parent_path()
            / L"OnlineProfiles"
            / PathFromUtf8(key);
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::move(key),
            false
        };
    }

    void PersistenceProfiles::Rebind(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        const PersistenceProfilePaths& profile) const
    {
        if (preferences.IsBindingLeased()
            || saves.IsBindingLeased())
        {
            throw std::logic_error(
                "Persistence binding is owned by online persistence.");
        }
        // 状態交換の後で割り当てに失敗しないよう、両パスの複製を先に作ります。
        // 切り替える設定ファイルのパス
        auto preferencesPath = profile.playerPrefsFile;
        // 切り替えるスロットの保存領域
        auto savesDirectory = profile.saveDataDirectory;
        // 設定の読込を先に完了し、失敗しないスロット保存先の交換を最後に行います。
        preferences.Rebind(std::move(preferencesPath));
        saves.Rebind(std::move(savesDirectory));
    }

    void PersistenceProfiles::RebindGuest(
        PlayerPrefs& preferences,
        SaveDataStore& saves) const
    {
        Rebind(preferences, saves, Guest());
    }

    void PersistenceProfiles::RebindAccount(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        const std::string_view playerId) const
    {
        Rebind(preferences, saves, Account(playerId));
    }

    GuestPersistenceImportResult
        PersistenceProfiles::ImportGuestToAccount(
            PlayerPrefs& activePreferences,
            SaveDataStore& activeSaves,
            const std::string_view playerId) const
    {
        // コピー元のゲスト保存先
        PersistenceProfilePaths guest;
        // 公開するアカウント保存先
        PersistenceProfilePaths account;
        // コピーと検証に使う固有ステージ
        std::filesystem::path staging;
        // 自身が作成したステージがあるか
        bool ownsStaging{};
        // 正式保存先への公開を試みたか
        bool attemptedFinalRename{};
        try
        {
            if (activePreferences.IsBindingLeased()
                || activeSaves.IsBindingLeased())
            {
                return FailedImport(
                    "Persistence binding is owned by online persistence.");
            }
            guest = Guest();
            account = Account(playerId);

            if (activePreferences.FilePath()
                    != guest.playerPrefsFile
                || activeSaves.Directory()
                    != guest.saveDataDirectory)
            {
                return FailedImport(
                    "Guest persistence must be active before import.");
            }
            if (activePreferences.HasLoadFailure())
            {
                return FailedImport(
                    "Guest PlayerPrefs must be recovered before import.");
            }

            if (std::filesystem::exists(account.rootDirectory))
            {
                return {
                    GuestPersistenceImportStatus::AccountAlreadyHasData,
                    {}
                };
            }

            if (activePreferences.IsDirty())
            {
                // アカウント側を作成する前にゲストの未保存設定を確定します。
                activePreferences.Save();
            }

            // ゲスト設定ファイルをコピーするか
            bool copyPreferences{};
            // リンクを追わない設定ファイル種別
            const auto preferencesStatus =
                std::filesystem::symlink_status(
                    guest.playerPrefsFile);
            if (std::filesystem::exists(preferencesStatus))
            {
                if (!std::filesystem::is_regular_file(
                        preferencesStatus))
                {
                    return FailedImport(
                        "Guest PlayerPrefs is not a regular file.");
                }
                copyPreferences = true;
            }

            // コピー元ファイルとスロット名の組
            std::vector<std::pair<
                std::filesystem::path,
                std::string>> saveFiles;
            // リンクを追わない保存領域の種別
            const auto savesStatus =
                std::filesystem::symlink_status(
                    guest.saveDataDirectory);
            if (std::filesystem::exists(savesStatus))
            {
                if (!std::filesystem::is_directory(savesStatus))
                {
                    return FailedImport(
                        "Guest save data is not a regular directory.");
                }
                // ゲストの保存領域の各項目
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        guest.saveDataDirectory))
                {
                    if (!HasSaveSuffix(entry.path()))
                    {
                        continue;
                    }
                    // リンクを追わない保存項目の種別
                    const auto status = entry.symlink_status();
                    if (!std::filesystem::is_regular_file(status))
                    {
                        return FailedImport(
                            "Guest save data contains an unsafe entry.");
                    }
                    // スロットの拡張子付きファイル名
                    const auto fileName =
                        entry.path().filename().wstring();
                    // UTF-16表記のスロット拡張子
                    constexpr std::wstring_view wideSuffix =
                        L".save.json";
                    saveFiles.emplace_back(
                        entry.path(),
                        WideToUtf8(
                            std::wstring_view(fileName).substr(
                                0,
                                fileName.size()
                                    - wideSuffix.size())));
                }
            }

            if (!copyPreferences && saveFiles.empty())
            {
                return {
                    GuestPersistenceImportStatus::NothingToImport,
                    {}
                };
            }

            // コピー先を作成する前に、保存文書を各ローダーで全て検証します。
            if (copyPreferences)
            {
                // ゲスト設定を検証する読取管理
                PlayerPrefs validation(guest.playerPrefsFile);
                validation.Load();
            }
            // ゲストスロットを検証する読取管理
            SaveDataStore saveValidation(
                guest.saveDataDirectory);
            // source: コピー元の保存ファイル
            // slot: UTF-8のスロット名
            for (const auto& [source, slot] : saveFiles)
            {
                static_cast<void>(source);
                if (!saveValidation.LoadJson(slot).has_value())
                {
                    throw std::runtime_error(
                        "Guest save data disappeared during validation.");
                }
            }

            std::filesystem::create_directories(
                account.rootDirectory.parent_path());

            // 既存のステージには触れず、名前が衝突した場合は新しい乱数で再試行します。
            // 固有ステージ名の作成試行回数
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                // 作成を試す固有ステージパス
                auto candidate = account.rootDirectory;
                candidate += L".importing.";
                candidate += PathFromUtf8(RandomStageId());
                if (std::filesystem::create_directory(candidate))
                {
                    staging = std::move(candidate);
                    ownsStaging = true;
                    break;
                }
            }
            if (!ownsStaging)
            {
                return FailedImport(
                    "A unique guest import stage could not be created.");
            }

            if (copyPreferences)
            {
                std::filesystem::copy_file(
                    guest.playerPrefsFile,
                    staging / L"PlayerPrefs.json");
            }
            if (!saveFiles.empty())
            {
                // ステージ内のスロット保存領域
                const auto stagingSaves = staging / L"Saves";
                std::filesystem::create_directory(stagingSaves);
                // source: コピー元の保存ファイル
                // slot: UTF-8のスロット名
                for (const auto& [source, slot] : saveFiles)
                {
                    static_cast<void>(slot);
                    std::filesystem::copy_file(
                        source,
                        stagingSaves / source.filename());
                }
            }

            // コピー中の変更も検出するためステージを再検証し、公開後へ引き継ぐ状態も準備します。
            // 公開後へ引き継ぐ検証済み設定
            PlayerPrefs preparedPreferences(
                staging / L"PlayerPrefs.json");
            preparedPreferences.Load();
            // ステージのスロットを検証する管理
            SaveDataStore stagedSaveValidation(
                staging / L"Saves");
            // source: コピー元の保存ファイル
            // slot: UTF-8のスロット名
            for (const auto& [source, slot] : saveFiles)
            {
                static_cast<void>(source);
                if (!stagedSaveValidation.LoadJson(slot).has_value())
                {
                    throw std::runtime_error(
                        "Staged save data failed validation.");
                }
            }

            // 正式公開後の割り当てを避けるため、最終パスの複製を先に完了します。
            // 正式公開後の設定ファイルのパス
            auto finalPreferencesPath = account.playerPrefsFile;
            // 正式公開後のスロット保存領域
            auto finalSavesDirectory = account.saveDataDirectory;
            preparedPreferences.RelocateBinding(
                std::move(finalPreferencesPath));

            // 検証済みのディレクトリを同じ親の中で正式な保存先へ公開します。
            attemptedFinalRename = true;
            std::filesystem::rename(
                staging,
                account.rootDirectory);
            ownsStaging = false;

            // 公開後は例外を送出せずに状態を交換し、外部が持つ管理オブジェクトへの参照を保ちます。
            activePreferences.SwapLoadedState(preparedPreferences);
            activeSaves.Rebind(std::move(finalSavesDirectory));
            return {
                GuestPersistenceImportStatus::Imported,
                {}
            };
        }
        catch (const std::exception&)
        {
            if (ownsStaging)
            {
                // 自身のステージを除去した結果
                std::error_code rollbackError;
                std::filesystem::remove_all(
                    staging,
                    rollbackError);
                if (rollbackError)
                {
                    return FailedImport(
                        "Guest import failed and staged data could not be removed.");
                }
            }
            if (attemptedFinalRename)
            {
                // 公開先の存在確認エラー
                std::error_code targetError;
                // 他の処理が保存先を公開したか
                const bool targetExists =
                    std::filesystem::exists(
                        account.rootDirectory,
                        targetError);
                if (!targetError && targetExists)
                {
                    return {
                        GuestPersistenceImportStatus::AccountAlreadyHasData,
                        {}
                    };
                }
            }
            return FailedImport("Guest persistence import failed.");
        }
    }
}
