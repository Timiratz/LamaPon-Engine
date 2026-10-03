#include "LamaPon/Assets/ModelCache.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/ModelLod.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
    using ModelVertex =
        DirectX::VertexPositionNormalTangentColorTextureSkinning;

    // 保存項目・頂点配置・インポーターの意味を変えたら、キャッシュ版を更新する。
    // キャッシュ格納形式の版
    constexpr std::uint32_t FormatVersion = 3;

    // モデルキャッシュの識別子
    constexpr char Magic[4] = { 'T', 'M', 'D', 'L' };


    // 記録列の最大要素数
    constexpr std::uint32_t MaximumCount = 1u << 24;
    // 文字列の最大バイト数
    constexpr std::uint32_t MaximumStringLength = 1u << 16;

    // 保存先差し替えの排他制御
    std::mutex g_directoryMutex;
    // 差し替えたキャッシュ保存先
    std::filesystem::path g_directoryOverride;

    // LocalAppData、取得できなければOS一時領域のキャッシュパスを返す。
    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        // ユーザー領域の環境変数
        std::wstring localAppData(32768, L'\0');
        // 環境変数のUTF-16要素数
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        // 既定保存先の基準フォルダー
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            // ファイル操作の結果
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"model-cache";
    }

    // キーの16桁十六進名からキャッシュのパスを作る(key: キャッシュキー)。
    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        // 現在のキャッシュ保存先
        const auto directory =
            LamaPon::ModelCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        // 16桁キーのファイル名
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.tmdl", key);
        return directory / name;
    }

    // バイト列のFNV-1a 64ビット識別値を返す(bytes: ハッシュ対象のバイト列)。
    [[nodiscard]] std::uint64_t HashBytes(
        const std::span<const std::uint8_t> bytes) noexcept
    {

        // 内容と形式を混ぜる識別値
        std::uint64_t hash = 14695981039346656037ull;
        // ハッシュへ加える1バイト
        for (const std::uint8_t byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // 同名候補への書き込みを直列化し、完了後に保存先へ移す(destination: 保存先, bytes: 書き込むバイト列)。
    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {

        // 候補への書き込みの排他制御
        // 同じプロセス内の保存を直列化し、共通の.tmpへの競合を防ぐ。
        static std::mutex writeMutex;
        // 候補への同時書き込みの防止
        const std::scoped_lock lock(writeMutex);

        // 保存完了前の候補パス
        auto temporary = destination;
        temporary += L".tmp";
        {
            // 候補ファイルの書き込み先
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
                // 削除失敗を無視する結果
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        // ファイル操作の結果
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }



    // 指定範囲のバイト列を追記する(output: 保存先のバイト列, data: 追記元の先頭, size: 追記するバイト数)。
    void AppendBytes(
        std::vector<std::uint8_t>& output,
        const void* data,
        const std::size_t size)
    {
        // コピー元バイト列の先頭
        const auto* begin =
            static_cast<const std::uint8_t*>(data);
        output.insert(output.end(), begin, begin + size);
    }

    // 8ビット整数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendU8(
        std::vector<std::uint8_t>& output,
        const std::uint8_t value)
    {
        output.push_back(value);
    }

    // 符号無し32ビット整数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendU32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    // 符号無し64ビット整数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendU64(
        std::vector<std::uint8_t>& output,
        const std::uint64_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    // 符号付き64ビット整数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendI64(
        std::vector<std::uint8_t>& output,
        const std::int64_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    // 符号付き32ビット整数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendI32(
        std::vector<std::uint8_t>& output,
        const std::int32_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    // 32ビット実数を追記する(output: 保存先のバイト列, value: 保存する値)。
    void AppendF32(
        std::vector<std::uint8_t>& output,
        const float value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    // バイト長を前置して文字列を追記する(output: 保存先のバイト列, value: 保存する文字列)。
    void AppendString(
        std::vector<std::uint8_t>& output,
        const std::string& value)
    {
        AppendU32(
            output,
            static_cast<std::uint32_t>(value.size()));
        AppendBytes(output, value.data(), value.size());
    }



    // 読み取り失敗後は以降の読み取りも無効にする。
    struct Reader final
    {
        // 呼出元が保持する元バイト列
        const std::uint8_t* data{};
        // 元バイト列の総バイト数
        std::size_t size{};
        // 次に読むバイト位置
        std::size_t offset{};
        // 検出済みの読み取り失敗
        bool failed{};

        // 残り範囲からコピーし、失敗を記録する(destination: count以上の書き込み先, count: 読み取るバイト数)。
        [[nodiscard]] bool ReadRaw(
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

        // 次の値をコピーし、読み取り失敗時はゼロ初期化値を返す。
        template <typename T>
        [[nodiscard]] T Read() noexcept
        {
            // 次の値のコピー
            T value{};
            static_cast<void>(ReadRaw(&value, sizeof(T)));
            return value;
        }

        // 長さ上限を検査して文字列を読み、範囲不正なら失敗を記録する。
        [[nodiscard]] std::string ReadString() noexcept
        {
            // 文字列のバイト数
            const auto length = Read<std::uint32_t>();
            if (failed || length > MaximumStringLength
                || offset + length > size)
            {
                failed = true;
                return {};
            }
            // 読み取った文字列
            std::string value(
                reinterpret_cast<const char*>(data + offset),
                length);
            offset += length;
            return value;
        }
    };

    // HRESULTが失敗なら例外を送出する(result: APIの結果, what: 例外の診断文)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* what)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(what);
        }
    }

    // 転送予算を待ち、不変のD3D11バッファーを作る(device: 作成先デバイス, assets: 転送予算の管理元, data: 初期データの先頭, byteCount: 初期データのバイト数, bindFlags: バインド用途, output: COM参照の返却先)。
    void CreateBuffer(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const void* data,
        const std::size_t byteCount,
        const UINT bindFlags,
        ID3D11Buffer** output)
    {
        if (byteCount == 0
            || byteCount > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "Cached model buffer has an invalid size.");
        }
        // 不変バッファーの設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byteCount);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = bindFlags;
        // バッファーの初期データ
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        assets.WaitForModelUploadBudget(byteCount);
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                &initialData,
                output),
            "Creating a cached model buffer");
    }

    // 4ボーン対応の標準エフェクトと入力配置を作る(device: 作成先デバイス, primitive: 復元する描画部分)。
    void BuildPrimitiveEffect(
        ID3D11Device* device,
        LamaPon::SkeletalPrimitive& primitive)
    {
        primitive.effect =
            std::make_shared<DirectX::SkinnedEffect>(device);
        primitive.effect->SetWeightsPerVertex(4);
        // 入力配置用の頂点シェーダー
        const void* shaderBytecode{};
        // シェーダーのバイト数
        std::size_t shaderBytecodeSize{};
        primitive.effect->GetVertexShaderBytecode(
            &shaderBytecode,
            &shaderBytecodeSize);
        ThrowIfFailed(
            device->CreateInputLayout(
                ModelVertex::InputElements,
                ModelVertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.inputLayout.ReleaseAndGetAddressOf()),
            "Creating a cached model input layout");
    }

    // アルファ破棄対応のエフェクトと入力配置を作る(device: 作成先デバイス, primitive: 復元する描画部分)。
    void BuildCutoutEffect(
        ID3D11Device* device,
        LamaPon::SkeletalPrimitive& primitive)
    {
        primitive.cutoutEffect =
            std::make_shared<DirectX::SkinnedDGSLEffect>(device);
        primitive.cutoutEffect->SetWeightsPerVertex(4);
        primitive.cutoutEffect->SetTextureEnabled(true);
        primitive.cutoutEffect->SetAlphaDiscardEnable(true);
        // 入力配置用の頂点シェーダー
        const void* shaderBytecode{};
        // シェーダーのバイト数
        std::size_t shaderBytecodeSize{};
        primitive.cutoutEffect->GetVertexShaderBytecode(
            &shaderBytecode,
            &shaderBytecodeSize);
        ThrowIfFailed(
            device->CreateInputLayout(
                ModelVertex::InputElements,
                ModelVertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.cutoutInputLayout
                    .ReleaseAndGetAddressOf()),
            "Creating a cached model cutout input layout");
    }

    // 直接インポートと同じ共通画像経路でビューを作る(assets: 画像生成の管理元, bytes: 元画像のバイト列, isDds: DDS形式か, usage: 元の画像用途)。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateImageView(
            LamaPon::AssetManager& assets,
            const std::span<const std::uint8_t> bytes,
            const bool isDds,
            const LamaPon::TextureLoader::TextureUsage usage)
    {

        return assets.CreateTextureViewFromMemory(
            bytes,
            isDds,
            usage);
    }



    // 補間形式とベクトルキー列を追記する(output: 保存先のバイト列, channel: 保存するチャンネル)。
    void AppendVectorChannel(
        std::vector<std::uint8_t>& output,
        const LamaPon::SkeletalVectorChannel& channel)
    {
        AppendU8(
            output,
            static_cast<std::uint8_t>(channel.interpolation));
        AppendU32(
            output,
            static_cast<std::uint32_t>(channel.keys.size()));
        // キー列は構造体のバイト配置のまま保存する。
        AppendBytes(
            output,
            channel.keys.data(),
            channel.keys.size()
                * sizeof(LamaPon::SkeletalVectorKey));
    }

    // 補間形式と回転キー列を追記する(output: 保存先のバイト列, channel: 保存するチャンネル)。
    void AppendQuaternionChannel(
        std::vector<std::uint8_t>& output,
        const LamaPon::SkeletalQuaternionChannel& channel)
    {
        AppendU8(
            output,
            static_cast<std::uint8_t>(channel.interpolation));
        AppendU32(
            output,
            static_cast<std::uint32_t>(channel.keys.size()));
        AppendBytes(
            output,
            channel.keys.data(),
            channel.keys.size()
                * sizeof(LamaPon::SkeletalQuaternionKey));
    }

    // 補間形式とキー数を検査し、ベクトルキーを読む(reader: 読み取り位置, channel: 復元先のチャンネル)。
    [[nodiscard]] bool ReadVectorChannel(
        Reader& reader,
        LamaPon::SkeletalVectorChannel& channel)
    {
        // 記録された補間形式
        const auto interpolation = reader.Read<std::uint8_t>();
        if (interpolation > 2)
        {
            return false;
        }
        channel.interpolation =
            static_cast<LamaPon::SkeletalInterpolation>(
                interpolation);
        // 記録された変換キーの数
        const auto keyCount = reader.Read<std::uint32_t>();
        if (reader.failed || keyCount > MaximumCount)
        {
            return false;
        }
        channel.keys.resize(keyCount);
        return reader.ReadRaw(
            channel.keys.data(),
            keyCount * sizeof(LamaPon::SkeletalVectorKey));
    }

    // 補間形式とキー数を検査し、回転キーを読む(reader: 読み取り位置, channel: 復元先のチャンネル)。
    [[nodiscard]] bool ReadQuaternionChannel(
        Reader& reader,
        LamaPon::SkeletalQuaternionChannel& channel)
    {
        // 記録された補間形式
        const auto interpolation = reader.Read<std::uint8_t>();
        if (interpolation > 2)
        {
            return false;
        }
        channel.interpolation =
            static_cast<LamaPon::SkeletalInterpolation>(
                interpolation);
        // 記録された変換キーの数
        const auto keyCount = reader.Read<std::uint32_t>();
        if (reader.failed || keyCount > MaximumCount)
        {
            return false;
        }
        channel.keys.resize(keyCount);
        return reader.ReadRaw(
            channel.keys.data(),
            keyCount * sizeof(LamaPon::SkeletalQuaternionKey));
    }
}

namespace LamaPon::ModelCache
{
    void Recorder::RegisterEmbeddedImage(
        ID3D11ShaderResourceView* const view,
        const std::span<const std::uint8_t> bytes,
        const bool isDds,
        const bool hasTransparency,
        const TextureLoader::TextureUsage usage)
    {
        if (view == nullptr)
        {
            return;
        }
        // 記録する画像情報
        Image image;
        image.bytes.assign(bytes.begin(), bytes.end());
        image.isDds = isDds;
        image.hasTransparency = hasTransparency;
        image.usage = usage;
        m_imageBySrv.emplace(
            view,
            static_cast<std::int32_t>(images.size()));
        images.emplace_back(std::move(image));
    }

    void Recorder::RegisterExternalImage(
        ID3D11ShaderResourceView* const view,
        const std::filesystem::path& path,
        const std::span<const std::uint8_t> bytes,
        const bool isDds,
        const bool hasTransparency,
        const TextureLoader::TextureUsage usage)
    {
        if (view == nullptr)
        {
            return;
        }
        // 記録する画像情報
        Image image;
        image.external = true;
        image.externalPath = path;
        image.externalHash = HashBytes(bytes);
        image.isDds = isDds;
        image.hasTransparency = hasTransparency;
        image.usage = usage;
        m_imageBySrv.emplace(
            view,
            static_cast<std::int32_t>(images.size()));
        images.emplace_back(std::move(image));
    }

    void Recorder::RegisterDependency(
        const std::filesystem::path& path,
        const bool exists,
        const std::span<const std::uint8_t> bytes)
    {
        // 記録・照合する依存情報
        Dependency dependency;
        dependency.path = path;
        dependency.exists = exists;
        dependency.hash = exists ? HashBytes(bytes) : 0;
        dependencies.emplace_back(std::move(dependency));
    }

    void Recorder::AddGeometry(
        const void* const vertices,
        const std::size_t vertexCount,
        const std::size_t vertexStride,
        const std::uint32_t* const indices,
        const std::size_t indexCount)
    {
        // 描画部分に対応する幾何
        Geometry geometry;
        geometry.vertexStride =
            static_cast<std::uint32_t>(vertexStride);
        // コピー元バイト列の先頭
        const auto* begin =
            static_cast<const std::uint8_t*>(vertices);
        geometry.vertexBytes.assign(
            begin,
            begin + vertexCount * vertexStride);
        if (indices != nullptr)
        {
            geometry.indices.assign(
                indices,
                indices + indexCount);
        }
        geometries.emplace_back(std::move(geometry));
    }

    std::int32_t Recorder::ImageIndexFor(
        ID3D11ShaderResourceView* const view) const noexcept
    {
        if (view == nullptr)
        {
            return -1;
        }
        // 画像番号の検索結果
        const auto found = m_imageBySrv.find(view);
        return found != m_imageBySrv.end() ? found->second : -1;
    }

    std::filesystem::path CacheDirectory()
    {
        {
            // 保存先差し替えの読み取りロック
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
        // 保存先差し替えの更新ロック
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t ComputeKey(
        const std::span<const std::uint8_t> sourceBytes,
        const std::uint32_t importerKind) noexcept
    {
        // 内容と形式を混ぜる識別値
        std::uint64_t hash = HashBytes(sourceBytes);
        hash ^= importerKind * 0x9e3779b97f4a7c15ull;
        hash *= 1099511628211ull;
        hash ^= FormatVersion;
        hash *= 1099511628211ull;
        // 頂点レイアウトの変更時にキャッシュを無効化するため、レイアウト識別値をキーへ含めます。
        hash ^= sizeof(ModelVertex);
        hash *= 1099511628211ull;
        return hash;
    }

    std::shared_ptr<SkeletalModel> TryLoad(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        AssetManager& assets,
        const std::uint64_t key)
    {

        static_cast<void>(context);
        try
        {
            // キャッシュのファイルパス
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return nullptr;
            }
            // キャッシュの読み取り元
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return nullptr;
            }
            input.seekg(0, std::ios::end);
            // キャッシュのバイト数
            const std::streamoff fileSize = input.tellg();
            if (fileSize <= 0)
            {
                return nullptr;
            }
            input.seekg(0, std::ios::beg);
            // 読み取ったキャッシュ列
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>(fileSize));
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                fileSize);
            if (!input)
            {
                return nullptr;
            }
            input.close();

            // キャッシュ列の読み取り位置
            Reader reader{ bytes.data(), bytes.size() };
            // 読み取った形式識別子
            char magic[4]{};
            if (!reader.ReadRaw(magic, sizeof(magic))
                || std::memcmp(magic, Magic, sizeof(Magic)) != 0)
            {
                return nullptr;
            }
            if (reader.Read<std::uint32_t>() != FormatVersion)
            {
                return nullptr;
            }
            if (reader.Read<std::uint32_t>()
                != static_cast<std::uint32_t>(
                    sizeof(ModelVertex)))
            {
                return nullptr;
            }

            // 依存先の存在・不在と内容のいずれかが変われば、キャッシュを使わない。
            // 照合する依存先の数
            const auto dependencyCount =
                reader.Read<std::uint32_t>();
            if (reader.failed
                || dependencyCount > MaximumCount)
            {
                return nullptr;
            }
            // 記録列の要素番号
            for (std::uint32_t index = 0;
                // 照合する依存先の数
                index < dependencyCount;
                ++index)
            {
                // 照合する依存先のパス
                const auto dependencyPath =
                    LamaPon::PathFromUtf8(reader.ReadString());
                // 記録時の依存先の存在状態
                const bool expectedExists =
                    reader.Read<std::uint8_t>() != 0;
                // 記録時の内容ハッシュ
                const auto expectedHash =
                    reader.Read<std::uint64_t>();
                if (reader.failed)
                {
                    return nullptr;
                }
                // 現在の依存先の存在状態
                const bool exists =
                    assets.FileExists(dependencyPath);
                if (exists != expectedExists)
                {
                    return nullptr;
                }
                if (exists)
                {
                    // 現在の依存ファイル内容
                    const auto dependencyBytes =
                        assets.ReadFileBytes(dependencyPath);
                    if (HashBytes(dependencyBytes)
                        != expectedHash)
                    {
                        return nullptr;
                    }
                }
            }

            // 外部画像の内容を照合し、同じバイト列からビューを復元する。
            // 復元する画像の数
            const auto imageCount = reader.Read<std::uint32_t>();
            if (reader.failed || imageCount > MaximumCount)
            {
                return nullptr;
            }
            // 復元した画像ビューの一覧
            std::vector<
                Microsoft::WRL::ComPtr<
                    ID3D11ShaderResourceView>> views(imageCount);
            // 記録された画像別の透過情報
            std::vector<std::uint8_t> imageTransparency(
                imageCount);
            // 記録列の要素番号
            for (std::uint32_t index = 0;
                // 復元する画像の数
                index < imageCount;
                ++index)
            {
                // 外部画像参照か
                const bool external =
                    reader.Read<std::uint8_t>() != 0;
                // DDS形式か
                const bool isDds =
                    reader.Read<std::uint8_t>() != 0;
                imageTransparency[index] =
                    reader.Read<std::uint8_t>();
                // 記録された画像用途の値
                const auto rawUsage =
                    reader.Read<std::uint8_t>();
                // 未知の画像用途は色として復元する。
                // 復元時に使う画像用途
                const auto usage = rawUsage
                        <= static_cast<std::uint8_t>(
                            TextureLoader::TextureUsage::DataMap)
                    ? static_cast<TextureLoader::TextureUsage>(
                        rawUsage)
                    : TextureLoader::TextureUsage::Color;
                if (external)
                {
                    // 元の外部画像のパス
                    const auto imagePath =
                        LamaPon::PathFromUtf8(
                            reader.ReadString());
                    // 記録時の内容ハッシュ
                    const auto expectedHash =
                        reader.Read<std::uint64_t>();
                    if (reader.failed
                        || !assets.FileExists(imagePath))
                    {
                        return nullptr;
                    }
                    // 照合した外部画像の内容
                    const auto imageBytes =
                        assets.ReadFileBytes(imagePath);
                    if (HashBytes(imageBytes) != expectedHash)
                    {
                        return nullptr;
                    }
                    views[index] = CreateImageView(
                        assets,
                        imageBytes,
                        isDds,
                        usage);
                }
                else
                {
                    // 内蔵画像のバイト数
                    const auto byteCount =
                        reader.Read<std::uint64_t>();
                    // サイズと位置の加算前に、記録サイズ単体も検査する。
                    if (reader.failed
                        || byteCount > reader.size
                        || reader.offset + byteCount
                            > reader.size)
                    {
                        return nullptr;
                    }
                    views[index] = CreateImageView(
                        assets,
                        std::span<const std::uint8_t>(
                            reader.data + reader.offset,
                            static_cast<std::size_t>(
                                byteCount)),
                        isDds,
                        usage);
                    reader.offset += static_cast<std::size_t>(
                        byteCount);
                }
            }

            // 復元するCPUモデル
            auto model = std::make_shared<SkeletalModel>();
            model->hasLocalBounds =
                reader.Read<std::uint8_t>() != 0;
            if (model->hasLocalBounds
                && !reader.ReadRaw(
                    &model->localBounds,
                    sizeof(model->localBounds)))
            {
                return nullptr;
            }

            // 復元するノードの数
            const auto nodeCount = reader.Read<std::uint32_t>();
            // ノードの無いモデルは復元を拒否する。
            if (reader.failed
                || nodeCount == 0
                || nodeCount > MaximumCount)
            {
                return nullptr;
            }
            model->nodes.resize(nodeCount);
            // 記録・復元するノード
            for (auto& node : model->nodes)
            {
                node.name = reader.ReadString();
                node.parent = static_cast<std::ptrdiff_t>(
                    reader.Read<std::int64_t>());
                if (!reader.ReadRaw(
                    &node.bindPose,
                    sizeof(node.bindPose)))
                {
                    return nullptr;
                }
            }

            // 復元するスキンの数
            const auto skinCount = reader.Read<std::uint32_t>();
            if (reader.failed || skinCount > MaximumCount)
            {
                return nullptr;
            }
            model->skins.resize(skinCount);
            // 記録・復元するスキン
            for (auto& skin : model->skins)
            {
                skin.name = reader.ReadString();
                // スキンのボーン数
                const auto jointCount =
                    reader.Read<std::uint32_t>();
                if (reader.failed || jointCount > MaximumCount)
                {
                    return nullptr;
                }
                skin.joints.resize(jointCount);
                skin.inverseBindMatrices.resize(jointCount);
                // ボーンのノード番号
                for (auto& joint : skin.joints)
                {
                    joint = static_cast<std::size_t>(
                        reader.Read<std::uint64_t>());

                    if (joint >= model->nodes.size())
                    {
                        return nullptr;
                    }
                }
                if (!reader.ReadRaw(
                    skin.inverseBindMatrices.data(),
                    jointCount
                        * sizeof(DirectX::XMFLOAT4X4)))
                {
                    return nullptr;
                }
            }

            // 復元するクリップの数
            const auto animationCount =
                reader.Read<std::uint32_t>();
            if (reader.failed || animationCount > MaximumCount)
            {
                return nullptr;
            }
            model->animations.resize(animationCount);
            // 記録・復元するクリップ
            for (auto& clip : model->animations)
            {
                clip.name = reader.ReadString();
                clip.duration = reader.Read<float>();
                // クリップの変換トラック数
                const auto trackCount =
                    reader.Read<std::uint32_t>();
                if (reader.failed || trackCount > MaximumCount)
                {
                    return nullptr;
                }
                clip.tracks.resize(trackCount);
                // 記録・復元する変換トラック
                for (auto& track : clip.tracks)
                {
                    track.node = static_cast<std::size_t>(
                        reader.Read<std::uint64_t>());
                    if (track.node >= model->nodes.size())
                    {
                        return nullptr;
                    }
                    if (!ReadVectorChannel(
                            reader,
                            track.translation)
                        || !ReadQuaternionChannel(
                            reader,
                            track.rotation)
                        || !ReadVectorChannel(
                            reader,
                            track.scale))
                    {
                        return nullptr;
                    }
                }
            }

            // 復元する描画部分の数
            const auto primitiveCount =
                reader.Read<std::uint32_t>();
            if (reader.failed
                || primitiveCount == 0
                || primitiveCount > MaximumCount)
            {
                return nullptr;
            }
            model->primitives.resize(primitiveCount);
            // 記録・復元する描画部分
            for (auto& primitive : model->primitives)
            {
                primitive.meshNode = static_cast<std::size_t>(
                    reader.Read<std::uint64_t>());
                primitive.skin = static_cast<std::ptrdiff_t>(
                    reader.Read<std::int64_t>());
                primitive.indexCount =
                    reader.Read<std::uint32_t>();
                primitive.hasLocalBounds =
                    reader.Read<std::uint8_t>() != 0;
                if (primitive.hasLocalBounds
                    && !reader.ReadRaw(
                        &primitive.localBounds,
                        sizeof(primitive.localBounds)))
                {
                    return nullptr;
                }

                if (primitive.meshNode >= model->nodes.size()
                    || primitive.skin
                        >= static_cast<std::ptrdiff_t>(
                            model->skins.size())
                    || primitive.skin < -1)
                {
                    return nullptr;
                }
                if (!reader.ReadRaw(
                        &primitive.baseColor,
                        sizeof(primitive.baseColor)))
                {
                    return nullptr;
                }
                primitive.roughness = reader.Read<float>();
                primitive.metallic = reader.Read<float>();
                primitive.occlusionStrength =
                    reader.Read<float>();
                if (!reader.ReadRaw(
                        &primitive.emissiveFactor,
                        sizeof(primitive.emissiveFactor)))
                {
                    return nullptr;
                }
                primitive.alpha =
                    reader.Read<std::uint8_t>() != 0;
                primitive.textureHasTransparency =
                    reader.Read<std::uint8_t>() != 0;
                primitive.doubleSided =
                    reader.Read<std::uint8_t>() != 0;
                // アルファ破棄エフェクトの有無
                const bool hasCutout =
                    reader.Read<std::uint8_t>() != 0;

                // 6用途の画像番号
                std::int32_t slots[6]{};
                if (!reader.ReadRaw(slots, sizeof(slots)))
                {
                    return nullptr;
                }
                // 画像番号からビューを取得する(slot: 記録された画像番号)。
                const auto viewAt =
                    [&views](const std::int32_t slot)
                    -> Microsoft::WRL::ComPtr<
                        ID3D11ShaderResourceView>
                {
                    if (slot < 0
                        || static_cast<std::size_t>(slot)
                            >= views.size())
                    {
                        return {};
                    }
                    return views[
                        static_cast<std::size_t>(slot)];
                };
                primitive.texture = viewAt(slots[0]);
                primitive.normalTexture = viewAt(slots[1]);
                primitive.roughnessTexture = viewAt(slots[2]);
                primitive.metallicTexture = viewAt(slots[3]);
                primitive.occlusionTexture = viewAt(slots[4]);
                primitive.emissiveTexture = viewAt(slots[5]);

                // 復元する頂点の数
                const auto vertexCount =
                    reader.Read<std::uint64_t>();
                // バイト数の乗算前に、元ファイルサイズから頂点数の上限を検査する。
                if (reader.failed
                    || vertexCount == 0
                    || vertexCount
                        > reader.size / sizeof(ModelVertex))
                {
                    return nullptr;
                }
                // 共通CPU頂点列のバイト数
                const std::size_t vertexBytes =
                    static_cast<std::size_t>(vertexCount)
                    * sizeof(ModelVertex);
                if (reader.offset + vertexBytes > reader.size)
                {
                    return nullptr;
                }
                // 復元した共通CPU頂点列
                std::vector<ModelVertex> cpuVertices(
                    static_cast<std::size_t>(vertexCount));
                std::memcpy(
                    cpuVertices.data(),
                    reader.data + reader.offset,
                    vertexBytes);
                primitive.cpuVertexData.assign(
                    reader.data + reader.offset,
                    reader.data + reader.offset + vertexBytes);
                primitive.cpuVertexStride = sizeof(ModelVertex);
                CreateBuffer(
                    device,
                    assets,
                    cpuVertices.data(),
                    vertexBytes,
                    D3D11_BIND_VERTEX_BUFFER,
                    primitive.vertexBuffer
                        .ReleaseAndGetAddressOf());
                reader.offset += vertexBytes;

                // 記録された索引の数
                const auto storedIndexCount =
                    reader.Read<std::uint64_t>();
                if (reader.failed)
                {
                    return nullptr;
                }
                // 復元したCPU索引列
                std::vector<std::uint32_t> cpuIndices;
                if (storedIndexCount == 0)
                {
                    // 索引未記録時は描画部分の索引数だけ0からの連番を作る。
                    cpuIndices.resize(
                        primitive.indexCount);
                    // 記録列の要素番号
                    for (std::uint32_t index = 0;
                        index < primitive.indexCount;
                        ++index)
                    {
                        cpuIndices[index] = index;
                    }
                    CreateBuffer(
                        device,
                        assets,
                        cpuIndices.data(),
                        cpuIndices.size()
                            * sizeof(std::uint32_t),
                        D3D11_BIND_INDEX_BUFFER,
                        primitive.indexBuffer
                            .ReleaseAndGetAddressOf());
                }
                else
                {
                    if (storedIndexCount
                        > reader.size
                            / sizeof(std::uint32_t))
                    {
                        return nullptr;
                    }
                    // 索引列のバイト数
                    const std::size_t indexBytes =
                        static_cast<std::size_t>(
                            storedIndexCount)
                        * sizeof(std::uint32_t);
                    if (reader.offset + indexBytes
                        > reader.size)
                    {
                        return nullptr;
                    }
                    cpuIndices.resize(
                        static_cast<std::size_t>(
                            storedIndexCount));
                    std::memcpy(
                        cpuIndices.data(),
                        reader.data + reader.offset,
                        indexBytes);
                    CreateBuffer(
                        device,
                        assets,
                        cpuIndices.data(),
                        indexBytes,
                        D3D11_BIND_INDEX_BUFFER,
                        primitive.indexBuffer
                            .ReleaseAndGetAddressOf());
                    reader.offset += indexBytes;
                }
                primitive.cpuIndices = cpuIndices;

                if (primitive.skin < 0
                    && primitive.hasLocalBounds)
                {
                    // 生成した詳細度別の索引列
                    const auto lodLevels =
                        ModelLod::BuildLevels<ModelVertex>(
                            cpuVertices,
                            cpuIndices,
                            primitive.localBounds);
                    // 詳細度の段階番号
                    for (std::size_t level = 0;
                        level < lodLevels.size();
                        ++level)
                    {
                        if (lodLevels[level].empty())
                        {
                            continue;
                        }
                        primitive.cpuLodIndices[level] =
                            lodLevels[level];
                        CreateBuffer(
                            device,
                            assets,
                            lodLevels[level].data(),
                            lodLevels[level].size()
                                * sizeof(std::uint32_t),
                            D3D11_BIND_INDEX_BUFFER,
                            primitive.lodIndexBuffers[level]
                                .ReleaseAndGetAddressOf());
                        primitive.lodIndexCounts[level] =
                            static_cast<std::uint32_t>(
                                lodLevels[level].size());
                    }
                }

                BuildPrimitiveEffect(device, primitive);
                if (hasCutout)
                {
                    BuildCutoutEffect(device, primitive);
                }
            }

            // 未消費の末尾データも復元失敗として扱う。
            if (reader.failed || reader.offset != reader.size)
            {
                return nullptr;
            }
            return model;
        }
        catch (...)
        {
            // 復元失敗は空を返し、呼出元に再インポートを委ねる。
            return nullptr;
        }
    }

    void Store(
        const std::uint64_t key,
        const SkeletalModel& model,
        const Recorder& recorder) noexcept
    {
        try
        {
            // 描画部分と幾何の数が揃わなければ保存しない。
            if (model.primitives.empty()
                || recorder.geometries.size()
                    != model.primitives.size())
            {
                return;
            }
            // 描画部分に対応する幾何
            for (const auto& geometry : recorder.geometries)
            {
                if (geometry.vertexStride
                    != sizeof(ModelVertex))
                {
                    return;
                }
            }

            // キャッシュのファイルパス
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            // ファイル操作の結果
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            // 保存列の概算バイト数
            std::size_t estimate = 4096;
            // 描画部分に対応する幾何
            for (const auto& geometry : recorder.geometries)
            {
                estimate += geometry.vertexBytes.size()
                    + geometry.indices.size() * 4 + 64;
            }
            // 記録する画像情報
            for (const auto& image : recorder.images)
            {
                estimate += image.bytes.size() + 64;
            }
            // 保存するキャッシュの列
            std::vector<std::uint8_t> output;
            output.reserve(estimate);

            AppendBytes(output, Magic, sizeof(Magic));
            AppendU32(output, FormatVersion);
            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    sizeof(ModelVertex)));

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    recorder.dependencies.size()));
            // 記録・照合する依存情報
            for (const auto& dependency : recorder.dependencies)
            {
                AppendString(
                    output,
                    LamaPon::PathToUtf8(dependency.path));
                AppendU8(output, dependency.exists ? 1 : 0);
                AppendU64(output, dependency.hash);
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    recorder.images.size()));
            // 記録する画像情報
            for (const auto& image : recorder.images)
            {
                AppendU8(output, image.external ? 1 : 0);
                AppendU8(output, image.isDds ? 1 : 0);
                AppendU8(
                    output,
                    image.hasTransparency ? 1 : 0);
                AppendU8(
                    output,
                    static_cast<std::uint8_t>(image.usage));
                if (image.external)
                {
                    AppendString(
                        output,
                        LamaPon::PathToUtf8(
                            image.externalPath));
                    AppendU64(output, image.externalHash);
                }
                else
                {
                    AppendU64(output, image.bytes.size());
                    AppendBytes(
                        output,
                        image.bytes.data(),
                        image.bytes.size());
                }
            }

            AppendU8(output, model.hasLocalBounds ? 1 : 0);
            if (model.hasLocalBounds)
            {
                AppendBytes(
                    output,
                    &model.localBounds,
                    sizeof(model.localBounds));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.nodes.size()));
            // 記録・復元するノード
            for (const auto& node : model.nodes)
            {
                AppendString(output, node.name);
                AppendI64(
                    output,
                    static_cast<std::int64_t>(node.parent));
                AppendBytes(
                    output,
                    &node.bindPose,
                    sizeof(node.bindPose));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.skins.size()));
            // 記録・復元するスキン
            for (const auto& skin : model.skins)
            {
                AppendString(output, skin.name);
                AppendU32(
                    output,
                    static_cast<std::uint32_t>(
                        skin.joints.size()));
                // ボーンのノード番号
                for (const auto joint : skin.joints)
                {
                    AppendU64(
                        output,
                        static_cast<std::uint64_t>(joint));
                }
                AppendBytes(
                    output,
                    skin.inverseBindMatrices.data(),
                    skin.inverseBindMatrices.size()
                        * sizeof(DirectX::XMFLOAT4X4));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.animations.size()));
            // 記録・復元するクリップ
            for (const auto& clip : model.animations)
            {
                AppendString(output, clip.name);
                AppendF32(output, clip.duration);
                AppendU32(
                    output,
                    static_cast<std::uint32_t>(
                        clip.tracks.size()));
                // 記録・復元する変換トラック
                for (const auto& track : clip.tracks)
                {
                    AppendU64(
                        output,
                        static_cast<std::uint64_t>(
                            track.node));
                    AppendVectorChannel(
                        output,
                        track.translation);
                    AppendQuaternionChannel(
                        output,
                        track.rotation);
                    AppendVectorChannel(output, track.scale);
                }
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.primitives.size()));
            // 記録列の要素番号
            for (std::size_t index = 0;
                index < model.primitives.size();
                ++index)
            {
                // 記録・復元する描画部分
                const auto& primitive =
                    model.primitives[index];
                // 描画部分に対応する幾何
                const auto& geometry =
                    recorder.geometries[index];
                AppendU64(
                    output,
                    static_cast<std::uint64_t>(
                        primitive.meshNode));
                AppendI64(
                    output,
                    static_cast<std::int64_t>(
                        primitive.skin));
                AppendU32(output, primitive.indexCount);
                AppendU8(
                    output,
                    primitive.hasLocalBounds ? 1 : 0);
                if (primitive.hasLocalBounds)
                {
                    AppendBytes(
                        output,
                        &primitive.localBounds,
                        sizeof(primitive.localBounds));
                }
                AppendBytes(
                    output,
                    &primitive.baseColor,
                    sizeof(primitive.baseColor));
                AppendF32(output, primitive.roughness);
                AppendF32(output, primitive.metallic);
                AppendF32(output, primitive.occlusionStrength);
                AppendBytes(
                    output,
                    &primitive.emissiveFactor,
                    sizeof(primitive.emissiveFactor));
                AppendU8(output, primitive.alpha ? 1 : 0);
                AppendU8(
                    output,
                    primitive.textureHasTransparency
                        ? 1
                        : 0);
                AppendU8(
                    output,
                    primitive.doubleSided ? 1 : 0);
                AppendU8(
                    output,
                    primitive.cutoutEffect != nullptr
                        ? 1
                        : 0);

                // 6用途の画像番号
                const std::int32_t slots[6] = {
                    recorder.ImageIndexFor(
                        primitive.texture.Get()),
                    recorder.ImageIndexFor(
                        primitive.normalTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.roughnessTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.metallicTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.occlusionTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.emissiveTexture.Get())
                };
                AppendBytes(output, slots, sizeof(slots));

                AppendU64(
                    output,
                    geometry.vertexBytes.size()
                        / sizeof(ModelVertex));
                AppendBytes(
                    output,
                    geometry.vertexBytes.data(),
                    geometry.vertexBytes.size());
                AppendU64(output, geometry.indices.size());
                AppendBytes(
                    output,
                    geometry.indices.data(),
                    geometry.indices.size()
                        * sizeof(std::uint32_t));
            }

            WriteFileAtomically(path, output);
        }
        catch (...)
        {

        }
    }
}
