#include "LamaPon/Graphics/EnvironmentCache.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

namespace
{
    // 畳み込みの計算やキューブの構成を変えた場合は、形式版を上げる。
    // キャッシュのファイル形式版
    constexpr std::uint32_t FormatVersion = 1;

    // キャッシュ形式の識別列
    constexpr char Magic[4] = { 'T', 'E', 'N', 'V' };


    // 固定のRGBA16Fキューブ形式
    constexpr DXGI_FORMAT CubeFormat =
        DXGI_FORMAT_R16G16B16A16_FLOAT;
    // 一画素のバイト数
    constexpr std::uint32_t BytesPerPixel = 8;

    // キャッシュ配置指定の排他
    std::mutex g_directoryMutex;
    // 任意のキャッシュ配置先
    std::filesystem::path g_directoryOverride;

    // ユーザーデータ領域または一時領域から既定の配置先を求める。
    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        // ユーザー別データ配置先の文字列
        std::wstring localAppData(32768, L'\0');
        // 環境変数の取得文字数
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        // 既定配置先の基準フォルダー
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            // ファイル操作のエラー
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"environment-cache";
    }

    // 識別子の十六進表現から保存パスを求める(key: キャッシュ識別子)。
    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        // キャッシュの配置先
        const auto directory =
            LamaPon::EnvironmentCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        // 十六進のキャッシュファイル名
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.tenv", key);
        return directory / name;
    }

    // 一時保存して既存ファイルと置換する(destination: 保存先, bytes: 保存する全バイト列)。
    // 一時名は保存先に.tmpを足した固定名なので、同じ保存先への書込を並行実行しない。
    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {
        // 置換前の一時保存パス
        auto temporary = destination;
        temporary += L".tmp";
        {
            // 置換前の一時保存ファイル
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
                // 一時保存の削除エラー
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            // ファイル操作のエラー
            std::error_code error;
            std::filesystem::remove(temporary, error);
        }
    }


    // 六面の全ミップの総バイト数を求める(size: 一辺の画素数, mips: 有効なミップ段数)。
    [[nodiscard]] std::size_t CubeByteCount(
        const std::uint32_t size,
        const std::uint32_t mips) noexcept
    {
        // 六面の全ミップのバイト数
        std::size_t total = 0;
        // 処理するミップ番号
        for (std::uint32_t mip = 0; mip < mips; ++mip)
        {
            // 対象ミップの一辺の画素数
            const std::size_t edge =
                std::max<std::uint32_t>(size >> mip, 1);
            total += edge * edge * BytesPerPixel * 6;
        }
        return total;
    }


    // GPUを待ってキューブを詰めた列へ読み戻す(device: 描画デバイス, context: 描画コンテキスト, view: キューブ参照, size: 一辺の画素数の出力, mips: ミップ段数の出力, bytes: 詰めたバイト列の出力)。
    [[nodiscard]] bool ReadCube(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        ID3D11ShaderResourceView* const view,
        std::uint32_t& size,
        std::uint32_t& mips,
        std::vector<std::uint8_t>& bytes)
    {
        if (view == nullptr)
        {
            return false;
        }
        // 参照元の描画資源
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.ReleaseAndGetAddressOf());
        // 読み取るか作成するキューブ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture)))
        {
            return false;
        }
        // キューブ資源の仕様
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != CubeFormat
            || description.ArraySize != 6
            || description.Width != description.Height)
        {
            return false;
        }
        size = description.Width;
        mips = description.MipLevels;

        // CPU読取用資源の仕様
        D3D11_TEXTURE2D_DESC staging = description;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        // CPU読取用の複製キューブ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(
            &staging,
            nullptr,
            copy.ReleaseAndGetAddressOf())))
        {
            return false;
        }
        context->CopyResource(copy.Get(), texture.Get());

        bytes.clear();
        bytes.reserve(CubeByteCount(size, mips));
        // 処理するキューブ面の番号
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            // 処理するミップ番号
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                // 対象面とミップの資源番号
                const UINT subresource =
                    D3D11CalcSubresource(
                        mip,
                        face,
                        mips);
                // CPU読取領域の情報
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(
                    copy.Get(),
                    subresource,
                    D3D11_MAP_READ,
                    0,
                    &mapped)))
                {
                    return false;
                }
                // 対象ミップの一辺の画素数
                const std::uint32_t edge =
                    std::max<std::uint32_t>(size >> mip, 1);
                // 一行の詰めたバイト数
                const std::size_t rowBytes =
                    static_cast<std::size_t>(edge)
                    * BytesPerPixel;
                // CPU読取領域の先頭
                const auto* source =
                    static_cast<const std::uint8_t*>(
                        mapped.pData);
                // コピーする画素行番号
                for (std::uint32_t row = 0;
                    // 対象ミップの一辺の画素数
                    row < edge;
                    ++row)
                {
                    bytes.insert(
                        bytes.end(),
                        source + row * mapped.RowPitch,
                        source + row * mapped.RowPitch
                            + rowBytes);
                }
                context->Unmap(copy.Get(), subresource);
            }
        }
        return true;
    }


    // 面ごとのミップ列から読取専用キューブを作る(device: 描画デバイス, size: 一辺の画素数, mips: 有効なミップ段数, bytes: 全面とミップのバイト列)。
    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateCube(
            ID3D11Device* const device,
            const std::uint32_t size,
            const std::uint32_t mips,
            const std::uint8_t* bytes)
    {
        // 各面とミップの初期データ
        std::vector<D3D11_SUBRESOURCE_DATA> initialData(
            static_cast<std::size_t>(mips) * 6);
        // 初期データの読取位置
        const std::uint8_t* cursor = bytes;
        // ReadCubeと同じ面ごとのミップ順に並べ、サブリソース番号をmip + face * mipsとする。
        // 処理するキューブ面の番号
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            // 処理するミップ番号
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                // 対象ミップの一辺の画素数
                const std::uint32_t edge =
                    std::max<std::uint32_t>(size >> mip, 1);
                // 一行の詰めたバイト数
                const std::size_t rowBytes =
                    static_cast<std::size_t>(edge)
                    * BytesPerPixel;
                // 対象面とミップの初期情報
                auto& data = initialData[
                    D3D11CalcSubresource(mip, face, mips)];
                data.pSysMem = cursor;
                data.SysMemPitch =
                    static_cast<UINT>(rowBytes);
                cursor += rowBytes * edge;
            }
        }

        // キューブ資源の仕様
        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = size;
        description.MipLevels = mips;
        description.ArraySize = 6;
        description.Format = CubeFormat;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_TEXTURECUBE;

        // 読み取るか作成するキューブ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(
            &description,
            initialData.data(),
            texture.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        // キューブ参照の仕様
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = CubeFormat;
        viewDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURECUBE;
        viewDescription.TextureCube.MipLevels = mips;
        // 作成するキューブ参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(
            texture.Get(),
            &viewDescription,
            view.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        return view;
    }

    // Windowsの四バイト整数表現を追記する(output: 追記するバイト列, value: 保存する整数)。
    void AppendU32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        // 整数の四バイト表現の先頭
        const auto* begin =
            reinterpret_cast<const std::uint8_t*>(&value);
        output.insert(output.end(), begin, begin + 4);
    }
}

namespace LamaPon::EnvironmentCache
{
    std::filesystem::path CacheDirectory()
    {
        {
            // 配置指定の読取用排他
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
        // 配置指定の更新用排他
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t HashBytes(
        const std::span<const std::uint8_t> bytes) noexcept
    {
        // 計算中のFNV-1a識別子
        std::uint64_t hash = 14695981039346656037ull;
        // 取り込む一バイト
        for (const std::uint8_t byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void Store(
        const std::uint64_t key,
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const EnvironmentRenderer::OwnedPrefilteredEnvironment&
            environment) noexcept
    {
        try
        {
            if (!environment.IsValid()
                || device == nullptr
                || context == nullptr)
            {
                return;
            }
            // 鏡面キューブの一辺の画素数
            std::uint32_t specularSize{};
            // 鏡面キューブのミップ段数
            std::uint32_t specularMips{};
            // 鏡面キューブの保存バイト列
            std::vector<std::uint8_t> specularBytes;
            // 拡散キューブの一辺の画素数
            std::uint32_t irradianceSize{};
            // 拡散キューブのミップ段数
            std::uint32_t irradianceMips{};
            // 拡散キューブの保存バイト列
            std::vector<std::uint8_t> irradianceBytes;
            if (!ReadCube(
                    device,
                    context,
                    environment.specular.Get(),
                    specularSize,
                    specularMips,
                    specularBytes)
                || !ReadCube(
                    device,
                    context,
                    environment.irradiance.Get(),
                    irradianceSize,
                    irradianceMips,
                    irradianceBytes))
            {
                return;
            }
            if (specularSize
                    != EnvironmentRenderer::PrefilteredSpecularSize
                || specularMips
                    != EnvironmentRenderer::PrefilteredSpecularMipLevels
                || irradianceSize
                    != EnvironmentRenderer::PrefilteredIrradianceSize
                || irradianceMips
                    != EnvironmentRenderer::PrefilteredIrradianceMipLevels)
            {
                return;
            }

            // キャッシュファイルのパス
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            // ファイル操作のエラー
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            // 保存するキャッシュのバイト列
            std::vector<std::uint8_t> output;
            output.reserve(
                64
                + specularBytes.size()
                + irradianceBytes.size());
            output.insert(
                output.end(),
                Magic,
                Magic + sizeof(Magic));
            AppendU32(output, FormatVersion);
            AppendU32(output, specularSize);
            AppendU32(output, specularMips);
            AppendU32(output, irradianceSize);
            AppendU32(output, irradianceMips);
            output.insert(
                output.end(),
                specularBytes.begin(),
                specularBytes.end());
            output.insert(
                output.end(),
                irradianceBytes.begin(),
                irradianceBytes.end());
            WriteFileAtomically(path, output);
        }
        catch (...)
        {
            // 保存の失敗を描画の失敗に波及させない。
        }
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment TryLoad(
        ID3D11Device* const device,
        const std::uint64_t key)
    {
        // 復元する二キューブの所有参照
        EnvironmentRenderer::OwnedPrefilteredEnvironment result;
        try
        {
            // キャッシュファイルのパス
            const auto path = EntryPath(key);
            if (path.empty() || device == nullptr)
            {
                return result;
            }
            // 読み込むキャッシュファイル
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return result;
            }
            input.seekg(0, std::ios::end);
            // キャッシュ全体のバイト数
            const std::streamoff fileSize = input.tellg();
            // 固定形式の正確なファイル寸法を確保前に検証し、不正なキャッシュを読まない。
            // 固定ヘッダーのバイト数
            constexpr std::size_t HeaderBytes = 4 + 4 * 5;
            // 規定形式のファイルバイト数
            const std::size_t expectedFileSize =
                HeaderBytes
                + CubeByteCount(
                    EnvironmentRenderer::PrefilteredSpecularSize,
                    EnvironmentRenderer::PrefilteredSpecularMipLevels)
                + CubeByteCount(
                    EnvironmentRenderer::PrefilteredIrradianceSize,
                    EnvironmentRenderer::PrefilteredIrradianceMipLevels);
            if (fileSize <= 0
                || static_cast<std::uintmax_t>(fileSize)
                    != expectedFileSize)
            {
                return result;
            }
            input.seekg(0, std::ios::beg);
            // 読み込んだキャッシュの全バイト列
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>(fileSize));
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                fileSize);
            if (!input)
            {
                return result;
            }
            input.close();

            // ヘッダーは識別列・形式版・二キューブの寸法とミップ数で構成する。
            if (bytes.size() < HeaderBytes
                || std::memcmp(
                    bytes.data(),
                    Magic,
                    sizeof(Magic)) != 0)
            {
                return result;
            }
            // 四バイト整数を読む(offset: 読取開始位置)。
            const auto readU32 =
                [&bytes](const std::size_t offset)
            {
                // 読み取る四バイト整数
                std::uint32_t value{};
                std::memcpy(&value, bytes.data() + offset, 4);
                return value;
            };
            if (readU32(4) != FormatVersion)
            {
                return result;
            }
            // 鏡面キューブの一辺の画素数
            const std::uint32_t specularSize = readU32(8);
            // 鏡面キューブのミップ段数
            const std::uint32_t specularMips = readU32(12);
            // 拡散キューブの一辺の画素数
            const std::uint32_t irradianceSize = readU32(16);
            // 拡散キューブのミップ段数
            const std::uint32_t irradianceMips = readU32(20);
            // 現在の畳み込み仕様と一致しないキャッシュは未保存として扱い、呼出し側で再生成する。
            if (specularSize
                    != LamaPon::EnvironmentRenderer::
                        PrefilteredSpecularSize
                || specularMips
                    != LamaPon::EnvironmentRenderer::
                        PrefilteredSpecularMipLevels
                || irradianceSize
                    != LamaPon::EnvironmentRenderer::
                        PrefilteredIrradianceSize
                || irradianceMips
                    != LamaPon::EnvironmentRenderer::
                        PrefilteredIrradianceMipLevels)
            {
                return result;
            }
            // 鏡面キューブの必要バイト数
            const std::size_t specularBytes =
                CubeByteCount(specularSize, specularMips);
            // 拡散キューブの必要バイト数
            const std::size_t irradianceBytes =
                CubeByteCount(irradianceSize, irradianceMips);

            if (bytes.size()
                != HeaderBytes + specularBytes
                    + irradianceBytes)
            {
                return result;
            }

            // 復元した鏡面キューブ参照
            auto specular = CreateCube(
                device,
                specularSize,
                specularMips,
                bytes.data() + HeaderBytes);
            // 復元した拡散キューブ参照
            auto irradiance = CreateCube(
                device,
                irradianceSize,
                irradianceMips,
                bytes.data() + HeaderBytes + specularBytes);
            if (specular == nullptr || irradiance == nullptr)
            {
                return result;
            }
            result.specular = std::move(specular);
            result.irradiance = std::move(irradiance);
            result.specularMaximumMip =
                static_cast<float>(specularMips - 1);
            return result;
        }
        catch (...)
        {
            return {};
        }
    }
}
