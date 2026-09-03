#include "LamaPon/Assets/AssetPacker.h"

#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    // v2で「暗号文のHMAC-SHA256」を足しました。v1は読みません
    // （読めるようにすると、改ざんした側がv1で作り直すだけで
    // 検証を外せてしまうため）。書き出し直しが必要です。
    constexpr std::array<char, 8> ArchiveMagic{
        'T', 'R', 'D', 'N', 'P', 'A', 'K', '2'
    };

    bool IsMetaFile(const std::filesystem::path& path)
    {
        return path.extension() == L".meta";
    }

    // AssetDatabase側と同じ判定です（一時ファイルと、移行が残した
    // `<名前>.bak`バックアップは書き出しに含めません）。
    bool IsTemporaryAssetFile(const std::filesystem::path& path)
    {
        const auto name = path.filename().wstring();
        return name.find(L".lamapon-delete") != std::wstring::npos
            || name.ends_with(L".lamapon-remap.tmp")
            || name.ends_with(L".bak");
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
        const std::vector<std::wstring>& skipExtensions)
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
        AssetPackResult result;

        std::error_code iteratorError;
        const auto options =
            std::filesystem::directory_options::
                skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator{
                sourceDirectory,
                options,
                iteratorError
            };
            iterator
                != std::filesystem::
                    recursive_directory_iterator{};
            iterator.increment(iteratorError))
        {
            if (iteratorError)
            {
                iteratorError.clear();
                continue;
            }
            if (!iterator->is_regular_file(iteratorError)
                || iteratorError)
            {
                iteratorError.clear();
                continue;
            }
            if (!skipExtensions.empty())
            {
                auto extension =
                    iterator->path().extension().wstring();
                std::transform(
                    extension.begin(),
                    extension.end(),
                    extension.begin(),
                    [](const wchar_t value)
                    {
                        return static_cast<wchar_t>(
                            std::towlower(value));
                    });
                if (std::find(
                        skipExtensions.begin(),
                        skipExtensions.end(),
                        extension)
                    != skipExtensions.end())
                {
                    continue;
                }
            }
            const auto& path = iterator->path();
            if (IsMetaFile(path) || IsTemporaryAssetFile(path))
            {
                continue;
            }

            const auto relativePath =
                path.lexically_relative(sourceDirectory);
            const auto plainBytes = ReadWholeFile(path);
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
            pending.push_back(
                PendingEntry{
                    relativePath,
                    std::move(cipherText),
                    iv,
                    mac
                });
        }

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
        const auto indexIv = Crypto::RandomIv();
        const std::vector<std::uint8_t> indexPlainBytes(
            indexText.begin(),
            indexText.end());
        const auto indexCipherText = Crypto::AesEncrypt(
            indexPlainBytes,
            key,
            indexIv);
        // 索引そのものも改ざん検知の対象です。ここを守らないと、
        // エントリのMACを丸ごと差し替えられます。
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
