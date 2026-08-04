#![no_std]
#![no_main]
use core::panic::PanicInfo;
#[panic_handler] fn panic(_: &PanicInfo) -> ! { loop {} }
static KERNEL_BIN: &[u8] = include_bytes!("../kernel/kernel.kern");

/* minimal COM1 serial debug for the bootloader itself */
fn ser(c: u8) { unsafe { core::arch::asm!("out dx, al", in("dx") 0x3F8u16, in("al") c, options(nostack)); } }
fn ser_str(s: &str) { for b in s.bytes() { ser(b); } }
fn ser_hex(v: usize) {
    let h = b"0123456789abcdef";
    for i in (0..16).rev() { ser(h[(v >> (i*4)) & 0xF]); }
}
#[repr(C)]
struct BootInfo { fb_base: usize, fb_size: usize, hr: u32, vr: u32, stride: u32, px_fmt: u32, tm_year: u16, tm_mon: u8, tm_mday: u8, tm_hour: u8, tm_min: u8, tm_sec: u8 }

#[no_mangle]
extern "efiapi" fn efi_main(_h: usize, st: usize) -> usize {
    let bs = unsafe { *((st as *const u8).add(96) as *const *const u8) };
    let lp: unsafe extern "efiapi" fn(*const u8,*const u8,*mut *mut u8)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(320) as *const *const u8)) };  /* LocateProtocol @320 */
    let guid: [u8;16] = [0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a];
    let mut gop: *mut u8 = core::ptr::null_mut();
    let lpr = unsafe { lp(guid.as_ptr(),core::ptr::null(),&mut gop) };
    if lpr != 0 || gop.is_null() { loop {} }
    /* OVMF GOP protocol: Mode ptr is at offset 24 (4th member) */
    let mode_ptr = unsafe { *((gop as *const u8).add(24) as *const *const u8) };  /* Mode ptr value */
    let mode = unsafe { &*(mode_ptr as *const GopMode) };  /* &GopMode */

    /* QueryMode = first member @0 */
    let qm: unsafe extern "efiapi" fn(*mut u8,u32,*mut usize,*mut *mut u8)->usize =
        unsafe { core::mem::transmute(*((gop as *const u8).add(0) as *const *const u8)) };
    let mut info_size: usize = 0;
    let mut info_ptr: *mut u8 = core::ptr::null_mut();
    let mut hr: u32 = 0; let mut vr: u32 = 0; let mut stride: u32 = 0;
    let qr1 = unsafe { qm(gop, mode.b, &mut info_size, &mut info_ptr) };

    if qr1 == 0 && !info_ptr.is_null() {
        let gi = unsafe { &*(info_ptr as *const GopInfo) };
        hr = gi.hr; vr = gi.vr; stride = gi.stride;

    } else {
        let mut sz2: usize = 0; let mut p2: *mut u8 = core::ptr::null_mut();
        let qr2 = unsafe { qm(gop, 0, &mut sz2, &mut p2) };
        if qr2 == 0 && !p2.is_null() {
            let gi = unsafe { &*(p2 as *const GopInfo) };
            hr = gi.hr; vr = gi.vr; stride = gi.stride;
    
        }
    }
    let ap: unsafe extern "efiapi" fn(usize,u32,usize,*mut usize)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(40) as *const *const u8)) };
    let total = KERNEL_BIN.len() + 0x600000 + core::mem::size_of::<BootInfo>(); // +1MB for BSS (map)
    let mut kern: usize = 0;
    unsafe { ap(0,7,(total+0xFFF)>>12,&mut kern); }
    unsafe { core::ptr::copy_nonoverlapping(KERNEL_BIN.as_ptr(),kern as *mut u8,KERNEL_BIN.len()); }
    /* BootInfo goes at the END of the allocation (after the 6MB BSS zone) —
       placing it right after KERNEL_BIN overlaps the kernel's .bss and the
       kernel's BSS-zeroing loop wipes hr/vr/stride! */
    let total = KERNEL_BIN.len() + 0x600000 + core::mem::size_of::<BootInfo>();
    let info = unsafe { (kern as *mut u8).add(total - core::mem::size_of::<BootInfo>()) as *mut BootInfo };
    /* ── GetTime：Runtime Services @88，GetTime=第一个函数 @24（EFI_TIME 16B）── */
    let mut tm_year: u16 = 0; let mut tm_mon: u8 = 0; let mut tm_mday: u8 = 0;
    let mut tm_hour: u8 = 0; let mut tm_min: u8 = 0; let mut tm_sec: u8 = 0;
    let rt = unsafe { *((st as *const u8).add(88) as *const *const u8) };
    if !rt.is_null() {
        let gt: unsafe extern "efiapi" fn(*mut u8, *mut u8)->usize =
            unsafe { core::mem::transmute(*((rt as *const u8).add(24) as *const *const u8)) };
        let mut tm = [0u8; 16];
        let gr = unsafe { gt(tm.as_mut_ptr(), core::ptr::null_mut()) };
        if gr == 0 {
            tm_year = u16::from_le_bytes([tm[0], tm[1]]);
            tm_mon = tm[2]; tm_mday = tm[3]; tm_hour = tm[4]; tm_min = tm[5]; tm_sec = tm[6];
        }
    }
    unsafe { *info = BootInfo { fb_base: mode.fb, fb_size: mode.fb_sz, hr, vr, stride, px_fmt: 0, tm_year, tm_mon, tm_mday, tm_hour, tm_min, tm_sec }; }


    //
    // // ── ExitBootServices: take control from firmware ──
    // let mut map_sz: usize = 0;
    // let mut map_key: usize = 0;
    // let mut desc_sz: u32 = 0;
    // let mut desc_ver: u32 = 0;
    // let mmap: unsafe extern "efiapi" fn(*mut u8, *mut usize, *mut usize, *mut u32, *mut u32)->usize =
    //     unsafe { core::mem::transmute(*((bs as *const u8).add(360) as *const *const u8)) };
    // // first call: get required buffer size (EFI_BUFFER_TOO_SMALL expected)
    // unsafe { mmap(core::ptr::null_mut(),&mut map_sz,&mut map_key,&mut desc_sz,&mut desc_ver); }
    // // allocate buffer via AllocatePool (EfiLoaderData=1)
    // let alloc_pool: unsafe extern "efiapi" fn(u32,usize,*mut *mut u8)->usize =
    //     unsafe { core::mem::transmute(*((bs as *const u8).add(56) as *const *const u8)) };
    // let mut map_buf: *mut u8 = core::ptr::null_mut();
    // unsafe { alloc_pool(1, map_sz, &mut map_buf); }
    // // second call: fill the map, get the real key
    // unsafe { mmap(map_buf,&mut map_sz,&mut map_key,&mut desc_sz,&mut desc_ver); }
    // // ExitBootServices (offset 104)
    // let ebs: unsafe extern "efiapi" fn(usize,usize)->usize =
    //     unsafe { core::mem::transmute(*((bs as *const u8).add(104) as *const *const u8)) };
    // unsafe { ebs(st, map_key); }

    unsafe { core::arch::asm!("mov rdi, {}", "call rax", in(reg) info, in("rax") kern, options(noreturn)); }
}
#[repr(C)] struct GopMode { a:u32,b:u32,c:u64,d:u64,fb:usize,fb_sz:usize }

#[repr(C)] struct GopInfo { _v:u32,hr:u32,vr:u32,_f:u32,_pi:[u32;4],stride:u32 }
