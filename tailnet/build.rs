use std::time::{SystemTime, UNIX_EPOCH};

fn main() {
    // Build stamp for the startup log line (same scheme as the upstream demo).
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let days = now / 86400;
    let t = now % 86400;
    println!(
        "cargo::rustc-env=BUILD_TIMESTAMP={}.{:02}:{:02}:{:02}",
        days,
        t / 3600,
        (t % 3600) / 60,
        t % 60
    );
    // Nonexistent path: rerun every build so the stamp is never stale.
    println!("cargo::rerun-if-changed=NONEXISTENT_FORCE_RERUN");

    // sceKernelPowerTick (SceProcessmgr) and sceKernelGetFreeMemorySize
    // (SceSysmem) are not in arm-vita-eabi-gcc's default stub set.
    println!("cargo::rerun-if-env-changed=VITASDK");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("vita") {
        let vitasdk = std::env::var("VITASDK").unwrap_or_else(|_| "/usr/local/vitasdk".into());
        println!("cargo::rustc-link-search=native={vitasdk}/arm-vita-eabi/lib");
        println!("cargo::rustc-link-arg=-lSceProcessmgr_stub");
        println!("cargo::rustc-link-arg=-lSceSysmem_stub");

        // vita-elf-create appends ~2 KiB of SCE module data after the text
        // segment. The data segment starts at the next 64 KiB boundary, so
        // that gap depends on the text size and can be too small
        // ("Cannot allocate 1760 bytes for SCE data at end of segment 0;
        // segment 1 overlaps": one build had 808 bytes). VitaSDK's default
        // linker script adds `__sce_headroom` before the 64 KiB alignment,
        // so 4 KiB here always leaves room. Only link addresses move.
        println!("cargo::rustc-link-arg=-Wl,--defsym,__sce_headroom=0x1000");
    }
}
