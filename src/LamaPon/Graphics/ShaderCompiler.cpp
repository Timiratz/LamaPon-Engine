#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Graphics/ShaderVariants.h"

#include <nlohmann/json.hpp>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    std::atomic<bool> g_cacheEnabled{ true };
    std::mutex g_searchMutex;
    // 読み取り専用のキャッシュ置き場（書き出したゲームへ同梱した
    // ものなど）。書き込みは常にShaderCacheDirectory()の側だけ。
    std::vector<std::filesystem::path> g_searchDirectories;
    // 事前コンパイル中は専用ディレクトリへ書き込み、読み取りも
    // 止めます（キャッシュに当たると何も書かれず、配布フォルダーへ
    // ファイルが出来ないため）。
    // Export用のcompile contextは呼び出しthreadだけへ適用します。
    // Editorの非同期warm-upが同時に走っても、配布先cacheへ混ざりません。
    thread_local std::filesystem::path g_writeOverride;
    thread_local bool g_forceCompile{};

    class ScopedPrecompileContext final
    {
    public:
        explicit ScopedPrecompileContext(
            const std::filesystem::path& destination)
            : m_previousWriteOverride(g_writeOverride)
            , m_previousForceCompile(g_forceCompile)
        {
            g_writeOverride = destination;
            g_forceCompile = true;
        }

        ~ScopedPrecompileContext()
        {
            g_forceCompile = m_previousForceCompile;
            g_writeOverride = std::move(m_previousWriteOverride);
        }

        ScopedPrecompileContext(const ScopedPrecompileContext&) = delete;
        ScopedPrecompileContext& operator=(
            const ScopedPrecompileContext&) = delete;

    private:
        std::filesystem::path m_previousWriteOverride;
        bool m_previousForceCompile{};
    };

    class ShaderCacheWriteFailure final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
    struct ShaderSourceMetadata final
    {
        LamaPon::ShaderRenderState renderState;
        LamaPon::ShaderVariantDeclaration variants;
    };

    struct PendingShaderCacheIndex final
    {
        std::vector<std::pair<std::string, std::string>> entries;
        std::unordered_map<std::string, ShaderSourceMetadata>
            sourceMetadata;
    };

    // 事前コンパイル中に貯める索引とHLSL由来metadata。並行する
    // Export同士で混ざらないよう、正規化した出力先ごとに分離します。
    // WriteShaderCacheIndexが該当する出力先だけを取り出します。
    std::mutex g_pendingMutex;
    std::unordered_map<std::string, PendingShaderCacheIndex>
        g_pendingByDestination;

    // 読み込んだ索引。ソースが無いときの引き先です。
    std::mutex g_indexMutex;
    std::unordered_map<std::string, std::string> g_cacheIndex;
    std::unordered_map<std::string, ShaderSourceMetadata>
        g_sourceMetadata;
    std::mutex g_cacheFileWriteMutex;

    struct LiveCompileDependencies final
    {
        std::string defineKey;
        std::vector<std::filesystem::path> files;
    };

    struct LiveShaderDependencies final
    {
        std::string sourceHash;
        std::unordered_map<std::string, LiveCompileDependencies>
            compileIdentities;
    };

    // hot reload用の依存一覧。D3DCompileが実際に開いたincludeをcompile
    // identityごとに保持します。cacheを無効にした開発環境でも機能する
    // よう、disk cacheとは独立したprocess内registryです。
    std::mutex g_liveDependencyMutex;
    std::unordered_map<std::string, LiveShaderDependencies>
        g_liveDependencies;

    [[nodiscard]] std::string NormalizeIndexedAssetPath(
        std::string path)
    {
        std::ranges::transform(
            path,
            path.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return path;
    }

    [[nodiscard]] std::string PendingDestinationKey(
        std::filesystem::path directory)
    {
        std::error_code error;
        auto absolute = std::filesystem::absolute(directory, error);
        if (!error)
        {
            directory = std::move(absolute);
        }
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(directory.lexically_normal()));
    }

    [[nodiscard]] std::string MakeIndexKey(
        const std::string& relativePath,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& defines)
    {
        std::string key = relativePath;
        key.push_back('|');
        key.append(entryPoint != nullptr ? entryPoint : "");
        key.push_back('|');
        key.append(target != nullptr ? target : "");
        key.push_back('|');
        // definesの順は呼び出し側で整列済み（ShaderKeywordSet）。
        for (const auto& define : defines)
        {
            key.append(define);
            key.push_back('+');
        }
        return key;
    }

    [[nodiscard]] std::string MakeDefineKey(
        const std::vector<std::string>& defines)
    {
        std::string key;
        for (const auto& define : defines)
        {
            key.append(std::to_string(define.size()));
            key.push_back(':');
            key.append(define);
            key.push_back(';');
        }
        return key;
    }

    [[nodiscard]] std::string MakeLiveSourceKey(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path)
    {
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(
                assets.ResolvePath(path).lexically_normal()));
    }

    [[nodiscard]] std::string MakeLiveCompileIdentity(
        const char* const entryPoint,
        const char* const target,
        const std::string_view defineKey)
    {
        std::string key;
        const auto append = [&key](const std::string_view value)
        {
            key.append(std::to_string(value.size()));
            key.push_back(':');
            key.append(value);
            key.push_back(';');
        };
        append(entryPoint != nullptr
            ? std::string_view{ entryPoint }
            : std::string_view{});
        append(target != nullptr
            ? std::string_view{ target }
            : std::string_view{});
        append(defineKey);
        return key;
    }
    std::mutex g_statisticsMutex;
    LamaPon::ShaderCompileStats g_statistics{};
    std::mutex g_compileCompletionHookMutex;
    std::function<void()> g_compileCompletionHook;

    void RunShaderCompileCompletionHook()
    {
        std::function<void()> hook;
        {
            const std::lock_guard<std::mutex> lock(
                g_compileCompletionHookMutex);
            hook = std::move(g_compileCompletionHook);
            g_compileCompletionHook = {};
        }
        if (hook)
        {
            hook();
        }
    }

    // キャッシュ形式を変更した場合は版番号を更新します。古いファイルは
    // キーが変わって参照されなくなるので、消さなくても害はありません。
    constexpr int CacheFormatVersion = 2;

    // 内容のSHA-256を16進文字列で返します。異なるシェーダーの
    // バイトコードを共有しないよう、キャッシュの識別に使います。
    [[nodiscard]] std::string HashBytes(
        const std::uint8_t* data,
        const std::size_t size)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0)))
        {
            return {};
        }
        std::array<std::uint8_t, 32> digest{};
        const NTSTATUS status = BCryptHash(
            algorithm,
            nullptr,
            0,
            const_cast<PUCHAR>(data),
            static_cast<ULONG>(size),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!BCRYPT_SUCCESS(status))
        {
            return {};
        }
        static constexpr char Digits[] = "0123456789abcdef";
        std::string text;
        text.reserve(digest.size() * 2);
        for (const auto byte : digest)
        {
            text.push_back(Digits[byte >> 4]);
            text.push_back(Digits[byte & 0x0F]);
        }
        return text;
    }

    [[nodiscard]] std::string HashBytes(
        const std::vector<std::uint8_t>& data)
    {
        return HashBytes(data.data(), data.size());
    }

    // アセットルートからの相対パス（UTF-8）。ルート外や失敗時は
    // 絶対パスのまま返します。
    [[nodiscard]] std::string RelativeToAssetRoot(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path)
    {
        const auto& root = assets.AssetRoot();
        if (!root.empty())
        {
            std::error_code error;
            const auto relative = std::filesystem::relative(
                path,
                root,
                error);
            if (!error
                && !relative.empty()
                && relative.native().rfind(L"..", 0) != 0)
            {
                return NormalizeIndexedAssetPath(
                    LamaPon::PathToUtf8(
                        relative.lexically_normal()));
            }
        }
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(path.lexically_normal()));
    }

    void RememberLiveShaderDependencies(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& sourcePath,
        const char* const entryPoint,
        const char* const target,
        const std::vector<std::string>& defines,
        const std::vector<std::uint8_t>& source,
        const std::vector<std::pair<std::string, std::string>>&
            dependencies,
        const std::vector<std::filesystem::path>& missingDependencies = {})
    {
        const auto sourceKey = MakeLiveSourceKey(assets, sourcePath);
        const auto defineKey = MakeDefineKey(defines);
        const auto identity = MakeLiveCompileIdentity(
            entryPoint,
            target,
            defineKey);

        std::vector<std::filesystem::path> files;
        files.reserve(
            dependencies.size() + missingDependencies.size() + 1);
        files.push_back(
            assets.ResolvePath(sourcePath).lexically_normal());
        for (const auto& [dependency, hash] : dependencies)
        {
            static_cast<void>(hash);
            files.push_back(
                assets.ResolvePath(LamaPon::PathFromUtf8(dependency))
                    .lexically_normal());
        }
        for (const auto& dependency : missingDependencies)
        {
            files.push_back(
                assets.ResolvePath(dependency).lexically_normal());
        }
        std::ranges::sort(
            files,
            [](const auto& left, const auto& right)
            {
                return NormalizeIndexedAssetPath(
                    LamaPon::PathToUtf8(left))
                    < NormalizeIndexedAssetPath(
                        LamaPon::PathToUtf8(right));
            });
        files.erase(
            std::unique(
                files.begin(),
                files.end(),
                [](const auto& left, const auto& right)
                {
                    return NormalizeIndexedAssetPath(
                        LamaPon::PathToUtf8(left))
                        == NormalizeIndexedAssetPath(
                            LamaPon::PathToUtf8(right));
                }),
            files.end());

        const auto sourceHash = HashBytes(source);
        const std::lock_guard<std::mutex> lock(
            g_liveDependencyMutex);
        auto& tracked = g_liveDependencies[sourceKey];
        if (tracked.sourceHash != sourceHash)
        {
            tracked.sourceHash = sourceHash;
            tracked.compileIdentities.clear();
        }
        tracked.compileIdentities.insert_or_assign(
            identity,
            LiveCompileDependencies{
                defineKey,
                std::move(files)
            });
    }

    void RecordShaderSourceMetadata(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory)
    {
        const auto relativePath = RelativeToAssetRoot(assets, path);
        const auto destinationKey =
            PendingDestinationKey(destinationDirectory);
        if (!assets.FileExists(path))
        {
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            const auto pending =
                g_pendingByDestination.find(destinationKey);
            if (pending != g_pendingByDestination.end()
                && pending->second.sourceMetadata.contains(
                    relativePath))
            {
                return;
            }
        }

        try
        {
            const auto source = assets.ReadFileBytesFresh(path);
            const std::string_view text{
                reinterpret_cast<const char*>(source.data()),
                source.size()
            };
            ShaderSourceMetadata metadata;
            metadata.renderState =
                LamaPon::ParseShaderRenderState(text);
            metadata.variants = LamaPon::ParseShaderVariants(text);
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pendingByDestination[destinationKey]
                .sourceMetadata.try_emplace(
                relativePath,
                std::move(metadata));
        }
        catch (const std::exception&)
        {
            // 事前コンパイル本体が同じ読み取り失敗を診断します。
            // metadataの抽出だけで別の例外は増やしません。
        }
    }

    [[nodiscard]] nlohmann::json SerializeShaderSourceMetadata(
        const std::string& path,
        const ShaderSourceMetadata& metadata)
    {
        nlohmann::json groups = nlohmann::json::array();
        for (const auto& group : metadata.variants.groups)
        {
            groups.push_back({
                { "kind", static_cast<int>(group.kind) },
                { "keywords", group.keywords }
            });
        }
        return {
            { "version", 1 },
            { "path", path },
            { "renderState", {
                { "blend", static_cast<int>(
                    metadata.renderState.blend) },
                { "cull", static_cast<int>(
                    metadata.renderState.cull) },
                { "depthWrite", metadata.renderState.depthWrite },
                { "depthTest", metadata.renderState.depthTest },
                { "declared", metadata.renderState.declared }
            } },
            { "variants", {
                { "groups", std::move(groups) },
                { "error", metadata.variants.error }
            } }
        };
    }

    [[nodiscard]] bool DeserializeShaderSourceMetadata(
        const nlohmann::json& document,
        std::string& path,
        ShaderSourceMetadata& metadata)
    {
        if (!document.is_object()
            || document.value("version", 0) != 1
            || !document.contains("path")
            || !document["path"].is_string()
            || !document.contains("renderState")
            || !document["renderState"].is_object()
            || !document.contains("variants")
            || !document["variants"].is_object())
        {
            return false;
        }

        const auto& state = document["renderState"];
        const int blend = state.value("blend", -1);
        const int cull = state.value("cull", -1);
        if (blend < static_cast<int>(
                LamaPon::ShaderBlendMode::Opaque)
            || blend > static_cast<int>(
                LamaPon::ShaderBlendMode::Premultiplied)
            || cull < static_cast<int>(
                LamaPon::ShaderCullMode::Back)
            || cull > static_cast<int>(
                LamaPon::ShaderCullMode::None)
            || !state.contains("depthWrite")
            || !state["depthWrite"].is_boolean()
            || !state.contains("depthTest")
            || !state["depthTest"].is_boolean()
            || !state.contains("declared")
            || !state["declared"].is_boolean())
        {
            return false;
        }

        ShaderSourceMetadata parsed;
        parsed.renderState.blend =
            static_cast<LamaPon::ShaderBlendMode>(blend);
        parsed.renderState.cull =
            static_cast<LamaPon::ShaderCullMode>(cull);
        parsed.renderState.depthWrite =
            state["depthWrite"].get<bool>();
        parsed.renderState.depthTest =
            state["depthTest"].get<bool>();
        parsed.renderState.declared =
            state["declared"].get<bool>();

        const auto& variants = document["variants"];
        if (!variants.contains("groups")
            || !variants["groups"].is_array())
        {
            return false;
        }
        parsed.variants.error = variants.value(
            "error",
            std::string{});
        for (const auto& value : variants["groups"])
        {
            if (!value.is_object()
                || !value.contains("kind")
                || !value["kind"].is_number_integer()
                || !value.contains("keywords")
                || !value["keywords"].is_array())
            {
                return false;
            }
            const int kind = value["kind"].get<int>();
            if (kind < static_cast<int>(
                    LamaPon::ShaderVariantKind::MultiCompile)
                || kind > static_cast<int>(
                    LamaPon::ShaderVariantKind::ShaderFeature))
            {
                return false;
            }
            LamaPon::ShaderVariantGroup group;
            group.kind =
                static_cast<LamaPon::ShaderVariantKind>(kind);
            try
            {
                group.keywords = value["keywords"]
                    .get<std::vector<std::string>>();
            }
            catch (const std::exception&)
            {
                return false;
            }
            parsed.variants.groups.push_back(std::move(group));
        }

        path = NormalizeIndexedAssetPath(
            document["path"].get<std::string>());
        if (path.empty())
        {
            return false;
        }
        metadata = std::move(parsed);
        return true;
    }

    // 通常フォルダーとパッケージ化済みアセットで同じHLSLを使えるよう、
    // 相対#includeもAssetManager経由で解決します。
    //
    // キャッシュのために「実際に開いたファイルとその中身のハッシュ」を
    // 記録します。#includeした側だけを直したときにキャッシュが古い
    // ままにならないようにするためです。
    class RecordingInclude final : public ID3DInclude
    {
    public:
        RecordingInclude(
            LamaPon::AssetManager& assets,
            std::filesystem::path rootShader)
            : m_assets(assets)
            , m_rootShader(std::move(rootShader))
        {
        }

        HRESULT Open(
            const D3D_INCLUDE_TYPE includeType,
            const LPCSTR fileName,
            const LPCVOID parentData,
            LPCVOID* data,
            UINT* bytes) override
        {
            if (fileName == nullptr
                || data == nullptr
                || bytes == nullptr)
            {
                return E_INVALIDARG;
            }

            std::filesystem::path parent =
                m_rootShader.parent_path();
            if (includeType == D3D_INCLUDE_LOCAL
                && parentData != nullptr)
            {
                const auto found =
                    m_parentDirectories.find(parentData);
                if (found != m_parentDirectories.end())
                {
                    parent = found->second;
                }
            }

            auto path =
                (parent / std::filesystem::path(fileName))
                    .lexically_normal();
            const auto localCandidate = path;
            if (!m_assets.FileExists(path))
            {
                path = m_assets.ResolvePath(
                    std::filesystem::path(fileName))
                    .lexically_normal();
                // 現在はasset-root fallbackを使えても、後からlocal側へ
                // 同名fileが作られればinclude解決の優先先が変わります。
                // 不在候補も依存へ残し、その出現でcache/hot reloadを
                // 無効化します。
                if (path != localCandidate)
                {
                    m_missingDependencies.push_back(localCandidate);
                }
            }
            if (!m_assets.FileExists(path))
            {
                if (path == localCandidate)
                {
                    m_missingDependencies.push_back(localCandidate);
                }
                else
                {
                    m_missingDependencies.push_back(path);
                }
                return E_FAIL;
            }

            try
            {
                auto source = m_assets.ReadFileBytesFresh(path);
                // パスはアセットルート相対で記録します。書き出し時は
                // プロジェクトのassetsフォルダー、実行時はassets.tpakの
                // 中と、同じファイルでも絶対パスが変わるためです。
                // 相対で持てば、同梱したキャッシュがそのまま通ります。
                m_dependencies.emplace_back(
                    RelativeToAssetRoot(m_assets, path),
                    HashBytes(source));
                auto storage =
                    std::make_unique<
                        std::vector<std::uint8_t>>(
                            std::move(source));
                const void* pointer = storage->data();
                *data = pointer;
                *bytes = static_cast<UINT>(storage->size());
                m_parentDirectories[pointer] =
                    path.parent_path();
                m_sources[pointer] = std::move(storage);
                return S_OK;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

        HRESULT Close(const LPCVOID data) override
        {
            m_parentDirectories.erase(data);
            m_sources.erase(data);
            return S_OK;
        }

        [[nodiscard]] const std::vector<
            std::pair<std::string, std::string>>&
            Dependencies() const noexcept
        {
            return m_dependencies;
        }

        [[nodiscard]] const std::vector<std::filesystem::path>&
            MissingDependencies() const noexcept
        {
            return m_missingDependencies;
        }

    private:
        LamaPon::AssetManager& m_assets;
        std::filesystem::path m_rootShader;
        std::unordered_map<
            const void*,
            std::filesystem::path> m_parentDirectories;
        std::unordered_map<
            const void*,
            std::unique_ptr<std::vector<std::uint8_t>>>
            m_sources;
        // 開いた順の (パス, 中身のハッシュ)。
        std::vector<std::pair<std::string, std::string>>
            m_dependencies;
        std::vector<std::filesystem::path>
            m_missingDependencies;
    };

    [[nodiscard]] UINT CompileFlags() noexcept
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG
            | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        return flags;
    }

    [[nodiscard]] bool IsGeometryShaderTarget(
        const std::string_view target) noexcept
    {
        return target.size() >= 3
            && std::tolower(static_cast<unsigned char>(target[0])) == 'g'
            && std::tolower(static_cast<unsigned char>(target[1])) == 's'
            && target[2] == '_';
    }

    void ValidateGeometryShaderInput(
        ID3DBlob* const byteCode,
        const std::string_view entryPoint)
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        const auto reflectionResult = D3DReflect(
            byteCode->GetBufferPointer(),
            byteCode->GetBufferSize(),
            IID_ID3D11ShaderReflection,
            &reflection);
        if (FAILED(reflectionResult))
        {
            throw std::runtime_error(
                "Could not inspect geometry shader entry '"
                + std::string{ entryPoint }
                + "' while preparing the export.");
        }

        D3D11_SHADER_DESC shaderDescription{};
        if (FAILED(reflection->GetDesc(&shaderDescription)))
        {
            throw std::runtime_error(
                "Could not read geometry shader entry '"
                + std::string{ entryPoint }
                + "' while preparing the export.");
        }
        if (shaderDescription.InputPrimitive
            != D3D_PRIMITIVE_TRIANGLE)
        {
            throw std::runtime_error(
                "Geometry shader entry '"
                + std::string{ entryPoint }
                + "' must take triangle input; LamaPon material "
                  "renderers do not submit point or line input.");
        }
    }

    // キャッシュのファイルを1つ読みます。
    //
    // 書き出したゲームへ同梱するキャッシュは暗号化されています
    // （HLSLソースを外しても、DXBCがそのまま置いてあれば中身は
    // 読めてしまうため）。開発中の%LOCALAPPDATA%側は平文のままなので、
    // 先頭を見てどちらかを判断します。
    //
    // 復号できないときはキャッシュ不在として扱い、通常のコンパイルへ
    // フォールバックします。キャッシュの不具合で描画を停止させません。
    [[nodiscard]] std::optional<std::vector<std::uint8_t>>
        ReadCacheFile(const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            return std::nullopt;
        }
        const auto end = input.tellg();
        if (end < 0)
        {
            return std::nullopt;
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
                return std::nullopt;
            }
        }
        if (LamaPon::Crypto::IsSealed(
                bytes.data(),
                bytes.size()))
        {
            return LamaPon::Crypto::Unseal(
                bytes.data(),
                bytes.size(),
                LamaPon::Crypto::ArchiveKey());
        }
        return bytes;
    }

    [[nodiscard]] std::string ReadCacheText(
        const std::filesystem::path& path)
    {
        const auto bytes = ReadCacheFile(path);
        if (!bytes)
        {
            return {};
        }
        return std::string(bytes->begin(), bytes->end());
    }

    // 依存ファイルの一覧が今も同じ中身かどうか。
    [[nodiscard]] bool DependenciesMatch(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& dependencyFile,
        std::vector<std::pair<std::string, std::string>>*
            matchedDependencies = nullptr)
    {
        const auto manifest = ReadCacheFile(dependencyFile);
        if (!manifest)
        {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> matched;
        std::istringstream input(
            std::string(manifest->begin(), manifest->end()));
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            const auto separator = line.find(' ');
            if (separator == std::string::npos)
            {
                return false;
            }
            const std::string expected =
                line.substr(0, separator);
            const auto dependency = line.substr(separator + 1);
            const auto path = assets.ResolvePath(
                LamaPon::PathFromUtf8(
                    dependency));
            if (expected == "-")
            {
                if (assets.FileExists(path))
                {
                    return false;
                }
                matched.emplace_back(dependency, expected);
                continue;
            }
            if (!assets.FileExists(path))
            {
                return false;
            }
            try
            {
                if (HashBytes(assets.ReadFileBytesFresh(path))
                    != expected)
                {
                    return false;
                }
            }
            catch (...)
            {
                return false;
            }
            matched.emplace_back(dependency, expected);
        }
        if (matchedDependencies != nullptr)
        {
            *matchedDependencies = std::move(matched);
        }
        return true;
    }

    // 失敗キャッシュには、入口が存在しないX3501だけを記録します。
    // その他の失敗は環境や依存ファイルの修正で解消される可能性があり、
    // キャッシュすると修正後も失敗が返り続けるためです。
    // X3501を記録するのは、LitEffectが
    // VSInstancedMain/GSMain/HSMain/DSMain/outline/occluded入口を
    // 「あれば使う」方式で毎回試すからです（下の呼び出し側を参照）。
    // 存在しない入口の探索コストを避けるためです。未知の失敗は記録
    // しないよう、許可リストで判定します。
    [[nodiscard]] bool ShouldRememberFailure(
        const std::string& details)
    {
        return details.find("X3501") != std::string::npos
            || details.find("entrypoint not found")
                != std::string::npos;
    }

    // 索引からバイトコードを読みます（ソースが無いとき用）。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
        LoadFromIndexInDirectories(
            const std::string& indexKey,
            const std::vector<std::filesystem::path>&
                directories)
    {
        std::string key;
        {
            const std::lock_guard<std::mutex> lock(
                g_indexMutex);
            const auto found = g_cacheIndex.find(indexKey);
            if (found == g_cacheIndex.end())
            {
                return {};
            }
            key = found->second;
        }
        for (const auto& directory : directories)
        {
            const auto byteCodePath =
                directory / (key + ".cso");
            std::error_code error;
            if (!std::filesystem::exists(byteCodePath, error))
            {
                continue;
            }
            const auto bytes = ReadCacheFile(byteCodePath);
            if (!bytes || bytes->empty())
            {
                continue;
            }
            Microsoft::WRL::ComPtr<ID3DBlob> blob;
            if (FAILED(D3DCreateBlob(
                    static_cast<SIZE_T>(bytes->size()),
                    blob.ReleaseAndGetAddressOf())))
            {
                continue;
            }
            std::memcpy(
                blob->GetBufferPointer(),
                bytes->data(),
                bytes->size());
            return blob;
        }
        return {};
    }

    [[nodiscard]] bool WriteFileAtomically(
        const std::filesystem::path& destination,
        const void* data,
        const std::size_t size)
    {
        // 不完全なキャッシュを残さないよう、別名で書いてから
        // 保存先へ置き換えます。
        auto temporary = destination;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output.write(
                static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
            output.flush();
            if (!output)
            {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            output.close();
            if (!output)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    // 依存の一覧と、結果（バイトコードまたはエラーメッセージ）を書きます。
    // successPathとfailurePathは一方だけを残し、矛盾する結果の再利用を防ぎます。
    [[nodiscard]] bool WriteCacheEntry(
        const std::filesystem::path& cacheDirectory,
        const std::filesystem::path& dependencyPath,
        const std::filesystem::path& writePath,
        LamaPon::AssetManager& assets,
        const std::vector<std::uint8_t>& source,
        const std::filesystem::path& sourcePath,
        const std::vector<std::pair<std::string, std::string>>&
            dependencies,
        const std::vector<std::filesystem::path>&
            missingDependencies,
        const void* payload,
        const std::size_t payloadSize)
    {
        if (writePath.empty() || cacheDirectory.empty())
        {
            return false;
        }
        const std::lock_guard<std::mutex> writeLock(
            g_cacheFileWriteMutex);
        std::error_code error;
        if (!LamaPon::EnsureDirectoryExists(
                cacheDirectory,
                error))
        {
            return false;
        }
        auto other = writePath;
        other.replace_extension(
            writePath.extension() == L".cso"
                ? L".fail"
                : L".cso");
        // 同じkeyの古いpayloadが残ったまま依存一覧だけを先に
        // 置き換えると、途中終了時に古いDXBCを新しいinclude内容の
        // 結果と誤認できます。結果を先に外してから組を作り直します。
        std::filesystem::remove(writePath, error);
        if (error)
        {
            return false;
        }
        std::filesystem::remove(other, error);
        if (error)
        {
            return false;
        }
        // 依存の一覧を先に書きます。逆順だと、間で落ちたときに
        // 「結果はあるが依存が古い」組み合わせが成立してしまいます。
        std::string manifest;
        manifest.append(HashBytes(source));
        manifest.push_back(' ');
        manifest.append(RelativeToAssetRoot(assets, sourcePath));
        manifest.push_back('\n');
        for (const auto& [dependencyName, hash] : dependencies)
        {
            manifest.append(hash);
            manifest.push_back(' ');
            manifest.append(dependencyName);
            manifest.push_back('\n');
        }
        for (const auto& dependency : missingDependencies)
        {
            manifest.append("- ");
            manifest.append(RelativeToAssetRoot(assets, dependency));
            manifest.push_back('\n');
        }
        if (!WriteFileAtomically(
                dependencyPath,
                manifest.data(),
                manifest.size())
            || !WriteFileAtomically(
                writePath,
                payload,
                payloadSize))
        {
            // 片方だけ新しくなると、古いDXBCを新しい依存一覧で
            // 正常と誤認できます。不完全な組は両方とも破棄します。
            std::filesystem::remove(dependencyPath, error);
            std::filesystem::remove(writePath, error);
            return false;
        }

        return true;
    }
}

namespace LamaPon
{
    std::filesystem::path ShaderCacheDirectory()
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
        return root / L"LamaPon" / L"shader-cache";
    }

    namespace
    {
        [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
            LoadFromIndex(
                AssetManager& assets,
                const std::filesystem::path& path,
                const char* entryPoint,
                const char* target,
                const std::vector<std::string>& defines)
        {
            std::vector<std::filesystem::path> directories;
            {
                const std::lock_guard<std::mutex> lock(
                    g_searchMutex);
                directories = g_searchDirectories;
            }
            directories.push_back(ShaderCacheDirectory());
            auto blob = LoadFromIndexInDirectories(
                MakeIndexKey(
                    RelativeToAssetRoot(assets, path),
                    entryPoint,
                    target,
                    defines),
                directories);
            if (blob)
            {
                const std::lock_guard<std::mutex> lock(
                    g_statisticsMutex);
                ++g_statistics.cacheHitCount;
            }
            return blob;
        }
    }

    void WriteShaderCacheIndex(
        const std::filesystem::path& directory)
    {
        if (directory.empty())
        {
            return;
        }
        std::vector<std::pair<std::string, std::string>> pendingIndex;
        std::unordered_map<std::string, ShaderSourceMetadata>
            pendingSourceMetadata;
        {
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            const auto pending = g_pendingByDestination.find(
                PendingDestinationKey(directory));
            if (pending == g_pendingByDestination.end())
            {
                return;
            }
            pendingIndex = std::move(pending->second.entries);
            pendingSourceMetadata =
                std::move(pending->second.sourceMetadata);
            g_pendingByDestination.erase(pending);
        }
        // 同じdestinationへ別のprecompile sessionが重なっても、先に
        // 確定した索引を後発sessionの部分集合で上書きしないよう、既存内容へ
        // 追記してからatomic replaceします。cache payloadの書き込みとも同じ
        // mutexで直列化し、索引だけが先に公開される隙間を作りません。
        const std::lock_guard<std::mutex> writeLock(
            g_cacheFileWriteMutex);
        std::string text = ReadCacheText(directory / L"index.txt");
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
        }
        for (const auto& [indexKey, cacheKey] : pendingIndex)
        {
            text.append(cacheKey);
            text.push_back(' ');
            text.append(indexKey);
            text.push_back('\n');
        }
        std::vector<std::string> metadataPaths;
        metadataPaths.reserve(pendingSourceMetadata.size());
        for (const auto& [path, metadata] :
            pendingSourceMetadata)
        {
            static_cast<void>(metadata);
            metadataPaths.push_back(path);
        }
        std::sort(metadataPaths.begin(), metadataPaths.end());
        for (const auto& path : metadataPaths)
        {
            const auto found = pendingSourceMetadata.find(path);
            if (found == pendingSourceMetadata.end())
            {
                continue;
            }
            text.append("@metadata ");
            text.append(SerializeShaderSourceMetadata(
                path,
                found->second).dump());
            text.push_back('\n');
        }
        std::error_code error;
        if (LamaPon::EnsureDirectoryExists(directory, error))
        {
            if (!WriteFileAtomically(
                    directory / L"index.txt",
                    text.data(),
                    text.size()))
            {
                throw ShaderCacheWriteFailure(
                    "Could not write the precompiled shader cache "
                    "index: "
                    + LamaPon::PathToUtf8(
                        directory / L"index.txt"));
            }
        }
        else
        {
            throw ShaderCacheWriteFailure(
                "Could not create the precompiled shader cache "
                "directory: "
                + LamaPon::PathToUtf8(directory));
        }
    }

    void AddShaderCacheSearchDirectory(
        std::filesystem::path directory)
    {
        if (directory.empty())
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_searchMutex);
        for (const auto& existing : g_searchDirectories)
        {
            if (existing == directory)
            {
                return;
            }
        }
        const auto indexPath = directory / L"index.txt";
        g_searchDirectories.push_back(std::move(directory));
        // 索引があれば読み込みます（ソースを外した配布物用）。
        const auto indexText = ReadCacheText(indexPath);
        if (indexText.empty())
        {
            return;
        }
        std::istringstream input(indexText);
        const std::lock_guard<std::mutex> indexLock(
            g_indexMutex);
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            constexpr std::string_view metadataPrefix =
                "@metadata ";
            if (line.rfind(metadataPrefix, 0) == 0)
            {
                try
                {
                    const auto document = nlohmann::json::parse(
                        line.begin()
                            + static_cast<std::ptrdiff_t>(
                                metadataPrefix.size()),
                        line.end());
                    std::string path;
                    ShaderSourceMetadata metadata;
                    if (DeserializeShaderSourceMetadata(
                            document,
                            path,
                            metadata))
                    {
                        g_sourceMetadata.insert_or_assign(
                            std::move(path),
                            std::move(metadata));
                    }
                }
                catch (const std::exception&)
                {
                    // 壊れたmetadata行だけを無視し、DXBC索引は読み続けます。
                }
                continue;
            }
            const auto separator = line.find(' ');
            if (separator == std::string::npos)
            {
                continue;
            }
            auto indexKey = line.substr(separator + 1);
            const auto pathSeparator = indexKey.find('|');
            if (pathSeparator != std::string::npos)
            {
                auto path = NormalizeIndexedAssetPath(
                    indexKey.substr(0, pathSeparator));
                indexKey.replace(0, pathSeparator, path);
            }
            g_cacheIndex.insert_or_assign(
                std::move(indexKey),
                line.substr(0, separator));
        }
    }

    void ClearShaderCacheSearchDirectories()
    {
        const std::lock_guard<std::mutex> lock(g_searchMutex);
        g_searchDirectories.clear();
        const std::lock_guard<std::mutex> indexLock(g_indexMutex);
        g_cacheIndex.clear();
        g_sourceMetadata.clear();
    }

    bool LoadPrecompiledShaderMetadata(
        AssetManager& assets,
        const std::filesystem::path& path,
        ShaderRenderState* const renderState,
        ShaderVariantDeclaration* const variants)
    {
        const auto key = RelativeToAssetRoot(assets, path);
        const std::lock_guard<std::mutex> lock(g_indexMutex);
        const auto found = g_sourceMetadata.find(key);
        if (found == g_sourceMetadata.end())
        {
            return false;
        }
        if (renderState != nullptr)
        {
            *renderState = found->second.renderState;
        }
        if (variants != nullptr)
        {
            *variants = found->second.variants;
        }
        return true;
    }

    const std::vector<ShaderEntryPoint>&
        KnownShaderEntryPoints()
    {
        // 実行時に探索する全入口を事前コンパイルし、配布後の初回起動で
        // コンパイルが発生しないようにします。
        static const std::vector<ShaderEntryPoint> entries{
            // Lit系マテリアル（LitEffect）。
            { "VSMain", "vs_5_0" },
            { "PSMain", "ps_5_0" },
            { "VSSkinnedMain", "vs_5_0" },
            { "PSSkinnedMain", "ps_5_0" },
            { "VSInstancedMain", "vs_5_0" },
            { "GSMain", "gs_5_0" },
            { "HSMain", "hs_5_0" },
            { "DSMain", "ds_5_0" },
            { "VSOutline", "vs_5_0" },
            { "VSSkinnedOutline", "vs_5_0" },
            { "PSOutline", "ps_5_0" },
            { "PSOccluded", "ps_5_0" },
            // 環境（EnvironmentRenderer）。
            { "PSSky", "ps_5_0" },
            { "PSBloom", "ps_5_0" },
            { "PSScreenOutline", "ps_5_0" },
            { "PSScreenSpaceLensFlare", "ps_5_0" },
            { "PSLensFlareStreak", "ps_5_0" },
            { "PSToneMap", "ps_5_0" },
            { "PSFXAA", "ps_5_0" },
            { "PSCopy", "ps_5_0" },
            { "PSCopyMirrorX", "ps_5_0" },
            { "PSTemporalAntiAliasing", "ps_5_0" },
            { "PSVolumetricLight", "ps_5_0" },
            { "PSDepthOfFieldPrepare", "ps_5_0" },
            { "PSDepthOfFieldBlur", "ps_5_0" },
            { "PSDepthOfFieldComposite", "ps_5_0" },
            { "PSMotionBlur", "ps_5_0" },
            { "PSLuminance", "ps_5_0" },
            { "PSAmbientOcclusion", "ps_5_0" },
            { "PSAmbientOcclusionBlur", "ps_5_0" },
            { "PSPrefilterEnvironment", "ps_5_0" },
            { "PSIrradiance", "ps_5_0" },
            { "PSReflectionDepthLinearize", "ps_5_0" },
            { "PSReflectionDepthDownsample", "ps_5_0" },
            // クラスタライトカリング（ClusteredLights）。
            { "CSMain", "cs_5_0" }
        };
        return entries;
    }

    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::vector<std::string>& defines,
        const std::vector<std::string>* usedKeywords)
    {
        return PrecompileShaderVariants(
            assets,
            path,
            destinationDirectory,
            std::span<const ShaderEntryPoint>{
                KnownShaderEntryPoints() },
            defines,
            usedKeywords);
    }

    std::uint32_t PrecompileShaderVariants(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines,
        const std::vector<std::string>* usedKeywords,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (destinationDirectory.empty())
        {
            return 0;
        }
        // 書き込み先を配布フォルダーへ向け、読み取りは止めます。
        // 止めないと、開発機のキャッシュに当たったときに何も書かれず、
        // 配布フォルダーが空のままになります。
        // バリアントの全組み合わせを焼きます。呼び出し側のdefinesは
        // 固定の追加キーワードとして扱います。
        std::vector<ShaderKeywordSet> variants;
        try
        {
            const auto source = assets.ReadFileBytesFresh(path);
            const auto declaration = ParseShaderVariants(
                std::string_view{
                    reinterpret_cast<const char*>(source.data()),
                    source.size() });
            const auto total =
                EnumerateShaderVariants(declaration).size();
            variants = EnumerateShaderVariants(
                declaration,
                usedKeywords);
            // 上限超過で削減したバリアント数を、原因調査用にログへ記録します。
            if (variants.size() < total)
            {
                Logger::Instance().Info(
                    "shader_feature stripped "
                    + std::to_string(total - variants.size())
                    + " unused variant(s) of "
                    + PathToUtf8(path)
                    + " ("
                    + std::to_string(variants.size())
                    + " kept).");
            }
        }
        catch (const std::exception&)
        {
            variants.clear();
        }
        if (variants.empty())
        {
            // 宣言が無い（または上限超過で無効）シェーダーは
            // キーワード無しの1本だけ。
            variants.emplace_back();
        }

        std::uint32_t succeeded{};
        for (const auto& variant : variants)
        {
            auto keywords = defines;
            for (const auto& keyword : variant.Keywords())
            {
                keywords.push_back(keyword);
            }
            std::string variantError;
            succeeded += PrecompileShader(
                assets,
                path,
                destinationDirectory,
                entryPoints,
                keywords,
                &variantError);
            if (error != nullptr
                && error->empty()
                && !variantError.empty())
            {
                *error = std::move(variantError);
            }
        }
        return succeeded;
    }

    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (destinationDirectory.empty())
        {
            return 0;
        }

        RecordShaderSourceMetadata(
            assets,
            path,
            destinationDirectory);
        const ScopedPrecompileContext context(destinationDirectory);
        std::uint32_t succeeded{};
        for (const auto& entry : entryPoints)
        {
            try
            {
                const auto byteCode = CompileShaderCached(
                    assets,
                    path,
                    entry.entryPoint,
                    entry.target,
                    defines);
                if (IsGeometryShaderTarget(entry.target))
                {
                    ValidateGeometryShaderInput(
                        byteCode.Get(),
                        entry.entryPoint);
                }
                ++succeeded;
            }
            catch (const ShaderCacheWriteFailure&)
            {
                // Export成果物の書き込み失敗は「入口が無い」通常の
                // probe失敗ではありません。上位でcache全体を破棄し、
                // source-strip時は書き出しを中止できるよう伝播します。
                throw;
            }
            catch (const std::exception& exception)
            {
                // 入口が無いのは正常です。失敗もキャッシュへ
                // 残るので、実行時に試し直されません。
                if (error != nullptr && error->empty())
                {
                    *error = exception.what();
                }
            }
        }
        return succeeded;
    }

    void WarmShaderCache(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& keywords)
    {
        for (const auto& entry : KnownShaderEntryPoints())
        {
            try
            {
                static_cast<void>(CompileShaderCached(
                    assets,
                    path,
                    entry.entryPoint,
                    entry.target,
                    keywords));
            }
            catch (const std::exception&)
            {
                // 入口が無いのは正常です。失敗もキャッシュへ残ります。
            }
        }
    }

    ShaderCompileStats ShaderCompileStatistics() noexcept
    {
        const std::lock_guard<std::mutex> lock(
            g_statisticsMutex);
        return g_statistics;
    }

    void ResetShaderCompileStatistics() noexcept
    {
        const std::lock_guard<std::mutex> lock(
            g_statisticsMutex);
        g_statistics = {};
    }

    void SetShaderCompileCompletionHookForTesting(
        std::function<void()> hook)
    {
        const std::lock_guard<std::mutex> lock(
            g_compileCompletionHookMutex);
        g_compileCompletionHook = std::move(hook);
    }

    void ClearShaderCache()
    {
        {
            const std::lock_guard<std::mutex> lock(
                g_liveDependencyMutex);
            g_liveDependencies.clear();
        }
        const auto directory = ShaderCacheDirectory();
        if (directory.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::uint64_t ShaderSourceDependencyRevision(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& defines) noexcept
    {
        try
        {
            if (path.empty() || assets.IsArchived())
            {
                return 0;
            }

            std::vector<std::filesystem::path> files;
            const auto sourceKey = MakeLiveSourceKey(assets, path);
            const auto defineKey = MakeDefineKey(defines);
            {
                const std::lock_guard<std::mutex> lock(
                    g_liveDependencyMutex);
                const auto shader = g_liveDependencies.find(sourceKey);
                if (shader == g_liveDependencies.end())
                {
                    return 0;
                }
                for (const auto& [identity, dependencies] :
                    shader->second.compileIdentities)
                {
                    static_cast<void>(identity);
                    if (dependencies.defineKey == defineKey)
                    {
                        files.insert(
                            files.end(),
                            dependencies.files.begin(),
                            dependencies.files.end());
                    }
                }
            }
            if (files.empty())
            {
                return 0;
            }

            const auto normalizedPath = [](const auto& file)
            {
                return NormalizeIndexedAssetPath(
                    PathToUtf8(file.lexically_normal()));
            };
            std::ranges::sort(
                files,
                [&](const auto& left, const auto& right)
                {
                    return normalizedPath(left) < normalizedPath(right);
                });
            files.erase(
                std::unique(
                    files.begin(),
                    files.end(),
                    [&](const auto& left, const auto& right)
                    {
                        return normalizedPath(left)
                            == normalizedPath(right);
                    }),
                files.end());

            // FNV-1aでpath／存在状態／timestamp／sizeを一つのrevisionへ
            // 束ねます。中身は再compile時に必ずfresh readするため、poll
            // 側では軽量なmetadataだけを調べます。
            std::uint64_t revision = 14695981039346656037ull;
            const auto append = [&](const void* data, const std::size_t size)
            {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t index = 0; index < size; ++index)
                {
                    revision ^= bytes[index];
                    revision *= 1099511628211ull;
                }
            };
            for (const auto& file : files)
            {
                const auto name = normalizedPath(file);
                append(name.data(), name.size());
                const std::uint8_t separator{};
                append(&separator, sizeof(separator));

                std::error_code error;
                const bool exists = std::filesystem::exists(file, error);
                const std::uint8_t state = error
                    ? 2u
                    : (exists ? 1u : 0u);
                append(&state, sizeof(state));
                if (!error && exists)
                {
                    const auto timestamp =
                        std::filesystem::last_write_time(file, error);
                    const auto ticks = error
                        ? std::filesystem::file_time_type::duration::rep{}
                        : timestamp.time_since_epoch().count();
                    append(&ticks, sizeof(ticks));
                    error.clear();
                    const auto size = std::filesystem::file_size(file, error);
                    const std::uintmax_t bytes = error ? 0u : size;
                    append(&bytes, sizeof(bytes));
                }
            }
            return revision == 0 ? 1 : revision;
        }
        catch (...)
        {
            return 0;
        }
    }

    std::size_t ClearShaderCacheFailures()
    {
        const auto directory = ShaderCacheDirectory();
        if (directory.empty())
        {
            return 0;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return 0;
        }
        // 記録した失敗（.fail）と対応する依存情報（.deps）だけを
        // 削除します。成功済みのバイトコード（.cso）は再利用します。
        std::size_t removed = 0;
        std::vector<std::filesystem::path> failures;
        for (const auto& entry :
            std::filesystem::directory_iterator(
                directory,
                error))
        {
            if (entry.path().extension() == L".fail")
            {
                failures.push_back(entry.path());
            }
        }
        for (const auto& failure : failures)
        {
            auto dependencies = failure;
            dependencies.replace_extension(L".deps");
            if (std::filesystem::remove(failure, error))
            {
                ++removed;
            }
            std::filesystem::remove(dependencies, error);
        }
        return removed;
    }

    void SetShaderCacheEnabled(const bool enabled) noexcept
    {
        g_cacheEnabled.store(enabled);
    }

    bool IsShaderCacheEnabled() noexcept
    {
        return g_cacheEnabled.load();
    }

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShaderCached(
        AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& defines)
    {
        if (!assets.FileExists(path))
        {
            // 配布物にHLSLソースが無い場合は、ハッシュの代わりに索引から読み込みます。
            if (auto blob = LoadFromIndex(
                    assets,
                    path,
                    entryPoint,
                    target,
                    defines))
            {
                return blob;
            }
            if (assets.IsArchived())
            {
                std::string keywordList;
                for (const auto& define : defines)
                {
                    if (!keywordList.empty())
                    {
                        keywordList.push_back('+');
                    }
                    keywordList.append(define);
                }
                throw std::runtime_error(
                    "Precompiled shader cache entry was not found in "
                    "the source-stripped archive: "
                    + RelativeToAssetRoot(assets, path)
                    + " (entry '"
                    + (entryPoint != nullptr ? entryPoint : "")
                    + "', target '"
                    + (target != nullptr ? target : "")
                    + "', keywords '" + keywordList
                    + "'). Re-export the game so this shader "
                    "variant is included.");
            }
            throw std::runtime_error(
                "Shader file was not found: "
                + PathToUtf8(path));
        }
        const auto source = assets.ReadFileBytesFresh(path);
        const auto sourceName = PathToUtf8(path);
        const UINT flags = CompileFlags();

        // キャッシュのキーはHLSL本体の中身＋入口＋ターゲット＋
        // コンパイルフラグです。#includeの内容はキーには含めません
        // （コンパイルするまで何を読むか分からないため）。代わりに
        // コンパイル時に読み取った依存一覧を.depsへ保存し、再利用前に照合します。
        std::string key;
        std::filesystem::path cacheDirectory;
        std::filesystem::path byteCodePath;
        std::filesystem::path dependencyPath;
        // Exportは公開設定でcacheが無効でも必ず成果物を作ります。
        // ここで無効化を尊重するとHLSLを外した配布物が実行不能に
        // なるため、ScopedPrecompileContextを優先します。
        const bool useCache = g_forceCompile
            || g_cacheEnabled.load();
        if (useCache)
        {
            std::string material;
            material.reserve(source.size() + 128);
            material.append(
                reinterpret_cast<const char*>(source.data()),
                source.size());
            material.append("\n#lamapon-shader-cache v");
            material.append(
                std::to_string(CacheFormatVersion));
            material.append("\nentry=");
            material.append(
                entryPoint != nullptr ? entryPoint : "");
            material.append("\ntarget=");
            material.append(target != nullptr ? target : "");
            material.append("\nflags=");
            material.append(std::to_string(flags));
            // バリアントのキーワード。組み合わせごとに別の
            // バイトコードになるので、キーにも必ず入れます。
            material.append("\ndefines=");
            for (const auto& define : defines)
            {
                material.append(define);
                material.push_back(';');
            }
            key = HashBytes(
                reinterpret_cast<const std::uint8_t*>(
                    material.data()),
                material.size());
            cacheDirectory = g_writeOverride.empty()
                ? ShaderCacheDirectory()
                : g_writeOverride;
        }

        std::filesystem::path failurePath;
        // 読み取りは同梱キャッシュも見ますが、書き込みは常に
        // 書き込み可能な側だけです。
        std::filesystem::path writeByteCodePath;
        std::filesystem::path writeDependencyPath;
        std::filesystem::path writeFailurePath;
        if (!key.empty() && !cacheDirectory.empty())
        {
            writeByteCodePath = cacheDirectory / (key + ".cso");
            writeDependencyPath =
                cacheDirectory / (key + ".deps");
            writeFailurePath = cacheDirectory / (key + ".fail");
            // 読むときは、同梱された読み取り専用のキャッシュを先に
            // 見ます（書き出したゲームの初回起動でコンパイルを
            // 走らせないため）。書き込み先は常にcacheDirectoryです。
            //
            // 読み取り先は依存関係の一致を確認した候補だけに設定します。
            // 初期値を空にし、未検証の結果を読み込まないようにします。
            std::vector<std::filesystem::path> readDirectories;
            {
                const std::lock_guard<std::mutex> lock(
                    g_searchMutex);
                readDirectories = g_searchDirectories;
            }
            readDirectories.push_back(cacheDirectory);
            if (g_forceCompile)
            {
                readDirectories.clear();
            }
            std::vector<std::pair<std::string, std::string>>
                cachedDependencies;
            for (const auto& directory : readDirectories)
            {
                const auto candidateDeps =
                    directory / (key + ".deps");
                const auto candidateByteCode =
                    directory / (key + ".cso");
                const auto candidateFailure =
                    directory / (key + ".fail");
                std::error_code exists;
                if (!std::filesystem::exists(
                        candidateDeps,
                        exists))
                {
                    continue;
                }
                std::vector<std::pair<std::string, std::string>>
                    candidateDependencies;
                if (!DependenciesMatch(
                        assets,
                        candidateDeps,
                        &candidateDependencies))
                {
                    continue;
                }
                cachedDependencies = std::move(candidateDependencies);
                byteCodePath = candidateByteCode;
                failurePath = candidateFailure;
                dependencyPath = candidateDeps;
                break;
            }
            if (!dependencyPath.empty())
            {
                RememberLiveShaderDependencies(
                    assets,
                    path,
                    entryPoint,
                    target,
                    defines,
                    source,
                    cachedDependencies);
            }
            const auto started =
                std::chrono::steady_clock::now();
            std::error_code error;
            // 入口不足の失敗もキャッシュします。LitEffectは
            // VSInstancedMain/HSMain/DSMain/VSOutline/PSOutlineを
            // 「あれば使う」方式で毎回試し、無いシェーダーでは
            // その全部を試すため、記録しないと同じ探索コストが繰り返されます。
            // ソースのハッシュがキーなので、直せば自動的に
            // 試し直されます。
            if (std::filesystem::exists(failurePath, error))
            {
                const auto stored = ReadCacheText(failurePath);
                if (!stored.empty())
                {
                    if (!ShouldRememberFailure(stored))
                    {
                        // 現在の保存条件に合わない古い失敗記録は無効として扱い、
                        // 次の処理で再コンパイルします。読み取り専用の場所では
                        // 削除に失敗しても、この記録を再利用しなければ問題ありません。
                        std::filesystem::remove(
                            failurePath,
                            error);
                        std::filesystem::remove(
                            dependencyPath,
                            error);
                    }
                    else
                    {
                        const std::chrono::duration<
                            double,
                            std::milli> elapsed =
                            std::chrono::steady_clock::now()
                            - started;
                        {
                            const std::lock_guard<std::mutex>
                                lock(g_statisticsMutex);
                            ++g_statistics.cacheHitCount;
                            g_statistics.cacheReadMilliseconds
                                += elapsed.count();
                        }
                        throw std::runtime_error(stored);
                    }
                }
            }
            if (std::filesystem::exists(byteCodePath, error))
            {
                const auto bytes = ReadCacheFile(byteCodePath);
                if (bytes && !bytes->empty())
                {
                    Microsoft::WRL::ComPtr<ID3DBlob> blob;
                    if (SUCCEEDED(D3DCreateBlob(
                            static_cast<SIZE_T>(bytes->size()),
                            blob.ReleaseAndGetAddressOf())))
                    {
                        std::memcpy(
                            blob->GetBufferPointer(),
                            bytes->data(),
                            bytes->size());
                        const std::chrono::duration<
                            double,
                            std::milli> elapsed =
                            std::chrono::steady_clock::now()
                            - started;
                        const std::lock_guard<std::mutex>
                            lock(g_statisticsMutex);
                        ++g_statistics.cacheHitCount;
                        g_statistics.cacheReadMilliseconds
                            += elapsed.count();
                        return blob;
                    }
                }
            }
        }

        // D3D_SHADER_MACROは終端に{nullptr,nullptr}が要ります。
        std::vector<D3D_SHADER_MACRO> macros;
        macros.reserve(defines.size() + 1);
        for (const auto& define : defines)
        {
            macros.push_back({ define.c_str(), "1" });
        }
        macros.push_back({ nullptr, nullptr });

        RecordingInclude includeHandler{ assets, path };
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const auto started = std::chrono::steady_clock::now();
        const HRESULT result = D3DCompile(
            source.data(),
            source.size(),
            sourceName.c_str(),
            defines.empty() ? nullptr : macros.data(),
            &includeHandler,
            entryPoint,
            target,
            flags,
            0,
            shader.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        RememberLiveShaderDependencies(
            assets,
            path,
            entryPoint,
            target,
            defines,
            source,
            includeHandler.Dependencies(),
            includeHandler.MissingDependencies());
        RunShaderCompileCompletionHook();
        const std::chrono::duration<double, std::milli>
            elapsed =
                std::chrono::steady_clock::now() - started;
        if (FAILED(result))
        {
            const std::string details = errors
                ? std::string{
                    static_cast<const char*>(
                        errors->GetBufferPointer()),
                    errors->GetBufferSize()
                }
                : "Unknown shader compiler error.";
            const std::string message =
                "Failed to compile shader "
                + sourceName
                + " ("
                + (entryPoint != nullptr ? entryPoint : "")
                + "): "
                + details;
            {
                const std::lock_guard<std::mutex> lock(
                    g_statisticsMutex);
                ++g_statistics.compiledCount;
                g_statistics.compileMilliseconds
                    += elapsed.count();
            }
            // 記録するのは「入口が無い」失敗だけです。読む側でも同じ
            // 条件で弾きます（ShouldRememberFailureの説明を参照）。
            // include失敗などは依存一覧へ完全に記録できないため、
            // 修正を検出できず古い失敗を返すおそれがあります。
            if (ShouldRememberFailure(details))
            {
                static_cast<void>(WriteCacheEntry(
                    cacheDirectory,
                    writeDependencyPath,
                    writeFailurePath,
                    assets,
                    source,
                    path,
                    includeHandler.Dependencies(),
                    includeHandler.MissingDependencies(),
                    message.data(),
                    message.size()));
            }
            throw std::runtime_error(message);
        }

        {
            const std::lock_guard<std::mutex> lock(
                g_statisticsMutex);
            ++g_statistics.compiledCount;
            g_statistics.compileMilliseconds
                += elapsed.count();
        }
        std::ostringstream message;
        message.precision(1);
        message << std::fixed
            << "シェーダーをコンパイルしました: "
            << sourceName
            << " ("
            << (entryPoint != nullptr ? entryPoint : "")
            << ") "
            << elapsed.count()
            << "ms";
        Logger::Instance().Info(message.str());

        const bool cacheWritten = WriteCacheEntry(
            cacheDirectory,
            writeDependencyPath,
            writeByteCodePath,
            assets,
            source,
            path,
            includeHandler.Dependencies(),
            includeHandler.MissingDependencies(),
            shader->GetBufferPointer(),
            shader->GetBufferSize());
        if (!g_writeOverride.empty())
        {
            if (!cacheWritten || key.empty())
            {
                throw ShaderCacheWriteFailure(
                    "Could not write a precompiled shader cache "
                    "entry for " + sourceName + " ("
                    + (entryPoint != nullptr ? entryPoint : "")
                    + ") to "
                    + LamaPon::PathToUtf8(g_writeOverride));
            }
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pendingByDestination[
                PendingDestinationKey(g_writeOverride)]
                .entries.emplace_back(
                MakeIndexKey(
                    RelativeToAssetRoot(assets, path),
                    entryPoint,
                    target,
                    defines),
                key);
        }
        return shader;
    }
}
