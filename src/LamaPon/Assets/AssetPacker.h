#pragma once

#include "LamaPon/Core/Crypto.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace LamaPon
{
    struct AssetPackResult final
    {
        std::size_t fileCount{};
        std::uint64_t totalBytes{};
        // エディター内で配布内容を確認するための一覧です。
        // 索引の平文を配布フォルダーへ出力してはいけません。
        std::vector<std::filesystem::path> includedFiles;
        std::vector<std::filesystem::path> excludedFiles;
    };

    // ファイルを暗号化する直前に、メモリ上の内容だけを変換します。
    // 元アセットは変更しません。Game ExporterがGUID参照の現在pathを
    // 配布JSONへ反映する用途などに使います。
    using AssetPackTransform = std::function<void(
        const std::filesystem::path& relativePath,
        std::vector<std::uint8_t>& contents)>;

    // sourceDirectory以下のファイルを暗号化し、archiveOutputPathの
    // 単一アーカイブへ格納します。
    // C++ソース、デバッグ情報、実行可能ファイルは常に除外します。
    // 秘密情報らしい名前のファイルやシンボリックリンクは拒否します。
    // その他の独自アセット形式は拡張子の登録なしで梱包できます。
    // skipExtensionsに挙げた拡張子（小文字、ドット付き）も除外します。
    //
    // 鍵は呼び出し側が渡します。書き出しのたびに新しい鍵を作り、
    // それを実行ファイルへ焼き込むためです（エンジン共通の鍵だと、
    // 1本解かれた時点で全ゲームのアーカイブが開いてしまいます）。
    [[nodiscard]] AssetPackResult PackAssets(
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& archiveOutputPath,
        const Crypto::AesKey& key,
        const std::vector<std::wstring>& skipExtensions = {},
        const AssetPackTransform& transform = {});
}
