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
    // v2は暗号文をHMAC-SHA256で検証します。認証を回避できるv1は拒否し、
    // 読み込みにはv2形式での再書き出しを要求します。
    constexpr std::array<char, 8> ArchiveMagic{
        'T', 'R', 'D', 'N', 'P', 'A', 'K', '2'
    };

    // AssetDatabase側と同じ判定です（一時ファイルと、移行が残した
    // <名前>.bakバックアップは書き出しに含めません）。
    bool IsTemporaryAssetFile(const std::filesystem::path& path)
    {
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete") != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
    }

    std::wstring Lowercase(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        return value;
    }

    bool IsSecretFile(const std::filesystem::path& path)
    {
        const auto name = Lowercase(path.filename().wstring());
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

    bool IsDevelopmentDirectory(const std::filesystem::path& path)
    {
        const auto name = Lowercase(path.filename().wstring());
        return name == L".git"
            || name == L".github"
            || name == L".lamapon"
            || name == L".vs"
            || name == L".vscode";
    }

    bool IsBuildArtifact(const std::filesystem::path& path)
    {
        constexpr std::array<std::wstring_view, 25> excluded{
            L".c", L".cc", L".cpp", L".cxx", L".h", L".hpp",
            L".hxx", L".inl", L".ixx", L".cs", L".py",
            L".pdb", L".idb", L".obj", L".lib", L".exp",
            L".ilk", L".map", L".pch", L".ipch", L".exe",
            L".dll", L".bat", L".cmd", L".ps1"
        };
        const auto extension = Lowercase(path.extension().wstring());
        return std::find(excluded.begin(), excluded.end(), extension)
            != excluded.end();
    }

    std::vector<std::uint8_t> ReadWholeFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open asset for packing: "
                + LamaPon::PathToUtf8(path));
        }
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine asset size: "
                + LamaPon::PathToUtf8(path));
        }
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

        const auto macKey = Crypto::DeriveMacKey(key);

        struct PendingEntry final
        {
            std::filesystem::path relativePath;
            std::vector<std::uint8_t> cipherText;
            Crypto::AesIv iv{};
            Crypto::MacTag mac{};
        };
        std::vector<PendingEntry> pending;
        std::unordered_set<std::string> normalizedPaths;
        AssetPackResult result;

        for (std::filesystem::recursive_directory_iterator iterator{
                sourceDirectory
            };
            iterator
                != std::filesystem::
                    recursive_directory_iterator{};
            ++iterator)
        {
            const auto& path = iterator->path();
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
            auto normalizedPath = PathToUtf8(
                relativePath.lexically_normal());
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
            // PKCS#7 can add a full AES block. Check before loading the
            // entire source file into memory.
            constexpr auto MaxPlainBytes =
                AssetArchiveLimits::MaxEntryCipherBytes
                - Crypto::AesIvSize;
            if (std::filesystem::file_size(path) > MaxPlainBytes)
            {
                throw std::runtime_error(
                    "Asset is too large for the archive: "
                    + PathToUtf8(relativePath));
            }

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
            const auto iv = Crypto::RandomIv();
            auto cipherText = Crypto::AesEncrypt(
                plainBytes,
                key,
                iv);

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

        Json index;
        index["entries"] = Json::array();
        std::uint64_t offset{};
        for (const auto& entry : pending)
        {
            Json ivArray = Json::array();
            for (const auto value : entry.iv)
            {
                ivArray.push_back(
                    static_cast<unsigned>(value));
            }
            Json macArray = Json::array();
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

        const std::string indexText = index.dump();
        if (indexText.size()
            > AssetArchiveLimits::MaxIndexCipherBytes
                - Crypto::AesIvSize)
        {
            throw std::runtime_error(
                "Asset archive index is too large.");
        }
        const auto indexIv = Crypto::RandomIv();
        const std::vector<std::uint8_t> indexPlainBytes(
            indexText.begin(),
            indexText.end());
        const auto indexCipherText = Crypto::AesEncrypt(
            indexPlainBytes,
            key,
            indexIv);
        // エントリのMAC差し替えを防ぐため、索引自体も改ざん検知の対象にします。
        const auto indexMac = Crypto::MacForCipherText(
            macKey,
            indexIv,
            indexCipherText.data(),
            indexCipherText.size());

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
