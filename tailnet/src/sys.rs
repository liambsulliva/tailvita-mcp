//! Small SCE calls: auto-suspend inhibit, free-memory readout, and newlib heap counters.

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
    /// newlib's `struct mallinfo` (all `size_t`).
    #[repr(C)]
    pub struct MallInfo {
        pub arena: usize,
        pub ordblks: usize,
        pub smblks: usize,
        pub hblks: usize,
        pub hblkhd: usize,
        pub usmblks: usize,
        pub fsmblks: usize,
        pub uordblks: usize,
        pub fordblks: usize,
        pub keepcost: usize,
    }
    extern "C" {
        pub fn mallinfo() -> MallInfo;
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

/// newlib heap use, in KiB. Rust's allocator is newlib's malloc on the Vita,
/// so this covers every Rust and C allocation (not thread stacks).
#[derive(Clone, Copy, Default)]
pub struct HeapUse {
    /// Taken from the heap block so far (newlib's sbrk high-water, minus trims).
    pub arena_kb: usize,
    /// Handed out to callers right now.
    pub used_kb: usize,
}

#[cfg(target_os = "vita")]
pub fn heap_use() -> HeapUse {
    // SAFETY: mallinfo takes the malloc lock and returns a plain struct.
    let mi = unsafe { ffi::mallinfo() };
    HeapUse { arena_kb: mi.arena / 1024, used_kb: mi.uordblks / 1024 }
}

#[cfg(not(target_os = "vita"))]
pub fn heap_use() -> HeapUse {
    HeapUse::default()
}

/// Highest heap use seen across samples.
#[derive(Default)]
pub struct HeapPeak {
    pub arena_kb: usize,
    pub used_kb: usize,
}

impl HeapPeak {
    pub fn sample(&mut self, h: HeapUse) {
        self.arena_kb = self.arena_kb.max(h.arena_kb);
        self.used_kb = self.used_kb.max(h.used_kb);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn heap_peak_keeps_the_highest_values() {
        let mut p = HeapPeak::default();
        p.sample(HeapUse { arena_kb: 100, used_kb: 80 });
        p.sample(HeapUse { arena_kb: 90, used_kb: 95 });
        assert_eq!((p.arena_kb, p.used_kb), (100, 95));
    }
}
