#include "LamaPon/Core/JobSystem.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/VersionCompare.h"
#include "LamaPon/Portable/PortableInputState.h"

#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    void Check(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void TestUnicode()
    {
        using namespace LamaPon;
        // 境界値、補助平面、BOM、埋め込みNULを含む既知のUTF-8/UTF-16/UTF-32表現。
        constexpr char encoded[] = "\x00\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf"
            "\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf\xef\xbb\xbf";
        const std::string utf8(encoded, sizeof(encoded) - 1);
        const std::u16string utf16{0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
            0xd800, 0xdc00, 0xdbff, 0xdfff, 0xfeff};
        const std::u32string utf32{0, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
            0x10000, 0x10ffff, 0xfeff};
        Check(Detail::Utf8ToCodeUnits<char16_t>(utf8) == utf16, "UTF-16 known values");
        Check(Detail::Utf8ToCodeUnits<char32_t>(utf8) == utf32, "UTF-32 known values");
        Check(Detail::CodeUnitsToUtf8<char16_t>(utf16) == utf8, "UTF-16 encoding");
        Check(Detail::CodeUnitsToUtf8<char32_t>(utf32) == utf8, "UTF-32 encoding");
        const std::array<std::string_view, 13> invalid{
            "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xf0\x80\x80\x80",
            "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xc2",
            "\xe2\x82", "\xf0\x9f\x92", "ok\xe2x\x80"};
        for (const auto value : invalid)
        {
            Check(Utf8ToWide(value).empty(), "Invalid UTF-8 rejected completely");
            Check(Detail::Utf8ToCodeUnits<char32_t>(value).empty(), "Invalid UTF-8 on UTF-32");
        }
        const std::array<std::u16string, 4> invalid16{
            std::u16string{0xd800}, {0xdc00}, {0xd800, u'A'}, {u'A', 0xdc00}};
        for (const auto& value : invalid16)
            Check(Detail::CodeUnitsToUtf8<char16_t>(value).empty(), "Invalid UTF-16 rejected");
        for (const char32_t value : {char32_t{0xd800}, char32_t{0xdfff}, char32_t{0x110000}, char32_t{0xffffffff}})
            Check(Detail::CodeUnitsToUtf8<char32_t>(std::u32string{value}).empty(), "Invalid UTF-32 rejected");
        Check(Utf8ToWide({}).empty() && WideToUtf8({}).empty(), "Empty text");
        Check(WideToUtf8(Utf8ToWide(utf8)) == utf8, "Native wchar_t round trip");

#if defined(_WIN32)
        // Windows版の既存変換契約をOSの変換関数と比較する。
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring expected(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), expected.data(), length);
        Check(Utf8ToWide(utf8) == expected, "Windows UTF-16 compatibility");
        // 全Unicode scalarをOSのエンコーダーと比較し、既存Windowsセーブ名の互換性を確認する。
        std::wstring scalars;
        for (std::uint32_t scalar = 0; scalar <= 0x10ffff; ++scalar)
        {
            if (scalar >= 0xd800 && scalar <= 0xdfff) continue;
            if (scalar < 0x10000) scalars.push_back(static_cast<wchar_t>(scalar));
            else
            {
                const auto value = scalar - 0x10000;
                scalars.push_back(static_cast<wchar_t>(0xd800 + (value >> 10)));
                scalars.push_back(static_cast<wchar_t>(0xdc00 + (value & 0x3ff)));
            }
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, scalars.data(),
            static_cast<int>(scalars.size()), nullptr, 0, nullptr, nullptr);
        Check(bytes > 0, "Windows Unicode reference conversion");
        std::string reference(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, scalars.data(),
            static_cast<int>(scalars.size()), reference.data(), bytes, nullptr, nullptr);
        Check(WideToUtf8(scalars) == reference, "All Unicode scalars match Windows UTF-8 encoder");
        Check(Utf8ToWide(reference) == scalars, "All Unicode scalars match Windows UTF-16 decoding");
        const auto decoded32 = Detail::Utf8ToCodeUnits<char32_t>(reference);
        Check(decoded32.size() == 0x110000 - 0x800, "All scalars decoded to UTF-32");
        Check(Detail::CodeUnitsToUtf8<char32_t>(decoded32) == reference, "UTF-32 matches Windows encoder");
#endif
    }

    void TestPaths()
    {
        using namespace LamaPon;
        const std::string name = "assets/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e-\xf0\x9f\x8e\xae.png";
        Check(PathToUtf8(PathFromUtf8(name)) == name, "Unicode asset paths");
        Check(PathFromUtf8({}).empty(), "Empty path");
        const auto applicationCache = PathFromUtf8(
#if defined(_WIN32)
            "C:/LamaPonProbe/cache"
#else
            "/lamapon-probe/cache"
#endif
        );
        const auto unicodeCache = AndroidApplicationCachePath(applicationCache, L"\u691c\u8a3c");
        Check(unicodeCache == applicationCache / "LamaPon" / PathFromUtf8("\xe6\xa4\x9c\xe8\xa8\xbc"),
            "Application cache path and UTF-8 subfolder");
        bool invalidCacheRejected = false;
        try { static_cast<void>(AndroidApplicationCachePath("relative", L"cache")); }
        catch (const std::invalid_argument&) { invalidCacheRejected = true; }
        Check(invalidCacheRejected, "Relative application cache path rejected");
        std::error_code error = std::make_error_code(std::errc::invalid_argument);
        Check(EnsureDirectoryExists({}, error) && !error, "Empty directory check");
        const auto executable = ExecutableDirectory();
        Check(!executable.empty() && executable.is_absolute()
            && std::filesystem::is_directory(executable), "Executable location");
        const auto upper = PathCacheKey(executable / "PlatformAsset.png");
        const auto lower = PathCacheKey(executable / "platformasset.png");
        Check(upper.size() == 16 && lower.size() == 16, "Cache key width");
#if defined(_WIN32)
        Check(upper == lower, "Windows case insensitive cache keys");
        Check(IsUncPath(L"\\\\server\\share"), "Windows UNC");
        Check(IsUncPath(L"\\\\?\\UNC\\server\\share"), "Extended Windows UNC");
        Check(!IsUncPath(L"\\\\?\\C:\\assets"), "Extended local path");
        Check(!IsUncPath(L"\\\\.\\pipe\\test"), "Device path");
#else
        Check(upper != lower, "POSIX case sensitive cache keys");
        Check(!IsUncPath("//server/share"), "POSIX double slash is not UNC");
#endif
        Check(PathCacheKey(executable / "folder" / ".." / "image.png")
            == PathCacheKey(executable / "image.png"), "Normalized cache key");

#if defined(__ANDROID__)
        bool rejected = false;
        try { static_cast<void>(LocalEngineCachePath(L"test")); }
        catch (const std::logic_error&) { rejected = true; }
        Check(rejected, "Android requires an application cache path");
#else
        const auto cache = LocalEngineCachePath(L"\u691c\u8a3c");
        Check(cache.is_absolute() && PathToUtf8(cache.filename()) == "\xe6\xa4\x9c\xe8\xa8\xbc",
            "Unicode cache path");
#if !defined(_WIN32)
        // 環境変数の値だけを変更し、ファイルやフォルダーは作成しない。
        const char* prior = std::getenv("XDG_CACHE_HOME");
        const bool hadPrior = prior != nullptr;
        const std::string saved = prior ? prior : "";
        const char* priorHome = std::getenv("HOME");
        const bool hadPriorHome = priorHome != nullptr;
        const std::string savedHome = priorHome ? priorHome : "";
        struct Restore final
        {
            bool hadPrior;
            std::string saved;
            bool hadPriorHome;
            std::string savedHome;
            ~Restore()
            {
                if (hadPrior) setenv("XDG_CACHE_HOME", saved.c_str(), 1);
                else unsetenv("XDG_CACHE_HOME");
                if (hadPriorHome) setenv("HOME", savedHome.c_str(), 1);
                else unsetenv("HOME");
            }
        } restore{hadPrior, saved, hadPriorHome, savedHome};
        Check(setenv("XDG_CACHE_HOME", "/lamapon-path-test", 1) == 0, "Set XDG test value");
        Check(LocalEngineCachePath(L"cache") == "/lamapon-path-test/LamaPon/cache", "XDG cache root");
        Check(setenv("XDG_CACHE_HOME", "relative-cache", 1) == 0, "Set relative XDG value");
        const auto relativeIgnored = LocalEngineCachePath(L"cache");
        Check(setenv("XDG_CACHE_HOME", "", 1) == 0, "Clear XDG test value");
        Check(relativeIgnored == LocalEngineCachePath(L"cache"), "Relative XDG root ignored");

        Check(setenv("HOME", "/lamapon-home-test", 1) == 0, "Set HOME test value");
        Check(LocalEngineCachePath(L"cache")
            == "/lamapon-home-test/.cache/LamaPon/cache", "HOME cache fallback");
        Check(setenv("HOME", "relative-home", 1) == 0, "Set relative HOME test value");
        const auto relativeHomeIgnored = LocalEngineCachePath(L"cache");
        const auto systemTempFallback = std::filesystem::temp_directory_path()
            / "LamaPon" / "cache";
        Check(setenv("HOME", "", 1) == 0, "Clear HOME test value");
        Check(relativeHomeIgnored == systemTempFallback
            && LocalEngineCachePath(L"cache") == systemTempFallback,
            "Relative or empty HOME falls back to the system temporary directory");
#endif
#endif
    }

    void TestJobs()
    {
        auto& jobs = LamaPon::JobSystem::Instance();
        std::array<std::atomic<unsigned>, 257> hits{};
        jobs.ParallelFor(hits.size(), 7, [&](const std::size_t first, const std::size_t last)
        {
            for (auto index = first; index < last; ++index) ++hits[index];
        });
        for (const auto& hit : hits) Check(hit == 1, "Job covers every element exactly once");
        std::atomic<unsigned> nested{};
        jobs.ParallelFor(17, 3, [&](const std::size_t first, const std::size_t last)
        {
            jobs.ParallelFor(last - first, 1, [&](const std::size_t start, const std::size_t end)
            { nested += static_cast<unsigned>(end - start); });
        });
        Check(nested == 17, "Nested jobs complete");
        bool propagated = false;
        try
        {
            jobs.ParallelFor(19, 2, [](std::size_t, std::size_t)
            { throw std::runtime_error("expected job failure"); });
        }
        catch (const std::runtime_error&) { propagated = true; }
        Check(propagated, "Worker exception propagated");
        unsigned emptyCalls{};
        jobs.ParallelFor(0, 1, [&](std::size_t, std::size_t) { ++emptyCalls; });
        Check(emptyCalls == 0, "Empty jobs do not execute body");
        std::atomic<unsigned> afterFailure{};
        jobs.ParallelFor(11, 0, [&](const std::size_t first, const std::size_t last)
        { afterFailure += static_cast<unsigned>(last - first); });
        Check(afterFailure == 11, "Pool recovers after exception and clamps grain size");
    }

    void TestInput()
    {
        LamaPon::Portable::InputState input;
        const auto verifyKeyboardControl = [&](const std::string& control, const std::string& code)
        {
            input.Reset();
            input.KeyEvent(code, true);
            Check(input.ControlValue(control) == 1.0f && input.ControlWasPressed(control),
                "Portable keyboard control press mapping");
            input.EndFrame();
            input.KeyEvent(code, false);
            Check(input.ControlValue(control) == 0.0f && input.ControlWasReleased(control),
                "Portable keyboard control release mapping");
        };
        for (char letter = 'A'; letter <= 'Z'; ++letter)
        {
            verifyKeyboardControl(std::string("Keyboard") + letter, std::string("Key") + letter);
        }
        for (char digit = '0'; digit <= '9'; ++digit)
        {
            verifyKeyboardControl(std::string("KeyboardAlpha") + digit, std::string("Digit") + digit);
            verifyKeyboardControl(std::string("Keyboard") + digit, std::string("Digit") + digit);
        }
        for (int function = 1; function <= 12; ++function)
        {
            const auto code = "F" + std::to_string(function);
            verifyKeyboardControl("Keyboard" + code, code);
        }
        constexpr std::pair<const char*, const char*> namedKeys[]{
            { "KeyboardSpace", "Space" }, { "KeyboardEnter", "Enter" },
            { "KeyboardEscape", "Escape" }, { "KeyboardTab", "Tab" },
            { "KeyboardBackspace", "Backspace" }, { "KeyboardDelete", "Delete" },
            { "KeyboardInsert", "Insert" }, { "KeyboardHome", "Home" },
            { "KeyboardEnd", "End" }, { "KeyboardPageUp", "PageUp" },
            { "KeyboardPageDown", "PageDown" }, { "KeyboardUp", "ArrowUp" },
            { "KeyboardDown", "ArrowDown" }, { "KeyboardLeft", "ArrowLeft" },
            { "KeyboardRight", "ArrowRight" }, { "KeyboardLeftShift", "ShiftLeft" },
            { "KeyboardRightShift", "ShiftRight" }, { "KeyboardLeftControl", "ControlLeft" },
            { "KeyboardRightControl", "ControlRight" }, { "KeyboardLeftAlt", "AltLeft" },
            { "KeyboardRightAlt", "AltRight" },
        };
        for (const auto& [control, code] : namedKeys) verifyKeyboardControl(control, code);
        input.KeyEvent("F13", true);
        Check(input.ControlValue("KeyboardF13") == 0.0f
            && !input.ControlWasPressed("KeyboardF13"), "Unsupported keyboard controls stay unmapped");
        input.Reset();

        input.KeyEvent("KeyW", true);
        input.KeyEvent("KeyW", true);
        Check(input.IsDown("KeyW") && input.ControlWasPressed("KeyboardW"), "Shared keyboard press");
        input.EndFrame();
        Check(input.IsDown("KeyW") && !input.WasPressed("KeyW"), "Held key across frames");
        input.KeyEvent("KeyW", false);
        Check(!input.IsDown("KeyW") && input.ControlWasReleased("KeyboardW"), "Shared keyboard release");
        input.PointerEvent(100, 200);
        input.PointerEvent(140, 190);
        input.ButtonEvent(0, true);
        input.WheelEvent(2);
        Check(input.PointerDeltaX() == 40 && input.PointerDeltaY() == -10, "Pointer movement");
        Check(input.ControlWasPressed("MouseLeft") && input.ControlValue("MouseWheel") == 2, "Pointer inputs");
        std::array<bool, 32> buttons{};
        std::array<float, 6> axes{0.5f, -0.4f, 0, 0, 0, 0.8f};
        buttons[0] = true;
        input.GamepadSnapshot(buttons, axes);
        Check(input.ControlWasPressed("GamePadA") && input.AccelerateAxis() == 0.8f, "Gamepad snapshot");
        input.EndFrame();
        input.GamepadSnapshot({}, {});
        Check(input.ControlWasReleased("GamePadA") && input.AccelerateAxis() == 0, "Gamepad disconnect release");
        for (int direction = 0; direction < 4; ++direction)
        {
            axes.fill(0);
            const auto index = static_cast<std::size_t>(direction < 2 ? 1 : 0);
            const float sign = direction == 1 || direction == 2 ? -1.0f : 1.0f;
            axes[index] = sign * 0.54f;
            input.GamepadSnapshot({}, axes);
            Check(!input.WasGamepadNavigationPressed(direction), "Stick below navigation threshold");
            axes[index] = sign * 0.6f;
            input.GamepadSnapshot({}, axes);
            Check(input.WasGamepadNavigationPressed(direction), "Stick direction entered");
            input.EndFrame();
            for (const float value : {0.6f, 0.4f, 0.56f})
            {
                axes[index] = sign * value;
                input.GamepadSnapshot({}, axes);
                Check(!input.WasGamepadNavigationPressed(direction), "Held or jittering stick does not repeat");
            }
            input.GamepadSnapshot({}, {});
            input.GamepadSnapshot({}, axes);
            Check(input.WasGamepadNavigationPressed(direction), "Neutral re-arms stick direction");
            input.Reset();
            Check(!input.WasGamepadNavigationPressed(direction), "Focus reset clears stick edge");
        }
        input.KeyEvent("Space", true);
        input.Reset();
        Check(!input.PointerValid() && !input.IsDown("Space") && !input.PointerButtonDown(0)
            && !input.ControlWasPressed("GamePadA") && !input.ControlWasReleased("GamePadA"), "Focus loss resets input");
    }
}

int main()
{
    try
    {
        TestUnicode();
        TestPaths();
        TestJobs();
        TestInput();
        Check(LamaPon::ParseVersionNumbers("v1.2.3") == std::vector<std::uint32_t>{1, 2, 3},
            "Shared version parser");
        Check(LamaPon::ParseVersionNumbers("1.4294967296").empty(), "Version overflow rejected");
        std::cout << "Platform foundation: Unicode, paths, jobs and versions passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
