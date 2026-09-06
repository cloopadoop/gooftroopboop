// Goof Troop Boop - Tauri backend.
// Manages a long-lived gtb-engine child process (the C++ Capcom SNES editor
// core) and proxies line-delimited JSON requests to it.

use serde_json::Value;
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::Mutex;
use std::time::Duration;
use tauri::State;

struct Engine {
    child: Option<Child>,
    stdin: Option<ChildStdin>,
    reader: Option<BufReader<ChildStdout>>,
}

impl Engine {
    fn new() -> Self {
        Engine { child: None, stdin: None, reader: None }
    }

    // Bundled sidecar dir: <exe dir>/bin (packaged as a Tauri resource and
    // also populated in the repo for dev builds run from target/debug).
    fn sidecar_dir() -> Option<std::path::PathBuf> {
        let exe = std::env::current_exe().ok()?;
        let dir = exe.parent()?.join("bin");
        if dir.join("gtb-engine.exe").exists() {
            return Some(dir);
        }
        // Dev fallback: the repo's src-tauri/bin next to the manifest.
        let dev = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("bin");
        if dev.join("gtb-engine.exe").exists() {
            return Some(dev);
        }
        None
    }

    fn engine_path() -> String {
        if let Ok(p) = std::env::var("GTB_ENGINE") {
            return p;
        }
        if let Some(dir) = Self::sidecar_dir() {
            return dir.join("gtb-engine.exe").to_string_lossy().into_owned();
        }
        // Last resort: let the OS resolve it from PATH.
        "gtb-engine.exe".to_string()
    }

    fn ensure(&mut self) -> Result<(), String> {
        if self.child.is_some() {
            return Ok(());
        }
        let path = Self::engine_path();
        let mut cmd = Command::new(&path);
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
        // gtb-engine is a console-subsystem exe; without this flag Windows
        // pops a visible console window next to the app.
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            cmd.creation_flags(CREATE_NO_WINDOW);
        }
        // Point the engine's own helpers at the bundled sidecars.
        if let Some(dir) = Self::sidecar_dir() {
            for (var, file) in [
                ("GTB_FFMPEG", "ffmpeg.exe"),
                ("GTB_CLI", "GTBoop-cli.exe"),
                ("GTB_SOUNDBANK", "soundbank.spc"),
            ] {
                let p = dir.join(file);
                if p.exists() && std::env::var(var).is_err() {
                    cmd.env(var, p);
                }
            }
        }
        let mut child = cmd
            .spawn()
            .map_err(|e| format!("spawn engine ({path}): {e}"))?;
        self.stdin = child.stdin.take();
        self.reader = child.stdout.take().map(BufReader::new);
        self.child = Some(child);
        Ok(())
    }

    fn request(&mut self, req: Value) -> Result<Value, String> {
        self.request_with_timeout(req, Duration::from_secs(120))
    }

    fn request_with_timeout(&mut self, req: Value, timeout: Duration) -> Result<Value, String> {
        self.ensure()?;
        // Pipes can block forever when a helper hangs. Keep the I/O off the
        // waiting thread so the child can be terminated at a bounded deadline.
        let mut stdin = self.stdin.take().ok_or("engine stdin missing")?;
        let mut reader = self.reader.take().ok_or("engine stdout missing")?;
        let (tx, rx) = std::sync::mpsc::channel();
        let worker = std::thread::spawn(move || {
            let result = Self::exchange(&mut stdin, &mut reader, req);
            let _ = tx.send((result, stdin, reader));
        });
        let result = match rx.recv_timeout(timeout) {
            Ok((result, stdin, reader)) => {
                self.stdin = Some(stdin);
                self.reader = Some(reader);
                result
            }
            Err(_) => Err("engine response timed out or worker closed".into()),
        };
        if result.is_err() {
            self.reset();
        }
        // reset kills a timed-out child, unblocking its reader/writer.
        let _ = worker.join();
        result
    }

    fn reset(&mut self) {
        self.stdin = None;
        self.reader = None;
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }

    fn exchange(stdin: &mut ChildStdin, reader: &mut BufReader<ChildStdout>, req: Value) -> Result<Value, String> {
        let line = req.to_string();
        {
            stdin.write_all(line.as_bytes()).map_err(|e| e.to_string())?;
            stdin.write_all(b"\n").map_err(|e| e.to_string())?;
            stdin.flush().map_err(|e| e.to_string())?;
        }
        let mut resp = String::new();
        let n = reader.read_line(&mut resp).map_err(|e| e.to_string())?;
        if n == 0 {
            // Engine died; drop it so the next call respawns.
            return Err("engine closed".into());
        }
        serde_json::from_str(&resp).map_err(|e| format!("bad engine response: {e}"))
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn fixture(mode: &str) -> Engine {
        let mut command = Command::new("python");
        command.args(["-u", concat!(env!("CARGO_MANIFEST_DIR"), "/tests/sidecar_fixture.py"), mode]);
        command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn().expect("Python is required for bridge fault tests");
        Engine { stdin: child.stdin.take(), reader: child.stdout.take().map(BufReader::new), child: Some(child) }
    }

    #[test]
    fn persistent_protocol_preserves_json_and_request_order() {
        let mut engine = fixture("echo");
        for n in 0..10 {
            let request = json!({"cmd":"echo", "n":n, "text":"quote\" newline\n Unicode ♫"});
            assert_eq!(engine.request(request.clone()).unwrap(), request);
        }
    }

    #[test]
    fn engine_validation_error_does_not_destroy_live_process() {
        let mut engine = fixture("echo");
        assert_eq!(engine.request(json!({"ok":false,"error":"invalid"})).unwrap()["ok"], false);
        assert!(engine.child.is_some());
        assert_eq!(engine.request(json!({"ok":true})).unwrap()["ok"], true);
    }

    #[test]
    fn malformed_response_clears_process_without_echoing_payload() {
        let mut engine = fixture("invalid");
        let error = engine.request(json!({})).unwrap_err();
        assert!(error.starts_with("bad engine response:"));
        assert!(!error.contains("fixture-private-sentinel"));
        assert!(engine.child.is_none() && engine.stdin.is_none() && engine.reader.is_none());
    }

    #[test]
    fn eof_clears_process() {
        let mut engine = fixture("eof");
        assert!(engine.request(json!({})).is_err());
        assert!(engine.child.is_none());
    }

    #[test]
    fn broken_pipe_clears_process() {
        let mut engine = fixture("echo");
        engine.child.as_mut().unwrap().kill().unwrap();
        engine.child.as_mut().unwrap().wait().unwrap();
        assert!(engine.request(json!({"cmd":"state"})).is_err());
        assert!(engine.child.is_none() && engine.stdin.is_none() && engine.reader.is_none());
    }

    #[test]
    fn unresponsive_sidecar_is_terminated_at_deadline() {
        let mut engine = fixture("hang");
        let start = std::time::Instant::now();
        let error = engine.request_with_timeout(json!({}), Duration::from_millis(100)).unwrap_err();
        assert!(error.contains("timed out"));
        assert!(start.elapsed() < Duration::from_secs(5));
        assert!(engine.child.is_none() && engine.stdin.is_none() && engine.reader.is_none());
    }
}

#[tauri::command]
fn quit_app(app: tauri::AppHandle) {
    app.exit(0);
}

#[tauri::command]
fn engine_request(state: State<Mutex<Engine>>, request: Value) -> Result<Value, String> {
    state.lock().unwrap().request(request)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Mutex::new(Engine::new()))
        .invoke_handler(tauri::generate_handler![engine_request, quit_app])
        .run(tauri::generate_context!())
        .expect("error while running Goof Troop Boop");
}
