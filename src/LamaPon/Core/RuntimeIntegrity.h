#pragma once

#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// 書き出した配布物の自己整合性（ランタイムとGame Moduleの改ざん検知）を扱います。
// 書き出し時に各バイナリのSHA-256をアーカイブ鍵で封印したマニフェストを残し、
// 起動時に同じ鍵で開いて照合します。鍵が配布物にある以上、鍵を取り出した攻撃者は
// マニフェストも作り直せるため、これは改ざんの検知であって防止ではありません。
namespace LamaPon::RuntimeIntegrity
{
    // ManifestFileName は配布物に置く整合性マニフェストのファイル名。
    inline constexpr std::wstring_view ManifestFileName = L"integrity.dat";

    // ファイル全体のSHA-256を小文字16進で返します(path: 対象ファイル)。
    // 開けない場合は空文字列を返します。
    inline std::string HashFileHex(const std::filesystem::path& path)
    {
        // ファイル全体の入力ストリーム
        std::ifstream input(
            ExtendedLengthPath(path),
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            return {};
        }
        // ファイル終端の位置
        const auto end = input.tellg();
        if (end < 0)
        {
            return {};
        }
        // ファイル全体の読込先
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
                return {};
            }
        }
        return Crypto::Sha256Hex(bytes.data(), bytes.size());
    }

    // 配布物の整合性マニフェストを作り、directory/integrity.datへ書き込みます。
    // (directory: 配布フォルダー, fileNames: 照合する相対ファイル名, key: この書き出しのアーカイブ鍵)。
    // 署名など全ての加工が済んだ最終バイトに対して呼び出します。
    inline void WriteManifest(
        const std::filesystem::path& directory,
        const std::vector<std::wstring>& fileNames,
        const Crypto::AesKey& key)
    {
        // 記録するファイル名とハッシュの対応
        nlohmann::json files = nlohmann::json::object();
        for (const auto& name : fileNames)
        {
            // このファイルのSHA-256(16進)
            const auto hashHex = HashFileHex(directory / name);
            if (hashHex.empty())
            {
                continue;
            }
            files[PathToUtf8(name)] = hashHex;
        }
        // マニフェスト本体
        nlohmann::json manifest;
        manifest["files"] = std::move(files);
        // マニフェストのUTF-8バイト列
        const auto text = manifest.dump();
        // 鍵で封印したマニフェスト
        const auto sealed = Crypto::Seal(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size(),
            key);
        // マニフェストの出力ストリーム
        std::ofstream output(
            ExtendedLengthPath(directory / ManifestFileName),
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the integrity manifest: "
                + PathToUtf8(directory / ManifestFileName));
        }
        output.write(
            reinterpret_cast<const char*>(sealed.data()),
            static_cast<std::streamsize>(sealed.size()));
        output.flush();
        if (!output)
        {
            throw std::runtime_error(
                "Could not finish writing the integrity manifest: "
                + PathToUtf8(directory / ManifestFileName));
        }
    }

    // directoryの整合性マニフェストをkeyで開き、記録された各ファイルのSHA-256を照合します。
    // (directory: 配布フォルダー, key: 照合に使うアーカイブ鍵, reason: 失敗理由の出力)。
    // 一致すればtrue、欠落・認証失敗・不一致ならreasonを設定してfalseを返します。
    inline bool VerifyManifest(
        const std::filesystem::path& directory,
        const Crypto::AesKey& key,
        std::string& reason)
    {
        reason.clear();
        // マニフェストの読込ストリーム
        std::ifstream input(
            ExtendedLengthPath(directory / ManifestFileName),
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            reason = "integrity manifest is missing";
            return false;
        }
        // マニフェスト末尾の位置
        const auto end = input.tellg();
        if (end < 0)
        {
            reason = "integrity manifest could not be read";
            return false;
        }
        // 封印されたマニフェストのバイト列
        std::vector<std::uint8_t> sealed(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!sealed.empty())
        {
            input.read(
                reinterpret_cast<char*>(sealed.data()),
                static_cast<std::streamsize>(sealed.size()));
            if (!input)
            {
                reason = "integrity manifest could not be read";
                return false;
            }
        }
        // 復号・認証したマニフェスト本文
        const auto text = Crypto::Unseal(
            sealed.data(),
            sealed.size(),
            key);
        if (!text.has_value())
        {
            reason = "integrity manifest failed authentication";
            return false;
        }
        // 解析したマニフェスト
        nlohmann::json manifest;
        try
        {
            manifest = nlohmann::json::parse(
                text->begin(),
                text->end());
        }
        catch (const std::exception&)
        {
            reason = "integrity manifest is malformed";
            return false;
        }
        if (!manifest.contains("files")
            || !manifest["files"].is_object())
        {
            reason = "integrity manifest is malformed";
            return false;
        }
        // name: 相対ファイル名, expected: 記録したSHA-256
        for (const auto& [name, expected] :
            manifest["files"].items())
        {
            if (!expected.is_string())
            {
                reason = "integrity manifest is malformed";
                return false;
            }
            // 実ファイルのSHA-256(16進)
            const auto actual = HashFileHex(
                directory / PathFromUtf8(name));
            if (actual.empty())
            {
                reason = "a protected file is missing: " + name;
                return false;
            }
            if (actual != expected.get<std::string>())
            {
                reason = "a protected file was modified: " + name;
                return false;
            }
        }
        return true;
    }

    // 書き出し済み配布物なら整合性を検証し、未書き出し(開発)ならスキップします。
    // (directory: 配布フォルダー, reason: 失敗理由の出力)。
    inline bool VerifyExportedArtifacts(
        const std::filesystem::path& directory,
        std::string& reason)
    {
        reason.clear();
        if (!Crypto::IsExportedArchiveKey())
        {
            return true;
        }
        return VerifyManifest(directory, Crypto::ArchiveKey(), reason);
    }
}
