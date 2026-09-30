# -*- coding: utf-8 -*-
"""
XOS 构建脚本
============================================================
产物：
  build/boot.bin      512 B   MBR 主引导扇区
  build/stage2.bin    <=16 KB 第二阶段引导
  build/kernel.bin    <=64 KB 内核扁平二进制
  build/xos.img       10 MB   可引导磁盘镜像（可挂 VirtualBox）
============================================================
磁盘布局（LBA）：
  0             MBR
  1 .. 32       Stage2
  33 .. 160     Kernel（主副本）
  161 .. 288    Kernel（备用副本，主副本签名校验失败时回退读取）
============================================================
"""
import os
import subprocess
import sys

GCC = r'D:\develop\gcc\bin\gcc.exe'
LD = r'D:\develop\gcc\bin\ld.exe'
OBJCOPY = r'D:\develop\gcc\bin\objcopy.exe'
SIZE = r'D:\develop\gcc\bin\size.exe'

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_DIR = os.path.join(ROOT, 'boot')
KERNEL_DIR = os.path.join(ROOT, 'kernel')
INCLUDE_DIR = os.path.join(ROOT, 'include')
BUILD = os.path.join(ROOT, 'build')

IMG_SIZE = 10 * 1024 * 1024
STAGE2_SECTORS = 32
KERNEL_SECTORS = 660          # 登录界面加入后内核扩容至 660*512=337920B
KERNEL_LBA = 33          # 内核主副本起始 LBA
BKUP_LBA = 693           # 内核备用副本起始 LBA (KERNEL_LBA + KERNEL_SECTORS)
BKUP_SECTORS = KERNEL_SECTORS

# ---- MBR 分区表参数 ----------------------------------------------------
# XOS 引导分区：位于 LBA 1，覆盖 Stage2(32) + Kernel(192) + 备用内核(192) 扇区
PT_OFFSET = 446          # 分区表在引导扇区内的偏移
PT_ENTRIES = 4           # 固定 4 个分区项
PART_ACTIVE = 0x80       # 活动分区标志
PART_TYPE = 0x7F         # XOS 自定义分区类型
PART_LBA = 1             # 引导分区起始 LBA
PART_SECS = STAGE2_SECTORS + KERNEL_SECTORS + BKUP_SECTORS
CODE_LIMIT = 444         # 代码与数据不得越过完整性校验值存放位置
SUM_OFFSET = 444         # 完整性校验值存放位置
CHECK_LEN = SUM_OFFSET   # 校验覆盖偏移 0..443

CFLAGS = [
    '-m32', '-ffreestanding', '-nostdlib', '-nostdinc',
    '-fno-builtin', '-fno-stack-protector', '-fno-pic',
    '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
    '-fno-omit-frame-pointer',   # 栈回溯：异常诊断依赖 EBP 帧链
    '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-O2', '-I' + INCLUDE_DIR,
]

KERNEL_SOURCES = [
    'entry.S',        # 必须位于首位，保证 _start 落在 0x10000
    'isr_stubs.S',
    'console.c',
    'string.c',
    'idt.c',
    'e820.c',
    'pmm.c',
    'vmm.c',          # 分页核心：页表、地址转换、标志位、TLB、大页、隔离、ASLR
    'vmm_mem.c',      # 区域管理、缺页处理、写时复制、页换出与换入
    'vmm_cache.c',    # 后备存储、页缓存回写、反向映射、内存压力回收
    'vmm_test.c',     # 分页子系统自检（真机缺页链路验证）
    'div64.c',        # 64 位除法/取模运行时（-nostdlib 下 GCC 隐式调用 __udivdi3 等）
    'kmalloc.c',      # 内核堆分配器：slab/大块/内存池/调试/泄漏追踪/自检
    'memdetect.c',    # 固件内存描述探测：E820/ACPI/SMBIOS/物理位宽/空洞/持久化/监控
    'irq.c',          # 中断与异常子系统：PIC/PIT/IRQ 分发/softirq/看门狗/风暴/栈回溯
    'task.c',          # 进程与调度子系统：任务描述符/上下文切换/就绪队列/CFS/时间片/等待/信号/回收
    'switch.S',        # 上下文切换与任务入口汇编
    'sync.c',          # 同步原语：原子/自旋/互斥/读写锁/信号量/完成量/顺序锁/RCU/futex/等待队列/死锁检测/SPSC
    'syscall.c',       # 系统调用接口：编号表/0x80 分发/参数校验/用户态拷贝/错误码/审计/seccomp/限流/统计
    'fs.c',            # 文件系统核心：VFS/superblock/inode/dentry/fd表/路径解析/tmpfs/devfs/挂载/权限/一致性
    'sound.c',         # 音频子系统：设备模型/PC扬声器(PIT+0x61)/PCI探测(AC97/HDA)/PCM环形缓冲/格式转换/音量/时钟/省电/路由/统计
    'security.c',      # 安全机制子系统（第27册）：RNG(RDRAND+混合熵)/SHA-256/RC4/CRC32/密钥环/文件加密/IMA/审计/LSM钩子/MAC矩阵/AppArmor/seccomp/KASLR/NX/隔离验证/canary/签名哈希/信任链/基线/加固
    'display.c',       # 显示驱动子系统（第12册）：VGA文本/VBE探测/帧缓冲/模式切换/像素格式/双缓冲/硬件光标/垂直同步/EDID/多显示器/缩放/伽马亮度/2D软件加速/DRM-KMS/省电
    'keyboard.c',      # 键盘驱动子系统（第13册）：PS/2控制器/扫描码集1-2-3/修饰键/typematic/LED/组合键/键映射/输入事件队列/USB HID/多键盘/热插拔/输入法/布局切换/过滤/用户态分发
    'mouse.c',         # 鼠标驱动子系统（第14册）：PS/2鼠标协议/数据包解析(3-4-5字节)/位移累计/按键跟踪/滚轮/加速度平滑/光标裁剪/USB HID/多鼠标/热插拔/事件队列
    'disk.c',          # 存储驱动子系统（第15册）：ATA/PATA PIO/IDENTIFY/LBA28/AHCI探测/NVMe寄存器/SCSI命令/块设备bio/调度器FIFO+电梯/MBR与GPT分区/缓存预读/写回/TRIM/热插拔/错误重试/坏道重映射/SMART/磁盘加密/软RAID/LVM/配额/性能统计/省电停转
    'net.c',           # 网络设备驱动子系统（第17册）：PCI总线枚举(82540EM等)/以太网控制器驱动/DMA描述符环/收发缓冲池/中断入口/NAPI轮询/多队列/固件加载CRC16/链路状态机/MAC管理/混杂过滤/卸载能力/WOL省电/热插拔/复位恢复/性能调优/蓝牙HCI/veth对/统计诊断/自检12组
    'netstack.c',      # 网络协议栈子系统（第18册）：sk_buff/ARP/IPv4-6/ICMP/TCP/UDP/socket/DHCP/DNS/路由/NAT/防火墙/命名空间/QoS/统计
    'gui.c',           # 图形子系统GUI（第19册）：图形上下文/画布/调色板/图元/裁剪/位图/字体/事件/窗口/合成/光标/双缓冲/缩放/AA/统计
    'winman.c',       # 窗口管理器（第20册）：生命周期/Z序/移动缩放/装饰/最小化最大化/焦点/激活/阴影/工作区/吸附平铺/切换器/多显示器/输入路由/模态/动画/持久化/权限/合成/统计
    'desktop.c',      # 桌面环境（第21册）：背景/图标/任务栏/开始菜单/托盘/通知/时钟/启动器/右键/拖放/主题/设置/锁屏/登录/会话/搜索/语言/无障碍/诊断
    'widget.c',       # 控件库（第22册）：按钮/复选单选/文本/列表/下拉/进度滑块/滚动/菜单/工具状态栏/对话框/标签分组/树/选项卡/富文本/图表/布局/皮肤/事件
    'font.c',         # 字体渲染（第23册）：位图/TTF/OTF/光栅化/AA/亚像素/度量/CMAP/字距/回退/缓存/子集/合成/排版/BiDi/整形/CJK/许可
    'app.c',          # 应用程序生态（第24册）：ELF/程序加载/动态链接器/共享库管理
    'shell.c',        # 用户空间Shell（第25册）：命令行解析/环境变量/管道重定向/作业控制/历史
    'multiuser.c',    # 多用户与权限（第26册）：账户/用户组/登录认证/密码哈希/会话
    'pm.c',           # 电源管理（第28册）：ACPI表解析/机器状态/S3睡眠/S4休眠/S5关机
    'build.c',        # 构建系统（第29册）：配置解析/编译调度/汇编器/链接/格式转换
    'tester.c',       # 测试与验证（第30册）：单元/集成框架/用例管理/断言库/桩与模拟器
    'dbg.c',          # 调试与监控（第31册）：串口输出/日志缓冲/级别过滤/符号栈回溯/kgdb
    'inst.c',         # 安装程序-包管理（第32册）：介质引导/分区格式化/文件复制/引导装载/驱动选择
    'virt.c',         # 虚拟化支持（第33册）：hypervisor 探测/CPUID 特性/品牌识别/VirtIO 扫描/ACPI/TSC
    'shell_interactive.c',  # 交互式终端 Shell：自检通过后接管控制台，提供可用命令终端
    'login.c',         # 登录界面（第34册）：文本模式图形化登录/首登创建管理员/失败锁定
    'kmain.c',
]


def run(cmd, desc):
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if p.returncode != 0:
        print('[FAIL] %s' % desc)
        print('  cmd : %s' % ' '.join(cmd))
        if p.stdout:
            print('  out : %s' % p.stdout.strip()[:2000])
        if p.stderr:
            print('  err : %s' % p.stderr.strip()[:2000])
        sys.exit(1)
    warn = (p.stderr or '').strip()
    if warn:
        print('  [warn] %s' % warn[:600])
    return p


def check_code_space(path):
    """校验 MBR 的代码与数据未侵占分区表区（偏移 446 起）"""
    with open(path, 'rb') as f:
        data = f.read()
    last = -1
    for i in range(CODE_LIMIT - 1, -1, -1):
        if data[i] != 0:
            last = i
            break
    used = last + 1
    if used > CODE_LIMIT:
        print('[FAIL] MBR 代码越界：占用 %d 字节，上限 %d 字节' % (used, CODE_LIMIT))
        sys.exit(1)
    print('  代码占用    : %d B / %d B (余 %d B)' % (used, CODE_LIMIT, CODE_LIMIT - used))
    return used


def write_partition_table(path):
    """向引导扇区写入 4 个分区项：第 1 项为活动分区，其余为空项"""
    entry = bytearray(16)
    entry[0] = PART_ACTIVE                      # 活动标志
    entry[1:4] = b'\xFE\xFF\xFF'                # 起始 CHS 无效，以 LBA 为准
    entry[4] = PART_TYPE                        # 分区类型
    entry[5:8] = b'\xFE\xFF\xFF'                # 结束 CHS 无效，以 LBA 为准
    entry[8:12] = PART_LBA.to_bytes(4, 'little')
    entry[12:16] = PART_SECS.to_bytes(4, 'little')
    empty = bytes(16)
    table = bytes(entry) + empty * (PT_ENTRIES - 1)

    with open(path, 'r+b') as f:
        f.seek(PT_OFFSET)
        f.write(table)
        f.seek(510)
        sig = f.read(2)
    if sig != b'\x55\xAA':
        print('[FAIL] 引导签名缺失，偏移 510-511 实际为 %s' % sig.hex())
        sys.exit(1)
    print('  分区表      : 1 活动项(类型 %#04x, LBA %d, %d 扇区) + %d 空项'
          % (PART_TYPE, PART_LBA, PART_SECS, PT_ENTRIES - 1))
    print('  引导签名    : 55 AA (OK)')


def write_checksum(path):
    """按偏移 0..443 的字节累加和写入偏移 444，供 MBR 自身完整性校验使用
    算法须与 boot.S 的 addb/adcb 序列完全一致：低字节进位累加到高字节。
    """
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        lo = 0
        hi = 0
        for b in data[:CHECK_LEN]:
            lo += b
            if lo > 0xFF:
                lo &= 0xFF
                hi = (hi + 1) & 0xFF
        value = (hi << 8) | lo
        data[SUM_OFFSET:SUM_OFFSET + 2] = value.to_bytes(2, 'little')
        f.seek(0)
        f.write(data)
    print('  完整性校验值: %#06x (覆盖 0..%d)' % (value, CHECK_LEN - 1))
    return value


def verify_checksum(path):
    """独立回读校验：按同一算法复算并与扇区中存放的值比对"""
    data = open(path, 'rb').read()
    lo = 0
    hi = 0
    for b in data[:CHECK_LEN]:
        lo += b
        if lo > 0xFF:
            lo &= 0xFF
            hi = (hi + 1) & 0xFF
    expect = (hi << 8) | lo
    actual = int.from_bytes(data[SUM_OFFSET:SUM_OFFSET + 2], 'little')
    if expect != actual:
        print('[FAIL] 完整性校验值不符：期望 %#06x，实际 %#06x' % (expect, actual))
        sys.exit(1)
    print('  完整性回读  : %#06x == %#06x (OK)' % (expect, actual))


def build_boot():
    """MBR：汇编 → PE → 扁平二进制，必须严格 512 字节"""
    src = os.path.join(BOOT_DIR, 'boot.S')
    obj = os.path.join(BUILD, 'boot.o')
    pe = os.path.join(BUILD, 'boot.pe')
    out = os.path.join(BUILD, 'boot.bin')
    ldscript = os.path.join(BOOT_DIR, 'boot.ld')

    run([GCC, '-m32', '-c', src, '-o', obj], '汇编 MBR')
    run([LD, '-m', 'i386pe', '-T', ldscript, obj, '-o', pe], '链接 MBR')
    run([OBJCOPY, '-O', 'binary', pe, out], '生成 MBR 二进制')

    size = os.path.getsize(out)
    if size != 512:
        print('[FAIL] MBR 大小必须为 512 字节，实际 %d 字节' % size)
        sys.exit(1)
    print('  boot.bin    : %d B  (OK)' % size)
    check_code_space(out)
    write_partition_table(out)
    write_checksum(out)
    verify_checksum(out)
    return out


def build_stage2():
    src = os.path.join(BOOT_DIR, 'stage2.S')
    obj = os.path.join(BUILD, 'stage2.o')
    pe = os.path.join(BUILD, 'stage2.pe')
    out = os.path.join(BUILD, 'stage2.bin')
    ldscript = os.path.join(BOOT_DIR, 'stage2.ld')

    run([GCC, '-m32', '-c', src, '-o', obj], '汇编 Stage2')
    run([LD, '-m', 'i386pe', '-T', ldscript, obj, '-o', pe], '链接 Stage2')
    run([OBJCOPY, '-O', 'binary', pe, out], '生成 Stage2 二进制')

    size = os.path.getsize(out)
    limit = STAGE2_SECTORS * 512
    if size > limit:
        print('[FAIL] Stage2 超出 %d 字节上限，实际 %d 字节' % (limit, size))
        sys.exit(1)
    print('  stage2.bin  : %d B  (上限 %d B)' % (size, limit))
    return out


def build_kernel():
    objs = []
    for src in KERNEL_SOURCES:
        path = os.path.join(KERNEL_DIR, src)
        obj = os.path.join(BUILD, src.replace('.', '_') + '.o')
        run([GCC] + CFLAGS + ['-c', path, '-o', obj], '编译 %s' % src)
        objs.append(obj)

    pe = os.path.join(BUILD, 'kernel.pe')
    out = os.path.join(BUILD, 'kernel.bin')
    ldscript = os.path.join(KERNEL_DIR, 'linker.ld')

    # entry.o 必须排在最前，保证 _start 落在 0x10000
    run([LD, '-m', 'i386pe', '-T', ldscript] + objs + ['-o', pe], '链接内核')
    run([OBJCOPY, '-O', 'binary', pe, out], '生成内核二进制')

    size = os.path.getsize(out)
    limit = KERNEL_SECTORS * 512
    if size > limit:
        print('[FAIL] 内核超出 %d 字节上限，实际 %d 字节' % (limit, size))
        sys.exit(1)
    print('  kernel.bin  : %d B  (上限 %d B)' % (size, limit))

    p = subprocess.run([SIZE, pe], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if p.returncode == 0:
        for line in p.stdout.strip().splitlines():
            print('  size        : %s' % line)
    return out


def make_image(boot_bin, stage2_bin, kernel_bin):
    out = os.path.join(BUILD, 'xos.img')
    data = bytearray(IMG_SIZE)

    with open(boot_bin, 'rb') as f:
        b = f.read()
    data[0:len(b)] = b

    with open(stage2_bin, 'rb') as f:
        s = f.read()
    off = 1 * 512
    data[off:off + len(s)] = s

    with open(kernel_bin, 'rb') as f:
        k = f.read()
    off = KERNEL_LBA * 512
    data[off:off + len(k)] = k
    # 备用副本与主副本内容一致，供 Stage2 在签名校验失败时回退读取
    off = BKUP_LBA * 512
    data[off:off + len(k)] = k
    print('  kernel 主副本: LBA %d..%d' % (KERNEL_LBA, KERNEL_LBA + KERNEL_SECTORS - 1))
    print('  kernel 备副本: LBA %d..%d' % (BKUP_LBA, BKUP_LBA + BKUP_SECTORS - 1))

    with open(out, 'wb') as f:
        f.write(data)
    print('  xos.img     : %d B  (%d MB)' % (len(data), IMG_SIZE // 1048576))
    return out


def main():
    os.makedirs(BUILD, exist_ok=True)
    print('=' * 60)
    print('XOS 构建开始')
    print('=' * 60)

    print('[1/4] 构建 MBR')
    boot_bin = build_boot()

    print('[2/4] 构建 Stage2')
    stage2_bin = build_stage2()

    print('[3/4] 构建内核')
    kernel_bin = build_kernel()

    print('[4/4] 生成磁盘镜像')
    make_image(boot_bin, stage2_bin, kernel_bin)

    print('=' * 60)
    print('构建成功')
    print('=' * 60)


if __name__ == '__main__':
    main()
