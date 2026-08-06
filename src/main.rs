#![no_std]
#![no_main]
use core::panic::PanicInfo;
#[panic_handler] fn panic(_: &PanicInfo) -> ! { loop {} }

/* minimal COM1 serial debug for the bootloader itself */
fn ser(c: u8) { unsafe { core::arch::asm!("out dx, al", in("dx") 0x3F8u16, in("al") c, options(nostack)); } }
fn ser_str(s: &str) { for b in s.bytes() { ser(b); } }
fn ser_hex(v: usize) {
    let h = b"0123456789abcdef";
    for i in (0..16).rev() { ser(h[(v >> (i*4)) & 0xF]); }
}
/* [UEFI/INFO] 引导串口日志 */
fn uinfo(s: &str) { ser_str("[UEFI/INFO] "); ser_str(s); ser(0x0D); ser(0x0A); }
fn uinfo_hex(s: &str, v: usize) { ser_str("[UEFI/INFO] "); ser_str(s); ser_str("=0x"); ser_hex(v); ser(0x0D); ser(0x0A); }
fn ucrit(s: &str) { ser_str("[UEFI/CRITICAL] "); ser_str(s); ser(0x0D); ser(0x0A); }

/* ── UEFI 文件读取（SimpleFileSystem——读 esp 的 memdisk.img）+ ExFAT 解析（读 KERNEL.BIN）── */
fn loc_proto(st: usize, bs: usize, guid: &[u8;16], out: &mut *mut u8) -> usize {
    let lp: unsafe extern "efiapi" fn(*const u8,*const u8,*mut *mut u8)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(320) as *const *const u8)) };
    unsafe { lp(guid.as_ptr(), core::ptr::null(), out) }
}
/* SimpleFileSystem: OpenVolume @0; EFI_FILE_PROTOCOL: Open@0,Close@8,Read@0x18 */
fn read_memdisk(st: usize, bs: usize, out_buf: *mut u8, max_len: usize) -> usize {
    uinfo_hex("rm st", st); uinfo_hex("rm bs", bs);
    // LocateProtocol(SimpleFileSystem)
    let fs_guid: [u8;16] = [0x22,0x5b,0x4e,0x96,0x59,0x64,0xd2,0x11,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b];
    let mut fs: *mut u8 = core::ptr::null_mut();
    let lr = loc_proto(st, bs, &fs_guid, &mut fs);
    uinfo_hex("fs proto", fs as usize); uinfo_hex("fs lr", lr);
    if lr != 0 || fs.is_null() { ucrit("FS proto fail"); return 0; }
    // OpenVolume
    let ov: unsafe extern "efiapi" fn(*mut u8,*mut *mut u8)->usize =
        unsafe { core::mem::transmute(*((fs as *const u8).add(8) as *const *const u8)) };  /* OpenVolume @8 (跳过 Revision) */
    let mut root: *mut u8 = core::ptr::null_mut();
    let ovr = unsafe { ov(fs, &mut root) };
    uinfo_hex("root", root as usize); uinfo_hex("ov ret", ovr);
    if ovr != 0 || root.is_null() { ucrit("OpenVolume fail"); return 0; }
    // root.Open("memdisk.img", OpenMode=1 read-only, Attributes=0)
    let open: unsafe extern "efiapi" fn(*mut u8,*mut *mut u8,*const u16,u64,u64)->usize =
        unsafe { core::mem::transmute(*((root as *const u8).add(8) as *const *const u8)) };  /* Open @8 */
    let mut file: *mut u8 = core::ptr::null_mut();
    let fname: [u16;12] = [0x6d,0x65,0x6d,0x64,0x69,0x73,0x6b,0x2e,0x69,0x6d,0x67, 0]; // "memdisk.img\0" (null 结尾——EFI 需要)
    let opr = unsafe { open(root, &mut file, fname.as_ptr(), 1, 0) };
    uinfo_hex("open ret", opr); uinfo_hex("file", file as usize);
    if opr != 0 || file.is_null() { ucrit("open memdisk.img fail"); return 0; }
    // file.Read(max_len, &buf)
    let read: unsafe extern "efiapi" fn(*mut u8,*mut usize,*mut u8)->usize =
        unsafe { core::mem::transmute(*((file as *const u8).add(32) as *const *const u8)) };  /* Read @32 */
    let mut sz = max_len;
    uinfo_hex("file", file as usize);
    let rr = unsafe { read(file, &mut sz, out_buf) };
    uinfo_hex("read ret", rr);
    // close
    let close: unsafe extern "efiapi" fn(*mut u8)->usize =
        unsafe { core::mem::transmute(*((file as *const u8).add(16) as *const *const u8)) };  /* Close @16 */
    unsafe { close(file); }
    if rr != 0 { ucrit("read memdisk fail"); return 0; }
    uinfo_hex("memdisk read", sz);
    sz
}
/* ExFAT 解析：从整盘镜像找 KERNEL.BIN，返回 (簇数据区指针, 长度) */
fn exfat_find_kern(img: *const u8, total: usize) -> (usize, usize) {
    let b: &[u8] = unsafe { core::slice::from_raw_parts(img, total) };
    // MBR 分区起始 LBA @0x1C6
    if b.len() < 0x200 || b[0x1FE]!=0x55 || b[0x1FF]!=0xaa { ucrit("no MBR"); return (0,0); }
    let lba = u32::from_le_bytes([b[0x1C6],b[0x1C7],b[0x1C8],b[0x1C9]]) as usize;
    let po = lba * 512;
    if b.len() < po+512 { return (0,0); }
    let bp = &b[po..];
    let fat_off = u32::from_le_bytes([bp[0x50],bp[0x51],bp[0x52],bp[0x53]]) as usize;
    let clus_heap = u32::from_le_bytes([bp[0x58],bp[0x59],bp[0x5a],bp[0x5b]]) as usize;
    let root_clus = u32::from_le_bytes([bp[0x60],bp[0x61],bp[0x62],bp[0x63]]) as usize;
    let spc_shift = bp[0x72] as usize;
    let cluster = 512 << spc_shift;
    // 根目录
    let root_off = po + clus_heap*512 + (root_clus-2)*cluster;
    uinfo_hex("ex root", root_off); uinfo_hex("ex first", b[root_off] as usize);
    // 遍历目录条目（32 字节对齐）
    let mut off = root_off;
    while off + 96 <= b.len() {
        let t = b[off];
        if t == 0x00 { break; }
        uinfo_hex("ex type", t as usize);
        if t & 0x80 != 0 && t & 0x7f == 0x01 {  // File entry 0x81
            // 文件: 32B file + 32B stream + Name entries
            let stream = off + 32;
            let st = b[stream];
            if st & 0x7f == 0x05 {  // 0x85 stream
                let dl = u64::from_le_bytes(b[stream+24..stream+32].try_into().unwrap()) as usize;
                let fc = u32::from_le_bytes([b[stream+16],b[stream+17],b[stream+18],b[stream+19]]) as usize;
                // name at stream+32 (first Name entry 0xC0|n)
                let ne = off + 64;
                let nt = b[ne];
                if nt & 0xC0 == 0xC0 {  // Name 条目 0xC0+（bit6=1 secondary——之前 nt&0x40==0 判断错会跳过）
                    let nlen = b[ne+1] as usize;
                    let nchars: &[u8] = unsafe { core::slice::from_raw_parts(b.as_ptr().add(ne+2), nlen*2) };
                    // 手动比较 "KERNEL.BIN"
                    let want: [u8;20] = [0x4b,0,0x45,0,0x52,0,0x4e,0,0x45,0,0x4c,0,0x2e,0,0x42,0,0x49,0,0x4e,0]; // "KERNEL.BIN" UTF16
                    let mut is_kern = nlen==10;
                    if is_kern { for i in 0..20 { if nchars[i] != want[i] { is_kern=false; break; } } }
                    uinfo_hex("is_kern", is_kern as usize);
                    if is_kern {
                        // 数据簇 @ clus_heap + (fc-2)*cluster
                        let d = po + clus_heap*512 + (fc-2)*cluster;
                        uinfo_hex("FOUND kern", d); uinfo_hex("kern dl", dl);
                        return (img as usize + d, dl);
                    } else {
                        uinfo("name not kern");
                    }
                }
            }
        }
        off += 32;
    }
    ucrit("KERNEL.BIN not found");
    (0,0)
}

fn bsod(fb: usize, stride: u32, hr: u32, vr: u32) -> ! {
    ucrit("Operating System not found.");
    let fb = fb as *mut u32; let s32 = (stride/4) as usize;
    let hr_u = hr as usize; let vr_u = vr as usize;
    for y in 0..vr_u { for x in 0..hr_u { unsafe { *fb.add(y*s32+x) = 0x00000000; } } }
    let my = vr_u/2;
    for x in 0..hr_u { unsafe { *fb.add(my*s32+x) = 0x00FFFFFF; } }
    for x in 0..hr_u { unsafe { *fb.add((my+20)*s32+x) = 0x00FFFFFF; } }
    loop {}
}
#[repr(C)]
struct BootInfo { fb_base: usize, fb_size: usize, hr: u32, vr: u32, stride: u32, px_fmt: u32, tm_year: u16, tm_mon: u8, tm_mday: u8, tm_hour: u8, tm_min: u8, tm_sec: u8 }

/* 全局存 SystemTable——naked 入口存（rdx）——start_kernel 读——绕过 LLVM 寄存器传递（#GP 根因） */
static mut ST: usize = 0;

#[no_mangle]
#[unsafe(naked)]
unsafe extern "efiapi" fn efi_main(_h: usize, _st: usize) -> usize {
    /* naked：手动 prologue——MS x64 rdx=SystemTable——存到全局 ST——jmp start_kernel */
    core::arch::naked_asm!(
        "lea rax, [rip + {st}]",
        "mov [rax], rdx",
        "jmp start_kernel",
        st = sym ST,
    )
}
#[no_mangle]
fn start_kernel() -> ! {
    let st = unsafe { ST };
    let bs = unsafe { *((st as *const u8).add(96) as *const *const u8) };   /* BootServices @0x60 (OVMF 布局——5730 正常版验证) */
    let lp: unsafe extern "efiapi" fn(*const u8,*const u8,*mut *mut u8)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(320) as *const *const u8)) };  /* LocateProtocol @320 */
    let guid: [u8;16] = [0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a];
    let mut gop: *mut u8 = core::ptr::null_mut();
    let lpr = unsafe { lp(guid.as_ptr(),core::ptr::null(),&mut gop) };
    if lpr != 0 || gop.is_null() { ucrit("LocateProtocol GOP fail"); loop {} }
    uinfo_hex("gop", gop as usize);
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
        uinfo_hex("mode hr", hr as usize); uinfo_hex("fb", mode.fb);

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
    /* ── 放 ExFAT：读 memdisk.img（UEFI 文件协议）→ ExFAT 解析 → KERNEL.BIN（不内嵌——引导小）── */
    let mb_total = 0x40000; /* 256KB 缓冲存 memdisk.img */
    let mut mb_buf: usize = 0;
    unsafe { ap(0,2,mb_total>>12,&mut mb_buf); }
    uinfo_hex("mb buf", mb_buf);
    if mb_buf == 0 { ucrit("memdisk alloc fail"); bsod(mode.fb, stride, hr, vr); }
    let md_sz = read_memdisk(st, bs as usize, mb_buf as *mut u8, mb_total);
    if md_sz == 0 { ucrit("memdisk read fail"); bsod(mode.fb, stride, hr, vr); }
    let (kern_src, kern_len) = exfat_find_kern(mb_buf as *const u8, md_sz);
    uinfo_hex("kern src", kern_src); uinfo_hex("kern len", kern_len);
    if kern_len == 0 { ucrit("no kern"); bsod(mode.fb, stride, hr, vr); }
    /* 分配内核区（kern_len + 6MB BSS + BootInfo）*/
    let total = kern_len + 0x600000 + core::mem::size_of::<BootInfo>();
    let mut kern: usize = 0;
    unsafe { ap(0,2,(total+0xFFF)>>12,&mut kern); }
    uinfo_hex("kern alloc", kern); uinfo_hex("total", total);
    if kern == 0 { ucrit("kern alloc fail"); bsod(mode.fb, stride, hr, vr); }
    unsafe { core::ptr::copy_nonoverlapping(kern_src as *const u8, kern as *mut u8, kern_len); }
    /* BootInfo at END of allocation（内核 .bss 清零会覆盖 bin 末尾——放最后）*/
    let info = unsafe { (kern as *mut u8).add(total - core::mem::size_of::<BootInfo>()) as *mut BootInfo };
    /* ── GetTime：Runtime Services @88，GetTime=第一个函数 @24（EFI_TIME 16B）── */
    let mut tm_year: u16 = 0; let mut tm_mon: u8 = 0; let mut tm_mday: u8 = 0;
    let mut tm_hour: u8 = 0; let mut tm_min: u8 = 0; let mut tm_sec: u8 = 0;
    let rt = unsafe { *((st as *const u8).add(88) as *const *const u8) };
    uinfo_hex("rt", rt as usize);
    if !rt.is_null() {
        let gt: unsafe extern "efiapi" fn(*mut u8, *mut u8)->usize =
            unsafe { core::mem::transmute(*((rt as *const u8).add(24) as *const *const u8)) };
        let mut tm = [0u8; 16];
        let gr = unsafe { gt(tm.as_mut_ptr(), core::ptr::null_mut()) };
        uinfo_hex("GetTime gr", gr as usize);
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

    uinfo_hex("tm_hour", tm_hour as usize); uinfo("jump kernel");
    unsafe { core::arch::asm!("mov rdi, {}", "call rax", in(reg) info, in("rax") kern, options(noreturn)); }
}
#[repr(C)] struct GopMode { a:u32,b:u32,c:u64,d:u64,fb:usize,fb_sz:usize }

#[repr(C)] struct GopInfo { _v:u32,hr:u32,vr:u32,_f:u32,_pi:[u32;4],stride:u32 }
