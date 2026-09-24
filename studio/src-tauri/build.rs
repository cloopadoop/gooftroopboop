fn main() {
    // tauri_build embeds icons/icon.ico into the exe, but does not tell Cargo
    // to watch it: regenerated icons were silently ignored by incremental
    // builds and the exe kept the old one. Rebuild whenever an icon changes.
    println!("cargo:rerun-if-changed=icons");
    println!("cargo:rerun-if-changed=tauri.conf.json");
    tauri_build::build()
}
