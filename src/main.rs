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
fn read_memdisk(st: usize, bs: usize, devh: usize, out_buf: *mut u8, max_len: usize) -> usize {
    uinfo_hex("rm st", st); uinfo_hex("rm bs", bs);
    // LocateProtocol(SimpleFileSystem)
    let fs_guid: [u8;16] = [0x22,0x5b,0x4e,0x96,0x59,0x64,0xd2,0x11,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b];
    let mut fs: *mut u8 = core::ptr::null_mut();
    /* 先试"我启动的那个卷"（LoadedImage.DeviceHandle 上的 FS 协议）——多盘机器才不会拿错盘 */
    let mut lr = 0xFFFF_FFFFusize;
    if devh != 0 { lr = handle_proto(bs, devh, &fs_guid, &mut fs); uinfo_hex("fs via devh", lr); }
    if lr != 0 || fs.is_null() { lr = loc_proto(st, bs, &fs_guid, &mut fs); uinfo_hex("fs via locate", lr); }
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
/* ── 设备路径（DevicePath）解析：UEFI 已经知道"我从哪块盘来"，直接问到位置，不用猜 ── */
const GUID_LOADED_IMAGE: [u8;16] = [0xa1,0x31,0x1b,0x5b,0x62,0x95,0xd2,0x11,0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b];
const GUID_DEVICE_PATH:  [u8;16] = [0x91,0x6e,0x57,0x09,0x3f,0x6d,0xd2,0x11,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b];

/* HandleProtocol @0x98(152)：取"某个句柄上"的协议（LocateProtocol 是取"任意一个"，多盘时会拿错） */
fn handle_proto(bs: usize, h: usize, guid: &[u8;16], out: &mut *mut u8) -> usize {
    let hp: unsafe extern "efiapi" fn(usize, *const u8, *mut *mut u8) -> usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(152) as *const *const u8)) };
    unsafe { hp(h, guid.as_ptr(), out) }
}

/* 遍历 DevicePath 节点 → (控制器类型, PCI bus<<16|dev<<8|func, 分区起始LBA, 分区扇区数)
   类型：1=ATA/IDE 2=SATA(AHCI) 3=NVMe 4=USB 0=未知
   节点格式：Type(1) SubType(1) Length(2) Data...
     Hardware 0x01 / ACPI 0x02 : HID(4) UID(4)          → UID 即 PCI 总线号（PciRoot 惯例）
     Hardware 0x01 / PCI  0x01 : Func(1) Dev(1)          → dev/func
     Messaging 0x03 / 0x01 ATAPI / 0x10 SATA / 0x12 NVMe / 0x05 USB
     Media 0x04 / HardDrive 0x01 : PartNum(4) PartStart(8) PartSize(8)  ← 分区位置就在这儿 */
fn parse_dp(dp: *const u8) -> (u32, u32, u32, u32, u32, u32) {
    let mut kind = 0u32; let mut bus = 0u32; let mut dev = 0u32; let mut func = 0u32;
    let mut ach = 0u32; let mut adv = 0u32;   /* ATA 通道/盘位（仅 ATAPI 节点有）*/
    let mut plba = 0u32; let mut psz = 0u32; let mut seen_pci = false; let mut guard = 0;
    let mut p = dp as usize;
    loop {
        guard += 1; if guard > 64 { break; }
        let (t, st) = unsafe { (*(p as *const u8), *((p + 1) as *const u8)) };
        let len = unsafe { *((p + 2) as *const u16) } as usize;
        if len == 0 || t == 0x7f { break; }             /* End of Device Path */
        if t == 0x01 && st == 0x02 {                    /* ACPI: HID, UID */
            bus = unsafe { *((p + 8) as *const u32) };
        } else if t == 0x01 && st == 0x01 {             /* PCI: Func, Dev */
            func = unsafe { *(p as *const u8).add(4) } as u32;
            dev  = unsafe { *(p as *const u8).add(5) } as u32;
            seen_pci = true;
        } else if t == 0x03 {                           /* Messaging */
            if st == 0x01 {                             /* ATAPI: PrimarySecondary(1) SlaveMaster(1) Lun(2) */
                ach = unsafe { *(p as *const u8).add(4) } as u32;   /* 0=Primary 1=Secondary */
                adv = unsafe { *(p as *const u8).add(5) } as u32;   /* 0=Master  1=Slave  */
            }
            kind = match st { 0x01 => 1, 0x10 => 2, 0x12 => 3, 0x05 => 4, _ => kind };
        } else if t == 0x04 && st == 0x01 {             /* Media / HardDrive */
            plba = (unsafe { *((p + 8)  as *const u64) }) as u32;
            psz  = (unsafe { *((p + 16) as *const u64) }) as u32;
        }
        p += len;
    }
    let addr = if seen_pci { (bus << 16) | (dev << 8) | func } else { 0xFFFF_FFFF };
    (kind, addr, plba, psz, ach, adv)
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
struct BootInfo { fb_base: usize, fb_size: usize, hr: u32, vr: u32, stride: u32, px_fmt: u32, tm_year: u16, tm_mon: u8, tm_mday: u8, tm_hour: u8, tm_min: u8, tm_sec: u8,
                   /* ── 引导盘位置（UEFI 给的，不是猜的）── */
                   ctrl_kind: u32, pci_addr: u32, part_lba: u32, part_size: u32, ata_ch: u32, ata_dv: u32 }

/* 全局存 SystemTable / ImageHandle——naked 入口存（rdx / rcx）——start_kernel 读——
   绕过 LLVM 寄存器传递（#GP 根因）。rcx 必须存：ExitBootServices 第一个参数就是它 */
static mut ST: usize = 0;
static mut IH: usize = 0;

#[no_mangle]
#[unsafe(naked)]
unsafe extern "efiapi" fn efi_main(_h: usize, _st: usize) -> usize {
    /* naked：手动 prologue——MS x64 rcx=ImageHandle、rdx=SystemTable——都存全局——jmp start_kernel */
    core::arch::naked_asm!(
        "lea rax, [rip + {st}]",
        "mov [rax], rdx",
        "lea rax, [rip + {ih}]",
        "mov [rax], rcx",
        "jmp start_kernel",
        st = sym ST,
        ih = sym IH,
    )
}
#[no_mangle]
fn start_kernel() -> ! {
    let st = unsafe { ST };
    let ih = unsafe { IH };   /* ExitBootServices 要真 ImageHandle——不是 SystemTable */
    let bs = unsafe { *((st as *const u8).add(96) as *const *const u8) };
    /* ── 问 UEFI"我从哪块盘来"：LoadedImage.DeviceHandle = 加载我的设备；
       DevicePath 里直接带 控制器类型 / PCI bus-dev-func / 分区起始 LBA + 扇区数。
       这样内核不用猜盘、不用扫 PCI、也不用把 QEMU 的 BAR/vid 写死。 ── */
    let mut devh: usize = 0;
    let mut ctrl_kind: u32 = 0; let mut pci_addr: u32 = 0xFFFF_FFFF;
    let mut part_lba: u32 = 0; let mut part_size: u32 = 0;
    let mut ata_ch: u32 = 0; let mut ata_dv: u32 = 0;
    {
        let mut li: *mut u8 = core::ptr::null_mut();
        let lir = handle_proto(bs as usize, ih, &GUID_LOADED_IMAGE, &mut li);
        if lir == 0 && !li.is_null() {
            devh = unsafe { *((li as *const u8).add(24) as *const usize) };   /* DeviceHandle @24 */
            uinfo_hex("dev handle", devh);
            let mut dp: *mut u8 = core::ptr::null_mut();
            if handle_proto(bs as usize, devh, &GUID_DEVICE_PATH, &mut dp) == 0 && !dp.is_null() {
                let (k, a, l, s, c, d2) = parse_dp(dp as *const u8);
                ctrl_kind = k; pci_addr = a; part_lba = l; part_size = s;
                ata_ch = c; ata_dv = d2;
                uinfo_hex("ctrl kind", k as usize);   /* 1=ATA/IDE 2=SATA 3=NVMe 4=USB 0=? */
                uinfo_hex("pci addr", a as usize);    /* bus<<16 | dev<<8 | func */
                uinfo_hex("part lba", l as usize); uinfo_hex("part sz", s as usize);
                uinfo_hex("ata ch", c as usize); uinfo_hex("ata dv", d2 as usize);
            } else { ucrit("DevicePath fail"); }
        } else { ucrit("LoadedImage fail -> LocateProtocol 兜底"); }
    }   /* BootServices @0x60 (OVMF 布局——5730 正常版验证) */
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
    let md_sz = read_memdisk(st, bs as usize, devh, mb_buf as *mut u8, mb_total);
    if md_sz == 0 { ucrit("memdisk read fail"); bsod(mode.fb, stride, hr, vr); }
    let (kern_src, kern_len) = exfat_find_kern(mb_buf as *const u8, md_sz);
    uinfo_hex("kern src", kern_src); uinfo_hex("kern len", kern_len);
    if kern_len == 0 { ucrit("no kern"); bsod(mode.fb, stride, hr, vr); }
    /* 分配内核区（kern_len + 6MB BSS + BootInfo）*/
    let total = kern_len + 0x1000000 + core::mem::size_of::<BootInfo>();   /* +16MB：.bss 已涨到 ~10.3MB（back_buf 1.92MB 等） */
    let mut kern: usize = 0;
    unsafe { ap(0,2,(total+0xFFF)>>12,&mut kern); }
    uinfo_hex("kern alloc", kern); uinfo_hex("total", total);
    if kern == 0 { ucrit("kern alloc fail"); bsod(mode.fb, stride, hr, vr); }
    unsafe { core::ptr::copy_nonoverlapping(kern_src as *const u8, kern as *mut u8, kern_len); }
    /* BootInfo 放分配区最后（16MB .bss 区之后）——紧贴 bin 末尾会被内核 .bss 清零糊掉 */
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
    unsafe { *info = BootInfo { fb_base: mode.fb, fb_size: mode.fb_sz, hr, vr, stride, px_fmt: 0, tm_year, tm_mon, tm_mday, tm_hour, tm_min, tm_sec, ctrl_kind, pci_addr, part_lba, part_size, ata_ch, ata_dv }; }


    //
    /* ── ExitBootServices：从固件手里接管 ──
       顺序（规范）：GetMemoryMap@56 探大小 → AllocatePages 分配缓冲 → 再取一次图拿真 MapKey
       → ExitBootServices@232(ImageHandle, MapKey) + 有界重试 → cli → 跳内核。
       约束：取图之后到 EBS 之间不能再有任何分配（否则 MapKey 作废、EBS 返回 EFI_INVALID_PARAMETER），
       所以 memdisk 缓冲 / 内核 16MB / BootInfo / 这张图 的分配全都排在前面做完。
       偏移是 EFI_BOOT_SERVICES 真实位置（Hdr 24B 起数）：GetMemoryMap=0x38、SignalEvent=0x68、
       SetMem=0x168、ExitBootServices=0xE8 —— 之前误用的 360/56/104 分别是 SetMem/GetMemoryMap/SignalEvent。 */
    let gmm: unsafe extern "efiapi" fn(*mut usize, *mut u8, *mut usize, *mut usize, *mut u32)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(56) as *const *const u8)) };
    let ebs: unsafe extern "efiapi" fn(usize, usize)->usize =
        unsafe { core::mem::transmute(*((bs as *const u8).add(232) as *const *const u8)) };
    let mut map_sz: usize = 0; let mut map_key: usize = 0;
    let mut desc_sz: usize = 0; let mut desc_ver: u32 = 0;
    let mut map_pages: usize = 0; let mut map_buf: usize = 0;
    let mut tries = 0;
    loop {
        if map_buf == 0 {
            /* 探大小（返回 EFI_BUFFER_TOO_SMALL）→ 分配缓冲，留 8 个描述符余量 */
            map_sz = 0;
            unsafe { gmm(&mut map_sz, core::ptr::null_mut(), &mut map_key, &mut desc_sz, &mut desc_ver); }
            uinfo_hex("map sz", map_sz); uinfo_hex("desc sz", desc_sz);
            map_pages = (map_sz + 8 * desc_sz + 0xFFF) >> 12;
            if map_pages == 0 { map_pages = 1; }
            unsafe { ap(0, 2, map_pages, &mut map_buf); }
            if map_buf == 0 { ucrit("mmap alloc fail"); bsod(mode.fb, stride, hr, vr); }
            continue;
        }
        map_sz = map_pages << 12;
        let r = unsafe { gmm(&mut map_sz, map_buf as *mut u8, &mut map_key, &mut desc_sz, &mut desc_ver) };
        if r != 0 {
            if r == 0x8000000000000005 {   /* BUFFER_TOO_SMALL：加大缓冲重来（EBS 还没调，允许分配）*/
                map_pages = (map_sz + 0xFFF) >> 12; map_buf = 0; continue;
            }
            uinfo_hex("gmm ret", r); ucrit("GetMemoryMap fail"); bsod(mode.fb, stride, hr, vr);
        }
        uinfo_hex("map key", map_key);
        let e = unsafe { ebs(ih, map_key) };   /* ★ 真 ImageHandle——不是 SystemTable */
        if e == 0 { break; }
        uinfo_hex("ebs ret", e);
        tries += 1;
        if tries > 8 { ucrit("EBS retries exhausted"); bsod(mode.fb, stride, hr, vr); }
        /* MapKey 失效（这段时间内存图被改过）→ 重新取图再试，中间不再分配 */
    }
    uinfo("EBS OK");
    /* EBS 已经拆掉固件定时器，但固件 IDT 还在——内核装上自己的 IDT/PIC 之前
       有一段填满 4MB 显存的长循环，那段时间绝不能让中断落进固件处理程序 */
    unsafe { core::arch::asm!("cli"); }

    uinfo_hex("tm_hour", tm_hour as usize); uinfo("jump kernel");
    unsafe { core::arch::asm!("mov rdi, {}", "call rax", in(reg) info, in("rax") kern, options(noreturn)); }
}
#[repr(C)] struct GopMode { a:u32,b:u32,c:u64,d:u64,fb:usize,fb_sz:usize }

#[repr(C)] struct GopInfo { _v:u32,hr:u32,vr:u32,_f:u32,_pi:[u32;4],stride:u32 }
