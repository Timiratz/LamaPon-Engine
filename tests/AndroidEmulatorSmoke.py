"""Exercise an Android native game on the 16 KB CI emulator."""
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_android_screenshot import count_green_marker

PACKAGE = "com.lamapon.game.android_game_x86_64"
ACTIVITY = PACKAGE + "/com.lamapon.runtime.GameActivity"
APK = ROOT / "test-output/platform-core/android-apk-build-x86_64/app/outputs/apk/debug/app-debug.apk"


def adb(*arguments, check=True):
    result = subprocess.run(["adb", *arguments], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
    if check and result.returncode:
        raise RuntimeError("adb failed: " + result.stdout + result.stderr)
    return result


def saved_values():
    result = adb("shell", "run-as", PACKAGE, "cat", "files/values.json", check=False)
    if result.returncode:
        return None
    try:
        return json.loads(result.stdout)
    except (ValueError, TypeError):
        return None


def saved_start_count():
    values = saved_values()
    if values is None:
        return None
    try:
        return int(values.get("native-probe-starts", "0"))
    except (ValueError, TypeError):
        return None


def saved_button_clicks():
    values = saved_values()
    if values is None:
        return None, None
    try:
        return (int(values.get("native-button-clicks", "0")),
                values.get("native-button-sender"))
    except (ValueError, TypeError):
        return None, None


def wait_for_start_count(minimum):
    for _ in range(45):
        count = saved_start_count()
        if count is not None and count >= minimum:
            return count
        time.sleep(1)
    raise RuntimeError("Android game did not persist its startup count")


def game_pid():
    return adb("shell", "pidof", PACKAGE).stdout.strip()


def screenshot_render():
    screenshot = subprocess.run(["adb", "exec-out", "screencap", "-p"],
                                capture_output=True, timeout=30)
    if screenshot.returncode:
        raise RuntimeError("adb screencap failed: "
                           + screenshot.stderr.decode("utf-8", errors="replace"))
    try:
        rendered = count_green_marker(screenshot.stdout)
    except ValueError:
        rendered = {"greenMarkerPixels": 0}
    return screenshot.stdout, rendered


def main():
    page_size = adb("shell", "getconf", "PAGE_SIZE").stdout.strip()
    assert page_size == "16384", "Expected a 16 KB emulator, got " + page_size
    assert APK.is_file(), "The generated x86_64 APK is missing"
    adb("install", "-r", str(APK))
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    initial_count = wait_for_start_count(1)
    assert initial_count == 1, "Fresh install did not start from a clean save"
    initial_pid = game_pid()
    assert initial_pid, "Game process exited after startup"

    for _ in range(30):
        screenshot, rendered = screenshot_render()
        if rendered["greenMarkerPixels"] >= 1000:
            print("Android scene rendered the green UI marker: " + json.dumps(rendered))
            break
        time.sleep(1)
    else:
        raise RuntimeError("Android screenshot did not contain the exported scene green UI marker: "
                           + json.dumps(rendered))

    screen_width, screen_height = struct.unpack(">II", screenshot[16:24])
    adb("shell", "input", "tap", str(screen_width // 2), str(screen_height // 2))
    for _ in range(45):
        click_count, click_sender = saved_button_clicks()
        if click_count == 1:
            break
        time.sleep(1)
    if click_count != 1 or click_sender != "Button":
        values = saved_values() or {}
        raise AssertionError(
            "Android touchscreen did not dispatch one UIButton clickEvent with its sender; "
            "clicks={!r}, sender={!r}, pointer={!r}".format(
                click_count, click_sender,
                values.get("native-pointer-diagnostic")))

    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    time.sleep(2)
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    resumed_count = wait_for_start_count(initial_count)
    resumed_pid = game_pid()
    assert resumed_count == initial_count, "The game restarted during foreground resume"
    assert resumed_pid == initial_pid, "Android replaced the game process during foreground resume"

    try:
        adb("shell", "wm", "size", "720x1280")
        for _ in range(30):
            resized_screenshot, resized_render = screenshot_render()
            resized_width, resized_height = struct.unpack(">II", resized_screenshot[16:24])
            if ((resized_width, resized_height) != (screen_width, screen_height)
                    and resized_render["greenMarkerPixels"] >= 1000):
                break
            time.sleep(1)
        else:
            raise RuntimeError("Android game did not keep rendering after display resize: "
                               + json.dumps({"size": [resized_width, resized_height],
                                             **resized_render}))
        assert game_pid() == initial_pid, "Display resize restarted the Android game process"
        assert saved_start_count() == resumed_count, "Display resize restarted the game scene"
    finally:
        adb("shell", "wm", "size", "reset", check=False)

    adb("shell", "am", "force-stop", PACKAGE)
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    restarted_count = wait_for_start_count(resumed_count + 1)
    assert saved_start_count() == restarted_count
    assert saved_button_clicks() == (1, "Button"), (
        "Touch click event did not persist across game restart")
    print(f"Android game started on a {page_size}-byte page-size emulator; touchscreen UIButton event, "
          f"same-process activity resume, display resize and save restart passed "
          f"({resumed_count} -> {restarted_count}).")


if __name__ == "__main__":
    main()
