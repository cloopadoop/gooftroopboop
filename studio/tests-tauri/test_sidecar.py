"""End-to-end tests for the real Tauri WebView, Rust bridge, and C++ sidecar."""

import json
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
APP = ROOT / "src-tauri" / "target" / PROFILE / "goof-troop-boop.exe"
ARTIFACTS = ROOT / "output" / "tauri"


class GoofTroopBoopE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not APP.exists():
            raise RuntimeError(f"Tauri application is not built: {APP}")

        tauri_driver = shutil.which("tauri-driver")
        edge_driver = shutil.which("msedgedriver")
        if not tauri_driver or not edge_driver:
            raise RuntimeError("tauri-driver and matching msedgedriver must be on PATH")

        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        cls.driver_log = (ARTIFACTS / "driver.log").open("w")
        cls.driver_process = subprocess.Popen(
            [tauri_driver, "--native-driver", edge_driver],
            stdout=cls.driver_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        cls.addClassCleanup(cls.cleanup_driver)
        cls._wait_for_driver()

        options = ArgOptions()
        options.set_capability("browserName", "wry")
        options.set_capability("tauri:options", {"application": str(APP)})
        client = ClientConfig(remote_server_addr="http://127.0.0.1:4444")
        connection = RemoteConnection(client_config=client)
        cls.driver = webdriver.Remote(command_executor=connection, options=options)
        cls.wait = WebDriverWait(cls.driver, 15, ignored_exceptions=(StaleElementReferenceException,))
        # WebDriver may attach while the WebView is still about:blank. Never
        # refresh until Tauri's initial navigation and boot have completed.
        cls.wait.until(lambda d: "soundbank.spc" in d.find_element(By.ID, "song-title").text)
        cls.app_pid = int(subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='goof-troop-boop.exe'\" | "
             "Where-Object { $_.ExecutablePath -eq '" + str(APP).replace("'", "''") + "' } | Select-Object -ExpandProperty ProcessId"],
            capture_output=True, text=True, check=True).stdout.strip())

    @classmethod
    def cleanup_driver(cls):
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

    def setUp(self):
        (ARTIFACTS / f"{self._testMethodName}.png").unlink(missing_ok=True)
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
        self.wait_command("setNote", lambda: self.change("#in-len", 24))
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
                with wave.open(str(path), "rb") as wav:
                    self.assertAlmostEqual(wav.getnframes() / wav.getframerate(), 60, delta=.1)

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
        self.assertEqual((self.notes()[0]["tick"],self.notes()[0]["pitch"]),(144,97))
        self.wait_command("resizeNote", lambda: self.canvas_drag(160,215,-10,0))
        self.assertEqual(self.notes()[0]["len"],24)
        self.canvas_click(213,275)
        self.wait_command("setInstrument", lambda: self.change("#in-prog",5))
        self.canvas_click(141,215)
        self.click("#btn-eyedrop")
        self.wait_command("setInstrument", lambda: self.canvas_click(213,275))
        self.assertEqual([n["program"] for n in self.notes()], [5,5])

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
        self.wait_command("optimize",lambda:self.click("#btn-optimize"))
        self.wait.until(lambda _: "ghost(s) kept" in self.element("#status").text)
        self.driver.execute_script("testConfirm=true")
        self.wait_command("optimize",lambda:self.click("#btn-optimize"))
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
        self.canvas_drag(102,203,144,96)
        self.canvas_drag(117,227,24,-12)
        self.wait.until(lambda _: "Moved 2 notes" in self.element("#status").text)
        moved=sorted((n["tick"],n["pitch"]) for t in self.state()["tracks"] for n in t["notes"] if not n["rest"])
        self.assertEqual(moved,[(144,97),(336,93)])
        self.assertTrue(self.driver.title.startswith("•"))

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
        small=self.driver.find_elements(By.CSS_SELECTOR,".slot-row.too-small")
        self.assertTrue(small)
        self.assertTrue(all(button.get_property("disabled") for button in small))
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

    def test_real_tauri_sidecar_edit_cycle(self):
        title = self.driver.find_element(By.ID, "song-title")
        self.wait.until(lambda _driver: "soundbank.spc" in title.text)
        self.assertIn("soundbank.spc", self.driver.title)
        self.assertIn("971 / 971 bytes", self.driver.find_element(By.ID, "budget").text)
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
        self.assertIn("35 / 1388 bytes", self.driver.find_element(By.ID, "budget").text)

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

    def engine_pid(self):
        output = subprocess.run(["powershell", "-NoProfile", "-Command",
            f"Get-CimInstance Win32_Process -Filter \"Name='gtb-engine.exe' AND ParentProcessId={self.app_pid}\" | Select-Object -ExpandProperty ProcessId"],
            capture_output=True, text=True, check=True).stdout.strip()
        return int(output)  # fails if absent or ambiguous; never uses global tasklist

    def test_z_sidecar_crash_reports_error_and_allows_explicit_new(self):
        self.new_song()
        self.add_tracker_note()
        child = self.engine_pid()
        subprocess.run(["taskkill", "/PID", str(child), "/T", "/F"],capture_output=True,check=True)
        self.menu("act","new")
        self.wait.until(lambda _: "New failed" in self.element("#status").text)
        self.assertTrue(self.driver.title.startswith("•"))
        self.new_song()
        self.assertNotEqual(self.engine_pid(),child)
        self.add_tracker_note()
        self.assertEqual(len(self.notes()),1)

    def test_y_crash_never_exports_stale_saved_version_over_unsaved_work(self):
        self.new_song()
        self.add_tracker_note()
        self.save_session("crash-saved.gtb")
        self.add_tracker_note(row=1)
        child=self.engine_pid()
        subprocess.run(["taskkill","/PID",str(child),"/T","/F"],capture_output=True,check=True)
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
