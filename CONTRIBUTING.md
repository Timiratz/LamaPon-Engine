# Contributing to LamaPon

クラスを分けるときは、ファイルだけでなく状態と責務の所有者を分けてください。
EditorLayerの全体参照を渡す代わりに、必要なサービスと用途を限定した
コールバックを渡します。BgmLoopPanelはカタログ・波形・試聴を所有し、
EditorLayerは表示と保存後のコマンド実行を仲介する例です。
コメントは処理の目的、寿命、例外時の後始末を説明し、戻り値や実装と揃えます。

LamaPon currently targets Windows, MSVC, DirectX 11, and C++20.

## Development workflow

1. Open an x64 Visual Studio Developer Command Prompt.
2. Configure with `cmake --preset windows-debug`.
3. Build with `cmake --build --preset windows-debug`.
4. Test with `ctest --preset windows-debug`.
5. Before submitting, repeat the build and tests with `windows-release`.

Keep changes focused and preserve existing Japanese and UTF-8 path behavior.
New runtime behavior should include an automated regression test. Avoid adding
new third-party dependencies when the Windows SDK or standard library is
sufficient.

## Code style

- リポジトリのファイル名・フォルダー名は英語またはローマ字のASCII文字にします。
  コード内の表示名・文章・コメントは日本語で構いません。アセット改名時は
  `.meta`を一緒に移し、GUIDを保ったまま参照パスを更新してください。
- Use C++20 and four-space indentation.
- Treat `/W4` warnings in changed code as defects.
- Prefer RAII, `std::filesystem::path`, and explicit ownership.
- Keep platform-specific code behind a small interface.
- Give panels and systems their own state and narrow dependencies. Keep stateless
  component inspectors independent of `EditorLayer` and share only required operations.
- コードコメントは原則として日本語で記述します。API名や外部仕様など、
  英語の方が正確な場合は英語を使用して構いません。自明な各行ではなく、
  処理ブロック、互換処理、例外的な判断の意図が分かる単位で補足します。

## Pull requests

Describe the user-visible behavior, tests run, and compatibility impact.
Update `CHANGELOG.md` for notable behavior. Do not commit build directories,
logs, crash dumps, generated projects, or user-specific editor settings.
