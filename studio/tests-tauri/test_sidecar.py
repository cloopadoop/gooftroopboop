"""End-to-end tests for the real Tauri WebView, Rust bridge, and C++ sidecar."""

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest

from selenium import webdriver
from selenium.common.exceptions import StaleElementReferenceException
from selenium.webdriver.common.by import By
from selenium.webdriver.common.action_chains import ActionChains
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.common.options import ArgOptions
from selenium.webdriver.remote.client_config import ClientConfig
from selenium.webdriver.remote.remote_connection import RemoteConnection
from selenium.webdriver.support.ui import WebDriverWait


ROOT = Path(__file__).resolve().parents[1]
PROFILE = "release" if os.environ.get("GTB_TEST_RELEASE") == "1" else "debug"
APP = Path(os.environ.get("GTB_TEST_APP", str(ROOT / "src-tauri" / "target" / PROFILE / "goof-troop-boop.exe"))).resolve()
ARTIFACTS = ROOT / "output" / "tauri"


class GoofTroopBoopE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not APP.exists():
            raise RuntimeError(f"Tauri application is not built: {APP}")
        from native_windows import processes, windows
        cls.before_console_windows = set(windows(None, 'ConsoleWindowClass', 'msedgewebview2.exe'))
        cls.before_pids = {p["ProcessId"] for p in processes()}
        if any(str(p.get("ExecutablePath", "")).lower() == str(APP).lower() for p in processes()):
            raise RuntimeError(f"Close the existing instance before testing: {APP}")

        tauri_driver = shutil.which("tauri-driver")
        edge_driver = shutil.which("msedgedriver")
        if not tauri_driver or not edge_driver:
            raise RuntimeError("tauri-driver and matching msedgedriver must be on PATH")

        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        (ARTIFACTS / 'process-cleanup.json').unlink(missing_ok=True)
        (ARTIFACTS / 'console-visibility.json').unlink(missing_ok=True)
        (ARTIFACTS / 'browser-console.json').unlink(missing_ok=True)
        cls.driver_log = (ARTIFACTS / "driver.log").open("w")
        cls.driver_process = subprocess.Popen(
            [tauri_driver, "--native-driver", edge_driver],
            stdout=cls.driver_log,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
        )
        cls.addClassCleanup(cls.cleanup_driver)
        cls._wait_for_driver()

        options = ArgOptions()
        # Pass native capabilities through tauri-driver without its lossy
        # tauri:options translation, which cannot express excludeSwitches.
        options.set_capability("browserName", "webview2")
        options.set_capability("ms:edgeChromium", True)
        options.set_capability("ms:loggingPrefs", {"browser": "ALL"})
        # Native Chromium logging can allocate a visible console on Windows,
        # even when directed to a file. Enable only by explicit diagnostic opt-in.
        native_logging = os.environ.get('GTB_TEST_WEBVIEW_NATIVE_LOG') == '1'
        browser_args = []
        if native_logging:
            browser_args = ['--enable-logging', f"--log-file={ARTIFACTS / 'webview-native.log'}"]
        options.set_capability("ms:edgeOptions", {
            "binary": str(APP),
            "excludeSwitches": ["enable-logging"],
            "webviewOptions": {"additionalBrowserArguments": browser_args},
        })
        client = ClientConfig(remote_server_addr="http://127.0.0.1:4444")
        connection = RemoteConnection(client_config=client)
        cls.driver = webdriver.Remote(command_executor=connection, options=options)
        cls.wait = WebDriverWait(cls.driver, 15, ignored_exceptions=(StaleElementReferenceException,))
        # WebDriver may attach while the WebView is still about:blank. Never
        # refresh until Tauri's initial navigation and boot have completed.
        cls.wait.until(lambda d: "soundbank.spc" in d.find_element(By.ID, "song-title").text)
        applications = [p for p in processes()
                        if str(p.get("ExecutablePath", "")).lower() == str(APP).lower()]
        if len(applications) != 1:
            raise AssertionError(f"Expected one process at {APP}, got {len(applications)}")
        cls.app_pid = applications[0]["ProcessId"]
        from process_ownership import identity, observe
        snapshot = processes()
        roots = [p for p in snapshot if p['ProcessId'] in (cls.app_pid, cls.driver_process.pid)]
        if len(roots) != 2 or any(identity(p) is None for p in roots):
            raise AssertionError('Cannot establish test process identities')
        if (cls.driver_process.poll() is not None or
                identity(applications[0]) not in {identity(p) for p in roots}):
            raise AssertionError('Test process changed during identity capture')
        cls.owned_identities = observe(snapshot, {identity(p) for p in roots})
        engines = [p for p in processes() if p["ParentProcessId"] == cls.app_pid
                   and Path(p.get("ExecutablePath") or "").name.lower() == "gtb-engine.exe"]
        if len(engines) != 1:
            raise AssertionError(f"Expected one app-owned engine, got {len(engines)}")
        engine_path = Path(engines[0]["ExecutablePath"]).resolve(strict=True)
        (ARTIFACTS / "application-manifest.json").write_text(json.dumps({
            "application": {"path": str(APP), "sha256": hashlib.sha256(APP.read_bytes()).hexdigest()},
            "engine": {"path": str(engine_path), "sha256": hashlib.sha256(engine_path.read_bytes()).hexdigest()},
            "profile": PROFILE}, indent=2), encoding="utf-8")

    @classmethod
    def cleanup_driver(cls):
        from native_windows import processes, process_alive
        from process_ownership import observe, surviving
        # Discover only descendants of live, creation-time-verified identities.
        # Never expand ancestry from an exited or reused numeric PID.
        snapshot = processes()
        owned = observe(snapshot, getattr(cls, 'owned_identities', set()))
        driver = getattr(cls, "driver", None)
        if driver:
            cls.driver = None
            try:
                driver.quit()
            except Exception:
                pass
        process = getattr(cls, "driver_process", None)
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        if process and process.stdout:
            process.stdout.close()
        if getattr(cls, "driver_log", None):
            cls.driver_log.close()
        # Never terminate a PID-derived tree: stale parent IDs may now belong
        # to unrelated applications. Report survivors without forced cleanup.
        deadline = time.monotonic() + 5
        remaining = surviving(processes(), owned)
        while remaining and time.monotonic() < deadline:
            time.sleep(.05)
            remaining = surviving(processes(), owned)
        (ARTIFACTS / "process-cleanup.json").write_text(json.dumps({
            "application": str(APP), "ownedPids": sorted(key[0] for key in owned),
            "forcedPids": [], "remainingPids": remaining}, indent=2), encoding="utf-8")
        if remaining:
            raise AssertionError(f"Test-owned processes survived cleanup: {remaining}")

    def setUp(self):
        from native_windows import processes
        from process_ownership import observe
        type(self).owned_identities = observe(processes(), type(self).owned_identities)
        (ARTIFACTS / f"{self._testMethodName}.png").unlink(missing_ok=True)
        (ARTIFACTS / f"{self._testMethodName}.failure.txt").unlink(missing_ok=True)
        self.driver.refresh()
        self.wait.until(lambda d: "soundbank.spc" in d.find_element(By.ID, "song-title").text)
        self.driver.execute_script("""
          window.testCalls = []; window.testDialogs = []; window.testErrors = [];
          window.testConfirm = true;
          window.confirm = () => window.testConfirm;
          addEventListener('unhandledrejection', e => testErrors.push(String(e.reason)));
          const real = window.fetch;
          window.fetch = async (url, options) => {
            const target = new URL(String(url), location.href);
            if (target.hostname !== 'ipc.localhost') return real(url, options);
            const cmd = decodeURIComponent(target.pathname.slice(1));
            const args = JSON.parse(options.body);
            if (cmd.startsWith('plugin:dialog|') && !window.testNativeDialogs) {
              if (!testDialogs.length) {
                testErrors.push('Unexpected dialog: ' + cmd);
                return new Response(JSON.stringify(null), {headers:{'Content-Type':'application/json','Tauri-Response':'ok'}});
              }
              const next = testDialogs.shift();
              if (next.cmd !== cmd) throw Error('Wrong dialog: ' + cmd);
              return new Response(JSON.stringify(next.value), {headers:{'Content-Type':'application/json','Tauri-Response':'ok'}});
            }
            const response = await real(url, options);
            if (cmd === 'engine_request') testCalls.push({request: args.request, result:await response.clone().json()});
            return response;
          };
        """)

    def element(self, selector):
        return self.driver.find_element(By.CSS_SELECTOR, selector)

    def click(self, selector):
        for attempt in range(3):
            try:
                self.element(selector).click()
                return
            except StaleElementReferenceException:
                if attempt == 2:
                    raise

    def menu(self, attribute, value):
        self.click("#btn-file")
        self.click(f"#menu-file [data-{attribute}='{value}']")

    def dialog(self, kind, value):
        self.driver.execute_script("testDialogs.push({cmd:arguments[0],value:arguments[1]})",
                                   "plugin:dialog|" + kind, str(value) if isinstance(value, Path) else value)

    def request(self, cmd, **params):
        result = self.driver.execute_async_script("""
          const done = arguments[1];
          window.__TAURI__.core.invoke('engine_request', {request:arguments[0]})
            .then(done, e => done({ok:false,error:String(e)}));
        """, {"cmd": cmd, **params})
        self.assertTrue(result.get("ok"), f"{cmd}: {result.get('error')}")
        return result

    def state(self):
        return self.request("state")["state"]

    def notes(self, track=0):
        return [n for n in self.state()["tracks"][track]["notes"] if not n["rest"]]

    def wait_command(self, cmd, action):
        before = self.driver.execute_script("return testCalls.length")
        action()
        def completed(_):
            return self.driver.execute_script("return testCalls.slice(arguments[0]).find(x=>x.request.cmd===arguments[1])", before, cmd)
        record = self.wait.until(completed)
        self.assertTrue(record["result"]["ok"], f"{cmd}: {record['result'].get('error')}")
        self.driver.execute_async_script("const done=arguments[0];requestAnimationFrame(()=>requestAnimationFrame(done))")
        return record["result"]

    def change(self, selector, value):
        # Real DOM change event; do not blur a replaced/stale inspector element.
        self.driver.execute_script("arguments[0].value=arguments[1];arguments[0].dispatchEvent(new Event('change',{bubbles:true}))",
                                   self.element(selector), str(value))

    def key(self, key, shift=False):
        self.driver.execute_script("document.activeElement.blur()")
        actions = ActionChains(self.driver).key_down(Keys.CONTROL)
        if shift:
            actions.key_down(Keys.SHIFT)
        actions.send_keys(key)
        if shift:
            actions.key_up(Keys.SHIFT)
        actions.key_up(Keys.CONTROL).perform()

    def new_song(self):
        self.wait_command("new", lambda: self.menu("act", "new"))
        self.assertEqual(self.state()["budget"]["used"], 35)

    def optimize(self, merge=False):
        self.click("#btn-optimize")
        self.wait.until(lambda _: self.element("#opt-ok").is_displayed())
        self.assertTrue(self.element("#opt-compact").is_selected())
        self.assertFalse(self.element("#opt-merge").is_selected())
        if merge:
            self.click("#opt-merge")
        return self.wait_command("optimize", lambda: self.click("#opt-ok"))

    def add_tracker_note(self, track=0, row=0):
        if not self.element("#tracker").is_displayed():
            self.click("#btn-tracker")
        self.wait_command("insertNote", lambda: self.click(f"[data-tka='{track}:{row}']"))

    def select_tracker_note(self, track=0):
        note = self.notes(track)[0]
        self.click(f"[data-tk='{track}:{note['i']}']")
        self.wait.until(lambda _: self.element("#in-pitch"))
        return note

    def save_session(self, name):
        path = ARTIFACTS / name
        self.dialog("save", path)
        self.wait_command("saveSession", lambda: self.menu("act", "save"))
        self.assertTrue(path.is_file())
        self.wait.until(lambda d: not d.title.startswith("•"))
        return path

    @classmethod
    def _wait_for_driver(cls):
        import urllib.request

        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if cls.driver_process.poll() is not None:
                output = cls.driver_process.stdout.read() if cls.driver_process.stdout else ""
                raise RuntimeError(f"tauri-driver exited early: {output}")
            try:
                urllib.request.urlopen("http://127.0.0.1:4444/status", timeout=1).read()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("timed out waiting for tauri-driver")

    def screenshot_on_failure(self):
        result = getattr(self, "_outcome", None)
        failures = [] if result is None else result.result.failures + result.result.errors
        if any(test is self for test, _ in failures):
            ARTIFACTS.mkdir(parents=True, exist_ok=True)
            (ARTIFACTS / f"{self._testMethodName}.failure.txt").write_text(
                "\n".join(detail for test, detail in failures if test is self), encoding="utf-8")
            self.driver.save_screenshot(str(ARTIFACTS / f"{self._testMethodName}.png"))

    def tearDown(self):
        if getattr(self, "app_closed", False):
            return
        self.screenshot_on_failure()
        calls = self.driver.execute_script("return testCalls.map(x=>({cmd:x.request.cmd,ok:x.result.ok}))")
        (ARTIFACTS / (self._testMethodName + ".json")).write_text(json.dumps(calls, indent=2), encoding="utf-8")
        self.assertEqual(self.driver.execute_script("return testDialogs.length"), 0, "unconsumed dialog answers")
        self.assertEqual(self.driver.execute_script("return testErrors"), [], "unhandled UI errors")

    def test_note_inspector_rest_delete_and_history(self):
        self.new_song()
        self.add_tracker_note()
        self.assertTrue(self.driver.title.startswith("•"))
        self.select_tracker_note()
        self.wait_command("setNote", lambda: self.change("#in-pitch", 72))
        self.assertEqual(self.notes()[0]["pitch"], 72)
        self.wait_command("resizeNote", lambda: self.change("#in-len", 24))
        self.assertEqual(self.notes()[0]["len"], 24)
        self.wait_command("setInstrument", lambda: self.change("#in-prog", 5))
        self.assertEqual(self.notes()[0]["program"], 5)
        self.wait_command("renderNote", lambda: self.click("#btn-note-prev"))
        self.wait_command("setNote", lambda: self.click("#btn-note-rest"))
        self.assertEqual(self.notes(), [])
        self.wait_command("undo", lambda: self.click("#btn-undo"))
        self.assertEqual(self.notes()[0]["pitch"], 72)
        self.wait_command("eraseNote", lambda: self.click("#btn-note-del"))
        self.assertEqual(self.notes(), [])
        self.wait_command("undo", lambda: self.key("z"))
        self.assertEqual(len(self.notes()), 1)
        self.wait_command("redo", lambda: self.key("z", shift=True))
        self.assertEqual(self.notes(), [])

    def test_atomic_group_and_loop_sidecar_transactions(self):
        source = (".base $0D20\ndw a,b,b,b,b,b,b,b\n"
                  "a: db $08,1,$61,$60,$62,$60,$60,$60,$17\nb: db $17\n")
        loaded = self.request("importAsm", asm=source)["state"]
        track = next(t for t in loaded["tracks"] if any(not n["rest"] for n in t["notes"]))
        items = [{"track": track["index"], "tick": n["tick"], "pitch": n["pitch"]}
                 for n in track["notes"] if not n["rest"]]
        before = self.request("exportAsm")["asm"]
        self.request("moveNotes", items=items, dTick=12, dPitch=1)
        after = self.request("exportAsm")["asm"]
        self.assertNotEqual(before, after)
        self.request("undo")
        self.assertEqual(self.request("exportAsm")["asm"], before)
        self.request("redo")
        self.assertEqual(self.request("exportAsm")["asm"], after)
        self.request("importAsm", asm=source)
        self.request("createLoop", track=track["index"], startTick=0, endTick=24, slot=0, count=2)
        before = self.request("exportAsm")["asm"]
        self.request("replaceLoop", track=track["index"], loop=0, startTick=0, endTick=48, slot=0, count=3)
        after = self.request("exportAsm")["asm"]
        self.assertNotEqual(before, after)
        self.request("undo")
        self.assertEqual(self.request("exportAsm")["asm"], before)
        self.request("redo")
        self.assertEqual(self.request("exportAsm")["asm"], after)

    def test_settings_and_loops(self):
        self.new_song()
        self.add_tracker_note()
        self.add_tracker_note(row=1)
        old_settings = self.state()["tracks"][0]["settings"]
        self.change("#add-setting-type", "volume")
        self.change("#add-setting-val", 80)
        self.wait_command("addSetting", lambda: self.click("#btn-add-setting"))
        setting = next(s for s in self.state()["tracks"][0]["settings"] if s["type"] == "volume" and s["value"] == 80)
        self.wait_command("updateSetting", lambda: self.change(f"[data-set-value='{setting['i']}']", 65))
        self.assertTrue(any(s["type"] == "volume" and s["value"] == 65 for s in self.state()["tracks"][0]["settings"]))
        self.wait_command("removeSetting", lambda: self.click(f"[data-del-setting='{setting['i']}']"))
        self.assertEqual(self.state()["tracks"][0]["settings"], old_settings)
        self.change("#add-loop-start", 0)
        self.change("#add-loop-end", 24)
        self.wait_command("createLoop", lambda: self.click("#btn-add-loop"))
        loop = self.state()["tracks"][0]["loops"][0]
        self.assertEqual(loop["destTick"], 0)
        self.assertEqual(loop["tick"], 24)
        self.wait_command("updateLoopCount", lambda: self.change(f"[data-loop-count='{loop['i']}']", 3))
        self.assertEqual(self.state()["tracks"][0]["loops"][0]["count"], 3)
        self.wait_command("removeLoop", lambda: self.click(f"[data-del-loop='{loop['i']}']"))
        self.assertEqual(self.state()["tracks"][0]["loops"], [])
        self.change("#add-loop-end", 0)
        self.click("#btn-add-loop")
        self.assertIn("end tick must", self.element("#status").text)

    def test_session_save_open_cancel_failure_and_dirty_undo(self):
        self.new_song()
        self.add_tracker_note()
        expected = self.notes()
        path = self.save_session("roundtrip.gtb")
        self.wait_command("undo", lambda: self.click("#btn-undo"))
        self.assertTrue(self.driver.title.startswith("•"), "undo after save changes the saved document")
        self.dialog("open", path)
        self.wait_command("openSession", lambda: self.menu("act", "open"))
        self.assertEqual(self.notes(), expected)
        self.assertFalse(self.driver.title.startswith("•"))
        self.dialog("open", None)
        self.menu("act", "open")
        self.assertEqual(self.notes(), expected)
        self.dialog("save", None)
        self.menu("act", "save")
        self.assertEqual(self.notes(), expected)
        self.dialog("open", ARTIFACTS / "does-not-exist.gtb")
        self.menu("act", "open")
        self.wait.until(lambda _: "Open failed" in self.element("#status").text)
        self.assertEqual(self.notes(), expected)

    def test_file_format_roundtrips(self):
        self.new_song()
        self.add_tracker_note()
        expected = [(n["tick"], n["pitch"], n["len"]) for n in self.notes()]
        for fmt, ext, export, imp in [("spc", "spc", "save", "open"), ("asm", "asm", "exportAsm", "importAsm"), ("midi", "mid", "exportMidi", "importMidi")]:
            with self.subTest(format=fmt):
                path = ARTIFACTS / ("roundtrip." + ext)
                self.dialog("save", path)
                self.wait_command(export, lambda: self.menu("exp", fmt))
                self.assertGreater(path.stat().st_size, 0)
                self.new_song()
                self.dialog("open", path)
                def import_action():
                    self.menu("imp", fmt)
                    if fmt == "midi":
                        self.wait.until(lambda _: self.element("#mo-ok").is_displayed())
                        self.click("#mo-ok")
                    elif fmt == "asm":
                        self.wait.until(lambda _: self.element("#asm-ok").is_displayed())
                        self.click("#asm-ok")
                self.wait_command(imp, import_action)
                self.assertEqual([(n["tick"], n["pitch"], n["len"]) for n in self.notes()], expected)

    def test_transport_seek_mute_solo_and_audio_export(self):
        import wave
        self.wait_command("render", lambda: self.click("#btn-play"))
        self.wait.until(lambda _: float(self.element("#seek").get_property("value")) > 10)
        self.click("#btn-play")
        before = self.element("#seek").get_property("value")
        time.sleep(0.2)
        self.assertEqual(before, self.element("#seek").get_property("value"))
        self.driver.execute_script("const s=document.querySelector('#seek');s.value=500;s.dispatchEvent(new Event('input'))")
        self.assertAlmostEqual(float(self.element("#seek").get_property("value")), 500, delta=1)
        self.click("#btn-loop")
        self.assertIn("on", self.element("#btn-loop").get_attribute("class"))
        self.click("#btn-stop")
        self.assertEqual(float(self.element("#seek").get_property("value")), 0)
        self.click(".chan:nth-of-type(1) [data-act='mute']")
        result = self.wait_command("render", lambda: self.click("#btn-play"))
        request = self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='render').at(-1).request")
        self.assertEqual(request["mute"], [0])
        self.click("#btn-stop")
        self.click(".chan:nth-of-type(2) [data-act='solo']")
        self.wait_command("render", lambda: self.click("#btn-play"))
        request = self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='render').at(-1).request")
        self.assertEqual(request["mute"], [i for i in range(8) if i != 1])
        self.click("#btn-stop")
        for fmt, cmd in [("wav", "render"), ("mp3", "renderMp3")]:
            path = ARTIFACTS / ("export." + fmt)
            self.dialog("save", path)
            self.wait_command(cmd, lambda: self.menu("exp", fmt))
            self.assertGreater(path.stat().st_size, 1000)
            if fmt == "wav":
                # Export renders the song's own length (loop end or last event,
                # plus a short tail), not a fixed minute.
                request = self.driver.execute_script(
                    "return testCalls.filter(x=>x.request.cmd==='render').at(-1).request")
                self.assertGreater(request["seconds"], 2)
                self.assertLessEqual(request["seconds"], 15 * 60)
                with wave.open(str(path), "rb") as wav:
                    self.assertAlmostEqual(wav.getnframes() / wav.getframerate(), request["seconds"], delta=.1)

    def test_import_cancel_and_unsaved_guard(self):
        self.new_song()
        self.add_tracker_note()
        expected = self.notes()
        self.driver.execute_script("testConfirm=false")
        self.menu("act", "new")
        self.assertEqual(self.notes(), expected)
        self.menu("imp", "spc")
        self.assertEqual(self.notes(), expected)
        self.driver.execute_script("testConfirm=true")
        self.dialog("open", None)
        self.menu("imp", "spc")
        self.assertEqual(self.notes(), expected)
        self.assertTrue(self.driver.title.startswith("•"))

    def test_native_file_dialog_cancel_and_dirty_close_rejection(self):
        from native_windows import wait_dialog, click_dialog_button, request_close, windows
        self.new_song()
        self.add_tracker_note()
        expected = self.notes()
        self.driver.execute_script("window.testNativeDialogs=true")
        for action in ["open", "save"]:
            self.menu("act", action)
            dialog = wait_dialog(self.app_pid)
            click_dialog_button(dialog, 2)  # IDCANCEL
            self.wait.until(lambda _: not windows(self.app_pid, "#32770"))
            self.assertEqual(self.notes(), expected)
            self.assertTrue(self.driver.title.startswith("•"))
        main = windows(self.app_pid, title_contains="Goof Troop Boop")
        self.assertEqual(len(main), 1)
        request_close(main[0])
        dialog = wait_dialog(self.app_pid)
        click_dialog_button(dialog, 7)  # IDNO: reject dirty X-close
        self.wait.until(lambda _: not windows(self.app_pid, "#32770"))
        self.assertEqual(self.notes(), expected)
        self.menu("act", "quit")
        dialog = wait_dialog(self.app_pid)
        click_dialog_button(dialog, 7)
        time.sleep(.2)
        dialog = wait_dialog(self.app_pid)
        click_dialog_button(dialog, 7)
        self.wait.until(lambda _: not windows(self.app_pid, "#32770"))
        self.assertEqual(self.notes(), expected)

    def test_clipboard_paste_from_tracker_and_dirty_save_guard(self):
        self.new_song()
        self.add_tracker_note()
        self.select_tracker_note()
        self.key("c")
        self.assertIn("Copied 1 note", self.element("#status").text)
        self.save_session("before-paste.gtb")
        self.click("#btn-tracker")
        self.canvas_click(90, 12)  # ruler tick 48, before rendering any audio
        self.wait_command("insertNote", lambda: self.key("v"))
        self.assertEqual([(n["tick"], n["pitch"]) for n in self.notes()], [(0, 60), (48, 60)])
        self.assertTrue(self.driver.title.startswith("•"))
        self.dialog("message", "Yes")
        self.dialog("save", None)
        self.menu("act", "quit")
        self.wait.until(lambda _: self.driver.execute_script("return testDialogs.length") == 0)
        self.assertEqual(len(self.notes()), 2)

    def canvas_click(self, x, y):
        canvas = self.element("#roll")
        ActionChains(self.driver).move_to_element_with_offset(canvas, x-canvas.size["width"]/2, y-canvas.size["height"]/2).click().perform()

    def canvas_drag(self, x, y, dx, dy):
        canvas = self.element("#roll")
        ActionChains(self.driver).move_to_element_with_offset(canvas, x-canvas.size["width"]/2, y-canvas.size["height"]/2).click_and_hold().move_by_offset(dx, dy).release().perform()

    def test_canvas_add_move_resize_and_instrument_match(self):
        self.new_song()
        self.click("#btn-add")
        self.wait_command("insertNote", lambda: self.canvas_drag(114, 227, 24, 0))
        self.wait_command("insertNote", lambda: self.canvas_drag(210, 275, 24, 0))
        self.click("#btn-add")
        self.assertEqual([(n["tick"],n["pitch"],n["len"]) for n in self.notes()], [(96,96,48),(288,92,48)])
        self.wait_command("placeNote", lambda: self.canvas_drag(117,227,24,-12))
        sounding=lambda:[n for t in self.state()["tracks"] for n in t["notes"] if not n["rest"]]
        self.assertEqual(sorted((n["tick"],n["pitch"]) for n in sounding()),[(144,97),(288,92)])
        self.wait_command("resizeNote", lambda: self.canvas_drag(160,215,-10,0))
        self.assertEqual(next(n for n in sounding() if n["tick"]==144)["len"],24)
        before_inspector = self.request("exportAsm")["asm"]
        self.wait_command("resizeNote", lambda: self.change("#in-len", 12))
        self.assertEqual(sorted((n["tick"], n["len"]) for n in sounding()), [(144,12),(288,48)])
        after_inspector = self.request("exportAsm")["asm"]
        self.wait_command("undo", lambda: self.key("z"))
        self.assertEqual(self.request("exportAsm")["asm"], before_inspector)
        self.wait_command("redo", lambda: self.key("z", shift=True))
        self.assertEqual(self.request("exportAsm")["asm"], after_inspector)
        self.canvas_click(213,275)
        self.wait_command("setInstrument", lambda: self.change("#in-prog",5))
        self.canvas_click(141,215)
        self.click("#btn-eyedrop")
        self.wait_command("setInstrument", lambda: self.canvas_click(213,275))
        self.assertEqual([n["program"] for n in sounding()], [5,5])

    def test_ghost_session_cancel_legalize_discard_and_restore(self):
        # Editor extras can contain rejected notes: use a deliberately invalid
        # track so retry must reject, independently of optimizer improvements.
        self.new_song()
        seed = ARTIFACTS / "ghost-seed.gtb"
        ghost = {"track":99,"tick":48,"pitch":60,"len":24,"reason":"test rejection"}
        self.request("saveSession", path=str(seed), extra={"ghosts":[ghost]})
        self.dialog("open", seed)
        self.wait_command("openSession", lambda: self.menu("act","open"))
        self.dialog("open", None)
        self.menu("act","open")
        saved = self.save_session("ghost-after-cancel.gtb")
        self.assertEqual(self.request("openSession",path=str(saved))["extra"]["ghosts"],[ghost])
        self.driver.execute_script("testConfirm=false")
        self.optimize()
        self.wait.until(lambda _: "ghost(s) kept" in self.element("#status").text)
        self.driver.execute_script("testConfirm=true")
        self.optimize()
        self.wait.until(lambda _: "discarded" in self.element("#status").text)
        self.assertTrue(self.driver.title.startswith("•"))
        self.key("z")
        self.assertIn("Restored 1 ghost",self.element("#status").text)
        restored=self.save_session("ghost-restored.gtb")
        self.assertEqual(self.request("openSession",path=str(restored))["extra"]["ghosts"],[ghost])
        self.new_song()
        cleared=self.save_session("ghost-cleared.gtb")
        self.assertEqual(self.request("openSession",path=str(cleared))["extra"]["ghosts"],[])

    def test_group_drag_marks_saved_session_dirty(self):
        self.new_song()
        self.click("#btn-add")
        self.wait_command("insertNote",lambda:self.canvas_drag(114,227,24,0))
        self.wait_command("insertNote",lambda:self.canvas_drag(210,275,24,0))
        self.click("#btn-add")
        self.save_session("before-group-move.gtb")
        before = self.request("exportAsm")["asm"]
        self.canvas_drag(102,203,144,96)
        self.wait_command("moveNotes", lambda: self.canvas_drag(117,227,24,-12))
        self.wait.until(lambda _: "Moved 2 notes" in self.element("#status").text)
        moved=sorted((n["tick"],n["pitch"]) for t in self.state()["tracks"] for n in t["notes"] if not n["rest"])
        self.assertEqual(moved,[(144,97),(336,93)])
        self.assertTrue(self.driver.title.startswith("•"))
        after = self.request("exportAsm")["asm"]
        self.wait_command("undo", lambda: self.key("z"))
        self.assertEqual(self.request("exportAsm")["asm"], before)
        self.wait_command("redo", lambda: self.key("z", shift=True))
        self.assertEqual(self.request("exportAsm")["asm"], after)

    def test_all_setting_types_are_wired_to_engine_and_undo(self):
        self.new_song()
        self.add_tracker_note()
        for kind,value in [("tempo",455),("volume",80),("pan",64),("duration",192),("octave",5),("transpose",0),("lfo",1),("echo",1),("release",20)]:
            with self.subTest(setting=kind):
                baseline=self.state()["tracks"]
                self.change("#add-setting-type",kind)
                self.change("#add-setting-val",value)
                result=self.wait_command("addSetting",lambda:self.click("#btn-add-setting"))
                self.assertNotEqual(result["state"]["tracks"],baseline)
                self.wait_command("undo",lambda:self.click("#btn-undo"))
                self.assertEqual(self.state()["tracks"],baseline)

    def test_generic_midi_options_reach_real_converter(self):
        import struct
        self.new_song()
        events=bytes.fromhex("00 c0 05 00 90 40 64 60 80 40 00 00 ff 2f 00")
        midi=ARTIFACTS/"generic-options.mid"
        midi.write_bytes(b"MThd"+struct.pack(">IHHH",6,0,1,96)+b"MTrk"+struct.pack(">I",len(events))+events)
        self.dialog("open",midi)
        self.menu("imp","midi")
        self.wait.until(lambda _:self.element("#mo-ok").is_displayed())
        self.change("#mo-prog",0)
        self.change("#mo-dur",0)
        self.change("#mo-map","5:8")
        self.click("#mo-nodef")
        self.wait_command("importMidi",lambda:self.click("#mo-ok"))
        request=self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='importMidi').at(-1).request")
        self.assertEqual(request["options"],{"defaultProgram":0,"duration":0,"noDefaultMap":True,"programMap":[{"from":5,"to":8}]})
        sounding=[n for t in self.state()["tracks"] for n in t["notes"] if not n["rest"]]
        self.assertTrue(sounding)
        self.assertTrue(all(n["pitch"]==64 and n["program"]==8 for n in sounding))

    def test_rom_slot_import_export_and_cancel(self):
        rom=Path(os.environ.get("GTB_TEST_ROM", str(ROOT.parents[1]/"ROMs"/"Goof Troop (U) [!].smc")))
        self.assertTrue(rom.is_file(), "Provide a local ROM fixture with GTB_TEST_ROM; no ROM is checked in")
        baseline=self.state()["tracks"]
        self.dialog("open",rom)
        self.menu("exp","rom")
        self.wait.until(lambda _:self.element(".slot-row").is_displayed())
        small=[button for button in self.driver.find_elements(By.CSS_SELECTOR,".slot-row") if "expands ROM" in button.text]
        self.assertTrue(small)
        self.assertTrue(all(not button.get_property("disabled") for button in small))
        self.click(".modal-cancel")
        self.assertEqual(self.state()["tracks"],baseline)
        self.dialog("open",rom)
        self.menu("imp","rom")
        self.wait.until(lambda _: self.element("#modal-back").is_displayed() and len(self.driver.find_elements(By.CSS_SELECTOR,".slot-row"))==19)
        self.wait_command("openRom",lambda:self.click(".slot-row"))
        expected=self.state()["tracks"]
        self.dialog("open",rom)
        self.menu("imp","rom")
        self.wait.until(lambda _: self.element(".modal-cancel").is_displayed())
        self.click(".modal-cancel")
        self.assertEqual(self.state()["tracks"],expected)
        self.dialog("open",rom)
        self.menu("exp","rom")
        self.wait.until(lambda _: self.element(".slot-row").is_displayed())
        output=ARTIFACTS/"roundtrip.smc"
        self.dialog("save",output)
        self.wait_command("exportRom",lambda:self.click(".slot-row"))
        self.assertEqual(output.read_bytes(),rom.read_bytes())

    def test_failed_export_and_cancelled_midi_options_preserve_song(self):
        self.new_song()
        self.add_tracker_note()
        expected=self.notes()
        self.dialog("save", ARTIFACTS/"missing-parent"/"output.spc")
        self.menu("exp","spc")
        self.wait.until(lambda _: "Export SPC failed" in self.element("#status").text)
        self.assertEqual(self.notes(),expected)
        self.dialog("open",ARTIFACTS/"not-opened.mid")
        self.menu("imp","midi")
        self.wait.until(lambda _: self.element("#mo-cancel").is_displayed())
        self.click("#mo-cancel")
        self.assertEqual(self.notes(),expected)
        self.assertTrue(self.driver.title.startswith("•"))

    def test_rom_header_song_picker_dirty_cancel_switch_and_context_cleanup(self):
        rom=Path(os.environ.get("GTB_TEST_ROM", str(ROOT.parents[1]/"ROMs"/"Goof Troop (U) [!].smc")))
        self.assertTrue(rom.is_file())
        self.dialog("open",rom)
        self.menu("imp","rom")
        self.wait.until(lambda _: self.element(".slot-row").is_displayed())
        self.wait_command("openRom",lambda:self.click(".slot-row"))
        self.assertTrue(self.element("#songs-wrap").is_displayed())
        rows=self.driver.find_elements(By.CSS_SELECTOR,"#menu-songs .song-row")
        self.assertEqual(len(rows),19)
        current=self.element("#menu-songs .current").get_attribute("data-slot")
        other=next(row.get_attribute("data-slot") for row in rows if row.get_attribute("data-slot")!=current)
        self.click("#btn-tracker")
        track=next(t["index"] for t in self.state()["tracks"] if any(not n["rest"] for n in t["notes"]))
        pitch=self.select_tracker_note(track)["pitch"]
        self.wait_command("setNote",lambda:self.change("#in-pitch",pitch+1))
        baseline=self.state()["tracks"]
        self.driver.execute_script("testConfirm=false")
        self.click("#btn-songs")
        self.click(f"#menu-songs [data-slot='{other}']")
        self.assertEqual(self.state()["tracks"],baseline)
        self.assertEqual(self.element("#menu-songs .current").get_attribute("data-slot"),current)
        self.assertTrue(self.driver.title.startswith("•"))
        self.driver.execute_script("testConfirm=true")
        self.click("#btn-songs")
        self.wait_command("openRom",lambda:self.click(f"#menu-songs [data-slot='{other}']"))
        self.assertEqual(self.element("#menu-songs .current").get_attribute("data-slot"),other)
        self.assertFalse(self.driver.title.startswith("•"))
        self.assertNotEqual(self.state()["tracks"],baseline)
        self.new_song()
        self.assertFalse(self.element("#songs-wrap").is_displayed())
        self.assertEqual(self.driver.find_elements(By.CSS_SELECTOR,"#menu-songs .song-row"),[])

    def test_rom_relocation_slot_is_selectable_and_export_reopens_same_song(self):
        rom=Path(os.environ.get("GTB_TEST_ROM", str(ROOT.parents[1]/"ROMs"/"Goof Troop (U) [!].smc")))
        self.assertTrue(rom.is_file())
        baseline=self.state()["tracks"]
        signature=lambda tracks:[[(n["tick"],n["pitch"],n["len"],n["program"],n["rest"]) for n in t["notes"]] for t in tracks]
        original=rom.read_bytes()
        output=ARTIFACTS/"relocated.smc"
        output.unlink(missing_ok=True)
        self.dialog("open",rom)
        self.menu("exp","rom")
        self.wait.until(lambda _: self.element(".slot-row").is_displayed())
        rows=self.driver.find_elements(By.CSS_SELECTOR,".slot-row")
        index=next(i for i,row in enumerate(rows) if "expands ROM" in row.text)
        self.assertFalse(rows[index].get_property("disabled"))
        self.dialog("save",output)
        self.wait_command("exportRom",lambda:rows[index].click())
        self.assertEqual(output.stat().st_size,1048576+(512 if len(original)%1024==512 else 0))
        self.assertEqual(rom.read_bytes(),original,"export must not modify the ROM template")
        self.assertEqual(self.state()["tracks"],baseline)
        self.dialog("open",output)
        self.menu("imp","rom")
        self.wait.until(lambda _: self.element(".slot-row").is_displayed())
        self.wait_command("openRom",lambda:self.driver.find_elements(By.CSS_SELECTOR,".slot-row")[index].click())
        self.assertEqual(signature(self.state()["tracks"]),signature(baseline))

    def test_asm_review_cancel_validation_and_bulk_mapping(self):
        self.new_song()
        self.add_tracker_note()
        baseline=self.state()["tracks"]
        path=ARTIFACTS/"mapping-review.asm"
        self.dialog("save",path)
        self.wait_command("exportAsm",lambda:self.menu("exp","asm"))
        self.dialog("open",path)
        self.menu("imp","asm")
        self.wait.until(lambda _: self.element("#asm-cancel").is_displayed())
        self.click("#asm-cancel")
        self.assertEqual(self.state()["tracks"],baseline)
        self.assertTrue(self.driver.title.startswith("•"))
        self.dialog("open",path)
        self.menu("imp","asm")
        self.wait.until(lambda _: self.element("#asm-ok").is_displayed())
        rows=self.driver.find_elements(By.CSS_SELECTOR,"#asm-mappings select")
        self.assertTrue(rows)
        self.change("#"+rows[0].get_attribute("id"),"")
        self.click("#asm-ok")
        self.assertIn("Choose a valid target",self.element("#asm-error").text)
        self.assertEqual(self.state()["tracks"],baseline)
        self.change("#asm-all",5)
        self.click("#asm-apply-all")
        self.assertTrue(all(row.get_property("value")=="5" for row in rows))
        self.wait_command("importAsm",lambda:self.click("#asm-ok"))
        self.assertTrue(all(n["program"]==5 for n in self.notes()))
        self.assertEqual([(n["tick"],n["pitch"],n["len"]) for n in self.notes()],
                         [(n["tick"],n["pitch"],n["len"]) for n in baseline[0]["notes"] if not n["rest"]])

    def test_source_backed_repair_review_cancel_and_import(self):
        import random
        import struct
        rng = random.Random(724)
        notes = bytes(rng.randrange(0x61, 0x80) for _ in range(220))
        source = struct.pack(">8H", *([0xe10] * 8)) + bytes([8, 46, 24, 16, 8, 46, 6, 224]) + notes + b"\x17"
        rom = bytearray(4096)
        signature = bytes.fromhex("1c fd f6 01 04 2d f6 00 04 2d ad 08 90 10 fb 00 f4 08 da a0 8d 00 f7 a0 bb 00 d0 02 bb 08 6f")
        rom[32:32 + len(signature)] = signature
        table = 32 + len(signature)
        for opcode in (0x1e, 0x1f):
            rom[table + opcode * 2:table + opcode * 2 + 2] = bytes([0, 5])
        rom[table + 0x100] = 0x6f
        rom[512:512 + len(source)] = source
        rom_path = ARTIFACTS / "repair-synthetic.sfc"
        asm_path = ARTIFACTS / "repair-synthetic.asm"
        rom_path.write_bytes(rom)
        asm_path.write_text(".base $0D20\n!instrument_2E = #$08\n" +
                            "dw melody,melody,melody,melody,melody,melody,melody,melody\n" +
                            "melody: db 8,!instrument_2E\ndb $18\ndb $10,$08,$2D,$26\ndb $E0\n" +
                            "db " + ",".join(f"${n:02X}" for n in notes) + ",$17\n")
        self.new_song()
        self.add_tracker_note()
        baseline = self.state()["tracks"]
        for accept in (False, True):
            self.dialog("open", asm_path)
            self.menu("imp", "asm")
            self.wait.until(lambda _: self.element("#asm-repair").is_displayed())
            self.dialog("open", rom_path)
            self.wait_command("inspectAsmRepair", lambda: self.click("#asm-repair"))
            self.wait.until(lambda _: self.element("#asm-repair-ok").is_displayed())
            self.assertIn("2 byte changes", self.element("#asm-repair-changes").text)
            self.assertEqual(self.state()["tracks"], baseline)
            if not accept:
                self.click("#asm-repair-cancel")
                self.assertEqual(self.state()["tracks"], baseline)
            else:
                self.click("#asm-repair-ok")
                self.wait.until(lambda _: self.element("#asm-ok").is_displayed())
                self.wait_command("importAsm", lambda: self.click("#asm-ok"))
                self.assertTrue(self.notes())
                self.assertTrue(all(n["program"] == 8 for n in self.notes()))
        self.assertEqual(rom_path.read_bytes(), bytes(rom))

    def test_optimize_dialog_cancel_compact_merge_and_undo(self):
        self.new_song()
        # Two non-overlapping voices using the same instrument can be merged.
        self.request("insertNote",track=0,tick=0,pitch=60,len=24)
        self.request("insertNote",track=1,tick=48,pitch=64,len=24)
        baseline=self.state()["tracks"]
        self.click("#btn-optimize")
        self.wait.until(lambda _: self.element("#opt-cancel").is_displayed())
        self.assertTrue(self.element("#opt-compact").is_selected())
        self.assertFalse(self.element("#opt-merge").is_selected())
        count=self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='optimize').length")
        self.click("#opt-cancel")
        self.assertEqual(self.state()["tracks"],baseline)
        self.assertEqual(self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='optimize').length"),count)
        self.optimize()
        request=self.driver.execute_script("return testCalls.filter(x=>x.request.cmd==='optimize').at(-1).request")
        self.assertFalse(request.get("merge",False))
        compacted=self.state()["tracks"]
        self.assertEqual([[n for n in t["notes"] if not n["rest"]] for t in compacted],
                         [[n for n in t["notes"] if not n["rest"]] for t in baseline])
        result=self.optimize(merge=True)
        self.assertGreater(result["mergedTracks"],0)
        self.assertTrue(self.driver.title.startswith("•"))
        self.wait_command("undo",lambda:self.click("#btn-undo"))
        self.assertEqual(self.state()["tracks"],compacted)

    def test_native_click_jitter_and_draw_durations(self):
        self.new_song()
        self.click("#btn-add")
        self.wait_command("insertNote",lambda:self.canvas_click(114,227))
        self.wait_command("insertNote",lambda:self.canvas_drag(162,227,1,0))
        self.wait_command("insertNote",lambda:self.canvas_drag(210,227,24,0))
        self.assertEqual([(n["tick"],n["len"]) for n in self.notes()],[(96,24),(192,24),(288,48)])

    def test_setting_removal_restores_inherited_duration_and_undo(self):
        self.new_song()
        self.add_tracker_note()
        self.add_tracker_note(row=1)
        baseline=self.state()["tracks"]
        second_tick=self.notes()[1]["tick"]
        self.change("#add-setting-type","duration")
        self.change("#add-setting-tick",second_tick)
        self.change("#add-setting-val",128)
        self.wait_command("addSetting",lambda:self.click("#btn-add-setting"))
        changed=self.state()["tracks"]
        self.assertNotEqual(changed,baseline)
        setting=next(s for s in changed[0]["settings"] if s["tick"]==second_tick and s["value"]==128)
        self.wait_command("removeSetting",lambda:self.click(f"[data-del-setting='{setting['i']}']"))
        self.assertEqual(self.state()["tracks"],baseline)
        self.wait_command("undo",lambda:self.click("#btn-undo"))
        self.assertEqual(self.state()["tracks"],changed)
        self.wait_command("redo",lambda:self.click("#btn-redo"))
        self.assertEqual(self.state()["tracks"],baseline)

    def test_long_run_edit_save_reopen_undo_preserves_inheritance(self):
        cycles=int(os.environ.get("GTB_TEST_EDIT_CYCLES","20"))
        self.assertGreaterEqual(cycles,2)
        self.new_song()
        self.add_tracker_note()
        self.select_tracker_note()
        self.wait_command("setInstrument",lambda:self.change("#in-prog",5))
        baseline=self.state()["tracks"]
        started=time.monotonic()
        completed=0
        for cycle in range(cycles):
            with self.subTest(cycle=cycle):
                self.add_tracker_note(row=1)
                self.assertEqual([n["program"] for n in self.notes()],[5,5])
                self.wait_command("undo",lambda:self.click("#btn-undo"))
                self.assertEqual(self.state()["tracks"],baseline)
                path=self.save_session("long-run.gtb")
                self.dialog("open",path)
                self.wait_command("openSession",lambda:self.menu("act","open"))
                self.assertEqual(self.state()["tracks"],baseline)
                completed+=1
            (ARTIFACTS/"edit-soak.json").write_text(json.dumps({
                "requestedCycles":cycles,"completedCycles":completed,
                "attemptedCycles":cycle+1,"elapsedSeconds":time.monotonic()-started,
                "status":"running" if cycle+1<cycles else ("passed" if completed==cycles else "failed")
            }),encoding="utf-8")
            if (cycle+1)%10==0:
                print(f"  edit/session cycle {cycle+1}/{cycles}: {completed} passed, {time.monotonic()-started:.1f}s",flush=True)
        if PROFILE=="release" and completed==cycles:
            self.assertTrue(self.driver.save_screenshot(str(ARTIFACTS/f"edit-soak-release-{cycles}.png")))

    def test_real_tauri_sidecar_edit_cycle(self):
        title = self.driver.find_element(By.ID, "song-title")
        self.wait.until(lambda _driver: "soundbank.spc" in title.text)
        self.assertIn("soundbank.spc", self.driver.title)
        budget=self.state()["budget"]
        self.assertEqual(budget["used"],971)
        self.assertIn(f"971 / {budget['total']} bytes", self.driver.find_element(By.ID, "budget").text)
        self.assertFalse(self.driver.find_element(By.ID, "btn-file").get_property("disabled"))

        self.assertGreater(self.engine_pid(), 0)

        play = self.driver.find_element(By.ID, "btn-play")
        play.click()
        self.wait.until(lambda _driver: play.get_property("textContent").strip() == "⏸")
        self.assertIn("/", self.driver.find_element(By.ID, "time").text)
        self.driver.find_element(By.ID, "btn-stop").click()
        self.wait.until(lambda _driver: play.get_property("textContent").strip() == "▶")

        self.driver.find_element(By.ID, "btn-file").click()
        self.driver.find_element(By.CSS_SELECTOR, "#menu-file [data-act='new']").click()
        self.wait.until(lambda _driver: "untitled (new song)" in title.text)
        budget=self.state()["budget"]
        self.assertEqual(budget["used"],35)
        self.assertIn(f"35 / {budget['total']} bytes", self.driver.find_element(By.ID, "budget").text)

        self.driver.find_element(By.ID, "btn-add").click()
        self.driver.find_element(By.ID, "roll").click()
        budget = self.driver.find_element(By.ID, "budget")
        used_bytes = lambda: int(budget.text.split()[0])
        self.wait.until(lambda _driver: used_bytes() > 35)
        edited_bytes = used_bytes()
        self.assertIn("untitled (new song)", title.text)

        self.driver.find_element(By.ID, "btn-undo").click()
        self.wait.until(lambda _driver: used_bytes() == 35)
        self.driver.find_element(By.ID, "btn-redo").click()
        self.wait.until(lambda _driver: used_bytes() == edited_bytes)

        self.driver.find_element(By.ID, "btn-tracker").click()
        tracker = self.driver.find_element(By.ID, "tracker")
        self.wait.until(lambda _driver: tracker.is_displayed())
        self.assertGreaterEqual(len(tracker.find_elements(By.CSS_SELECTOR, "tbody tr")), 2)

    def test_no_webview_diagnostic_console(self):
        if os.environ.get('GTB_TEST_WEBVIEW_NATIVE_LOG') == '1':
            self.skipTest('Explicit native logging permits a Chromium diagnostic console')
        from native_windows import windows
        unexpected = set(windows(None, 'ConsoleWindowClass', 'msedgewebview2.exe')) - self.before_console_windows
        visible_app = windows(self.app_pid, title_contains='Goof Troop Boop')
        entries = self.driver.execute('getLog', {'type': 'browser'})['value']
        (ARTIFACTS / 'browser-console.json').write_text(json.dumps(entries), encoding='utf-8')
        (ARTIFACTS / 'console-visibility.json').write_text(json.dumps({
            'visibleEditorWindows': len(visible_app), 'newWebViewConsoleWindows': len(unexpected),
            'scope': 'window state at native assertion; not a frame-by-frame startup recording',
            'browserLogCaptured': True, 'nativeChromiumLogging': False}), encoding='utf-8')
        self.assertTrue(visible_app, 'The real editor must remain visible during UI tests')
        self.assertFalse(unexpected, 'A new WebView2 diagnostic console is visible')

    def engine_pid(self):
        from native_windows import processes
        from process_ownership import identity, observe
        snapshot = processes()
        parents = [p for p in snapshot if p['ProcessId'] == self.app_pid]
        if len(parents) != 1 or identity(parents[0]) not in type(self).owned_identities:
            raise AssertionError('Test application identity changed')
        children = [p for p in snapshot if p['ParentProcessId'] == self.app_pid
                    and Path(p.get('ExecutablePath') or '').name.lower() == 'gtb-engine.exe'
                    and identity(p) is not None and p['CreationTime'] >= parents[0]['CreationTime']]
        if len(children) != 1:
            raise AssertionError('Missing or ambiguous test engine identity')
        self.engine_identity = identity(children[0])
        type(self).owned_identities = observe(snapshot, type(self).owned_identities)
        return children[0]['ProcessId']

    def test_z_sidecar_crash_then_explicit_new_starts_fresh_engine(self):
        self.new_song()
        self.add_tracker_note()
        child = self.engine_pid()
        from native_windows import terminate_verified
        terminate_verified(self.engine_identity)
        # "new" needs nothing from the dead engine, so it retries on the fresh one.
        # The status already says "New song" from the start of this test, so
        # wait for the actual outcome: the song reloads clean.
        self.assertTrue(self.driver.title.startswith("•"))
        self.menu("act","new")
        self.wait.until(lambda _: not self.driver.title.startswith("•"))
        self.assertIn("New song", self.element("#status").text)
        self.assertNotEqual(self.engine_pid(),child)
        self.add_tracker_note()
        self.assertEqual(len(self.notes()),1)

    def test_y_crash_never_exports_stale_saved_version_over_unsaved_work(self):
        self.new_song()
        self.add_tracker_note()
        self.save_session("crash-saved.gtb")
        self.add_tracker_note(row=1)
        child=self.engine_pid()
        from native_windows import terminate_verified
        terminate_verified(self.engine_identity)
        output=ARTIFACTS/"must-not-export.spc"
        output.unlink(missing_ok=True)
        self.dialog("save",output)
        self.menu("exp","spc")
        self.wait.until(lambda _:"Export SPC failed" in self.element("#status").text)
        self.assertIn("unsaved changes cannot be recovered",self.element("#status").text)
        self.assertFalse(output.exists())
        self.assertTrue(self.driver.title.startswith("•"))
        self.new_song()

    def test_zz_native_dirty_close_accepts_and_reaps_sidecar(self):
        from native_windows import wait_dialog, click_dialog_button, request_close, windows, process_alive
        self.new_song()
        self.add_tracker_note()
        child=self.engine_pid()
        self.driver.execute_script("window.testNativeDialogs=true")
        main=windows(self.app_pid,title_contains="Goof Troop Boop")
        self.assertEqual(len(main),1)
        request_close(main[0])
        click_dialog_button(wait_dialog(self.app_pid),6)
        self.app_closed=True
        self.wait.until(lambda _: not process_alive(self.app_pid))
        self.wait.until(lambda _: not process_alive(child))


if __name__ == "__main__":
    unittest.main(verbosity=2)
