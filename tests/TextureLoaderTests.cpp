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
    // Require(condition: 成否, message: 失敗理由) は不成立時に例外を送出する。
    void Require(
        const bool condition,
        const char* message)
    {
        // 条件を満たさない場合はテストを失敗させる。
        if (!condition)
        {
            // 失敗理由を例外として呼び出し元へ伝える。
            throw std::runtime_error(message);
        }
    }

    // Crc32(data: CRC対象bytes, size: byte数) はPNG CRC-32を計算する。
    [[nodiscard]] std::uint32_t Crc32(
        const std::uint8_t* data,
        const std::size_t size) noexcept
    {
        // CRC accumulator
        std::uint32_t crc = 0xffffffffu;
        // index: byte
        for (std::size_t index = 0;
            index < size;
            ++index)
        {
            crc ^= data[index];
            // bit: position
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1)
                    ^ (0xedb88320u & (~(crc & 1u) + 1u));
            }
        }
        // CRC checksumを返す。
        return crc ^ 0xffffffffu;
    }

    // Adler32(data: zlib入力) はAdler-32 checksumを計算する。
    [[nodiscard]] std::uint32_t Adler32(
        const std::vector<std::uint8_t>& data) noexcept
    {
        // Adler low sum
        std::uint32_t a = 1;
        // Adler high sum
        std::uint32_t b = 0;
        // value: 入力byte
        for (const auto value : data)
        {
            a = (a + value) % 65521u;
            b = (b + a) % 65521u;
        }
        // Adler checksumを返す。
        return (b << 16) | a;
    }

    // AppendBigEndian(output: 出力バイト列, value: 32bit値) は整数をbig-endianで追加する。
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

    // AppendChunk(output: PNG bytes, type: 4文字種別, payload: chunk内容) はCRC付きchunkを追加する。
    void AppendChunk(
        std::vector<std::uint8_t>& output,
        const char* type,
        const std::vector<std::uint8_t>& payload)
    {
        AppendBigEndian(
            output,
            static_cast<std::uint32_t>(payload.size()));
        // chunk bytes
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

    // BuildPng(width: pixel幅, height: pixel高, rgbaPixels: RGBA8列) はWIC用PNG fixtureを作る。
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

        // PNG output bytes
        std::vector<std::uint8_t> png{
            0x89, 0x50, 0x4e, 0x47,
            0x0d, 0x0a, 0x1a, 0x0a };

        // IHDR chunk data
        std::vector<std::uint8_t> header;
        AppendBigEndian(header, width);
        AppendBigEndian(header, height);
        // ビット深度
        header.push_back(8);
        // カラータイプ: RGBA
        header.push_back(6);
        // 圧縮方式
        header.push_back(0);
        // フィルター方式
        header.push_back(0);
        // 非インターレース
        header.push_back(0);
        AppendChunk(png, "IHDR", header);

        // scanline bytes
        std::vector<std::uint8_t> raw;
        // y: scanline index
        for (std::uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            // filtered RGBA row
            const auto* row =
                rgbaPixels.data()
                + static_cast<std::size_t>(y)
                    * width * 4;
            raw.insert(raw.end(), row, row + width * 4);
        }

        // zlib IDAT stream
        std::vector<std::uint8_t> idat{ 0x78, 0x01 };
        // block offset
        std::size_t offset = 0;
        // Append 65535-byte stored blocks until the input is consumed.
        do
        {
            // bytes left
            const std::size_t remaining =
                raw.size() - offset;
            // block size
            const auto blockLength =
                static_cast<std::uint16_t>(
                    remaining < 65535 ? remaining : 65535);
            // last-block flag
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
        // 生成したPNG bytesを返す。
        return png;
    }

    // WriteBytes(path: 出力先, bytes: 内容) はテスト用バイト列をファイルへ保存する。
    void WriteBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // output stream
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

    // SolidImage(width: 画像幅, height: 画像高, color: RGBA値) は単色画像を生成する。
    [[nodiscard]] LamaPon::TextureLoader::CpuImage
        SolidImage(
            const std::uint32_t width,
            const std::uint32_t height,
            const std::array<std::uint8_t, 4>& color)
    {
        // decoded RGBA image
        LamaPon::TextureLoader::CpuImage image;
        image.width = width;
        image.height = height;
        image.pixels.resize(
            static_cast<std::size_t>(width)
            * height * 4);
        // pixel: RGBA offset
        for (std::size_t pixel = 0;
            pixel < image.pixels.size();
            pixel += 4)
        {
            image.pixels[pixel] = color[0];
            image.pixels[pixel + 1] = color[1];
            image.pixels[pixel + 2] = color[2];
            image.pixels[pixel + 3] = color[3];
        }
        // Return the completed solid-color image.
        return image;
    }

    // TestMipChain() はmip寸法とbox平均色を検証する。
    void TestMipChain()
    {
        // base image
        auto base = SolidImage(8, 4, { 10, 20, 30, 255 });
        // generated mip levels
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

        // 2x2 source image
        LamaPon::TextureLoader::CpuImage quad;
        quad.width = 2;
        quad.height = 2;
        quad.pixels = {
            0, 0, 0, 255,
            255, 255, 255, 255,
            100, 50, 200, 255,
            60, 150, 20, 255 };
        // averaged mip levels
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

    // TestBlockCompression() はBC1とBC3の端点およびindexを検証する。
    void TestBlockCompression()
    {
        // opaque source image
        const auto opaque =
            SolidImage(4, 4, { 200, 64, 32, 255 });
        Require(
            !LamaPon::TextureLoader::HasTransparentPixels(
                opaque),
            "opaque image must not report transparency");

        // BC1 compressed block
        const auto bc1 =
            LamaPon::TextureLoader::CompressBC1(opaque);
        Require(
            bc1.size() == 8,
            "one BC1 block is 8 bytes");
        // RGB565 endpoint 0
        std::uint16_t color0{};
        // RGB565 endpoint 1
        std::uint16_t color1{};
        std::memcpy(&color0, bc1.data(), 2);
        std::memcpy(&color1, bc1.data() + 2, 2);
        // expected RGB565
        const std::uint16_t expected565 =
            static_cast<std::uint16_t>(
                ((200 >> 3) << 11)
                | ((64 >> 2) << 5)
                | (32 >> 3));
        Require(
            color0 == expected565
                && color1 == expected565,
            "solid block endpoints must equal the color");
        // BC1 pixel selectors
        std::uint32_t indices{};
        std::memcpy(&indices, bc1.data() + 4, 4);
        Require(
            indices == 0,
            "solid block must select endpoint 0 everywhere");

        // BC3 alpha fixture
        auto alphaImage =
            SolidImage(4, 4, { 128, 128, 128, 255 });
        // row alpha values
        const std::array<std::uint8_t, 4> rowAlpha{
            255, 128, 64, 0 };
        // y: BC3 block row
        for (std::uint32_t y = 0; y < 4; ++y)
        {
            // x: BC3 block column
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
        // BC3 compressed block
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

    // TestNormalMapCompression() はBC5のchannel配置、format選択、容量比を検証する。
    void TestNormalMapCompression()
    {
        // BC5 source image
        auto image = SolidImage(4, 4, { 0, 90, 255, 255 });
        // row red values
        const std::array<std::uint8_t, 4> rowRed{
            255, 170, 85, 0 };
        // y: BC5 block row
        for (std::uint32_t y = 0; y < 4; ++y)
        {
            // x: BC5 block column
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                image.pixels[
                    (static_cast<std::size_t>(y) * 4 + x)
                        * 4] = rowRed[y];
            }
        }

        // BC5 compressed block
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

        // format mips
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
            "opaque color must choose BC1");
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                mips,
                false,
                Usage::NormalMap)
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "compression off must stay uncompressed for every usage");

        // odd-size image
        std::vector<LamaPon::TextureLoader::CpuImage> odd;
        odd.push_back(SolidImage(6, 4, { 10, 20, 30, 255 }));
        Require(
            LamaPon::TextureLoader::ChooseTextureFormat(
                odd,
                true,
                Usage::NormalMap)
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "non multiple-of-four must stay uncompressed");

        // BC5 upload data
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

        // RGBA8 mip data
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

        // 64x64 BC5 fixture
        const auto sizedNormal = SolidImage(
            64,
            64,
            { 128, 128, 255, 255 });
        // compressed mip data
        const auto sizedCompressed =
            LamaPon::TextureLoader::PrepareTextureData(
                LamaPon::TextureLoader::GenerateMipChain(
                    sizedNormal),
                true,
                Usage::NormalMap);
        // RGBA8 mip data
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

        // cache-key bytes
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

    // TestPngDecode() はworker thread上のRGBA decodeを検証する。
    void TestPngDecode()
    {
        // PNG source pixels
        const std::vector<std::uint8_t> pixels{
            255, 0, 0, 255,
            0, 255, 0, 255,
            0, 0, 255, 255,
            255, 255, 255, 128 };
        // encoded PNG bytes
        const auto png = BuildPng(2, 2, pixels);

        // async decode result
        auto decoded = std::async(
            std::launch::async,
            [&png]
            {
                // Return the decoded PNG image.
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

    // TestDeviceTextures() はWARP上の形式、snapshot、prefetch、disk cacheを検証する。
    void TestDeviceTextures()
    {
        // WARP graphics device
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>
            context;
        // WARP HRESULT
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

        // asset directory
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "texture-loader";
        std::filesystem::remove_all(root);

        // opaque RGBA pixel
        const std::vector<std::uint8_t> opaquePixel{
            200, 64, 32, 255 };
        // opaque image bytes
        std::vector<std::uint8_t> opaquePixels;
        // pixel: image index
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

        // alpha image bytes
        auto alphaPixels = opaquePixels;
        alphaPixels[3] = static_cast<std::uint8_t>(100);
        WriteBytes(
            root / "alpha.png",
            BuildPng(8, 8, alphaPixels));
        WriteBytes(
            root / "prefetch.png",
            BuildPng(8, 8, opaquePixels));

        // fixture AssetManager
        LamaPon::AssetManager assets(
            device.Get(),
            context.Get());
        assets.SetAssetRoot(root);
        assets.SetRuntimeTextureCompressionEnabled(true);

        // describe(texture: asset) returns its D3D11 descriptor.
        const auto describe =
            [](const LamaPon::TextureAsset& texture)
            {
                // resource snapshot
                const auto resources =
                    texture.resources.Acquire();
                Require(
                    resources != nullptr
                        && resources->d3d11ShaderResourceView
                            != nullptr,
                    "texture resource snapshot must contain a view");
                Microsoft::WRL::ComPtr<ID3D11Resource>
                    resource;
                resources->d3d11ShaderResourceView->GetResource(
                    resource.ReleaseAndGetAddressOf());
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    texture2D;
                Require(
                    SUCCEEDED(resource.As(&texture2D)),
                    "texture resource must be a Texture2D");
                // texture description
                D3D11_TEXTURE2D_DESC description{};
                texture2D->GetDesc(&description);
                // texture descriptionを返す。
                return description;
            };

        // opaque texture
        const auto opaqueTexture =
            assets.LoadTexture(L"opaque.png");
        Require(
            opaqueTexture->width == 8
                && opaqueTexture->height == 8,
            "loaded texture must keep its size");
        Require(
            !opaqueTexture->isCube,
            "2D texture must not be flagged as a cube");
        // opaque descriptor
        const auto opaqueDescription =
            describe(*opaqueTexture);
        Require(
            opaqueDescription.Format
                == DXGI_FORMAT_BC1_UNORM,
            "opaque PNG must compress to BC1");
        Require(
            opaqueDescription.MipLevels == 4,
            "8x8 must upload a full 4-level mip chain");

        // alpha texture
        const auto alphaTexture =
            assets.LoadTexture(L"alpha.png");
        Require(
            describe(*alphaTexture).Format
                == DXGI_FORMAT_BC3_UNORM,
            "transparent PNG must compress to BC3");

        // shared resource slot
        LamaPon::TextureResourceBinding binding;
        // opaque snapshot
        auto opaqueResources =
            opaqueTexture->resources.Acquire();
        // alpha snapshot
        const auto alphaResources =
            alphaTexture->resources.Acquire();
        Require(
            opaqueResources != nullptr
                && alphaResources != nullptr
                && opaqueResources->d3d11ShaderResourceView
                    != nullptr
                && alphaResources->d3d11ShaderResourceView
                    != nullptr,
            "loaded textures must publish resource snapshots");
        binding.Publish(*opaqueResources);
        // copied binding
        auto copiedBinding = binding;
        // held snapshot
        auto heldSnapshot = binding.Acquire();
        Require(
            heldSnapshot != nullptr
                && heldSnapshot
                    ->d3d11ShaderResourceView != nullptr,
            "the binding must expose its published snapshot");
        // held placeholder SRV
        auto* const heldView =
            heldSnapshot->d3d11ShaderResourceView.Get();
        copiedBinding.Publish(*alphaResources);
        // copied-slot snapshot
        const auto publishedThroughCopy = binding.Acquire();
        Require(
            publishedThroughCopy != nullptr
                && publishedThroughCopy
                    ->d3d11ShaderResourceView.Get()
                    == alphaResources
                        ->d3d11ShaderResourceView.Get(),
            "a copied binding must publish through the shared slot");
        Require(
            heldSnapshot != nullptr
                && heldSnapshot
                    ->d3d11ShaderResourceView.Get()
                    == heldView
                && heldView
                    == opaqueResources
                        ->d3d11ShaderResourceView.Get(),
            "a previously acquired snapshot must survive a later publish");
        heldSnapshot.reset();
        opaqueResources.reset();

        // prefetch result
        const auto report = std::async(
            std::launch::async,
            [&assets]
            {
                // Return the asynchronous prefetch report.
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

        {
            // gradient bytes
            std::vector<std::uint8_t> gradient;
            gradient.reserve(512 * 512 * 4);
            // y: 512px row
            for (std::uint32_t y = 0; y < 512; ++y)
            {
                // x: 512px column
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
            // cacheEntryCount() counts files so the test can verify one new cache write.
            const auto cacheEntryCount = []
            {
                // cache-entry count
                std::size_t count = 0;
                // directory error
                std::error_code error;
                // entry: cache file
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        LamaPon::TextureCache::
                            CacheDirectory(),
                        error))
                {
                    static_cast<void>(entry);
                    ++count;
                }
                // Return the number of cache files.
                return count;
            };
            // initial cache count
            const auto beforeCount = cacheEntryCount();
            // cold-cache texture
            const auto cold =
                assets.LoadTexture(L"cached.png");
            // cold texture format
            const auto coldDescription = describe(*cold);
            // Emit texture metadata only when BC1 is missing.
            if (coldDescription.Format
                != DXGI_FORMAT_BC1_UNORM)
            {
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
            // warm-cache texture
            const auto warm =
                assets.LoadTexture(L"cached.png");
            // warm texture format
            const auto warmDescription = describe(*warm);
            Require(
                warmDescription.Format
                        == DXGI_FORMAT_BC1_UNORM
                    && warmDescription.MipLevels == 10
                    && warm->width == 512
                    && warm->height == 512,
                "a disk-cache hit must produce the same"
                " texture");

            assets.SetRuntimeTextureCompressionEnabled(
                false);
            assets.Clear();
            // large RGBA texture
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

        assets.SetRuntimeTextureCompressionEnabled(false);
        assets.Clear();
        // RGBA texture
        const auto uncompressed =
            assets.LoadTexture(L"opaque.png");
        Require(
            describe(*uncompressed).Format
                == DXGI_FORMAT_R8G8B8A8_UNORM,
            "compression toggle off must keep RGBA8");
    }

    // TestDiskCache() はcache key、往復、破損拒否、size下限を検証する。
    void TestDiskCache()
    {
        // cache fixture image
        LamaPon::TextureLoader::CpuImage image;
        image.width = 256;
        image.height = 256;
        image.pixels.reserve(256 * 256 * 4);
        // y: source row
        for (std::uint32_t y = 0; y < 256; ++y)
        {
            // x: source column
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
        // source PNG bytes
        const std::vector<std::uint8_t> sourceBytes =
            BuildPng(256, 256, image.pixels);

        // compressed cache key
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
        // modified PNG bytes
        auto changedBytes = sourceBytes;
        changedBytes[changedBytes.size() / 2] ^= 0xff;
        Require(
            keyCompressed
                != LamaPon::TextureCache::ComputeKey(
                    changedBytes,
                    true),
            "changed content must change the key");

        // compress: BC flag
        for (const bool compress : { true, false })
        {
            // generated mip levels
            auto mips =
                LamaPon::TextureLoader::GenerateMipChain(
                    image);
            // cache entry
            LamaPon::TextureCache::CachedTexture entry;
            std::copy_n(
                mips.back().pixels.begin(),
                entry.placeholderPixel.size(),
                entry.placeholderPixel.begin());
            entry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(mips),
                    compress);

            // per-mode cache key
            const auto key =
                LamaPon::TextureCache::ComputeKey(
                    sourceBytes,
                    compress);
            Require(
                !LamaPon::TextureCache::TryLoad(key)
                    .has_value(),
                "a missing entry must be a miss");
            LamaPon::TextureCache::Store(key, entry);
            // loaded cache entry
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
            // index: mip
            for (std::size_t index = 0;
                index < entry.data.levels.size();
                ++index)
            {
                // expected mip layout
                const auto& expected =
                    entry.data.levels[index];
                // loaded mip layout
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

        // per-file cache key
        const auto key =
            LamaPon::TextureCache::ComputeKey(
                sourceBytes,
                true);
        // cache file path
        const auto path =
            LamaPon::TextureCache::CacheDirectory()
            / (([&key]
                {
                    // name: cache filename
                    wchar_t name[32]{};
                    swprintf_s(
                        name,
                        L"%016llx.ttex",
                        key);
                    // Return the wide cache filename.
                    return std::wstring(name);
                })());
        // cached file bytes
        std::vector<std::uint8_t> fileBytes;
        {
            // cache input stream
            std::ifstream input(path, std::ios::binary);
            Require(
                static_cast<bool>(input),
                "the cache file must exist on disk");
            fileBytes.assign(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
        }
        // writeBytes(bytes: cache content) rewrites the cache file.
        const auto writeBytes =
            [&path](const std::vector<std::uint8_t>& bytes)
        {
            // cache output stream
            std::ofstream output(
                path,
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(
                    bytes.data()),
                static_cast<std::streamsize>(
                    bytes.size()));
        };
        // short cache bytes
        auto truncated = fileBytes;
        truncated.resize(truncated.size() / 2);
        writeBytes(truncated);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "a truncated cache file must be rejected");
        // extra-byte cache
        auto trailing = fileBytes;
        trailing.push_back(0);
        writeBytes(trailing);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "trailing garbage must be rejected");
        // invalid-magic cache
        auto badMagic = fileBytes;
        badMagic[0] ^= 0xff;
        writeBytes(badMagic);
        Require(
            !LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "a wrong magic must be rejected");
        writeBytes(fileBytes);
        Require(
            LamaPon::TextureCache::TryLoad(key)
                .has_value(),
            "the intact file must load again");

        // Tiny fixture remains below the cache size threshold.
        {
            // tiny RGBA image
            LamaPon::TextureLoader::CpuImage tiny;
            tiny.width = 8;
            tiny.height = 8;
            // pixel: tiny index
            for (int pixel = 0; pixel < 64; ++pixel)
            {
                tiny.pixels.push_back(
                    static_cast<std::uint8_t>(pixel));
                tiny.pixels.push_back(10);
                tiny.pixels.push_back(20);
                tiny.pixels.push_back(255);
            }
            // tiny PNG bytes
            const auto tinyPng = BuildPng(8, 8, tiny.pixels);
            // tiny mip levels
            auto tinyMips =
                LamaPon::TextureLoader::GenerateMipChain(
                    tiny);
            // tiny cache entry
            LamaPon::TextureCache::CachedTexture tinyEntry;
            std::copy_n(
                tinyMips.back().pixels.begin(),
                tinyEntry.placeholderPixel.size(),
                tinyEntry.placeholderPixel.begin());
            tinyEntry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(tinyMips),
                    true);
            // tiny cache key
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

        {
            // large RGBA fixture
            LamaPon::TextureLoader::CpuImage large;
            large.width = 1024;
            large.height = 1024;
            large.pixels.reserve(1024 * 1024 * 4);
            // stable LCG seed
            std::uint32_t state = 12345;
            // index: byte
            for (std::size_t index = 0;
                index < 1024 * 1024 * 4;
                ++index)
            {
                state = state * 1664525u + 1013904223u;
                large.pixels.push_back(
                    static_cast<std::uint8_t>(state >> 24));
            }
            // encoded large PNG
            const auto largePng =
                BuildPng(1024, 1024, large.pixels);

            // prepare start time
            const auto start =
                std::chrono::steady_clock::now();
            // large mip chain
            auto mips =
                LamaPon::TextureLoader::GenerateMipChain(
                    LamaPon::TextureLoader::DecodeImageBytes(
                        largePng));
            // large cache entry
            LamaPon::TextureCache::CachedTexture entry;
            std::copy_n(
                mips.back().pixels.begin(),
                entry.placeholderPixel.size(),
                entry.placeholderPixel.begin());
            entry.data =
                LamaPon::TextureLoader::PrepareTextureData(
                    std::move(mips),
                    true);
            // prepare finish time
            const auto prepared =
                std::chrono::steady_clock::now();
            // large cache key
            const auto largeKey =
                LamaPon::TextureCache::ComputeKey(
                    largePng,
                    true);
            LamaPon::TextureCache::Store(largeKey, entry);
            // write finish time
            const auto stored =
                std::chrono::steady_clock::now();
            // reloaded entry
            const auto reloaded =
                LamaPon::TextureCache::TryLoad(largeKey);
            // load finish time
            const auto loaded =
                std::chrono::steady_clock::now();
            Require(
                reloaded.has_value()
                    && reloaded->data.TotalBytes()
                        == entry.data.TotalBytes(),
                "the large entry must round-trip");
            // milliseconds(begin: start, end: finish) returns elapsed milliseconds.
            const auto milliseconds =
                [](const auto begin, const auto end)
            {
                // 経過時間をmillisecondsで返す。
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

    // TestProgressiveUpload() はplaceholder切替、段階upload、全mip公開を検証する。
    void TestProgressiveUpload()
    {
        // WARP graphics device
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>
            context;
        // WARP HRESULT
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

        // upload asset root
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "texture-progressive";
        std::filesystem::remove_all(root);

        // source pixels
        std::vector<std::uint8_t> pixels;
        pixels.reserve(64 * 64 * 4);
        // pixel: source index
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

        // fixture AssetManager
        LamaPon::AssetManager assets(
            device.Get(),
            context.Get());
        assets.SetAssetRoot(root);
        assets.SetProgressiveUploadThreshold(1);

        // large texture
        const auto texture = assets.LoadTexture(L"big.png");
        // placeholder snapshot
        auto placeholderResources =
            texture->resources.Acquire();
        Require(
            placeholderResources != nullptr
                && placeholderResources
                    ->d3d11ShaderResourceView != nullptr,
            "a placeholder view must exist immediately");
        Require(
            texture->width == 64 && texture->height == 64,
            "the final size must be reported before upload");
        Require(
            assets.PendingTextureUploadCount() == 1,
            "a large texture must enter the upload queue");

        // placeholder SRV
        auto* const placeholderView =
            placeholderResources
                // A one-byte budget must still advance at least one mip.
                ->d3d11ShaderResourceView.Get();
        assets.PumpTextureUploads(1);
        // uploaded snapshot
        auto uploadedResources =
            texture->resources.Acquire();
        Require(
            uploadedResources != nullptr
                && uploadedResources
                    ->d3d11ShaderResourceView.Get()
                    != placeholderView,
            "the first pump must swap in the real texture");
        placeholderResources.reset();
        uploadedResources.reset();
        Require(
            assets.PendingTextureUploadCount() == 1,
            "a tiny budget must leave the upload unfinished");

        // i: pump count
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

        // final SRV snapshot
        const auto completedResources =
            texture->resources.Acquire();
        Require(
            completedResources != nullptr
                && completedResources
                    ->d3d11ShaderResourceView != nullptr,
            "the completed upload must publish a texture view");
        // SRV description
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        completedResources->d3d11ShaderResourceView->GetDesc(
            &viewDescription);
        Require(
            viewDescription.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE2D
                && viewDescription.Texture2D
                    .MostDetailedMip == 0
                && viewDescription.Texture2D.MipLevels
                    == 7,
            "the finished view must expose the full mip chain");

        static_cast<void>(assets.LoadTexture(L"big2.png"));
        Require(
            assets.PendingTextureUploadCount() == 1,
            "the second texture must queue");
        assets.Clear();
        assets.PumpTextureUploads();
        Require(
            assets.PendingTextureUploadCount() == 0,
            "cleared textures must be dropped from the queue");

        assets.SetProgressiveUploadThreshold(
            std::numeric_limits<std::size_t>::max());
        static_cast<void>(assets.LoadTexture(L"small.png"));
        Require(
            assets.PendingTextureUploadCount() == 0,
            "small textures must upload immediately");
    }
}

// main() はCOMとcacheを初期化し、texture testsの結果を返す。
int main()
{
    // Catch failures from COM setup and all texture tests.
    try
    {
        // COM status
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static_cast<void>(comResult);

        // test cache root
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
    // error: captured texture-test failure
    catch (const std::exception& error)
    {
        std::cerr
            << "TextureLoader tests failed: "
            << error.what()
            << '\n';
        // Return failure after writing the exception diagnostic.
        return 1;
    }

    std::cout << "TextureLoader tests passed.\n";
    // Return success after all texture tests pass.
    return 0;
}
