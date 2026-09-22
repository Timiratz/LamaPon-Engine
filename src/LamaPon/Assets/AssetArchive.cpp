#include "LamaPon/Assets/AssetArchive.h"

#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    constexpr std::array<char, 8> ArchiveMagic{
        'T', 'R', 'D', 'N', 'P', 'A', 'K', '2'
    };
    constexpr std::uint64_t HeaderSize = ArchiveMagic.size()
        + sizeof(std::uint64_t)
        + LamaPon::Crypto::AesIvSize
        + LamaPon::Crypto::MacSize;

    // 索引のJSONに入っているバイト配列（IVとMAC）を読みます。
    template <std::size_t Size>
    void ReadByteArray(
        const Json& source,
        const char* field,
        std::array<std::uint8_t, Size>& destination)
    {
        const auto& values = source.at(field);
        if (!values.is_array()
            || values.size() != destination.size())
        {
            throw std::runtime_error(
                std::string("Corrupt ")
                + field
                + " in asset archive index.");
        }
        for (std::size_t index = 0; index < Size; ++index)
        {
            if (!values[index].is_number_integer())
            {
                throw std::runtime_error(
                    std::string("Invalid ") + field
                    + " in asset archive index.");
            }
            const auto value = values[index].get<std::int64_t>();
            if (value < 0 || value > 255)
            {
                throw std::runtime_error(
                    std::string("Invalid ") + field
                    + " in asset archive index.");
            }
            destination[index] = static_cast<std::uint8_t>(value);
        }
    }
}

namespace LamaPon
{
    AssetArchive::AssetArchive(
        std::filesystem::path archivePath,
        const Crypto::AesKey& key)
        : m_archivePath(std::move(archivePath))
        , m_key(key)
        , m_macKey(Crypto::DeriveMacKey(key))
    {
    }

    std::string AssetArchive::NormalizeKey(
        const std::filesystem::path& relativePath)
    {
        std::string key = PathToUtf8(
            relativePath.lexically_normal());
        std::ranges::transform(
            key,
            key.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(
                    std::tolower(character));
            });
        return key;
    }

    std::unique_ptr<AssetArchive> AssetArchive::Open(
        const std::filesystem::path& archivePath)
    {
        return Open(archivePath, Crypto::ArchiveKey());
    }

    std::unique_ptr<AssetArchive> AssetArchive::Open(
        const std::filesystem::path& archivePath,
        const Crypto::AesKey& key)
    {
        std::ifstream input(archivePath,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open asset archive: "
                + PathToUtf8(archivePath));
        }
        const auto end = input.tellg();
        if (end < 0
            || static_cast<std::uint64_t>(end) < HeaderSize)
        {
            throw std::runtime_error(
                "Truncated asset archive header: "
                + PathToUtf8(archivePath));
        }
        const auto archiveSize = static_cast<std::uint64_t>(end);
        input.seekg(0);

        std::array<char, ArchiveMagic.size()> magic{};
        input.read(magic.data(), magic.size());
        if (!input || magic != ArchiveMagic)
        {
            // 旧形式（TRDNPAK1）もここで弾きます。MACの無い形式を
            // 読めるままにすると、改ざんした側が旧形式で作り直して
            // 検証を回避できてしまいます。
            throw std::runtime_error(
                "Not a valid LamaPon asset archive: "
                + PathToUtf8(archivePath));
        }

        std::uint64_t indexCipherSize{};
        input.read(
            reinterpret_cast<char*>(&indexCipherSize),
            sizeof(indexCipherSize));
        Crypto::AesIv indexIv{};
        input.read(
            reinterpret_cast<char*>(indexIv.data()),
            static_cast<std::streamsize>(indexIv.size()));
        Crypto::MacTag indexMac{};
        input.read(
            reinterpret_cast<char*>(indexMac.data()),
            static_cast<std::streamsize>(indexMac.size()));
        if (!input)
        {
            throw std::runtime_error(
                "Truncated asset archive header: "
                + PathToUtf8(archivePath));
        }

        if (indexCipherSize == 0
            || indexCipherSize % Crypto::AesIvSize != 0
            || indexCipherSize
                > AssetArchiveLimits::MaxIndexCipherBytes
            || indexCipherSize > archiveSize - HeaderSize)
        {
            throw std::runtime_error(
                "Invalid asset archive index size: "
                + PathToUtf8(archivePath));
        }

        std::vector<std::uint8_t> indexCipherText(
            static_cast<std::size_t>(indexCipherSize));
        input.read(
            reinterpret_cast<char*>(indexCipherText.data()),
            static_cast<std::streamsize>(
                indexCipherText.size()));
        if (!input)
        {
            throw std::runtime_error(
                "Truncated asset archive index: "
                + PathToUtf8(archivePath));
        }

        const auto macKey = Crypto::DeriveMacKey(key);
        if (!Crypto::MacEquals(
                indexMac,
                Crypto::MacForCipherText(
                    macKey,
                    indexIv,
                    indexCipherText.data(),
                    indexCipherText.size())))
        {
            throw std::runtime_error(
                "Asset archive index failed its integrity check: "
                + PathToUtf8(archivePath));
        }

        const auto indexPlainText = Crypto::AesDecrypt(
            indexCipherText,
            key,
            indexIv);
        std::size_t parseEvents{};
        const auto limitIndexStructure =
            [&parseEvents](const int depth,
                const Json::parse_event_t,
                Json&)
            {
                if (depth < 0 || depth > 8
                    || ++parseEvents
                        > AssetArchiveLimits::MaxEntries * 64 + 16)
                {
                    throw std::runtime_error(
                        "Asset archive index structure is too large.");
                }
                return true;
            };
        const auto indexDocument = Json::parse(
            indexPlainText.begin(),
            indexPlainText.end(), limitIndexStructure);

        if (!indexDocument.is_object()
            || !indexDocument.contains("entries")
            || !indexDocument.at("entries").is_array()
            || indexDocument.at("entries").size()
                > AssetArchiveLimits::MaxEntries)
        {
            throw std::runtime_error(
                "Invalid asset archive entry list: "
                + PathToUtf8(archivePath));
        }

        auto archive = std::unique_ptr<AssetArchive>(
            new AssetArchive(archivePath, key));
        archive->m_payloadStart = HeaderSize + indexCipherSize;
        const auto payloadSize =
            archiveSize - archive->m_payloadStart;
        std::uint64_t expectedOffset{};
        for (const auto& entryJson :
            indexDocument.at("entries"))
        {
            if (!entryJson.is_object())
            {
                throw std::runtime_error(
                    "Invalid asset archive entry.");
            }
            Entry entry;
            entry.offset =
                entryJson.at("offset").get<std::uint64_t>();
            entry.size =
                entryJson.at("size").get<std::uint64_t>();
            ReadByteArray(entryJson, "iv", entry.iv);
            ReadByteArray(entryJson, "mac", entry.mac);
            if (entry.offset != expectedOffset
                || entry.size == 0
                || entry.size % Crypto::AesIvSize != 0
                || entry.size
                    > AssetArchiveLimits::MaxEntryCipherBytes
                || entry.size > payloadSize - expectedOffset)
            {
                throw std::runtime_error(
                    "Invalid asset archive entry range.");
            }
            expectedOffset += entry.size;
            const auto pathText =
                entryJson.at("path").get<std::string>();
            if (pathText.empty()
                || pathText.size()
                    > AssetArchiveLimits::MaxPathBytes
                || pathText.find('\0') != std::string::npos)
            {
                throw std::runtime_error(
                    "Invalid asset archive entry path.");
            }
            const auto path = PathFromUtf8(pathText);
            if (path.empty() || path == "."
                || path.is_absolute()
                || path.has_root_name()
                || path.has_root_directory())
            {
                throw std::runtime_error(
                    "Invalid asset archive entry path.");
            }
            for (const auto& component : path)
            {
                if (component == "." || component == "..")
                {
                    throw std::runtime_error(
                        "Invalid asset archive entry path.");
                }
            }
            if (!archive->m_entries.emplace(
                    NormalizeKey(path), entry).second)
            {
                throw std::runtime_error(
                    "Duplicate asset archive entry path.");
            }
        }
        if (expectedOffset != payloadSize)
        {
            throw std::runtime_error(
                "Unexpected data after asset archive entries.");
        }
        return archive;
    }

    bool AssetArchive::Contains(
        const std::filesystem::path& relativePath) const
    {
        return m_entries.contains(
            NormalizeKey(relativePath));
    }

    std::optional<std::vector<std::uint8_t>>
        AssetArchive::TryRead(
            const std::filesystem::path& relativePath) const
    {
        const auto found = m_entries.find(
            NormalizeKey(relativePath));
        if (found == m_entries.end())
        {
            return std::nullopt;
        }

        std::ifstream input(m_archivePath, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not reopen asset archive: "
                + PathToUtf8(m_archivePath));
        }
        input.seekg(
            static_cast<std::streamoff>(
                m_payloadStart + found->second.offset));
        if (!input)
        {
            throw std::runtime_error(
                "Could not seek to asset entry in archive: "
                + PathToUtf8(relativePath));
        }
        std::vector<std::uint8_t> cipherText(
            static_cast<std::size_t>(found->second.size));
        input.read(
            reinterpret_cast<char*>(cipherText.data()),
            static_cast<std::streamsize>(
                cipherText.size()));
        if (!input)
        {
            throw std::runtime_error(
                "Truncated asset entry in archive: "
                + PathToUtf8(relativePath));
        }

        // Entry::iv／macはCrypto::AesIv／MacTagと同じ型です。
        const auto& iv = found->second.iv;
        if (!Crypto::MacEquals(
                found->second.mac,
                Crypto::MacForCipherText(
                    m_macKey,
                    iv,
                    cipherText.data(),
                    cipherText.size())))
        {
            // 差し替えられたアセットを黙って使うくらいなら、
            // ここで止めます。
            throw std::runtime_error(
                "Asset failed its integrity check in the archive: "
                + PathToUtf8(relativePath));
        }

        return Crypto::AesDecrypt(
            cipherText,
            m_key,
            iv);
    }
}
