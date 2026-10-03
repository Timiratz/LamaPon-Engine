#include "LamaPon/Assets/AssetPacker.h"

#include "LamaPon/Assets/AssetArchive.h"
#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    // 認証付き形式TRDNPAK2の識別子
    constexpr std::array<char, 8> ArchiveMagic{
        'T', 'R', 'D', 'N', 'P', 'A', 'K', '2'
    };

    // 作業中の資源と移行バックアップを除外判定する(path: 対象のパス)。
    bool IsTemporaryAssetFile(const std::filesystem::path& path)
    {
        // 除外判定用のファイル名
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete") != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    // 文字列を小文字に変換する(value: 変換する文字列)。
    std::wstring Lowercase(std::wstring value)
    {
        // 各文字を小文字にする(character: 変換する文字)。
        std::transform(value.begin(), value.end(), value.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        return value;
    }

    // 機密情報候補を名前と拡張子で検出する(path: 対象のパス)。
    bool IsSecretFile(const std::filesystem::path& path)
    {
        // 除外判定用のファイル名
        const auto name = Lowercase(path.filename().wstring());
        // 除外判定用の小文字拡張子
        const auto extension = Lowercase(path.extension().wstring());
        return name == L".env"
            || name.starts_with(L".env.")
            || name == L".npmrc"
            || name == L".pypirc"
            || name == L".netrc"
            || name == L"id_rsa"
            || name == L"id_ed25519"
            || name == L"id_ecdsa"
            || name == L"credentials.json"
            || name == L"secrets.json"
            || name.starts_with(L"service-account")
            || extension == L".pem"
            || extension == L".pfx"
            || extension == L".p12"
            || extension == L".p8"
            || extension == L".key";
    }

    // 開発用フォルダーを名前で除外判定する(path: 対象のパス)。
    bool IsDevelopmentDirectory(const std::filesystem::path& path)
    {
        // 除外判定用のファイル名
        const auto name = Lowercase(path.filename().wstring());
        return name == L".git"
            || name == L".github"
            || name == L".lamapon"
            || name == L".vs"
            || name == L".vscode";
    }

    // ソース・実行形式・ビルド成果物を除外判定する(path: 対象のパス)。
    bool IsBuildArtifact(const std::filesystem::path& path)
    {
        // ソース・成果物の除外拡張子
        constexpr std::array<std::wstring_view, 25> excluded{
            L".c", L".cc", L".cpp", L".cxx", L".h", L".hpp",
            L".hxx", L".inl", L".ixx", L".cs", L".py",
            L".pdb", L".idb", L".obj", L".lib", L".exp",
            L".ilk", L".map", L".pch", L".ipch", L".exe",
            L".dll", L".bat", L".cmd", L".ps1"
        };
        // 除外判定用の小文字拡張子
        const auto extension = Lowercase(path.extension().wstring());
        return std::find(excluded.begin(), excluded.end(), extension)
            != excluded.end();
    }

    // ファイル全体をバイナリーで読み、失敗は例外にする(path: 読み込むパス)。
    std::vector<std::uint8_t> ReadWholeFile(
        const std::filesystem::path& path)
    {
        // サイズを測定するバイナリー入力
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open asset for packing: "
                + LamaPon::PathToUtf8(path));
        }
        // 元ファイルの総バイト数
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine asset size: "
                + LamaPon::PathToUtf8(path));
        }
        // 読み込んだ元ファイルの内容
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!input)
            {
                throw std::runtime_error(
                    "Failed to read asset for packing: "
                    + LamaPon::PathToUtf8(path));
            }
        }
        return bytes;
    }
}

namespace LamaPon
{
    AssetPackResult PackAssets(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& archiveOutputPath,
        const Crypto::AesKey& key,
        const std::vector<std::wstring>& skipExtensions,
        const AssetPackTransform& transform)
    {
        if (!std::filesystem::is_directory(sourceDirectory))
        {
            throw std::runtime_error(
                "Asset source directory was not found: "
                + PathToUtf8(sourceDirectory));
        }

        // 全体で共有する派生認証鍵
        const auto macKey = Crypto::DeriveMacKey(key);

        struct PendingEntry final
        {
            // 索引用の相対アセットパス
            // 梱包ルート内の相対パス
            std::filesystem::path relativePath;
            // 保持するエントリー暗号文
            // 各エントリーの暗号文
            std::vector<std::uint8_t> cipherText;
            // エントリーの初期化ベクトル
            // 各エントリーの独立したIV
            Crypto::AesIv iv{};
            // IVと暗号文の認証タグ
            // 各エントリーの認証タグ
            Crypto::MacTag mac{};
        };
        // 出力まで保持する暗号化資源
        std::vector<PendingEntry> pending;
        // 大小を区別しない重複検出キー
        std::unordered_set<std::string> normalizedPaths;
        // 梱包件数・容量・対象一覧
        AssetPackResult result;

        // 梱包元フォルダーの走査位置
        for (std::filesystem::recursive_directory_iterator iterator{
                sourceDirectory
            };
            iterator
                != std::filesystem::
                    recursive_directory_iterator{};
            ++iterator)
        {
            // 走査中のアセットパス
            const auto& path = iterator->path();
            // 梱包ルート内の相対パス
            const auto relativePath =
                path.lexically_relative(sourceDirectory);
            if (iterator->is_symlink())
            {
                throw std::runtime_error(
                    "Symbolic links cannot be exported from assets: "
                    + PathToUtf8(relativePath));
            }
            if (iterator->is_directory())
            {
                if (IsDevelopmentDirectory(path))
                {
                    iterator.disable_recursion_pending();
                    result.excludedFiles.push_back(relativePath);
                }
                continue;
            }
            if (!iterator->is_regular_file())
            {
                continue;
            }
            if (LamaPon::AssetDatabase::IsMetaFile(path)
                || IsTemporaryAssetFile(path))
            {
                continue;
            }
            if (IsSecretFile(path))
            {
                throw std::runtime_error(
                    "A file that may contain credentials is in assets: "
                    + PathToUtf8(relativePath));
            }
            // 除外判定用の小文字拡張子
            const auto extension = Lowercase(path.extension().wstring());
            if (IsBuildArtifact(path)
                || std::find(skipExtensions.begin(),
                    skipExtensions.end(), extension)
                    != skipExtensions.end())
            {
                result.excludedFiles.push_back(relativePath);
                continue;
            }

            if (pending.size() >= AssetArchiveLimits::MaxEntries
                || PathToUtf8(relativePath).size()
                    > AssetArchiveLimits::MaxPathBytes)
            {
                throw std::runtime_error(
                    "Asset archive entry count or path is too large: "
                    + PathToUtf8(relativePath));
            }
            // 重複検出用の小文字パス
            auto normalizedPath = PathToUtf8(
                relativePath.lexically_normal());
            // 相対パスを小文字キーへ変換する(value: 符号なしのUTF8バイト)。
            std::transform(normalizedPath.begin(), normalizedPath.end(),
                normalizedPath.begin(),
                [](const unsigned char value)
                {
                    return static_cast<char>(std::tolower(value));
                });
            if (!normalizedPaths.emplace(normalizedPath).second)
            {
                throw std::runtime_error(
                    "Duplicate asset archive path: "
                    + PathToUtf8(relativePath));
            }
            // 一ブロックの余裕を引く平文上限
            constexpr auto MaxPlainBytes =
                AssetArchiveLimits::MaxEntryCipherBytes
                - Crypto::AesIvSize;
            // パディングで一ブロック増える分を見込み、読み込み前に容量を検証する。
            if (std::filesystem::file_size(path) > MaxPlainBytes)
            {
                throw std::runtime_error(
                    "Asset is too large for the archive: "
                    + PathToUtf8(relativePath));
            }

            // 変換後に暗号化する平文
            auto plainBytes = ReadWholeFile(path);
            if (transform)
            {
                transform(relativePath, plainBytes);
            }
            if (plainBytes.size() > MaxPlainBytes)
            {
                throw std::runtime_error(
                    "Transformed asset is too large for the archive: "
                    + PathToUtf8(relativePath));
            }
            // 各エントリーの独立したIV
            const auto iv = Crypto::RandomIv();
            // 各エントリーの暗号文
            auto cipherText = Crypto::AesEncrypt(
                plainBytes,
                key,
                iv);

            // 各エントリーの認証タグ
            const auto mac = Crypto::MacForCipherText(
                macKey,
                iv,
                cipherText.data(),
                cipherText.size());

            result.totalBytes += plainBytes.size();
            ++result.fileCount;
            result.includedFiles.push_back(relativePath);
            pending.push_back(
                PendingEntry{
                    relativePath,
                    std::move(cipherText),
                    iv,
                    mac
                });
        }

        std::sort(result.includedFiles.begin(), result.includedFiles.end());
        std::sort(result.excludedFiles.begin(), result.excludedFiles.end());

        // 暗号文位置を列挙する索引JSON
        Json index;
        index["entries"] = Json::array();
        // 暗号データ内の次の相対位置
        std::uint64_t offset{};
        // 出力する暗号化エントリー
        for (const auto& entry : pending)
        {
            // 索引へ格納するIV配列
            Json ivArray = Json::array();
            // IV・認証タグの各バイト
            for (const auto value : entry.iv)
            {
                ivArray.push_back(
                    static_cast<unsigned>(value));
            }
            // 索引へ格納する認証タグ配列
            Json macArray = Json::array();
            // IV・認証タグの各バイト
            for (const auto value : entry.mac)
            {
                macArray.push_back(
                    static_cast<unsigned>(value));
            }
            index["entries"].push_back({
                {
                    "path",
                    PathToUtf8(entry.relativePath)
                },
                { "offset", offset },
                { "size", entry.cipherText.size() },
                { "iv", ivArray },
                { "mac", macArray }
            });
            offset += entry.cipherText.size();
        }

        // 文字列化した索引の平文
        const std::string indexText = index.dump();
        if (indexText.size()
            > AssetArchiveLimits::MaxIndexCipherBytes
                - Crypto::AesIvSize)
        {
            throw std::runtime_error(
                "Asset archive index is too large.");
        }
        // 索引の独立したIV
        const auto indexIv = Crypto::RandomIv();
        // 暗号化する索引の平文
        const std::vector<std::uint8_t> indexPlainBytes(
            indexText.begin(),
            indexText.end());
        // 出力する暗号化索引
        const auto indexCipherText = Crypto::AesEncrypt(
            indexPlainBytes,
            key,
            indexIv);

        // IVと索引暗号文の認証タグ
        const auto indexMac = Crypto::MacForCipherText(
            macKey,
            indexIv,
            indexCipherText.data(),
            indexCipherText.size());

        // 出力先フォルダーの生成結果
        std::error_code directoryError;
        if (!EnsureDirectoryExists(
                archiveOutputPath.parent_path(),
                directoryError))
        {
            throw std::filesystem::filesystem_error(
                "Could not create the asset archive directory",
                archiveOutputPath.parent_path(),
                directoryError);
        }
        // 既存内容を置き換える出力先
        std::ofstream output(
            archiveOutputPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create asset archive: "
                + PathToUtf8(archiveOutputPath));
        }

        output.write(ArchiveMagic.data(), ArchiveMagic.size());
        // 暗号化索引のバイト数
        const std::uint64_t indexCipherSize =
            indexCipherText.size();
        output.write(
            reinterpret_cast<const char*>(&indexCipherSize),
            sizeof(indexCipherSize));
        output.write(
            reinterpret_cast<const char*>(indexIv.data()),
            static_cast<std::streamsize>(indexIv.size()));
        output.write(
            reinterpret_cast<const char*>(indexMac.data()),
            static_cast<std::streamsize>(indexMac.size()));
        output.write(
            reinterpret_cast<const char*>(
                indexCipherText.data()),
            static_cast<std::streamsize>(
                indexCipherText.size()));
        // 出力する暗号化エントリー
        for (const auto& entry : pending)
        {
            output.write(
                reinterpret_cast<const char*>(
                    entry.cipherText.data()),
                static_cast<std::streamsize>(
                    entry.cipherText.size()));
        }
        if (!output)
        {
            throw std::runtime_error(
                "Failed to write asset archive: "
                + PathToUtf8(archiveOutputPath));
        }

        return result;
    }
}
