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
    // 変換方式を変えたら更新する版
    constexpr std::uint32_t FormatVersion = 1;

    // TTEX形式の識別子
    constexpr char Magic[4] = { 'T', 'T', 'E', 'X' };

    // 検証するミップ数の上限
    constexpr std::uint32_t MaximumLevels = 16;

    // 保存する結果の最小バイト数
    constexpr std::size_t MinimumStoredBytes = 64 * 1024;

    // 保存先差し替えの排他制御
    std::mutex g_directoryMutex;
    // 任意に差し替える保存先
    std::filesystem::path g_directoryOverride;

    // LocalAppDataまたはOS一時領域から保存先を得る。
    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        // LocalAppData環境変数の取得領域
        std::wstring localAppData(32768, L'\0');
        // 環境変数の取得文字数
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        // OSのキャッシュ保存基点
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            // OS保存先・保存操作の結果
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"texture-cache";
    }

    // キーを16桁の.ttexファイル名へ変換する(key: 内容由来のキー)。
    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        // 現在設定された保存先
        const auto directory =
            LamaPon::TextureCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        // キー由来の16桁ファイル名
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.ttex", key);
        return directory / name;
    }

    // 別名で書き終えてから移動し、失敗時は候補を削除する(destination: 正式な保存先, bytes: 完成したキャッシュ内容)。
    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {
        // 書き込み完了前の別名パス
        auto temporary = destination;
        temporary += L".tmp";
        {
            // 別名キャッシュの出力先
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
                // 失敗候補の削除結果
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        // OS保存先・保存操作の結果
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }

    // 32ビット値をリトルエンディアンで追加する(output: 出力バイト列, value: 追加する値)。
    void AppendUint32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        output.push_back(static_cast<std::uint8_t>(value));
        output.push_back(static_cast<std::uint8_t>(value >> 8));
        output.push_back(static_cast<std::uint8_t>(value >> 16));
        output.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    // 最初の範囲不足以降は、全ての読み取りを失敗として扱う。
    struct Reader final
    {
        // 借用する読取バイト列
        const std::uint8_t* data{};
        // 読取バイト列の容量
        // 保存ファイルのバイト数
        std::size_t size{};
        // 次に読むバイト位置
        std::size_t offset{};
        // 読み取り継続不能の状態
        bool failed{};

        // 32ビット値を読み、不足時は失敗状態にして0を返す。
        [[nodiscard]] std::uint32_t ReadUint32() noexcept
        {
            if (failed || offset + 4 > size)
            {
                failed = true;
                return 0;
            }
            // 読み出した32ビット整数
            const std::uint32_t value =
                static_cast<std::uint32_t>(data[offset])
                | static_cast<std::uint32_t>(data[offset + 1]) << 8
                | static_cast<std::uint32_t>(data[offset + 2]) << 16
                | static_cast<std::uint32_t>(data[offset + 3]) << 24;
            offset += 4;
            return value;
        }

        // 指定範囲をコピーし、不足時は失敗状態にする(destination: コピー先, count: コピーするバイト数)。
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

    // このキャッシュが生成できる画素形式か調べる(format: DXGI形式の数値)。
    [[nodiscard]] bool IsKnownFormat(
        const std::uint32_t format) noexcept
    {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM
            || format == DXGI_FORMAT_BC1_UNORM
            || format == DXGI_FORMAT_BC3_UNORM
            || format == DXGI_FORMAT_BC5_UNORM;
    }

    // 形式と寸法から必要容量を求め、不正な行幅は0にする(format: DXGI形式の数値, width: ミップの幅, height: ミップの高さ, rowPitch: 一行のバイト幅)。
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
        // 横方向の4×4圧縮ブロック数
        const std::uint32_t blocksX = (width + 3) / 4;
        // 縦方向の4×4圧縮ブロック数
        const std::uint32_t blocksY = (height + 3) / 4;
        // BC1は8、BC3・BC5は16バイト
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
            // 保存先設定の排他ロック
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
        // 保存先設定の排他ロック
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t ComputeKey(
        const std::span<const std::uint8_t> sourceBytes,
        const bool compress,
        const TextureLoader::TextureUsage usage) noexcept
    {

        // 内容と変換条件の64ビットキー
        std::uint64_t hash = 14695981039346656037ull;
        // キーへ混ぜる元画像のバイト
        for (const std::uint8_t byte : sourceBytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        hash ^= compress ? 0x9e3779b97f4a7c15ull : 0x2545f4914f6cdd1dull;
        hash *= 1099511628211ull;
        // 色用と法線用で異なる圧縮形式を取り違えないよう、用途をキーに含める。
        hash ^= static_cast<std::uint64_t>(usage);
        hash *= 1099511628211ull;
        hash ^= FormatVersion;
        hash *= 1099511628211ull;
        return hash;
    }

    std::optional<CachedTexture> TryLoad(const std::uint64_t key)
    {
        // キーに対応するキャッシュパス
        const auto path = EntryPath(key);
        if (path.empty())
        {
            return std::nullopt;
        }
        // 既存キャッシュのバイナリー入力
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return std::nullopt;
        }

        input.seekg(0, std::ios::end);
        // 保存ファイルのバイト数
        const std::streamoff size = input.tellg();
        if (size <= 0)
        {
            return std::nullopt;
        }
        input.seekg(0, std::ios::beg);
        // 既存キャッシュの全バイト列
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

        // 破損を追跡する読取位置
        Reader reader{ bytes.data(), bytes.size() };
        // 読み込んだ形式識別子
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

        // 検証済みの転送用画像
        CachedTexture result;
        // 保存されたDXGI画素形式
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

        // 保存されたミップ数
        const std::uint32_t levelCount = reader.ReadUint32();
        if (reader.failed
            || levelCount == 0
            || levelCount > MaximumLevels)
        {
            return std::nullopt;
        }
        result.data.levels.resize(levelCount);
        // 一つ前のミップの画素数
        std::uint64_t previousArea = 0;
        // 読み込むミップ番号
        for (std::uint32_t index = 0; index < levelCount; ++index)
        {
            // 処理中のミップ情報
            auto& level = result.data.levels[index];
            level.width = reader.ReadUint32();
            level.height = reader.ReadUint32();
            level.rowPitch = reader.ReadUint32();
            // 保存されたミップの容量
            const std::uint32_t byteCount = reader.ReadUint32();
            if (reader.failed
                || level.width == 0
                || level.height == 0)
            {
                return std::nullopt;
            }

            // 寸法を掛けたミップの画素数
            const std::uint64_t area =
                static_cast<std::uint64_t>(level.width)
                * level.height;
            // 1×Nの幅が変わらない段も許容するため、ミップの縮小を面積で検証する。
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
        // 全ミップを読んだ後に未解釈の余剰データが残る結果も拒否する。
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
            // 再生成の方が軽い小さな結果は保存しない。
            if (value.data.TotalBytes() < MinimumStoredBytes)
            {
                return;
            }
            // キーに対応するキャッシュパス
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            // OS保存先・保存操作の結果
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            // 保存する完成キャッシュ内容
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
            // 処理中のミップ情報
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
            // 保存失敗で、既に準備できた画像の読み込みを失敗させない。
        }
    }
}
