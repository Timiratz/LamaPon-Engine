#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/TextureCache.h"
#include "LamaPon/Assets/TextureLoader.h"

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // PNGチャンク用CRC32（多項式0xEDB88320）。
    [[nodiscard]] std::uint32_t Crc32(
        const std::uint8_t* data,
        const std::size_t size) noexcept
    {
        std::uint32_t crc = 0xffffffffu;
        for (std::size_t index = 0;
            index < size;
            ++index)
        {
            crc ^= data[index];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1)
                    ^ (0xedb88320u & (~(crc & 1u) + 1u));
            }
        }
        return crc ^ 0xffffffffu;
    }

    // zlibストリーム末尾のAdler-32。
    [[nodiscard]] std::uint32_t Adler32(
        const std::vector<std::uint8_t>& data) noexcept
    {
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const auto value : data)
        {
            a = (a + value) % 65521u;
            b = (b + a) % 65521u;
        }
        return (b << 16) | a;
    }

    void AppendBigEndian(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        output.push_back(
            static_cast<std::uint8_t>(value >> 24));
        output.push_back(
            static_cast<std::uint8_t>(value >> 16));
        output.push_back(
            static_cast<std::uint8_t>(value >> 8));
        output.push_back(
            static_cast<std::uint8_t>(value));
    }

    void AppendChunk(
        std::vector<std::uint8_t>& output,
        const char* type,
        const std::vector<std::uint8_t>& payload)
    {
        AppendBigEndian(
            output,
            static_cast<std::uint32_t>(payload.size()));
        std::vector<std::uint8_t> body(
            type,
            type + 4);
        body.insert(
            body.end(),
            payload.begin(),
            payload.end());
        output.insert(
            output.end(),
            body.begin(),
            body.end());
        AppendBigEndian(
            output,
            Crc32(body.data(), body.size()));
    }

    // RGBA8ピクセル列から無圧縮deflateのPNGを組み立てます。
    // 外部ライブラリなしでWICデコードの入力を作るためです。
    [[nodiscard]] std::vector<std::uint8_t> BuildPng(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::vector<std::uint8_t>& rgbaPixels)
    {
        Require(
            rgbaPixels.size()
                == static_cast<std::size_t>(width)
                    * height * 4,
            "BuildPng pixel count mismatch");

        std::vector<std::uint8_t> png{
            0x89, 0x50, 0x4e, 0x47,
            0x0d, 0x0a, 0x1a, 0x0a };

        std::vector<std::uint8_t> header;
        AppendBigEndian(header, width);
        AppendBigEndian(header, height);
        header.push_back(8);   // ビット深度
        header.push_back(6);   // カラータイプ: RGBA
        header.push_back(0);   // 圧縮方式
        header.push_back(0);   // フィルター方式
        header.push_back(0);   // 非インターレース
        AppendChunk(png, "IHDR", header);

        // 各行の先頭にフィルター種別0（None）を付けます。
        std::vector<std::uint8_t> raw;
        for (std::uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const auto* row =
                rgbaPixels.data()
                + static_cast<std::size_t>(y)
                    * width * 4;
            raw.insert(raw.end(), row, row + width * 4);
        }

        // zlibヘッダー＋store（無圧縮）deflateブロック列。
        // storeブロックの長さは16bitまでなので、65535バイト
        // ずつに割り、BFINALは最後のブロックにだけ立てます。
        // 1ブロックのまま書くと64KB超で長さが黙って切り捨て
        // られ、**下のほうの画素が壊れたPNG**になります
        // （2026-08-06に512x512で踏みました。WICはエラーを
        // 出さずに壊れた画素を返すので気付きにくい）。
        std::vector<std::uint8_t> idat{ 0x78, 0x01 };
        std::size_t offset = 0;
        do
        {
            const std::size_t remaining =
                raw.size() - offset;
            const auto blockLength =
                static_cast<std::uint16_t>(
                    remaining < 65535 ? remaining : 65535);
            const bool finalBlock =
                offset + blockLength == raw.size();
            idat.push_back(finalBlock ? 0x01 : 0x00);
            idat.push_back(
                static_cast<std::uint8_t>(
                    blockLength & 0xff));
            idat.push_back(
                static_cast<std::uint8_t>(
                    blockLength >> 8));
            idat.push_back(
                static_cast<std::uint8_t>(
                    ~blockLength & 0xff));
            idat.push_back(
                static_cast<std::uint8_t>(
                    (~blockLength >> 8) & 0xff));
            idat.insert(
                idat.end(),
                raw.begin()
                    + static_cast<std::ptrdiff_t>(offset),
                raw.begin()
                    + static_cast<std::ptrdiff_t>(
                        offset + blockLength));
            offset += blockLength;
        } while (offset < raw.size());
        AppendBigEndian(idat, Adler32(raw));
        AppendChunk(png, "IDAT", idat);

        AppendChunk(png, "IEND", {});
        return png;
    }

    void WriteBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        Require(
            static_cast<bool>(output),
            "Could not create test texture file");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] LamaPon::TextureLoader::CpuImage
        SolidImage(
            const std::uint32_t width,
            const std::uint32_t height,
            const std::array<std::uint8_t, 4>& color)
    {
        LamaPon::TextureLoader::CpuImage image;
        image.width = width;
        image.height = height;
        image.pixels.resize(
            static_cast<std::size_t>(width)
            * height * 4);
        for (std::size_t pixel = 0;
            pixel < image.pixels.size();
            pixel += 4)
        {
            image.pixels[pixel] = color[0];
            image.pixels[pixel + 1] = color[1];
            image.pixels[pixel + 2] = color[2];
            image.pixels[pixel + 3] = color[3];
        }
        return image;
    }

    void TestMipChain()
    {
        auto base = SolidImage(8, 4, { 10, 20, 30, 255 });
        const auto mips =
            LamaPon::TextureLoader::GenerateMipChain(
                std::move(base));
        Require(
            mips.size() == 4,
            "8x4 should produce 4 mip levels");
        Require(
            mips[1].width == 4 && mips[1].height == 2,
            "mip1 should be 4x2");
        Require(
            mips[2].width == 2 && mips[2].height == 1,
            "mip2 should be 2x1");
        Require(
            mips[3].width == 1 && mips[3].height == 1,
            "mip3 should be 1x1");
        Require(
            mips[3].pixels[0] == 10
                && mips[3].pixels[1] == 20
                && mips[3].pixels[2] == 30
                && mips[3].pixels[3] == 255,
            "solid color must survive mip filtering");

        // 2x2→1x1のボックス平均を厳密に確認します。
        LamaPon::TextureLoader::CpuImage quad;
        quad.width = 2;
        quad.height = 2;
        quad.pixels = {
            0, 0, 0, 255,
            255, 255, 255, 255,
            100, 50, 200, 255,
            60, 150, 20, 255 };
        const auto quadMips =
            LamaPon::TextureLoader::GenerateMipChain(
                std::move(quad));
        Require(
            quadMips.size() == 2,
            "2x2 should produce 2 mip levels");
        Require(
            quadMips[1].pixels[0] == 104
                && quadMips[1].pixels[1] == 114
                && quadMips[1].pixels[2] == 119
                && quadMips[1].pixels[3] == 255,
            "1x1 mip must be the rounded box average");
    }

    void TestBlockCompression()
    {
        const auto opaque =
            SolidImage(4, 4, { 200, 64, 32, 255 });
        Require(
            !LamaPon::TextureLoader::HasTransparentPixels(
                opaque),
            "opaque image must not report transparency");

        const auto bc1 =
            LamaPon::TextureLoader::CompressBC1(opaque);
        Require(
            bc1.size() == 8,
            "one BC1 block is 8 bytes");
        std::uint16_t color0{};
        std::uint16_t color1{};
        std::memcpy(&color0, bc1.data(), 2);
        std::memcpy(&color1, bc1.data() + 2, 2);
        const std::uint16_t expected565 =
            static_cast<std::uint16_t>(
                ((200 >> 3) << 11)
                | ((64 >> 2) << 5)
                | (32 >> 3));
        Require(
            color0 == expected565
                && color1 == expected565,
            "solid block endpoints must equal the color");
        std::uint32_t indices{};
        std::memcpy(&indices, bc1.data() + 4, 4);
        Require(
            indices == 0,
            "solid block must select endpoint 0 everywhere");

        // アルファのグラデーションでBC3の端点を確認します。
        auto alphaImage =
            SolidImage(4, 4, { 128, 128, 128, 255 });
        const std::array<std::uint8_t, 4> rowAlpha{
            255, 128, 64, 0 };
        for (std::uint32_t y = 0; y < 4; ++y)
        {
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                alphaImage.pixels[
                    (static_cast<std::size_t>(y) * 4 + x)
                        * 4 + 3] = rowAlpha[y];
            }
        }
        Require(
            LamaPon::TextureLoader::HasTransparentPixels(
                alphaImage),
            "alpha gradient must report transparency");
        const auto bc3 =
            LamaPon::TextureLoader::CompressBC3(
                alphaImage);
        Require(
            bc3.size() == 16,
            "one BC3 block is 16 bytes");
        Require(
            bc3[0] == 255 && bc3[1] == 0,
            "BC3 alpha endpoints must be max/min");
    }

    // BC5は法線マップ用。BC4ブロック2つ（先がR、後がG）で、
    // それぞれBC3のアルファ部と同じ符号化です。
    void TestNormalMapCompression()
    {
        // Rは行ごとに変え、Gは全画素同じにします。R側だけ端点が
        // 開いていれば「R・Gを別々のブロックへ入れている」ことが
        // 分かります（両方同じ値を入れると取り違えに気付けません）。
        auto image = SolidImage(4, 4, { 0, 90, 255, 255 });
        const std::array<std::uint8_t, 4> rowRed{
            255, 170, 85, 0 };
        for (std::uint32_t y = 0; y < 4; ++y)
        {
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                image.pixels[
                    (static_cast<std::size_t>(y) * 4 + x)
                        * 4] = rowRed[y];
            }
        }

        const auto bc5 =
            LamaPon::TextureLoader::CompressBC5(image);
        Require(
            bc5.size() == 16,
            "one BC5 block is 16 bytes");
        Require(
            bc5[0] == 255 && bc5[1] == 0,
            "BC5 red endpoints must be max/min of the red channel");
        Require(
            bc5[8] == 90 && bc5[9] == 90,
            "BC5 green endpoints must come from the green channel");

        // 用途で選ばれるフォーマットが変わることを確かめます。
        std::vector<LamaPon::TextureLoader::CpuImage> mips;
        mips.push_back(image);
        using Usage = LamaPon::TextureLoader::TextureUsage;
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                mips,
                true,
                Usage::NormalMap) == DXGI_FORMAT_BC5_UNORM,
            "normal maps must choose BC5");
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                mips,
                true,
                Usage::DataMap) == DXGI_FORMAT_BC1_UNORM,
            "data maps must choose BC1");
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                mips,
                true,
                Usage::Color) == DXGI_FORMAT_BC1_UNORM,
            "opaque colour must choose BC1");
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                mips,
                false,
                Usage::NormalMap)
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "compression off must stay uncompressed for every usage");

        // 4の倍数でない画像はどの用途でも非圧縮のままです。
        std::vector<LamaPon::TextureLoader::CpuImage> odd;
        odd.push_back(SolidImage(6, 4, { 10, 20, 30, 255 }));
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                odd,
                true,
                Usage::NormalMap)
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "non multiple-of-four must stay uncompressed");

        // 転送データの行ピッチもBC5は16バイト/ブロックです。
        const auto prepared =
            LamaPon::TextureLoader::PrepareTextureData(
                LamaPon::TextureLoader::GenerateMipChain(image),
                true,
                Usage::NormalMap);
        Require(
            prepared.format == DXGI_FORMAT_BC5_UNORM,
            "prepared normal map must be BC5");
        Require(
            prepared.levels[0].rowPitch == 16,
            "a 4-wide BC5 level is one block per row");

        // 削減量。BC5は16バイト/ブロック＝4x4画素なので、
        // トップレベルはRGBA8（64バイト/4x4画素）のちょうど1/4です。
        const auto uncompressed =
            LamaPon::TextureLoader::PrepareTextureData(
                LamaPon::TextureLoader::GenerateMipChain(image),
                false,
                Usage::NormalMap);
        Require(
            uncompressed.format == DXGI_FORMAT_R8G8B8A8_UNORM,
            "compression off must stay RGBA8");
        Require(
            prepared.levels[0].bytes.size() * 4
                == uncompressed.levels[0].bytes.size(),
            "the top BC5 level must be exactly a quarter of RGBA8");

        // ミップ列全体で見るときは実用的な大きさで測ります。
        // BCは4x4未満のミップでも1ブロック（BC5なら16バイト）要る
        // ので、末端の 2x2・1x1 が効いて 1/4 には収まりません。
        // 4x4の画像だと末端しか無いので逆に増えます。64x64なら
        // 5488 / 21844 = 約25.1%です。
        const auto sizedNormal = SolidImage(
            64,
            64,
            { 128, 128, 255, 255 });
        const auto sizedCompressed =
            LamaPon::TextureLoader::PrepareTextureData(
                LamaPon::TextureLoader::GenerateMipChain(
                    sizedNormal),
                true,
                Usage::NormalMap);
        const auto sizedUncompressed =
            LamaPon::TextureLoader::PrepareTextureData(
                LamaPon::TextureLoader::GenerateMipChain(
                    sizedNormal),
                false,
                Usage::NormalMap);
        Require(
            sizedCompressed.format == DXGI_FORMAT_BC5_UNORM,
            "the 64x64 normal map must be BC5");
        Require(
            sizedCompressed.TotalBytes() * 10
                < sizedUncompressed.TotalBytes() * 3,
            "a 64x64 BC5 mip chain must be under 30% of RGBA8");

        // 用途が鍵に入っていないと、法線用のBC5を色として引きます。
        const std::array<std::uint8_t, 4> source{ 1, 2, 3, 4 };
        Require(
            LamaPon::TextureCache::ComputeKey(
                source,
                true,
                Usage::Color)
                != LamaPon::TextureCache::ComputeKey(
                    source,
                    true,
                    Usage::NormalMap),
            "the cache key must separate usages");
    }

    void TestPngDecode()
    {
        const std::vector<std::uint8_t> pixels{
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 255, 255,
            255, 255, 255, 128 };
        const auto png = BuildPng(2, 2, pixels);

        // ワーカースレッドから呼べること（COM初期化を内包する
        // こと）も同時に検証します。
        auto decoded = std::async(
            std::launch::async,
            [&png]
            {
                return LamaPon::TextureLoader::
                    DecodeImageBytes(png);
            }).get();
        Require(
            decoded.width == 2 && decoded.height == 2,
            "decoded size must match the PNG header");
        Require(
            decoded.pixels == pixels,
            "decoded RGBA bytes must match the source");
    }

    void TestDeviceTextures()
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>
            context;
        const HRESULT deviceResult = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(),
            nullptr,
            context.ReleaseAndGetAddressOf());
        Require(
            SUCCEEDED(deviceResult),
            "WARP device creation must succeed");

        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "texture-loader";
        std::filesystem::remove_all(root);

        const std::vector<std::uint8_t> opaquePixel{
            200, 64, 32, 255 };
        std::vector<std::uint8_t> opaquePixels;
        for (int pixel = 0; pixel < 64; ++pixel)
        {
            opaquePixels.insert(
                opaquePixels.end(),
                opaquePixel.begin(),
                opaquePixel.end());
        }
        WriteBytes(
            root / "opaque.png",
            BuildPng(8, 8, opaquePixels));

        auto alphaPixels = opaquePixels;
        alphaPixels[3] = static_cast<std::uint8_t>(100);
        WriteBytes(
            root / "alpha.png",
            BuildPng(8, 8, alphaPixels));
        WriteBytes(
            root / "prefetch.png",
            BuildPng(8, 8, opaquePixels));

        LamaPon::AssetManager assets(
            device.Get(),
            context.Get());
        assets.SetAssetRoot(root);
        assets.SetRuntimeTextureCompressionEnabled(true);

        const auto describe =
            [](const LamaPon::TextureAsset& texture)
            {
                Microsoft::WRL::ComPtr<ID3D11Resource>
                    resource;
                texture.view->GetResource(
                    resource.ReleaseAndGetAddressOf());
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    texture2D;
                Require(
                    SUCCEEDED(resource.As(&texture2D)),
                    "texture resource must be a Texture2D");
                D3D11_TEXTURE2D_DESC description{};
                texture2D->GetDesc(&description);
                return description;
            };

        const auto opaqueTexture =
            assets.LoadTexture(L"opaque.png");
        Require(
            opaqueTexture->width == 8
                && opaqueTexture->height == 8,
            "loaded texture must keep its size");
        Require(
            !opaqueTexture->isCube,
            "2D texture must not be flagged as a cube");
        const auto opaqueDescription =
            describe(*opaqueTexture);
        Require(
            opaqueDescription.Format
                == DXGI_FORMAT_BC1_UNORM,
            "opaque PNG must compress to BC1");
        Require(
            opaqueDescription.MipLevels == 4,
            "8x8 must upload a full 4-level mip chain");

        const auto alphaTexture =
            assets.LoadTexture(L"alpha.png");
        Require(
            describe(*alphaTexture).Format
                == DXGI_FORMAT_BC3_UNORM,
            "transparent PNG must compress to BC3");

        // プリフェッチでワーカースレッド側からGPUテクスチャ
        // まで作成されることを確認します。
        const auto report = std::async(
            std::launch::async,
            [&assets]
            {
                return assets.PrefetchFiles(
                    { L"prefetch.png" });
            }).get();
        Require(
            report.loadedFiles == 1
                && report.failedFiles == 0,
            "prefetch must load the file bytes");
        Require(
            report.preparedTextures == 1,
            "prefetch must also create the GPU texture");
        Require(
            assets.CachedTextureCount() == 3,
            "prefetched texture must be in the cache");

        // ディスクキャッシュ経由（2回目の読み込み）でも同じ形の
        // テクスチャになること。保存の下限（64KB）がある
        // ので、BC1で確実に超える512x512で確かめます
        // （256x256のBC1はミップ込み約43KBで対象外）。
        {
            std::vector<std::uint8_t> gradient;
            gradient.reserve(512 * 512 * 4);
            for (std::uint32_t y = 0; y < 512; ++y)
            {
                for (std::uint32_t x = 0; x < 512; ++x)
                {
                    gradient.push_back(
                        static_cast<std::uint8_t>(x));
                    gradient.push_back(
                        static_cast<std::uint8_t>(y));
                    gradient.push_back(90);
                    gradient.push_back(255);
                }
            }
            WriteBytes(
                root / "cached.png",
                BuildPng(512, 512, gradient));
            // 読み込み前のキャッシュのファイル数。後で「1つ
            // 増えた」ことを見ます（is_emptyでは、他のテストが
            // 書いたエントリと区別できません）。
            const auto cacheEntryCount = []
            {
                std::size_t count = 0;
                std::error_code error;
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        LamaPon::TextureCache::
                            CacheDirectory(),
                        error))
                {
                    static_cast<void>(entry);
                    ++count;
                }
                return count;
            };
            const auto beforeCount = cacheEntryCount();
            const auto cold =
                assets.LoadTexture(L"cached.png");
            const auto coldDescription = describe(*cold);
            if (coldDescription.Format
                != DXGI_FORMAT_BC1_UNORM)
            {
                // 落ちたときに「実際は何だったのか」が分かる
                // ように出します（推測での切り分けを避ける）。
                std::cout
                    << "diag cached.png: format="
                    << coldDescription.Format
                    << " mips=" << coldDescription.MipLevels
                    << " size=" << coldDescription.Width
                    << "x" << coldDescription.Height
                    << " reportedSize=" << cold->width
                    << "x" << cold->height
                    << " pendingUploads="
                    << assets.PendingTextureUploadCount()
                    << std::endl;
            }
            Require(
                coldDescription.Format
                    == DXGI_FORMAT_BC1_UNORM,
                "the 512x512 gradient must compress to BC1");
            Require(
                cacheEntryCount() == beforeCount + 1,
                "the cold load must write one cache entry");
            assets.Clear();
            const auto warm =
                assets.LoadTexture(L"cached.png");
            const auto warmDescription = describe(*warm);
            Require(
                warmDescription.Format
                        == DXGI_FORMAT_BC1_UNORM
                    && warmDescription.MipLevels == 10
                    && warm->width == 512
                    && warm->height == 512,
                "a disk-cache hit must produce the same"
                " texture");

            // 圧縮を切って同じファイルを読むと非圧縮RGBA8になる
            // こと。これはディスクキャッシュの鍵の守りも兼ねて
            // います（鍵に圧縮設定が入っていないと、上で書かれた
            // BC1のエントリが返ってきてここで落ちます）。
            assets.SetRuntimeTextureCompressionEnabled(
                false);
            assets.Clear();
            const auto uncompressedLarge =
                assets.LoadTexture(L"cached.png");
            Require(
                describe(*uncompressedLarge).Format
                    == DXGI_FORMAT_R8G8B8A8_UNORM,
                "compression off must bypass the BC1 cache"
                " entry");
            assets.SetRuntimeTextureCompressionEnabled(
                true);
        }

        // 圧縮を切れば非圧縮RGBA8のまま読み込まれます。
        assets.SetRuntimeTextureCompressionEnabled(false);
        assets.Clear();
        const auto uncompressed =
            assets.LoadTexture(L"opaque.png");
        Require(
            describe(*uncompressed).Format
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "compression toggle off must keep RGBA8");
    }

    // テクスチャのディスクキャッシュ（保存→読み込みの往復と、
    // 壊れたファイルの拒否）を検証します。GPUは使いません。
    void TestDiskCache()
    {
        // 256x256のアルファ付きグラデーション。保存の下限
        // （64KB）を超える最小クラスの実サイズです。全ピクセル
        // 同色だとBCの端点が縮退して、バイト比較が「たまたま
        // 一致」で通ってしまうため、変化のある絵にします。
        LamaPon::TextureLoader::CpuImage image;
        image.width = 256;
        image.height = 256;
        image.pixels.reserve(256 * 256 * 4);
        for (std::uint32_t y = 0; y < 256; ++y)
        {
            for (std::uint32_t x = 0; x < 256; ++x)
            {
                image.pixels.push_back(
                    static_cast<std::uint8_t>(x));
                image.pixels.push_back(
                    static_cast<std::uint8_t>(y));
                image.pixels.push_back(
                    static_cast<std::uint8_t>(x ^ y));
                image.pixels.push_back(
                    static_cast<std::uint8_t>(
                        128 + (x % 100)));
            }
        }
        const std::vector<std::uint8_t> sourceBytes =
            BuildPng(256, 256, image.pixels);

        // 鍵の性質: 同じ入力なら同じ鍵、圧縮設定か内容が違えば
        // 別の鍵。ここが崩れると「設定を変えたのに前の結果が
        // 返る」という一番嫌な壊れ方をします。
        const auto keyCompressed =
            LamaPon::TextureCache::ComputeKey(
                sourceBytes,
                true);
        Require(
            keyCompressed
                == LamaPon::TextureCache::ComputeKey(
                    sourceBytes,
                    true),
            "the cache key must be deterministic");
        Require(
            keyCompressed
                != LamaPon::TextureCache::ComputeKey(
                    sourceBytes,
                    false),
            "the compression flag must change the key");
        auto changedBytes = sourceBytes;
        changedBytes[changedBytes.size() / 2] ^= 0xff;
        Require(
            keyCompressed
                != LamaPon::TextureCache::ComputeKey(
                    changedBytes,
                    true),
            "changed content must change the key");

        // 圧縮あり／なしの両方で、保存→読み込みがバイト単位で
        // 一致すること。
        for (const bool compress : { true, false })
        {
            auto mips =
                LamaPon::TextureLoader::GenerateMipChain(
                    image);
            LamaPon::TextureCache::CachedTexture entry;
            std::copy_n(
                mips.back().pixels.begin(),
                entry.placeholderPixel.size(),
                entry.placeholderPixel.begin());
            entry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(mips),
                    compress);

            const auto key =
                LamaPon::TextureCache::ComputeKey(
                    sourceBytes,
                    compress);
            Require(
                !LamaPon::TextureCache::TryLoad(key)
                    .has_value(),
                "a missing entry must be a miss");
            LamaPon::TextureCache::Store(key, entry);
            const auto loaded =
                LamaPon::TextureCache::TryLoad(key);
            Require(
                loaded.has_value(),
                "a stored entry must load back");
            Require(
                loaded->data.format == entry.data.format,
                "the cached format must round-trip");
            Require(
                loaded->placeholderPixel
                    == entry.placeholderPixel,
                "the placeholder pixel must round-trip");
            Require(
                loaded->data.levels.size()
                    == entry.data.levels.size(),
                "the level count must round-trip");
            for (std::size_t index = 0;
                index < entry.data.levels.size();
                ++index)
            {
                const auto& expected =
                    entry.data.levels[index];
                const auto& actual =
                    loaded->data.levels[index];
                Require(
                    actual.width == expected.width
                        && actual.height == expected.height
                        && actual.rowPitch
                            == expected.rowPitch,
                    "the level layout must round-trip");
                Require(
                    actual.bytes == expected.bytes,
                    "the level bytes must round-trip"
                    " exactly");
            }
        }

        // 壊れたファイルは黙って拒否されること（＝普通の
        // 作り直しへ落ちる）。壊し方は「途中で切れた」と
        // 「末尾にゴミ」の2通り。どちらも実際に起きます
        // （書き込み中の電源断、別プロセスの追記事故）。
        const auto key =
            LamaPon::TextureCache::ComputeKey(
                sourceBytes,
                true);
        const auto path =
            LamaPon::TextureCache::CacheDirectory()
            / (([&key]
                {
                    wchar_t name[32]{};
                    swprintf_s(
                        name,
                        L"%016llx.ttex",
                        key);
                    return std::wstring(name);
                })());
        std::vector<std::uint8_t> fileBytes;
        {
            std::ifstream input(path, std::ios::binary);
            Require(
                static_cast<bool>(input),
                "the cache file must exist on disk");
            fileBytes.assign(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        }
        const auto writeBytes =
            [&path](const std::vector<std::uint8_t>& bytes)
        {
            std::ofstream output(
                path,
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(
                    bytes.data()),
                static_cast<std::streamsize>(
                    bytes.size()));
        };
        auto truncated = fileBytes;
        truncated.resize(truncated.size() / 2);
        writeBytes(truncated);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "a truncated cache file must be rejected");
        auto trailing = fileBytes;
        trailing.push_back(0);
        writeBytes(trailing);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "trailing garbage must be rejected");
        auto badMagic = fileBytes;
        badMagic[0] ^= 0xff;
        writeBytes(badMagic);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "a wrong magic must be rejected");
        // 元へ戻せばまた読めること（検査が厳しすぎて正常な
        // ファイルまで弾いていないことの確認）。
        writeBytes(fileBytes);
        Require(
            LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "the intact file must load again");

        // 小さすぎる結果は保存されないこと。作り直すほうが
        // ファイルを開くより速いので、キャッシュの対象外です。
        {
            LamaPon::TextureLoader::CpuImage tiny;
            tiny.width = 8;
            tiny.height = 8;
            for (int pixel = 0; pixel < 64; ++pixel)
            {
                tiny.pixels.push_back(
                    static_cast<std::uint8_t>(pixel));
                tiny.pixels.push_back(10);
                tiny.pixels.push_back(20);
                tiny.pixels.push_back(255);
            }
            const auto tinyPng = BuildPng(8, 8, tiny.pixels);
            auto tinyMips =
                LamaPon::TextureLoader::GenerateMipChain(
                    tiny);
            LamaPon::TextureCache::CachedTexture tinyEntry;
            std::copy_n(
                tinyMips.back().pixels.begin(),
                tinyEntry.placeholderPixel.size(),
                tinyEntry.placeholderPixel.begin());
            tinyEntry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(tinyMips),
                    true);
            const auto tinyKey =
                LamaPon::TextureCache::ComputeKey(
                    tinyPng,
                    true);
            LamaPon::TextureCache::Store(tinyKey, tinyEntry);
            Require(
                !LamaPon::TextureCache::TryLoad(tinyKey)
                    .has_value(),
                "tiny textures must not be stored");
        }

        // 実測（診断出力のみ、判定はしません。WARPのVMでは
        // 時間の主張ができないためです）。1024x1024のノイズ画像で
        // 「キャッシュが省く仕事」と「キャッシュ自体のコスト」を
        // 並べます。
        {
            LamaPon::TextureLoader::CpuImage large;
            large.width = 1024;
            large.height = 1024;
            large.pixels.reserve(1024 * 1024 * 4);
            // 乱数ではなく決定的なLCG。毎回同じ絵でないと
            // 鍵が変わってキャッシュの意味が測れません。
            std::uint32_t state = 12345;
            for (std::size_t index = 0;
                index < 1024 * 1024 * 4;
                ++index)
            {
                state = state * 1664525u + 1013904223u;
                large.pixels.push_back(
                    static_cast<std::uint8_t>(state >> 24));
            }
            const auto largePng =
                BuildPng(1024, 1024, large.pixels);

            const auto start =
                std::chrono::steady_clock::now();
            auto mips =
                LamaPon::TextureLoader::GenerateMipChain(
                    LamaPon::TextureLoader::DecodeImageBytes(
                        largePng));
            LamaPon::TextureCache::CachedTexture entry;
            std::copy_n(
                mips.back().pixels.begin(),
                entry.placeholderPixel.size(),
                entry.placeholderPixel.begin());
            entry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(mips),
                    true);
            const auto prepared =
                std::chrono::steady_clock::now();
            const auto largeKey =
                LamaPon::TextureCache::ComputeKey(
                    largePng,
                    true);
            LamaPon::TextureCache::Store(largeKey, entry);
            const auto stored =
                std::chrono::steady_clock::now();
            const auto reloaded =
                LamaPon::TextureCache::TryLoad(largeKey);
            const auto loaded =
                std::chrono::steady_clock::now();
            Require(
                reloaded.has_value()
                    && reloaded->data.TotalBytes()
                        == entry.data.TotalBytes(),
                "the large entry must round-trip");
            const auto milliseconds =
                [](const auto begin, const auto end)
            {
                return std::chrono::duration_cast<
                    std::chrono::microseconds>(
                        end - begin).count() / 1000.0;
            };
            std::cout
                << "texture cache 1024x1024:"
                << " prepare="
                << milliseconds(start, prepared) << "ms"
                << " store="
                << milliseconds(prepared, stored) << "ms"
                << " load="
                << milliseconds(stored, loaded) << "ms\n";
        }
    }

    // 大きいテクスチャの段階的GPUアップロードを検証します。
    void TestProgressiveUpload()
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>
            context;
        const HRESULT deviceResult = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(),
            nullptr,
            context.ReleaseAndGetAddressOf());
        Require(
            SUCCEEDED(deviceResult),
            "WARP device creation must succeed");

        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "texture-progressive";
        std::filesystem::remove_all(root);

        std::vector<std::uint8_t> pixels;
        pixels.reserve(64 * 64 * 4);
        for (int pixel = 0; pixel < 64 * 64; ++pixel)
        {
            pixels.push_back(180);
            pixels.push_back(40);
            pixels.push_back(20);
            pixels.push_back(255);
        }
        WriteBytes(
            root / "big.png",
            BuildPng(64, 64, pixels));
        WriteBytes(
            root / "big2.png",
            BuildPng(64, 64, pixels));
        WriteBytes(
            root / "small.png",
            BuildPng(64, 64, pixels));

        LamaPon::AssetManager assets(
            device.Get(),
            context.Get());
        assets.SetAssetRoot(root);
        // しきい値1バイト＝必ず段階アップロード経路になります。
        assets.SetProgressiveUploadThreshold(1);

        const auto texture = assets.LoadTexture(L"big.png");
        Require(
            texture->view != nullptr,
            "a placeholder view must exist immediately");
        Require(
            texture->width == 64 && texture->height == 64,
            "the final size must be reported before upload");
        Require(
            assets.PendingTextureUploadCount() == 1,
            "a large texture must enter the upload queue");

        // 1バイト予算でも最低1レベルは進み、SRVが実テクスチャへ
        // 切り替わります（前進保証）。
        const auto placeholderView = texture->view;
        assets.PumpTextureUploads(1);
        Require(
            texture->view != placeholderView,
            "the first pump must swap in the real texture");
        Require(
            assets.PendingTextureUploadCount() == 1,
            "a tiny budget must leave the upload unfinished");

        for (int i = 0;
            i < 64
                && assets.PendingTextureUploadCount() > 0;
            ++i)
        {
            assets.PumpTextureUploads(1u << 30);
        }
        Require(
            assets.PendingTextureUploadCount() == 0,
            "the upload queue must drain");

        // 完了後のSRVは全ミップ（64x64は7レベル）を参照します。
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        texture->view->GetDesc(&viewDescription);
        Require(
            viewDescription.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE2D
                && viewDescription.Texture2D
                    .MostDetailedMip == 0
                && viewDescription.Texture2D.MipLevels
                    == 7,
            "the finished view must expose the full mip chain");

        // キャッシュから消えた保留テクスチャは転送せず破棄します。
        static_cast<void>(assets.LoadTexture(L"big2.png"));
        Require(
            assets.PendingTextureUploadCount() == 1,
            "the second texture must queue");
        assets.Clear();
        assets.PumpTextureUploads();
        Require(
            assets.PendingTextureUploadCount() == 0,
            "cleared textures must be dropped from the queue");

        // しきい値未満は従来どおり一括アップロードされます。
        assets.SetProgressiveUploadThreshold(
            std::numeric_limits<std::size_t>::max());
        static_cast<void>(assets.LoadTexture(L"small.png"));
        Require(
            assets.PendingTextureUploadCount() == 0,
            "small textures must upload immediately");
    }
}

int main()
{
    try
    {
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static_cast<void>(comResult);

        // 全テストを専用のキャッシュ置き場で走らせます。本物の
        // %LOCALAPPDATA%を汚さないためと、前回の実行結果が
        // 残っていて「コールドのつもりがヒット」になるのを
        // 防ぐためです。
        const auto cacheRoot =
            std::filesystem::current_path()
            / "test-output"
            / "texture-cache";
        std::filesystem::remove_all(cacheRoot);
        LamaPon::TextureCache::SetCacheDirectoryOverride(
            cacheRoot);

        TestMipChain();
        TestBlockCompression();
        TestNormalMapCompression();
        TestPngDecode();
        TestDiskCache();
        TestDeviceTextures();
        TestProgressiveUpload();
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "TextureLoader tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "TextureLoader tests passed.\n";
    return 0;
}
