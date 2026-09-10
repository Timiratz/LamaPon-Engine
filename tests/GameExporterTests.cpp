#include "LamaPon/Assets/AssetArchive.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/GltfImporter.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Editor/ExeIconTool.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Resources/WindowsResource.h"

#include <Windows.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // このテスト実行ファイル自身のパス（本物のPEとして使います）。
    std::filesystem::path SelfExecutablePath()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer,
            MAX_PATH);
        Require(
            length > 0 && length < MAX_PATH,
            "Could not resolve the test executable path.");
        return std::filesystem::path(buffer);
    }

    // 実行ファイルへ埋め込まれたアイコングループを検証します。
    void RequireEmbeddedIcon(
        const std::filesystem::path& executablePath,
        const std::uint16_t expectedImageCount)
    {
        const HMODULE module = LoadLibraryExW(
            executablePath.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(
            module != nullptr,
            "Exported executable could not be inspected.");
        const HRSRC group = FindResourceW(
            module,
            MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
            RT_GROUP_ICON);
        std::uint16_t imageCount = 0;
        if (group != nullptr)
        {
            const HGLOBAL loaded =
                LoadResource(module, group);
            const auto* data = loaded != nullptr
                ? static_cast<const unsigned char*>(
                    LockResource(loaded))
                : nullptr;
            if (data != nullptr
                && SizeofResource(module, group) >= 6)
            {
                std::memcpy(
                    &imageCount,
                    data + 4,
                    sizeof(imageCount));
            }
        }
        const bool firstIconPresent = FindResourceW(
            module,
            MAKEINTRESOURCEW(1),
            RT_ICON) != nullptr;
        FreeLibrary(module);
        Require(
            group != nullptr
                && imageCount == expectedImageCount
                && firstIconPresent,
            "Icon group was not embedded correctly.");
    }

    // 事前コンパイル済みシェーダーを除いた同梱ファイル数。
    // shader-cacheの中身は入口の一覧が増えれば変わるので、
    // 「想定どおりのランタイム一式が入っているか」を見る側では
    // 数えません（そちらは別途、空でないことだけ確かめます）。
    std::size_t CountExportedFilesExcludingShaderCache(
        const std::filesystem::path& outputDirectory)
    {
        std::size_t count{};
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                outputDirectory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            if (entry.path().parent_path().filename()
                == L"shader-cache"
                || entry.path().parent_path().filename() == L"licenses"
                || entry.path().filename() == L"THIRD_PARTY_NOTICES.md")
            {
                continue;
            }
            ++count;
        }
        return count;
    }

    std::vector<std::uint8_t> ReadBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        Require(
            static_cast<bool>(input),
            "Could not open a file for reading.");
        const auto end = input.tellg();
        Require(
            end >= 0,
            "Could not determine the size of a file.");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        return bytes;
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
            "Could not create a test file.");
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    // 既存のrigged GLBへ同じmeshのskin無しnodeを足し、1ファイル内で
    // Forward／Skinned primitiveが混在するexport検証用モデルにします。
    // JSON chunkの長さは変えず、元からコピー済みのテストファイルだけを
    // 上書きするため、追加fixtureは要りません。
    void AddUnskinnedInstanceToGlb(
        const std::filesystem::path& path)
    {
        constexpr std::uint32_t GlbMagic = 0x46546c67;
        constexpr std::uint32_t GlbVersion = 2;
        constexpr std::uint32_t JsonChunkType = 0x4e4f534a;
        constexpr std::size_t JsonChunkOffset = 20;
        auto bytes = ReadBytes(path);
        Require(
            bytes.size() >= JsonChunkOffset,
            "The mixed-role GLB fixture header is truncated.");

        std::uint32_t magic{};
        std::uint32_t version{};
        std::uint32_t jsonLength{};
        std::uint32_t chunkType{};
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        std::memcpy(
            &version,
            bytes.data() + sizeof(std::uint32_t),
            sizeof(version));
        std::memcpy(
            &jsonLength,
            bytes.data() + 12,
            sizeof(jsonLength));
        std::memcpy(
            &chunkType,
            bytes.data() + 16,
            sizeof(chunkType));
        Require(
            magic == GlbMagic
                && version == GlbVersion
                && chunkType == JsonChunkType
                && jsonLength <= bytes.size() - JsonChunkOffset,
            "The mixed-role GLB fixture has an invalid header.");

        auto document = nlohmann::json::parse(
            bytes.begin() + JsonChunkOffset,
            bytes.begin() + JsonChunkOffset + jsonLength);
        auto& nodes = document.at("nodes");
        const auto skinned = std::ranges::find_if(
            nodes,
            [](const nlohmann::json& node)
            {
                return node.contains("mesh")
                    && node.contains("skin");
            });
        Require(
            skinned != nodes.end(),
            "The mixed-role GLB fixture has no skinned mesh node.");
        const auto mesh = skinned->at("mesh");
        const auto nodeIndex = nodes.size();
        nodes.push_back(nlohmann::json{ { "mesh", mesh } });

        const auto sceneIndex = document.value("scene", 0u);
        auto& sceneNodes = document.at("scenes")
            .at(sceneIndex)
            .at("nodes");
        sceneNodes.push_back(nodeIndex);

        // 追加nodeぶんを既存JSON chunkへ収めるため、描画判定に影響しない
        // 表示名とgenerator文字列だけを落とします。
        for (auto& node : nodes)
        {
            node.erase("name");
        }
        document.at("asset").erase("generator");
        const auto json = document.dump();
        Require(
            json.size() <= jsonLength,
            "The mixed-role GLB JSON no longer fits its existing chunk.");
        std::fill(
            bytes.begin() + JsonChunkOffset,
            bytes.begin() + JsonChunkOffset + jsonLength,
            static_cast<std::uint8_t>(' '));
        std::copy(
            json.begin(),
            json.end(),
            bytes.begin() + JsonChunkOffset);
        WriteBytes(path, bytes);
    }

    // 偽のLamaPonRuntime.dll。本物と同じように鍵スロットを1つだけ
    // 持たせます。書き出しはこのスロットを、そのゲームだけの鍵で
    // 書き換えます（スロットが無ければ書き出しは失敗するのが正しい
    // 挙動なので、テストの偽物にも必ず入れます）。
    constexpr std::size_t FakeRuntimeKeySlotOffset = 64;

    void WriteFakeRuntimeLibrary(
        const std::filesystem::path& path)
    {
        std::vector<std::uint8_t> bytes(
            FakeRuntimeKeySlotOffset,
            0x2a);
        // 目印さえ入っていれば書き換えられます（続く64バイトは
        // 書き出しで丸ごと上書きされるので、中身は何でも構いません）。
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        bytes.insert(bytes.end(), marker.begin(), marker.end());
        bytes.insert(
            bytes.end(),
            LamaPon::Crypto::KeySlotSize - marker.size(),
            0x71);
        bytes.insert(bytes.end(), 32, 0x5c);
        WriteBytes(path, bytes);
    }

    // 書き出したDLLへ焼き込まれた鍵を、スロットの並びから戻します。
    LamaPon::Crypto::AesKey ReadEmbeddedArchiveKey(
        const std::filesystem::path& runtimeLibrary)
    {
        const auto bytes = ReadBytes(runtimeLibrary);
        Require(
            bytes.size()
                >= FakeRuntimeKeySlotOffset
                    + LamaPon::Crypto::KeySlotSize,
            "Exported runtime is too small to hold a key slot.");
        LamaPon::Crypto::AesKey key{};
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            const auto pad = bytes[
                FakeRuntimeKeySlotOffset
                + LamaPon::Crypto::KeySlotMarkerSize
                + index];
            const auto stored = bytes[
                FakeRuntimeKeySlotOffset
                + LamaPon::Crypto::KeySlotMarkerSize
                + LamaPon::Crypto::AesKeySize
                + index];
            key[index] = static_cast<std::uint8_t>(stored ^ pad);
        }
        return key;
    }

    // 封筒（暗号化）済みなら開いてから、平文ならそのまま文字列で返します。
    std::string ReadSealedText(
        const std::filesystem::path& path,
        const LamaPon::Crypto::AesKey& key)
    {
        const auto bytes = ReadBytes(path);
        if (!LamaPon::Crypto::IsSealed(
                bytes.data(),
                bytes.size()))
        {
            return std::string(bytes.begin(), bytes.end());
        }
        const auto plain = LamaPon::Crypto::Unseal(
            bytes.data(),
            bytes.size(),
            key);
        Require(
            plain.has_value(),
            "A sealed export file could not be opened with the"
                " key embedded in the exported runtime.");
        return std::string(plain->begin(), plain->end());
    }

    bool ContainsKeySlotMarker(
        const std::filesystem::path& path)
    {
        const auto bytes = ReadBytes(path);
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        return std::search(
            bytes.begin(),
            bytes.end(),
            marker.begin(),
            marker.end()) != bytes.end();
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create test file.");
        }
        output << contents;
    }

    // 配布先のコピーとして起動された子processで実行します。このprocessが
    // 読み込むLamaPonRuntime.dllにはexport固有鍵が埋め込まれているため、
    // 平文化せずに sealed index/CSO とmetadataを一続きで検証できます。
    void RunExportedShaderCacheProbe()
    {
        const HRESULT comResult = CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        Require(
            SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE,
            "COM initialization failed in the exported shader-cache probe.");
        try
        {
            const auto exportRoot = std::filesystem::current_path();
            const auto cacheDirectory = exportRoot / "shader-cache";
            for (const auto& entry :
                std::filesystem::directory_iterator(cacheDirectory))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const auto bytes = ReadBytes(entry.path());
                Require(
                    LamaPon::Crypto::IsSealed(
                        bytes.data(),
                        bytes.size()),
                    "The exported shader-cache probe found a plaintext file.");
            }

            LamaPon::AssetManager assets(nullptr, nullptr);
            assets.SetAssetRoot(
                exportRoot / "unpacked-assets-not-created");
            LamaPon::ClearShaderCacheSearchDirectories();
            LamaPon::AddShaderCacheSearchDirectory(cacheDirectory);
            const auto sourcePath = assets.ResolvePath(
                "Shaders/TESTEXPORT.HLSL");
            const auto blob = LamaPon::CompileShaderCached(
                assets,
                sourcePath,
                "VSMain",
                "vs_5_0",
                { "EXPORT_MULTI_ON" });
            Require(
                blob && blob->GetBufferSize() != 0,
                "The exported runtime could not decrypt a cached shader.");

            // D3DReflectを動的取得し、復号結果が単に非空なだけでなく、
            // 有効なDirect3D shader bytecodeであることまで確認します。
            const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
            Require(
                compiler != nullptr,
                "D3DCompiler could not be loaded by the cache probe.");
            using ReflectFunction = HRESULT(WINAPI*)(
                LPCVOID,
                SIZE_T,
                REFIID,
                void**);
            const auto reflect = reinterpret_cast<ReflectFunction>(
                GetProcAddress(compiler, "D3DReflect"));
            Require(
                reflect != nullptr,
                "D3DReflect was not available to the cache probe.");
            Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
            const HRESULT reflectResult = reflect(
                blob->GetBufferPointer(),
                blob->GetBufferSize(),
                __uuidof(ID3D11ShaderReflection),
                reinterpret_cast<void**>(
                    reflection.ReleaseAndGetAddressOf()));
            const bool reflected =
                SUCCEEDED(reflectResult) && reflection;
            reflection.Reset();
            FreeLibrary(compiler);
            Require(
                reflected,
                "The decrypted cache payload was not valid shader bytecode.");

            LamaPon::ShaderRenderState restoredState;
            LamaPon::ShaderVariantDeclaration restoredVariants;
            Require(
                LamaPon::LoadPrecompiledShaderMetadata(
                    assets,
                    sourcePath,
                    &restoredState,
                    &restoredVariants)
                    && restoredState.declared
                    && restoredState.blend
                        == LamaPon::ShaderBlendMode::Additive
                    && restoredVariants.groups.size() == 2,
                "The exported runtime could not decrypt shader metadata.");
            LamaPon::ClearShaderCacheSearchDirectories();
        }
        catch (...)
        {
            LamaPon::ClearShaderCacheSearchDirectories();
            if (uninitializeCom)
            {
                CoUninitialize();
            }
            throw;
        }
        if (uninitializeCom)
        {
            CoUninitialize();
        }
    }

    void RunChildProcess(
        const std::filesystem::path& executable,
        const std::filesystem::path& workingDirectory,
        const std::wstring_view arguments)
    {
        std::wstring commandLine = L"\"" + executable.wstring()
            + L"\" " + std::wstring(arguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Require(
            CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                workingDirectory.c_str(),
                &startup,
                &process) != FALSE,
            "Could not launch the exported shader-cache probe.");
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(
            process.hProcess,
            30000);
        DWORD exitCode = 1;
        if (wait == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.hProcess, &exitCode);
        }
        else
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hProcess);
        Require(
            wait == WAIT_OBJECT_0 && exitCode == 0,
            "The exported shader-cache probe failed.");
    }
}

int main(const int argumentCount, char** const arguments)
{
    try
    {
        if (argumentCount == 2
            && std::string_view{ arguments[1] }
                == "--shader-cache-probe")
        {
            RunExportedShaderCacheProbe();
            return 0;
        }
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "game-exporter";
        std::filesystem::remove_all(root);

        const auto runtimeDirectory = root / "runtime";
        const auto assetDirectory = root / "project" / "assets";
        const auto outputDirectory = root / "dist" / "MyGame";
        const auto startupScene =
            std::filesystem::path(L"scenes/日本語.scene.json");
        LamaPon::ProjectSettings projectSettings{
            "日本語ゲーム",
            1600,
            900,
            startupScene
        };
        projectSettings.splashScreenEnabled = false;
        // 1本目は「HLSLを残す」側の書き出しとして検証します
        // （外す側は後半のstrippedSettingsで別に見ます）。既定値へ
        // 任せると、既定が変わったときにshader_featureのバリアント数の
        // 検証が黙って別のことを測り始めます。
        projectSettings.stripShaderSourceOnExport = false;
        projectSettings.graphics =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Ultra);
        projectSettings.graphics.targetFrameRate = 144;
        projectSettings.inputActions = {
            {
                "Dash",
                {
                    {
                        LamaPon::InputControl::KeyboardLeftShift,
                        1.0f
                    },
                    {
                        LamaPon::InputControl::GamePadRightShoulder,
                        1.0f
                    }
                }
            }
        };

        // 書き出し時にシェーダーが事前コンパイルされることを見るため、
        // 本物としてコンパイルできるHLSLを1本置きます。#includeも
        // 使い、依存の記録がアセットルート相対で残ることも兼ねて
        // 確かめます（絶対パスで残すと、配布先で必ず外れます）。
        WriteFile(
            assetDirectory / "shaders" / "TestCommon.hlsli",
            "float TestTint() { return 0.5f; }\n");
        WriteFile(
            assetDirectory / "shaders" / "TestExport.hlsl",
            "/* LAMAPON_RENDER_STATE\n"
            "{ \"blend\": \"additive\", \"cull\": \"none\","
            " \"depthWrite\": false, \"depthTest\": false }\n"
            "*/\n"
            "#include \"TestCommon.hlsli\"\n"
            // multi_compileは常に全組み合わせ、shader_featureは
            // 使われているものだけ。ここでは EXPORT_FEATURE_ON を
            // どのシーンも使っていないので、落ちるのが正解です。
            "#pragma multi_compile _ EXPORT_MULTI_ON\n"
            "#pragma shader_feature _ EXPORT_FEATURE_ON\n"
            // source-stripped配布でも、Manifestだけでなく従来の
            // HLSL直接指定が固定entryの索引を使えることを検証します。
            "float4 VSMain(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
            "}\n"
            "float4 VSSkinnedMain(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSMain(\n"
            "    float4 color : COLOR0,\n"
            "    float2 uv : TEXCOORD0,\n"
            "    float4 position : SV_Position) : SV_Target\n"
            "{\n"
            "    return color * TestTint();\n"
            "}\n"
            "float4 PSSkinnedMain() : SV_Target\n"
            "{\n"
            "    return float4(TestTint(), 0, 1, 1);\n"
            "}\n"
            "struct ExportGeometryVertex\n"
            "{\n"
            "    float4 position : SV_Position;\n"
            "};\n"
            "[maxvertexcount(3)]\n"
            "void GSMain(\n"
            "    triangle ExportGeometryVertex input[3],\n"
            "    inout TriangleStream<ExportGeometryVertex> output)\n"
            "{\n"
            "    output.Append(input[0]);\n"
            "    output.Append(input[1]);\n"
            "    output.Append(input[2]);\n"
            "}\n"
            "[maxvertexcount(3)]\n"
            "void GSManifestForward(\n"
            "    triangle ExportGeometryVertex input[3],\n"
            "    inout TriangleStream<ExportGeometryVertex> output)\n"
            "{\n"
            "    output.Append(input[0]);\n"
            "    output.Append(input[1]);\n"
            "    output.Append(input[2]);\n"
            "}\n"
            "float4 PSOccluded() : SV_Target\n"
            "{\n"
            "    return float4(1, 0, 1, 1);\n"
            "}\n"
            "[numthreads(1, 1, 1)]\n"
            "void CSMain(uint3 id : SV_DispatchThreadID)\n"
            "{\n"
            "}\n"
            "float4 VSManifestMain(uint id : SV_VertexID)"
            " : SV_Position\n"
            "{\n"
            "    float2 positions[3] = {\n"
            "        float2(-1, -1),\n"
            "        float2(-1, 3),\n"
            "        float2(3, -1)\n"
            "    };\n"
            "    return float4(positions[id], 0, 1);\n"
            "}\n"
            "float4 PSManifestMain() : SV_Target\n"
            "{\n"
            "    return float4(TestTint(), 0, 0, 1);\n"
            "}\n"
            "float4 VSMaterialManifest(uint id : SV_VertexID)"
            " : SV_Position\n"
            "{\n"
            "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
            "}\n"
            "float4 PSMaterialManifest() : SV_Target\n"
            "{\n"
            "    return float4(0, TestTint(), 0, 1);\n"
            "}\n"
            "float4 VSSecondForward(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSSecondForward() : SV_Target\n"
            "{\n"
            "    return float4(1, 0.5f, 0, 1);\n"
            "}\n"
            "float4 VSManifestInstanced(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSManifestInstanced() : SV_Target\n"
            "{\n"
            "    return float4(0, 1, 1, 1);\n"
            "}\n"
            "float4 VSManifestSkinned(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSManifestSkinned() : SV_Target\n"
            "{\n"
            "    return float4(1, 1, 0, 1);\n"
            "}\n"
            "float4 VSManifestOutline(uint id : SV_VertexID) : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSManifestOutline() : SV_Target\n"
            "{\n"
            "    return float4(0, 0, 0, 1);\n"
            "}\n"
            "float4 VSManifestSkinnedOutline(uint id : SV_VertexID)"
            " : SV_Position\n"
            "{\n"
            "    return VSMain(id);\n"
            "}\n"
            "float4 PSManifestSkinnedOutline() : SV_Target\n"
            "{\n"
            "    return float4(0.1f, 0.1f, 0.1f, 1);\n"
            "}\n"
            "float4 PSManifestOccluded() : SV_Target\n"
            "{\n"
            "    return float4(1, 0, 0, 0.5f);\n"
            "}\n"
            "RWTexture2D<float4> ManifestOutput : register(u0);\n"
            "[numthreads(1, 1, 1)]\n"
            "void CSManifestMain(uint3 id : SV_DispatchThreadID)\n"
            "{\n"
            "    ManifestOutput[id.xy] = float4(0, 0, TestTint(), 1);\n"
            "}\n");
        WriteFile(
            assetDirectory / "shaders"
                / "TestExport.lamashader.json",
            R"json({
  "version": 1,
  "name": "Custom/ExportManifest",
  "type": "screenEffect",
  "source": "shaders/TestExport.hlsl",
  "passes": [
    {
      "name": "Main",
      "vertex": {
        "entry": "VSManifestMain",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSManifestMain",
        "target": "ps_5_0"
      }
    }
  ]
}
)json");
        WriteFile(
            assetDirectory / "shaders"
                / "TestMaterialExport.lamashader.json",
            R"json({
  "version": 1,
  "name": "Custom/ExportMaterialManifest",
  "type": "material",
  "source": "shaders/TestExport.hlsl",
  "passes": [
    {
      "name": "Forward",
      "role": "forward",
      "vertex": {
        "entry": "VSMaterialManifest",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSMaterialManifest",
        "target": "ps_5_0"
      }
    },
    {
      "name": "SecondForward",
      "role": "forward",
      "vertex": {
        "entry": "VSSecondForward",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSSecondForward",
        "target": "ps_5_0"
      },
      "geometry": {
        "entry": "GSManifestForward",
        "target": "gs_5_0"
      }
    },
    {
      "name": "Instanced",
      "role": "instanced",
      "vertex": {
        "entry": "VSManifestInstanced",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSManifestInstanced",
        "target": "ps_5_0"
      }
    },
    {
      "name": "Skinned",
      "role": "skinned",
      "vertex": {
        "entry": "VSManifestSkinned",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSManifestSkinned",
        "target": "ps_5_0"
      }
    },
    {
      "name": "Outline",
      "role": "outline",
      "vertex": {
        "entry": "VSManifestOutline",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSManifestOutline",
        "target": "ps_5_0"
      }
    },
    {
      "name": "SkinnedOutline",
      "role": "skinnedOutline",
      "vertex": {
        "entry": "VSManifestSkinnedOutline",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSManifestSkinnedOutline",
        "target": "ps_5_0"
      }
    },
    {
      "name": "Occluded",
      "role": "occluded",
      "pixel": {
        "entry": "PSManifestOccluded",
        "target": "ps_5_0"
      }
    }
  ]
}
)json");
        WriteFile(
            assetDirectory / "shaders"
                / "TestComputeExport.lamashader.json",
            R"json({
  "version": 1,
  "name": "Custom/ExportComputeManifest",
  "type": "compute",
  "source": "shaders/TestExport.hlsl",
  "passes": [
    {
      "name": "Dispatch",
      "compute": {
        "entry": "CSManifestMain",
        "target": "cs_5_0"
      }
    }
  ]
}
)json");
        WriteFile(
            assetDirectory / "shaders"
                / "InvalidExport.lamashader.json",
            R"json({
  "version": 2,
  "name": "Custom/InvalidExport",
  "type": "screenEffect",
  "source": "shaders/TestExport.hlsl",
  "passes": []
}
)json");
        WriteFile(
            assetDirectory / "shaders"
                / "BrokenEntryExport.lamashader.json",
            R"json({
  "version": 1,
  "name": "Custom/BrokenEntryExport",
  "type": "screenEffect",
  "source": "shaders/TestExport.hlsl",
  "passes": [
    {
      "name": "Main",
      "vertex": {
        "entry": "VSMissingFromExport",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSMissingFromExport",
        "target": "ps_5_0"
      }
    }
  ]
}
)json");

        // アイコン埋め込みを検証するため、ゲームexeには本物のPE
        // （このテスト自身のコピー）を使います。
        std::filesystem::create_directories(
            runtimeDirectory);
        std::filesystem::copy(
            SelfExecutablePath().parent_path() / "licenses",
            runtimeDirectory / "licenses",
            std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(
            SelfExecutablePath().parent_path() / "THIRD_PARTY_NOTICES.md",
            runtimeDirectory / "THIRD_PARTY_NOTICES.md");
        std::filesystem::copy_file(
            SelfExecutablePath(),
            runtimeDirectory / "LamaPonGame.exe");
        WriteFakeRuntimeLibrary(
            runtimeDirectory / "LamaPonRuntime.dll");
        WriteFile(
            runtimeDirectory / "xaudio2_9redist.dll",
            "audio-runtime");
        WriteFile(
            runtimeDirectory / "LamaPonGameModule.dll",
            "runtime-game-module");
        // 同梱対象のVC++ランタイム（あればコピーされる）。
        WriteFile(
            runtimeDirectory / "vcruntime140.dll",
            "crt");
        WriteFile(
            runtimeDirectory / "msvcp140.dll",
            "crt");

        // ゲームアイコン: 16pxと32pxの2枚を持つICOを生成して
        // アセットとして配置します。
        LamaPon::IconImage smallIcon;
        smallIcon.width = 16;
        smallIcon.height = 16;
        smallIcon.bgraPixels.resize(
            16 * 16 * 4,
            std::byte{ 0x7F });
        LamaPon::IconImage largeIcon;
        largeIcon.width = 32;
        largeIcon.height = 32;
        largeIcon.bgraPixels.resize(
            32 * 32 * 4,
            std::byte{ 0x3F });
        const auto icoBytes = LamaPon::BuildIcoFileBytes(
            { smallIcon, largeIcon });
        Require(
            icoBytes.size() > 6 + 16 * 2
                && icoBytes[0] == std::byte{ 0 }
                && icoBytes[2] == std::byte{ 1 }
                && icoBytes[4] == std::byte{ 2 },
            "Generated .ico header is malformed.");
        {
            std::filesystem::create_directories(
                assetDirectory / "icons");
            std::ofstream icoOutput(
                assetDirectory / "icons" / "game.ico",
                std::ios::binary | std::ios::trunc);
            icoOutput.write(
                reinterpret_cast<const char*>(
                    icoBytes.data()),
                static_cast<std::streamsize>(
                    icoBytes.size()));
        }
        projectSettings.gameIcon =
            std::filesystem::path("icons") / "game.ico";

        // .icoはヘッダー検証の上そのまま使われます。
        Require(
            LamaPon::BuildIcoFromImageFile(
                assetDirectory / "icons" / "game.ico")
                == icoBytes,
            ".ico passthrough altered the bytes.");
        const auto projectGameModule =
            root / "project" / ".lamapon" / "bin"
            / "LamaPonGameModule.dll";
        WriteFile(
            projectGameModule,
            "project-game-module");
        const auto projectScript =
            assetDirectory / "scripts" / "TestScript.cpp";
        WriteFile(
            projectScript,
            "// Export freshness test source.\n");
        WriteFile(
            projectGameModule.parent_path()
                / "middleware.dll",
            "project-middleware");
        // 平文漏れ検査の目印は長い一意な文字列にします。短い文字列
        // （"{}"など）は暗号化後のバイト列に偶然出現する確率が
        // 無視できず、テストがまれに失敗します。
        constexpr const char* sceneMarker =
            R"({"marker":"LAMAPON_PLAINTEXT_SCENE_MARKER"})";
        constexpr const char* textureMarker =
            "LAMAPON_PLAINTEXT_TEXTURE_MARKER";
        WriteFile(
            assetDirectory / startupScene,
            sceneMarker);
        WriteFile(
            assetDirectory / "textures" / "sample.bin",
            textureMarker);

        // エンジン更新後に古いGame Moduleを梱包するとNative Scriptが
        // 解決できないため、配布前に拒否します。
        const auto runtimeWriteTime =
            std::filesystem::last_write_time(
                runtimeDirectory / "LamaPonRuntime.dll");
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime - std::chrono::minutes(2));
        std::filesystem::last_write_time(
            projectGameModule,
            runtimeWriteTime - std::chrono::minutes(1));
        bool staleModuleRejected = false;
        try
        {
            static_cast<void>(LamaPon::ExportGamePackage(
                LamaPon::GameExportOptions{
                    runtimeDirectory,
                    assetDirectory,
                    root / "dist" / "StaleModule",
                    projectSettings,
                    projectGameModule
                }));
        }
        catch (const std::exception&)
        {
            staleModuleRejected = true;
        }
        Require(
            staleModuleRejected,
            "An export with a Game Module older than the Runtime was accepted.");

        std::filesystem::last_write_time(
            projectGameModule,
            runtimeWriteTime + std::chrono::minutes(1));
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime + std::chrono::minutes(2));
        bool staleSourceRejected = false;
        try
        {
            static_cast<void>(LamaPon::ExportGamePackage(
                LamaPon::GameExportOptions{
                    runtimeDirectory,
                    assetDirectory,
                    root / "dist" / "StaleSource",
                    projectSettings,
                    projectGameModule
                }));
        }
        catch (const std::exception&)
        {
            staleSourceRejected = true;
        }
        Require(
            staleSourceRejected,
            "An export with C++ sources newer than the Game Module was accepted.");
        std::filesystem::last_write_time(
            projectScript,
            runtimeWriteTime);

        const auto first = LamaPon::ExportGamePackage(
            LamaPon::GameExportOptions{
                runtimeDirectory,
                assetDirectory,
                outputDirectory,
                projectSettings,
                projectGameModule
            });
        {
            bool invalidManifestReported{};
            bool compileFailureReported{};
            for (const auto& entry :
                LamaPon::Logger::Instance().Snapshot())
            {
                if (entry.level == LamaPon::LogLevel::Warning
                    && entry.message.find(
                        "InvalidExport.lamashader.json")
                        != std::string::npos
                    && entry.message.find("version 2")
                        != std::string::npos)
                {
                    invalidManifestReported = true;
                }
                if (entry.level == LamaPon::LogLevel::Warning
                    && entry.message.find(
                        "BrokenEntryExport.lamashader.json")
                        != std::string::npos
                    && entry.message.find(
                        "VSMissingFromExport")
                        != std::string::npos
                    && (entry.message.find("X3501")
                            != std::string::npos
                        || entry.message.find("entrypoint")
                            != std::string::npos))
                {
                    compileFailureReported = true;
                }
            }
            Require(
                invalidManifestReported,
                "An invalid shader manifest was skipped without a"
                    " path-and-reason warning.");
            Require(
                compileFailureReported,
                "A required manifest stage compile failure omitted"
                    " the compiler diagnostic during export.");
        }
        Require(
            CountExportedFilesExcludingShaderCache(
                outputDirectory) == 9,
            "Unexpected exported file count.");
        // 実行ファイルはゲーム名を反映した名前になります。
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / L"日本語ゲーム.exe"),
            "Game executable was not exported with the game name.");
        Require(
            first.executablePath
                == outputDirectory / L"日本語ゲーム.exe",
            "Result did not report the renamed executable.");
        Require(
            !std::filesystem::exists(
                outputDirectory / "LamaPonGame.exe"),
            "Executable kept its default name.");
        RequireEmbeddedIcon(first.executablePath, 2);
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "vcruntime140.dll")
                && std::filesystem::is_regular_file(
                    outputDirectory / "msvcp140.dll"),
            "VC++ runtime DLLs were not bundled.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "LamaPonRuntime.dll"),
            "Runtime DLL was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "xaudio2_9redist.dll"),
            "XAudio2 Redistributable was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory
                    / "LamaPonGameModule.dll"),
            "Game Module was not exported.");
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "middleware.dll"),
            "Project-local runtime DLL was not exported.");
        {
            std::ifstream moduleInput(
                outputDirectory / "LamaPonGameModule.dll",
                std::ios::binary);
            const std::string moduleContents{
                std::istreambuf_iterator<char>{ moduleInput },
                std::istreambuf_iterator<char>{}
            };
            Require(
                moduleContents == "project-game-module",
                "Export did not prefer the project Game Module.");
        }
        Require(
            std::filesystem::is_regular_file(
                outputDirectory / "assets.tpak"),
            "Encrypted asset archive was not exported.");
        // 書き出しごとに配布物固有の鍵を生成し、配布用の
        // LamaPonRuntime.dllへ埋め込みます。以降はその鍵を取り出して検証します。
        const auto exportedRuntime =
            outputDirectory / "LamaPonRuntime.dll";
        const auto embeddedKey =
            ReadEmbeddedArchiveKey(exportedRuntime);
        {
            // 事前コンパイル済みシェーダーの同梱。無いと、
            // プレイヤーの初回起動で全部コンパイルすることになります。
            const auto shaderCache =
                outputDirectory / "shader-cache";
            Require(
                std::filesystem::is_directory(shaderCache),
                "Precompiled shader cache was not exported.");
            std::size_t byteCodeCount{};
            std::size_t manifestCount{};
            for (const auto& entry :
                std::filesystem::directory_iterator(
                    shaderCache))
            {
                if (entry.path().extension() == ".cso")
                {
                    ++byteCodeCount;
                }
                else if (entry.path().extension() == ".deps")
                {
                    ++manifestCount;
                }
            }
            Require(
                byteCodeCount > 0,
                "No compiled shader bytecode was exported.");
            Require(
                manifestCount >= byteCodeCount,
                "Compiled shaders were exported without their"
                    " dependency manifests.");

            // shader_featureのストリップ。EXPORT_MULTI_ONは
            // multi_compileなので必ず2通り焼かれ、
            // EXPORT_FEATURE_ONはどのシーンも使っていないので
            // 落ちます。つまりバリアントは2通りのはずです。
            // 落ちていなければ4通りぶんの.depsが出ます。
            std::size_t exportManifests{};
            for (const auto& entry :
                std::filesystem::directory_iterator(
                    shaderCache))
            {
                if (entry.path().extension() != ".deps")
                {
                    continue;
                }
                std::istringstream manifest(
                    ReadSealedText(entry.path(), embeddedKey));
                std::string line;
                if (std::getline(manifest, line)
                    && line.find("testexport.hlsl")
                        != std::string::npos)
                {
                    ++exportManifests;
                }
            }
            // 既知の入口は2バリアントずつ、Manifest固有の
            // vertex/pixelはScreenEffect実行時と同じキーワード無しを
            // 1本ずつ、Material全14 stageは使用中の2バリアントずつ、
            // Computeは実行時と同じ必須CSを1本焼きます。壊れた
            // Manifestの2 stageも失敗cache用の.depsを残します。
            const auto entryPoints =
                LamaPon::KnownShaderEntryPoints().size();
            const auto expectedExportManifests =
                entryPoints * 2 + 2 + 14 * 2 + 1 + 2;
            std::cout
                << "export shader dependency manifests: actual="
                << exportManifests
                << " expected="
                << expectedExportManifests
                << '\n';
            Require(
                exportManifests == expectedExportManifests,
                "shader_feature variants that no material uses"
                    " must be stripped without dropping manifest"
                    " entry points.");
        }
        Require(
            !std::filesystem::exists(
                outputDirectory / "assets"),
            "Loose assets were exported instead of the encrypted archive.");
        // 書き出しごとの配布物固有鍵が埋め込まれることを確認します。
        Require(
            embeddedKey != LamaPon::Crypto::ArchiveKey(),
            "The export must embed its own archive key, not the"
                " key that ships inside the engine.");
        Require(
            !ContainsKeySlotMarker(exportedRuntime),
            "The key slot marker must be erased from the"
                " exported runtime; otherwise the key can be"
                " located by pattern.");
        {
            // 既定鍵では開けず、書き出し固有鍵だけが有効であることを確認します。
            bool openedWithEngineKey = true;
            try
            {
                static_cast<void>(
                    LamaPon::AssetArchive::Open(
                        outputDirectory / "assets.tpak"));
            }
            catch (const std::exception&)
            {
                openedWithEngineKey = false;
            }
            Require(
                !openedWithEngineKey,
                "The exported archive must not open with the"
                    " engine's built-in key.");
        }
        {
            // 索引を1バイト書き換えたら開けないこと（改ざん検知）。
            const auto tampered =
                outputDirectory.parent_path() / "tampered.tpak";
            auto bytes = ReadBytes(
                outputDirectory / "assets.tpak");
            constexpr std::size_t headerSize = 8 + 8 + 16 + 32;
            Require(
                bytes.size() > headerSize,
                "Archive is too small to tamper with.");
            bytes[headerSize] =
                static_cast<std::uint8_t>(
                    bytes[headerSize] ^ 0xff);
            WriteBytes(tampered, bytes);
            bool openedTampered = true;
            try
            {
                static_cast<void>(
                    LamaPon::AssetArchive::Open(
                        tampered,
                        embeddedKey));
            }
            catch (const std::exception&)
            {
                openedTampered = false;
            }
            Require(
                !openedTampered,
                "A tampered archive index must be rejected.");
        }
        {
            // 同梱した事前コンパイル済みシェーダーが暗号化されて
            // いること（HLSLを外してもDXBCが素で置いてあれば読めます）。
            const auto index = ReadBytes(
                outputDirectory / "shader-cache" / "index.txt");
            Require(
                LamaPon::Crypto::IsSealed(
                    index.data(),
                    index.size()),
                "The bundled shader cache must be encrypted.");
        }
        {
            const auto archive = LamaPon::AssetArchive::Open(
                outputDirectory / "assets.tpak",
                embeddedKey);
            Require(
                archive != nullptr,
                "Encrypted asset archive could not be opened.");
            Require(
                archive->Contains(startupScene),
                "Startup scene is missing from the encrypted archive.");
            const auto decryptedScene =
                archive->TryRead(startupScene);
            Require(
                decryptedScene.has_value()
                    && std::string(
                        decryptedScene->begin(),
                        decryptedScene->end()) == sceneMarker,
                "Startup scene did not decrypt to its original contents.");

            const auto texturePath =
                std::filesystem::path("textures")
                    / "sample.bin";
            Require(
                archive->Contains(texturePath),
                "Texture asset is missing from the encrypted archive.");
            const auto decryptedTexture =
                archive->TryRead(texturePath);
            Require(
                decryptedTexture.has_value()
                    && std::string(
                        decryptedTexture->begin(),
                        decryptedTexture->end())
                        == textureMarker,
                "Texture asset did not decrypt to its original contents.");

            std::ifstream rawArchive(
                outputDirectory / "assets.tpak",
                std::ios::binary);
            const std::string rawArchiveBytes{
                std::istreambuf_iterator<char>{ rawArchive },
                std::istreambuf_iterator<char>{}
            };
            Require(
                rawArchiveBytes.find(textureMarker)
                        == std::string::npos
                    && rawArchiveBytes.find(
                        "LAMAPON_PLAINTEXT_SCENE_MARKER")
                        == std::string::npos,
                "Encrypted archive contains plaintext asset content.");
        }

        nlohmann::json settings;
        {
            std::ifstream input(
                outputDirectory / "LamaPonGame.json",
                std::ios::binary);
            input >> settings;
        }
        Require(
            settings.at("startupScene").get<std::string>()
                == "scenes/日本語.scene.json",
            "Startup scene setting was not exported.");
        Require(
            !settings.at("splashScreenEnabled").get<bool>(),
            "Startup splash setting was not exported.");
        Require(
            settings.at("gameName").get<std::string>()
                == "日本語ゲーム",
            "Game name was not exported.");
        Require(
            settings.at("window").at("width").get<int>()
                == 1600
                && settings.at("window").at("height").get<int>()
                    == 900,
            "Window size was not exported.");
        Require(
            settings.at("graphics").at("preset")
                .get<std::string>() == "Ultra"
                && settings.at("graphics")
                    .at("shadowResolution")
                    .get<int>() == 4096
                && settings.at("graphics")
                    .at("antiAliasingEnabled")
                    .get<bool>()
                && settings.at("graphics")
                    .at("targetFrameRate")
                    .get<int>() == 144,
            "Graphics quality was not exported.");
        Require(
            settings.at("inputActions").at(0)
                .at("name").get<std::string>() == "Dash"
                && settings.at("inputActions").at(0)
                    .at("bindings").at(1)
                    .at("control").get<std::string>()
                    == "GamePadRightShoulder",
            "Input actions were not exported.");
        const auto loadedSettings =
            LamaPon::LoadProjectSettings(
                outputDirectory / "LamaPonGame.json");
        Require(
            loadedSettings.gameName == projectSettings.gameName
                && loadedSettings.windowWidth == 1600
                && loadedSettings.windowHeight == 900
                && loadedSettings.startupScene == startupScene
                && !loadedSettings.splashScreenEnabled
                && loadedSettings.graphics.preset
                    == LamaPon::GraphicsQualityPreset::Ultra
                && loadedSettings.graphics.shadowResolution
                    == 4096
                && loadedSettings.graphics.shadowCascadeLimit
                    == 4
                && loadedSettings.graphics.targetFrameRate
                    == 144
                && loadedSettings.inputActions.size() == 1
                && loadedSettings.inputActions[0].name
                    == "Dash"
                && loadedSettings.inputActions[0]
                    .bindings[0].control
                    == LamaPon::InputControl::
                        KeyboardLeftShift,
            "Exported project settings did not round-trip.");

        bool invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.windowWidth = 100;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid project settings were accepted.");

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.renderScale =
                0.25f;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid graphics settings were accepted.");

        // 1.0を超える描画スケールはスーパーサンプリングとして有効です
        // （2.0が上限）。ここが再び1.0で弾かれないよう固定します。
        {
            auto supersampledSettings = projectSettings;
            supersampledSettings.graphics.renderScale = 2.0f;
            LamaPon::ValidateProjectSettings(
                supersampledSettings);
        }

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.renderScale = 2.5f;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Render scale above 2.0 was accepted.");

        invalidSettingsRejected = false;
        try
        {
            auto invalidSettings = projectSettings;
            invalidSettings.graphics.targetFrameRate =
                10;
            LamaPon::ValidateProjectSettings(
                invalidSettings);
        }
        catch (const std::exception&)
        {
            invalidSettingsRejected = true;
        }
        Require(
            invalidSettingsRejected,
            "Invalid target frame rate was accepted.");

        WriteFile(
            outputDirectory / "stale-file.txt",
            "stale");
        const auto second = LamaPon::ExportGamePackage(
            LamaPon::GameExportOptions{
                runtimeDirectory,
                assetDirectory,
                outputDirectory,
                projectSettings,
                projectGameModule
            });
        Require(
            CountExportedFilesExcludingShaderCache(
                outputDirectory) == 9,
            "Re-export produced an unexpected file count.");
        Require(
            !std::filesystem::exists(
                outputDirectory / "stale-file.txt"),
            "Re-export did not replace stale output.");

        // ZIP作成オプション: 出力フォルダーの隣へ.zipができます。
        LamaPon::GameExportOptions zipOptions{
            runtimeDirectory,
            assetDirectory,
            outputDirectory,
            projectSettings,
            projectGameModule
        };
        zipOptions.createZipArchive = true;
        const auto zipped = LamaPon::ExportGamePackage(
            zipOptions);
        Require(
            !zipped.zipPath.empty()
                && std::filesystem::is_regular_file(
                    zipped.zipPath)
                && std::filesystem::file_size(
                    zipped.zipPath) > 22,
            "Distribution zip was missing or empty.");
        Require(
            zipped.zipPath.parent_path()
                == outputDirectory.parent_path(),
            "Distribution zip is not beside the output folder.");

        // ゲーム名→ファイル名の整形規則。
        Require(
            LamaPon::SanitizeGameFileName("My: Game?")
                == L"My_ Game_",
            "Invalid characters were not replaced.");
        Require(
            LamaPon::SanitizeGameFileName(" . ")
                == L"LamaPonGame",
            "Empty sanitized names must fall back.");
        Require(
            LamaPon::SanitizeGameFileName("CON")
                == L"_CON",
            "Reserved device names must be escaped.");
        Require(
            LamaPon::SanitizeGameFileName("日本語ゲーム")
                == L"日本語ゲーム",
            "Japanese game names must pass through.");

        bool unsafeDestinationRejected = false;
        try
        {
            static_cast<void>(
                LamaPon::ExportGamePackage(
                    LamaPon::GameExportOptions{
                        runtimeDirectory,
                        assetDirectory,
                        assetDirectory / "export",
                        projectSettings
                    }));
        }
        catch (const std::exception&)
        {
            unsafeDestinationRejected = true;
        }
        Require(
            unsafeDestinationRejected,
            "Unsafe destination inside assets was accepted.");

        // HLSLソースを外す設定。配布物にはバイトコードと索引だけが
        // 入り、.hlslはアーカイブから消えることを確かめます。
        {
            // sourceを残す通常exportでは、壊れた未使用Manifestを
            // path付きwarningとして検証済みです。source-stripは実行時に
            // 再コンパイルできないため、正常なManifestだけを含む実運用と
            // 同じ入力へ戻してから完全cacheを検証します。
            Require(
                std::filesystem::remove(
                    assetDirectory / "shaders"
                        / "InvalidExport.lamashader.json")
                    && std::filesystem::remove(
                        assetDirectory / "shaders"
                            / "BrokenEntryExport.lamashader.json"),
                "The temporary invalid manifest fixtures could not be "
                "removed before source-stripped export testing.");
            auto strippedSettings = projectSettings;
            strippedSettings.stripShaderSourceOnExport = true;
            const auto strippedOutput =
                root / "dist" / "Stripped";

            // ModelRendererの必須entry／roleは、model未指定・静的model・
            // skin付きmodelで実行時経路が異なります。scene -> Material
            // Asset -> shaderをGUIDで辿り、fallback pathが古くても実際の
            // 描画経路どおりに検証することを確認します。
            constexpr const char* brokenShaderGuid =
                "11111111111111111111111111111111";
            constexpr const char* brokenMaterialGuid =
                "22222222222222222222222222222222";
            constexpr const char* lowerCaseGuid =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            constexpr const char* upperCaseGuid =
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
            constexpr const char* ignoredBackupGuid =
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl",
                "float4 VSMain(uint id : SV_VertexID) : SV_Position\n"
                "{\n"
                "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
                "}\n"
                "float4 PSMain() : SV_Target\n"
                "{\n"
                "    return float4(1, 0, 0, 1);\n"
                "}\n");
            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl.meta",
                std::string{
                    R"json({"format":"LamaPonAssetMeta","version":1,"guid":")json" }
                    + brokenShaderGuid
                    + R"json(","importer":"Shader"})json");
            WriteFile(
                assetDirectory / "BrokenDirect.material.json.meta",
                std::string{
                    R"json({"format":"LamaPonAssetMeta","version":1,"guid":")json" }
                    + brokenMaterialGuid
                    + R"json(","importer":"LitMaterial"})json");
            // AssetDatabaseはGUIDをcase-sensitiveに扱います。caseだけが
            // 異なる2件を同じkeyへ潰さず、指定された側へcanonicalize
            // することもpacked JSONで確認します。
            WriteFile(
                assetDirectory / "CaseLower.asset",
                "lower");
            WriteFile(
                assetDirectory / "CaseLower.asset.meta",
                std::string{
                    R"json({"format":"LamaPonAssetMeta","version":1,"guid":")json" }
                    + lowerCaseGuid
                    + R"json(","importer":"Default"})json");
            WriteFile(
                assetDirectory / "CaseUpper.asset",
                "upper");
            WriteFile(
                assetDirectory / "CaseUpper.asset.META",
                std::string{
                    R"json({"format":"LamaPonAssetMeta","version":1,"guid":")json" }
                    + upperCaseGuid
                    + R"json(","importer":"Default"})json");
            WriteFile(
                assetDirectory / "Ignored.asset.bak",
                "backup");
            WriteFile(
                assetDirectory / "Ignored.asset.bak.meta",
                std::string{
                    R"json({"format":"LamaPonAssetMeta","version":1,"guid":")json" }
                    + ignoredBackupGuid
                    + R"json(","importer":"Default"})json");
            WriteFile(
                assetDirectory / startupScene,
                std::string{
                    R"json({"marker":"LAMAPON_PLAINTEXT_SCENE_MARKER","type":"ModelRenderer","materialAsset":"OldMaterial.material.json","materialAssetGuid":")json" }
                    + brokenMaterialGuid
                    + R"json(","caseAsset":"OldCase.asset","caseAssetGuid":")json"
                    + upperCaseGuid
                    + R"json(","ignoredAsset":"BackupFallback.asset","ignoredAssetGuid":")json"
                    + ignoredBackupGuid + R"json("})json");
            const auto requireStrippedFailure =
                [&](const std::string_view firstDiagnostic,
                    const std::string_view secondDiagnostic,
                    const char* const failureMessage)
                {
                    bool rejected = false;
                    std::string diagnostic;
                    try
                    {
                        static_cast<void>(LamaPon::ExportGamePackage(
                            LamaPon::GameExportOptions{
                                runtimeDirectory,
                                assetDirectory,
                                strippedOutput,
                                strippedSettings
                            }));
                    }
                    catch (const std::exception& exception)
                    {
                        rejected = true;
                        diagnostic = exception.what();
                    }
                    const bool diagnosticMatched = rejected
                        && diagnostic.find(firstDiagnostic)
                            != std::string::npos
                        && diagnostic.find(secondDiagnostic)
                            != std::string::npos;
                    if (!diagnosticMatched)
                    {
                        std::cerr << "Unexpected stripped-export diagnostic: "
                            << diagnostic << '\n';
                    }
                    Require(
                        diagnosticMatched,
                        failureMessage);
                    Require(
                        !std::filesystem::exists(strippedOutput),
                        "A failed shader precompile published partial output.");
                };

            // Material Assetは単にJSONとして読めるだけでは不十分です。
            // 実行時loaderと同じschema検証をexportでも通します。
            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                R"json({"type":"NotALamaPonMaterial","shader":"shaders/BrokenDirect.hlsl"})json");
            requireStrippedFailure(
                "brokendirect.material.json",
                "Unsupported material asset",
                "A source-stripped export accepted an invalid material asset.");

            // 参照pathだけが残り、対応するHLSLが存在しないケースも、
            // 実在ファイルだけを走査する総当たりから漏らしません。
            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                R"json({"type":"LamaPonLitMaterial","version":2,"shader":"shaders/MissingDirect.hlsl"})json");
            requireStrippedFailure(
                "missingdirect.hlsl",
                "does not exist",
                "A source-stripped export accepted a missing direct HLSL.");

            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                R"json({"type":"LamaPonLitMaterial","version":2,"shader":"shaders/MissingMaterial.lamashader.json"})json");
            requireStrippedFailure(
                "missingmaterial.lamashader.json",
                "does not exist",
                "A source-stripped export accepted a missing shader "
                "manifest.");

            // model未指定ならnon-skeletal経路です。skinned entryだけの
            // HLSLを受理せず、通常のVSMain／PSMainを要求します。
            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl",
                "float4 VSSkinnedMain(uint id : SV_VertexID) : SV_Position\n"
                "{\n"
                "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
                "}\n"
                "float4 PSSkinnedMain() : SV_Target\n"
                "{\n"
                "    return float4(1, 0, 0, 1);\n"
                "}\n");
            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                std::string{
                    R"json({"type":"LamaPonLitMaterial","version":2,"shader":"shaders/MovedFromHere.hlsl","shaderGuid":")json" }
                    + brokenShaderGuid + R"json("})json");
            requireStrippedFailure(
                "BrokenDirect.hlsl",
                "VSMain",
                "A model-less ModelRenderer did not require the Forward "
                "direct-HLSL entry pair.");

            // GSMainは入口がコンパイルできるだけでなく、runtimeが流す
            // triangle入力を受ける必要があります。sourceを外した後の
            // 初回実行までpoint/line不整合を持ち越さないよう、export
            // 時点でbytecodeをreflectして拒否します。
            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl",
                "float4 VSMain(uint id : SV_VertexID) : SV_Position\n"
                "{\n"
                "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
                "}\n"
                "float4 PSMain() : SV_Target\n"
                "{\n"
                "    return float4(1, 0, 0, 1);\n"
                "}\n"
                "struct InvalidGeometryVertex\n"
                "{\n"
                "    float4 position : SV_Position;\n"
                "};\n"
                "[maxvertexcount(1)]\n"
                "void GSMain(\n"
                "    point InvalidGeometryVertex input[1],\n"
                "    inout PointStream<InvalidGeometryVertex> output)\n"
                "{\n"
                "    output.Append(input[0]);\n"
                "}\n");
            requireStrippedFailure(
                "BrokenDirect.hlsl",
                "must take triangle input",
                "A source-stripped export accepted a point-input direct "
                "geometry shader.");

            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl",
                "float4 VSMain(uint id : SV_VertexID) : SV_Position\n"
                "{\n"
                "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
                "}\n"
                "float4 PSMain() : SV_Target\n"
                "{\n"
                "    return float4(1, 0, 0, 1);\n"
                "}\n");

            const auto invalidGeometrySource =
                assetDirectory / "shaders"
                / "InvalidGeometryExport.hlsl";
            const auto invalidGeometryManifest =
                assetDirectory / "shaders"
                / "InvalidGeometryExport.lamashader.json";
            WriteFile(
                invalidGeometrySource,
                "struct GeometryVertex\n"
                "{\n"
                "    float4 position : SV_Position;\n"
                "};\n"
                "GeometryVertex VSGeometry(uint id : SV_VertexID)\n"
                "{\n"
                "    GeometryVertex output;\n"
                "    output.position = float4(0, 0, 0, 1);\n"
                "    return output;\n"
                "}\n"
                "float4 PSGeometry() : SV_Target\n"
                "{\n"
                "    return float4(1, 1, 1, 1);\n"
                "}\n"
                "[maxvertexcount(1)]\n"
                "void GSGeometryPoint(\n"
                "    point GeometryVertex input[1],\n"
                "    inout PointStream<GeometryVertex> output)\n"
                "{\n"
                "    output.Append(input[0]);\n"
                "}\n");
            WriteFile(
                invalidGeometryManifest,
                R"json({
  "version": 1,
  "name": "Custom/InvalidGeometryExport",
  "type": "material",
  "source": "shaders/InvalidGeometryExport.hlsl",
  "passes": [
    {
      "name": "Forward",
      "role": "forward",
      "vertex": { "entry": "VSGeometry", "target": "vs_5_0" },
      "geometry": { "entry": "GSGeometryPoint", "target": "gs_5_0" },
      "pixel": { "entry": "PSGeometry", "target": "ps_5_0" }
    }
  ]
}
)json");
            requireStrippedFailure(
                "InvalidGeometryExport.lamashader.json",
                "must take triangle input",
                "A source-stripped export accepted a point-input manifest "
                "geometry shader.");
            Require(
                std::filesystem::remove(invalidGeometryManifest)
                    && std::filesystem::remove(invalidGeometrySource),
                "The invalid geometry export fixtures could not be removed.");

            // model未指定のModelRendererはForward roleを使います。
            // Forward-only ManifestをSkinned不足で拒否せず、次のsource
            // 検証まで進むことを診断から確認します。
            const auto forwardOnlyManifest =
                assetDirectory / "shaders"
                / "ForwardOnlyModel.lamashader.json";
            WriteFile(
                forwardOnlyManifest,
                R"json({
  "version": 1,
  "name": "Custom/ForwardOnlyModel",
  "type": "material",
  "source": "shaders/MissingForwardSource.hlsl",
  "passes": [
    {
      "name": "Forward",
      "role": "forward",
      "vertex": {
        "entry": "VSMaterialManifest",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSMaterialManifest",
        "target": "ps_5_0"
      }
    }
  ]
}
)json");
            WriteFile(
                forwardOnlyManifest.string() + ".meta",
                R"json({"format":"LamaPonAssetMeta","version":1,"guid":"33333333333333333333333333333333","importer":"Default"})json");
            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                R"json({"type":"LamaPonLitMaterial","version":2,"shader":"shaders/ForwardOnlyModel.lamashader.json"})json");
            requireStrippedFailure(
                "ForwardOnlyModel.lamashader.json",
                "source file does not exist",
                "A model-less ModelRenderer did not accept a Forward-only "
                "material manifest.");

            // 同じManifestでもskin付きglTFならSkinned roleが必要です。
            // 実在fixtureをproject assetsへコピーし、さらにskin無しnodeを
            // 足してForward／Skinnedの混在判定をGPUなしで通します。
            const auto riggedModel =
                assetDirectory / "models" / "RiggedSimple.glb";
            std::filesystem::create_directories(
                riggedModel.parent_path());
            std::filesystem::copy_file(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models" / "RiggedSimple.glb",
                riggedModel);
            AddUnskinnedInstanceToGlb(riggedModel);
            {
                const HRESULT comResult = CoInitializeEx(
                    nullptr,
                    COINIT_MULTITHREADED);
                const bool uninitializeCom = SUCCEEDED(comResult);
                Require(
                    SUCCEEDED(comResult)
                        || comResult == RPC_E_CHANGED_MODE,
                    "COM initialization failed for the mixed glTF "
                        "role probe.");
                {
                    LamaPon::AssetManager probeAssets(nullptr, nullptr);
                    bool requiresForwardRole{};
                    Require(
                        LamaPon::GltfImporter::RequiresSkinning(
                            probeAssets,
                            riggedModel,
                            &requiresForwardRole)
                            && requiresForwardRole,
                        "The generated mixed glTF was not classified as "
                            "requiring both Forward and Skinned roles.");
                }
                if (uninitializeCom)
                {
                    CoUninitialize();
                }
            }
            WriteFile(
                forwardOnlyManifest,
                R"json({
  "version": 1,
  "name": "Custom/ForwardOnlyModel",
  "type": "material",
  "source": "shaders/TestExport.hlsl",
  "passes": [
    {
      "name": "Forward",
      "role": "forward",
      "vertex": {
        "entry": "VSMaterialManifest",
        "target": "vs_5_0"
      },
      "pixel": {
        "entry": "PSMaterialManifest",
        "target": "ps_5_0"
      }
    }
  ]
}
)json");
            WriteFile(
                assetDirectory / startupScene,
                std::string{
                    R"json({"marker":"LAMAPON_PLAINTEXT_SCENE_MARKER","type":"ModelRenderer","model":"models/RiggedSimple.glb","shader":"shaders/StaleComponentShader.hlsl","materialAsset":"OldMaterial.material.json","materialAssetGuid":")json" }
                    + brokenMaterialGuid
                    + R"json(","caseAsset":"OldCase.asset","caseAssetGuid":")json"
                    + upperCaseGuid
                    + R"json(","ignoredAsset":"BackupFallback.asset","ignoredAssetGuid":")json"
                    + ignoredBackupGuid + R"json("})json");
            requireStrippedFailure(
                "ForwardOnlyModel.lamashader.json",
                "no Skinned pass",
                "A source-stripped export accepted a mixed-role "
                "ModelRenderer manifest without a Skinned role.");
            Require(
                std::filesystem::remove(forwardOnlyManifest)
                    && std::filesystem::remove(
                        forwardOnlyManifest.string() + ".meta"),
                "The incompatible manifest fixture could not be removed.");

            // GUIDを優先して実際のHLSLまで辿り、Model用途のentryを
            // 要求します。VSMain／PSMainだけならここで拒否されます。
            WriteFile(
                assetDirectory / "BrokenDirect.material.json",
                std::string{
                    R"json({"type":"LamaPonLitMaterial","version":2,"shader":"shaders/MovedFromHere.hlsl","shaderGuid":")json" }
                    + brokenShaderGuid + R"json("})json");
            requireStrippedFailure(
                "BrokenDirect.hlsl",
                "VSSkinnedMain",
                "A source-stripped export did not follow ModelRenderer "
                "Material/GUID references or lost its skinned diagnostic.");

            // 逆にskinned pairだけを持つModel用HLSLは有効です。誤って
            // VSMain／PSMainまで要求するfalse-positiveも同じ経路で防ぎます。
            WriteFile(
                assetDirectory / "shaders" / "BrokenDirect.hlsl",
                "float4 VSSkinnedMain(uint id : SV_VertexID) : SV_Position\n"
                "{\n"
                "    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);\n"
                "}\n"
                "float4 PSSkinnedMain() : SV_Target\n"
                "{\n"
                "    return float4(1, 0, 0, 1);\n"
                "}\n");

            // Editor側の通常cacheを無効化していても、export用contextは
            // source-stripped成果物を必ず書く必要があります。
            LamaPon::SetShaderCacheEnabled(false);
            LamaPon::GameExportResult stripped;
            try
            {
                stripped = LamaPon::ExportGamePackage(
                    LamaPon::GameExportOptions{
                        runtimeDirectory,
                        assetDirectory,
                        strippedOutput,
                        strippedSettings
                    });
            }
            catch (...)
            {
                LamaPon::SetShaderCacheEnabled(true);
                throw;
            }
            LamaPon::SetShaderCacheEnabled(true);
            static_cast<void>(stripped);

            const auto sourceSceneBytes = ReadBytes(
                assetDirectory / startupScene);
            const auto sourceMaterialBytes = ReadBytes(
                assetDirectory / "BrokenDirect.material.json");
            const auto sourceScene = nlohmann::json::parse(
                sourceSceneBytes.begin(),
                sourceSceneBytes.end());
            const auto sourceMaterial = nlohmann::json::parse(
                sourceMaterialBytes.begin(),
                sourceMaterialBytes.end());
            Require(
                sourceScene.at("materialAsset").get<std::string>()
                        == "OldMaterial.material.json"
                    && sourceMaterial.at("shader").get<std::string>()
                        == "shaders/MovedFromHere.hlsl"
                    && sourceScene.at("shader").get<std::string>()
                        == "shaders/StaleComponentShader.hlsl"
                    && sourceScene.at("caseAsset").get<std::string>()
                        == "OldCase.asset"
                    && sourceScene.at("ignoredAsset").get<std::string>()
                        == "BackupFallback.asset",
                "Export modified the project's stale GUID fallbacks "
                "instead of transforming only the packed JSON.");
            Require(
                !std::filesystem::exists(
                    (assetDirectory / startupScene).wstring()
                        + L".meta"),
                "Export created missing metadata in the source project.");

            WriteFile(
                assetDirectory / startupScene,
                sceneMarker);
            Require(
                std::filesystem::remove(
                    assetDirectory / "BrokenDirect.material.json")
                    && std::filesystem::remove(
                        assetDirectory / "BrokenDirect.material.json.meta")
                    && std::filesystem::remove(
                        assetDirectory / "shaders" / "BrokenDirect.hlsl")
                    && std::filesystem::remove(
                        assetDirectory / "shaders"
                            / "BrokenDirect.hlsl.meta")
                    && std::filesystem::remove(riggedModel)
                    && std::filesystem::remove(
                        assetDirectory / "CaseLower.asset")
                    && std::filesystem::remove(
                        assetDirectory / "CaseLower.asset.meta")
                    && std::filesystem::remove(
                        assetDirectory / "CaseUpper.asset")
                    && std::filesystem::remove(
                        assetDirectory / "CaseUpper.asset.META")
                    && std::filesystem::remove(
                        assetDirectory / "Ignored.asset.bak")
                    && std::filesystem::remove(
                        assetDirectory / "Ignored.asset.bak.meta"),
                "The temporary direct-HLSL fixtures could not be removed "
                "after their successful stripped export.");

            const auto strippedKey = ReadEmbeddedArchiveKey(
                strippedOutput / "LamaPonRuntime.dll");
            Require(
                strippedKey != embeddedKey,
                "Every export must get a fresh archive key.");
            const auto archive = LamaPon::AssetArchive::Open(
                strippedOutput / "assets.tpak",
                strippedKey);
            Require(
                archive != nullptr,
                "Stripped export produced no archive.");
            const auto packedSceneBytes =
                archive->TryRead(startupScene);
            const auto packedMaterialBytes =
                archive->TryRead("BrokenDirect.material.json");
            Require(
                packedSceneBytes.has_value()
                    && packedMaterialBytes.has_value(),
                "The GUID rewrite fixtures were not packed.");
            const auto packedScene = nlohmann::json::parse(
                packedSceneBytes->begin(),
                packedSceneBytes->end());
            const auto packedMaterial = nlohmann::json::parse(
                packedMaterialBytes->begin(),
                packedMaterialBytes->end());
            Require(
                packedScene.at("materialAsset").get<std::string>()
                        == "BrokenDirect.material.json"
                    && packedMaterial.at("shader").get<std::string>()
                        == "shaders/BrokenDirect.hlsl"
                    && packedScene.at("caseAsset").get<std::string>()
                        == "CaseUpper.asset"
                    && packedScene.at("ignoredAsset").get<std::string>()
                        == "BackupFallback.asset",
                "GUID-resolved paths were not written into the packed "
                "JSON fallbacks with case-sensitive GUID semantics for "
                "the archive-only runtime.");
            Require(
                !archive->Contains("Ignored.asset.bak"),
                "A backup asset was included in the archive.");
            Require(
                !archive->Contains("CaseUpper.asset.META"),
                "Uppercase asset metadata was included in the archive.");
            Require(
                !archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestExport.hlsl"),
                "HLSL source must be excluded when"
                    " stripShaderSourceOnExport is set.");
            Require(
                !archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestCommon.hlsli"),
                "HLSL includes must be excluded too.");
            Require(
                archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestExport.lamashader.json"),
                "Shader manifests must remain in a source-stripped"
                    " export.");
            Require(
                archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestMaterialExport.lamashader.json"),
                "Material shader manifests must remain in a"
                    " source-stripped export.");
            Require(
                archive->Contains(
                    std::filesystem::path{ "shaders" }
                        / "TestComputeExport.lamashader.json"),
                "Compute shader manifests must remain in a"
                    " source-stripped export.");
            Require(
                archive->Contains(startupScene),
                "Stripping shaders must not drop other"
                    " assets.");
            // ソースが無いとハッシュからキーを作れないので、
            // パスから引ける索引が要ります。
            Require(
                std::filesystem::is_regular_file(
                    strippedOutput
                    / "shader-cache"
                    / "index.txt"),
                "A stripped export must ship the shader cache"
                    " index; without it the bytecode cannot be"
                    " looked up at runtime.");
            const auto cacheIndex = ReadSealedText(
                strippedOutput / "shader-cache" / "index.txt",
                strippedKey);
            Require(
                cacheIndex.find(
                    "shaders/brokendirect.hlsl|VSSkinnedMain|vs_5_0|")
                        != std::string::npos
                    && cacheIndex.find(
                        "shaders/brokendirect.hlsl|PSSkinnedMain|ps_5_0|")
                        != std::string::npos,
                "A valid skinned-only ModelRenderer HLSL was not cached.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSMain|vs_5_0|")
                    != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|PSMain|ps_5_0|")
                        != std::string::npos,
                "The stripped export cache index omitted the"
                    " direct material/screen HLSL entry points.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSSkinnedMain|"
                    "vs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|PSSkinnedMain|"
                        "ps_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " direct skinned material HLSL entry points.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|GSMain|gs_5_0|")
                    != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|PSOccluded|"
                        "ps_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " direct geometry/occluded HLSL entry points.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|CSMain|cs_5_0|")
                    != std::string::npos,
                "The stripped export cache index omitted the"
                    " direct compute HLSL entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSMain|vs_5_0|"
                    "EXPORT_MULTI_ON+") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|PSMain|ps_5_0|"
                        "EXPORT_MULTI_ON+") != std::string::npos,
                "The stripped export cache index omitted a"
                    " direct material HLSL keyword variant.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSManifestMain|"
                    "vs_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " manifest vertex entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|PSManifestMain|"
                    "ps_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " manifest pixel entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSMaterialManifest|"
                    "vs_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " material manifest vertex entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|PSMaterialManifest|"
                    "ps_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " material manifest pixel entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|CSManifestMain|"
                    "cs_5_0|") != std::string::npos,
                "The stripped export cache index omitted the"
                    " compute manifest entry point.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSMaterialManifest|"
                    "vs_5_0|EXPORT_MULTI_ON+")
                    != std::string::npos,
                "The stripped export cache index omitted a"
                    " material manifest keyword variant.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|PSMaterialManifest|"
                    "ps_5_0|EXPORT_MULTI_ON+")
                    != std::string::npos,
                "The stripped export cache index omitted the"
                    " pixel stage of a material manifest keyword"
                    " variant.");
            Require(
                cacheIndex.find(
                    "shaders/testexport.hlsl|VSSecondForward|"
                    "vs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|GSManifestForward|"
                        "gs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|VSManifestInstanced|"
                        "vs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|VSManifestSkinned|"
                        "vs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|VSManifestOutline|"
                        "vs_5_0|") != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|"
                        "VSManifestSkinnedOutline|vs_5_0|")
                        != std::string::npos
                    && cacheIndex.find(
                        "shaders/testexport.hlsl|PSManifestOccluded|"
                        "ps_5_0|") != std::string::npos,
                "The stripped export cache index omitted a"
                    " non-primary material manifest pass.");

            bool sourceMetadataFound{};
            std::istringstream indexLines(cacheIndex);
            std::string indexLine;
            while (std::getline(indexLines, indexLine))
            {
                constexpr std::string_view prefix = "@metadata ";
                if (indexLine.rfind(prefix, 0) != 0)
                {
                    continue;
                }
                const auto metadata = nlohmann::json::parse(
                    indexLine.substr(prefix.size()));
                if (metadata.value("path", std::string{})
                    != "shaders/testexport.hlsl")
                {
                    continue;
                }
                sourceMetadataFound = true;
                const auto& state = metadata.at("renderState");
                Require(
                    metadata.value("version", 0) == 1
                        && state.at("blend").get<int>()
                            == static_cast<int>(
                                LamaPon::ShaderBlendMode::Additive)
                        && state.at("cull").get<int>()
                            == static_cast<int>(
                                LamaPon::ShaderCullMode::None)
                        && !state.at("depthWrite").get<bool>()
                        && !state.at("depthTest").get<bool>()
                        && state.at("declared").get<bool>(),
                    "The stripped export cache metadata did not"
                        " preserve the direct HLSL render state.");
                const auto& groups =
                    metadata.at("variants").at("groups");
                Require(
                    groups.is_array() && groups.size() == 2
                        && groups[0].at("keywords")[1]
                            == "EXPORT_MULTI_ON"
                        && groups[1].at("keywords")[1]
                            == "EXPORT_FEATURE_ON",
                    "The stripped export cache metadata did not"
                        " preserve the direct HLSL variant declaration.");
            }
            Require(
                sourceMetadataFound,
                "The stripped export cache index omitted HLSL"
                    " source metadata.");

            // 配布先processではDLL内の鍵で自動復号されます。このtest
            // processは別の鍵を持つため、書き出し鍵で既存cache fileを
            // 同じ場所へ平文化してから、source無しのruntime lookupを
            // 実際に通します（新しい作業folder/fileは作りません）。
            const auto shaderCacheDirectory =
                strippedOutput / "shader-cache";
            for (const auto& cacheFile :
                std::filesystem::directory_iterator(
                    shaderCacheDirectory))
            {
                if (!cacheFile.is_regular_file())
                {
                    continue;
                }
                const auto sealed = ReadBytes(cacheFile.path());
                if (!LamaPon::Crypto::IsSealed(
                        sealed.data(),
                        sealed.size()))
                {
                    continue;
                }
                const auto plain = LamaPon::Crypto::Unseal(
                    sealed.data(),
                    sealed.size(),
                    strippedKey);
                Require(
                    plain.has_value(),
                    "An exported shader-cache file could not be"
                        " opened for runtime lookup testing.");
                WriteBytes(cacheFile.path(), *plain);
            }

            const HRESULT comResult = CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);
            const bool uninitializeCom = SUCCEEDED(comResult);
            Require(
                SUCCEEDED(comResult)
                    || comResult == RPC_E_CHANGED_MODE,
                "COM initialization failed for source-stripped"
                    " shader runtime lookup testing.");
            {
                LamaPon::AssetManager strippedAssets(nullptr, nullptr);
                // このpathは意図的に作りません。HLSLが存在しない状態で
                // index lookupだけを使うためのasset rootです。
                strippedAssets.SetAssetRoot(
                    strippedOutput / "unpacked-assets-not-created");
                LamaPon::ClearShaderCacheSearchDirectories();
                LamaPon::AddShaderCacheSearchDirectory(
                    shaderCacheDirectory);
                const auto sourcePath = strippedAssets.ResolvePath(
                    "Shaders/TESTEXPORT.HLSL");
                const auto requireCachedShader =
                    [&](const char* entryPoint,
                        const char* target,
                        std::vector<std::string> keywords = {})
                    {
                        const auto blob = LamaPon::CompileShaderCached(
                            strippedAssets,
                            sourcePath,
                            entryPoint,
                            target,
                            keywords);
                        Require(
                            blob && blob->GetBufferSize() != 0,
                            "A source-stripped shader cache entry"
                                " could not be loaded at runtime.");
                    };
                requireCachedShader(
                    "VSMain",
                    "vs_5_0",
                    { "EXPORT_MULTI_ON" });
                requireCachedShader("GSMain", "gs_5_0");
                requireCachedShader("PSOccluded", "ps_5_0");
                requireCachedShader(
                    "VSSecondForward",
                    "vs_5_0");
                requireCachedShader(
                    "PSManifestOccluded",
                    "ps_5_0");

                LamaPon::ShaderRenderState restoredState;
                LamaPon::ShaderVariantDeclaration restoredVariants;
                Require(
                    LamaPon::LoadPrecompiledShaderMetadata(
                        strippedAssets,
                        sourcePath,
                        &restoredState,
                        &restoredVariants)
                        && restoredState.declared
                        && restoredState.blend
                            == LamaPon::ShaderBlendMode::Additive
                        && restoredState.cull
                            == LamaPon::ShaderCullMode::None
                        && !restoredState.depthWrite
                        && !restoredState.depthTest
                        && restoredVariants.groups.size() == 2,
                    "Source-stripped runtime lookup did not restore"
                        " shader metadata.");
                LamaPon::ClearShaderCacheSearchDirectories();
            }
            if (uninitializeCom)
            {
                CoUninitialize();
            }

            // 最後にfake runtimeを実ビルドへ差し替え、同じStripped出力を
            // 再利用してexportします。配布先exeをそのフォルダーから起動する
            // ことで、埋込鍵 -> sealed index/CSO -> bytecode/metadata lookupを
            // 実際のLamaPonRuntime.dll内で検証します。
            const auto buildDirectory =
                SelfExecutablePath().parent_path();
            const auto realRuntime =
                buildDirectory / "LamaPonRuntime.dll";
            const auto realAudioRuntime =
                buildDirectory / "xaudio2_9redist.dll";
            Require(
                std::filesystem::is_regular_file(realRuntime)
                    && std::filesystem::is_regular_file(realAudioRuntime),
                "The real runtime dependencies for the exported cache "
                "probe were not found.");
            std::filesystem::copy_file(
                realRuntime,
                runtimeDirectory / "LamaPonRuntime.dll",
                std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(
                realAudioRuntime,
                runtimeDirectory / "xaudio2_9redist.dll",
                std::filesystem::copy_options::overwrite_existing);
            static_cast<void>(LamaPon::ExportGamePackage(
                LamaPon::GameExportOptions{
                    runtimeDirectory,
                    assetDirectory,
                    strippedOutput,
                    strippedSettings
                }));
            RunChildProcess(
                strippedOutput / L"日本語ゲーム.exe",
                strippedOutput,
                L"--shader-cache-probe");
        }

        std::cout << "Game exporter tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
