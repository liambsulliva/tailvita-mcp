//! Small SCE calls: auto-suspend inhibit and free-memory readout.

pub struct FreeMemory {
    pub user_kb: i32,
    pub cdram_kb: i32,
    pub phycont_kb: i32,
}

#[cfg(target_os = "vita")]
mod ffi {
    #[repr(C)]
    pub struct SceKernelFreeMemorySizeInfo {
        pub size: i32,
        pub size_user: i32,
        pub size_cdram: i32,
        pub size_phycont: i32,
    }
    extern "C" {
        pub fn sceKernelPowerTick(tick_type: i32) -> i32;
        pub fn sceKernelGetFreeMemorySize(info: *mut SceKernelFreeMemorySizeInfo) -> i32;
    }
    pub const SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND: i32 = 1;
}

/// Reset the auto-suspend timer. Call at least every few seconds.
#[cfg(target_os = "vita")]
pub fn power_tick() {
    // SAFETY: plain syscall with an integer argument.
    unsafe {
        ffi::sceKernelPowerTick(ffi::SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
}

#[cfg(not(target_os = "vita"))]
pub fn power_tick() {}

/// Free memory in this process's budget, in KiB (-1 if the call fails).
#[cfg(target_os = "vita")]
pub fn free_memory() -> FreeMemory {
    let mut mi = ffi::SceKernelFreeMemorySizeInfo {
        size: std::mem::size_of::<ffi::SceKernelFreeMemorySizeInfo>() as i32,
        size_user: 0,
        size_cdram: 0,
        size_phycont: 0,
    };
    // SAFETY: `mi` is a valid, correctly sized out-parameter.
    let rc = unsafe { ffi::sceKernelGetFreeMemorySize(&mut mi) };
    if rc < 0 {
        return FreeMemory { user_kb: -1, cdram_kb: -1, phycont_kb: -1 };
    }
    FreeMemory { user_kb: mi.size_user / 1024, cdram_kb: mi.size_cdram / 1024, phycont_kb: mi.size_phycont / 1024 }
}

#[cfg(not(target_os = "vita"))]
pub fn free_memory() -> FreeMemory {
    FreeMemory { user_kb: -1, cdram_kb: -1, phycont_kb: -1 }
}
