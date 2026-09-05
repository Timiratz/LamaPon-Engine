#include "LamaPon/Assets/TextureCache.h"

#include <Windows.h>

#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <vector>

namespace
{
    // エンコード方式を変えた場合に更新するキャッシュ形式の版です。
    constexpr std::uint32_t FormatVersion = 1;

    constexpr char Magic[4] = { 'T', 'T', 'E', 'X' };

    // 読み込み時に破損を検出するためのミップ数上限です。
    constexpr std::uint32_t MaximumLevels = 16;

    // ファイルI/Oより再生成の方が軽い小さな結果は保存しません。
    constexpr std::size_t MinimumStoredBytes = 64 * 1024;

    std::mutex g_directoryMutex;
    std::filesystem::path g_directoryOverride;

    // 生成物をプロジェクトへ混在させないよう、%LOCALAPPDATA%へ
    // 保存します。
    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        std::wstring localAppData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"texture-cache";
    }

    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        const auto directory =
            LamaPon::TextureCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.ttex", key);
        return directory / name;
    }

    // 書き込み途中で落ちても壊れたファイルが残らないよう、別名で
    // 書いてから置き換えます（shader-cacheと同じやり方）。壊れた
    // キャッシュは読み込み時の検査で弾けますが、そもそも作らない
    // のが確実です。
    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {
        auto temporary = destination;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!output)
            {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }

    void AppendUint32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        output.push_back(static_cast<std::uint8_t>(value));
        output.push_back(static_cast<std::uint8_t>(value >> 8));
        output.push_back(static_cast<std::uint8_t>(value >> 16));
        output.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    // 読み取りカーソル。範囲外を読もうとしたらfailに倒して、以降の
    // 読み取りを全部無効にします（1箇所でも壊れていたら全体を
    // 捨てるため）。
    struct Reader final
    {
        const std::uint8_t* data{};
        std::size_t size{};
        std::size_t offset{};
        bool failed{};

        [[nodiscard]] std::uint32_t ReadUint32() noexcept
        {
            if (failed || offset + 4 > size)
            {
                failed = true;
                return 0;
            }
            const std::uint32_t value =
                static_cast<std::uint32_t>(data[offset])
                | static_cast<std::uint32_t>(data[offset + 1]) << 8
                | static_cast<std::uint32_t>(data[offset + 2]) << 16
                | static_cast<std::uint32_t>(data[offset + 3]) << 24;
            offset += 4;
            return value;
        }

        [[nodiscard]] bool ReadBytes(
            void* destination,
            const std::size_t count) noexcept
        {
            if (failed || offset + count > size)
            {
                failed = true;
                return false;
            }
            std::memcpy(destination, data + offset, count);
            offset += count;
            return true;
        }
    };

    // このキャッシュが作りうるフォーマットだけを受け付けます。
    // それ以外の値が読めたらファイルが壊れているか、別物です。
    [[nodiscard]] bool IsKnownFormat(
        const std::uint32_t format) noexcept
    {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM
            || format == DXGI_FORMAT_BC1_UNORM
            || format == DXGI_FORMAT_BC3_UNORM
            || format == DXGI_FORMAT_BC5_UNORM;
    }

    // レベルの寸法から期待されるバイト数。これと合わない
    // ファイルは捨てます（サイズ検査が通れば、中身が多少
    // 化けていても絵が乱れるだけでクラッシュはしません）。
    [[nodiscard]] std::size_t ExpectedByteCount(
        const std::uint32_t format,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t rowPitch) noexcept
    {
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            if (rowPitch != width * 4)
            {
                return 0;
            }
            return static_cast<std::size_t>(rowPitch) * height;
        }
        const std::uint32_t blocksX = (width + 3) / 4;
        const std::uint32_t blocksY = (height + 3) / 4;
        // BC1は8バイト/ブロック、BC3とBC5は16バイト/ブロック。
        const std::uint32_t blockBytes =
            (format == DXGI_FORMAT_BC3_UNORM
                || format == DXGI_FORMAT_BC5_UNORM)
                ? 16u
                : 8u;
        if (rowPitch != blocksX * blockBytes)
        {
            return 0;
        }
        return static_cast<std::size_t>(rowPitch) * blocksY;
    }
}

namespace LamaPon::TextureCache
{
    std::filesystem::path CacheDirectory()
    {
        {
            const std::lock_guard<std::mutex> lock(
                g_directoryMutex);
            if (!g_directoryOverride.empty())
            {
                return g_directoryOverride;
            }
        }
        return DefaultDirectory();
    }

    void SetCacheDirectoryOverride(
        std::filesystem::path directory)
    {
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t ComputeKey(
        const std::span<const std::uint8_t> sourceBytes,
        const bool compress,
        const TextureLoader::TextureUsage usage) noexcept
    {
        // FNV-1a 64。暗号強度は要りません（自分のディスクの
        // キャッシュを自分で引くだけなので、衝突攻撃の相手が
        // いません）。速さと単純さで選んでいます。
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::uint8_t byte : sourceBytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        hash ^= compress ? 0x9e3779b97f4a7c15ull : 0x2545f4914f6cdd1dull;
        hash *= 1099511628211ull;
        // 同じ画像でも用途が違えばフォーマットが変わるので、
        // 鍵に混ぜないと法線用のBC5を色として引いてしまいます。
        hash ^= static_cast<std::uint64_t>(usage);
        hash *= 1099511628211ull;
        hash ^= FormatVersion;
        hash *= 1099511628211ull;
        return hash;
    }

    std::optional<CachedTexture> TryLoad(const std::uint64_t key)
    {
        const auto path = EntryPath(key);
        if (path.empty())
        {
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return std::nullopt;
        }
        // 1バイトごとの仮想関数呼び出しを避けるため、一括で読み込みます。
        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        if (size <= 0)
        {
            return std::nullopt;
        }
        input.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(size));
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            size);
        if (!input)
        {
            return std::nullopt;
        }
        input.close();

        Reader reader{ bytes.data(), bytes.size() };
        char magic[4]{};
        if (!reader.ReadBytes(magic, sizeof(magic))
            || std::memcmp(magic, Magic, sizeof(Magic)) != 0)
        {
            return std::nullopt;
        }
        if (reader.ReadUint32() != FormatVersion)
        {
            return std::nullopt;
        }

        CachedTexture result;
        const std::uint32_t format = reader.ReadUint32();
        if (!IsKnownFormat(format))
        {
            return std::nullopt;
        }
        result.data.format = static_cast<DXGI_FORMAT>(format);
        if (!reader.ReadBytes(
            result.placeholderPixel.data(),
            result.placeholderPixel.size()))
        {
            return std::nullopt;
        }

        const std::uint32_t levelCount = reader.ReadUint32();
        if (reader.failed
            || levelCount == 0
            || levelCount > MaximumLevels)
        {
            return std::nullopt;
        }
        result.data.levels.resize(levelCount);
        std::uint64_t previousArea = 0;
        for (std::uint32_t index = 0; index < levelCount; ++index)
        {
            auto& level = result.data.levels[index];
            level.width = reader.ReadUint32();
            level.height = reader.ReadUint32();
            level.rowPitch = reader.ReadUint32();
            const std::uint32_t byteCount = reader.ReadUint32();
            if (reader.failed
                || level.width == 0
                || level.height == 0)
            {
                return std::nullopt;
            }
            // ミップは必ず縮んでいくはず。並びが崩れていたら
            // ファイルが壊れています。幅ではなく面積で見るのは、
            // 1xNのような細長いテクスチャでは幅が1のまま並ぶためです
            // （幅の単調減少を要求すると正当なキャッシュを弾きます）。
            const std::uint64_t area =
                static_cast<std::uint64_t>(level.width)
                * level.height;
            if (index > 0 && area >= previousArea)
            {
                return std::nullopt;
            }
            previousArea = area;
            if (ExpectedByteCount(
                    format,
                    level.width,
                    level.height,
                    level.rowPitch)
                != byteCount)
            {
                return std::nullopt;
            }
            level.bytes.resize(byteCount);
            if (!reader.ReadBytes(level.bytes.data(), byteCount))
            {
                return std::nullopt;
            }
        }
        // 末尾にゴミが付いているファイルも信用しません。
        if (reader.offset != reader.size)
        {
            return std::nullopt;
        }
        return result;
    }

    void Store(
        const std::uint64_t key,
        const CachedTexture& value) noexcept
    {
        try
        {
            if (value.data.levels.empty()
                || value.data.levels.size() > MaximumLevels
                || !IsKnownFormat(value.data.format))
            {
                return;
            }
            // 小さい結果は、キャッシュを読むより生成するほうが速いため保存しません。
            // 呼び出し側のTryLoadはopen失敗を処理するため、サイズを取得できる
            // ここで保存対象か判定します。
            if (value.data.TotalBytes() < MinimumStoredBytes)
            {
                return;
            }
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            std::vector<std::uint8_t> bytes;
            bytes.reserve(64 + value.data.TotalBytes());
            bytes.insert(bytes.end(), Magic, Magic + sizeof(Magic));
            AppendUint32(bytes, FormatVersion);
            AppendUint32(
                bytes,
                static_cast<std::uint32_t>(value.data.format));
            bytes.insert(
                bytes.end(),
                value.placeholderPixel.begin(),
                value.placeholderPixel.end());
            AppendUint32(
                bytes,
                static_cast<std::uint32_t>(
                    value.data.levels.size()));
            for (const auto& level : value.data.levels)
            {
                AppendUint32(bytes, level.width);
                AppendUint32(bytes, level.height);
                AppendUint32(bytes, level.rowPitch);
                AppendUint32(
                    bytes,
                    static_cast<std::uint32_t>(
                        level.bytes.size()));
                bytes.insert(
                    bytes.end(),
                    level.bytes.begin(),
                    level.bytes.end());
            }
            WriteFileAtomically(path, bytes);
        }
        catch (...)
        {
            // 高速化のための書き込みが失敗しても、呼ぶ側の
            // 読み込み自体は成功しているので何もしません。
        }
    }
}
