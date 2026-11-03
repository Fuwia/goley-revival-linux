use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=ncrypt-shim/ncrypt_shim.c");
    println!("cargo:rerun-if-changed=ncrypt-shim/ncrypt.def");
    println!("cargo:rerun-if-changed=ncrypt-shim/dinput8_shim.c");
    println!("cargo:rerun-if-changed=ncrypt-shim/dinput8.def");
    println!("cargo:rerun-if-changed=ncrypt-shim/build.sh");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR missing"));
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR missing"));
    let script = manifest_dir.join("ncrypt-shim").join("build.sh");

    let status = Command::new("bash")
        .arg(&script)
        .arg(&out_dir)
        .status()
        .expect("Failed to execute ncrypt-shim/build.sh");

    if !status.success() {
        panic!(
            "Failed to build ncrypt64.dll / ncrypt32.dll shims! \
             Install `mingw-w64` (x86_64-w64-mingw32-gcc & i686-w64-mingw32-gcc) \
             or `clang` + `lld` + `wine` headers."
        );
    }
}
