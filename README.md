# XOS 内核源码（xos/）

完全自研的 x86 操作系统内核。不依赖任何外部核心、不使用任何收费或闭源组件，
引导程序、内核、内存管理、控制台、字符串库全部从零实现，仅使用 GCC 作为编译器。

**当前里程碑：M1 —— 内存子系统贯通（可引导、可运行、自检通过）**

---

## 一、当前已实现能力

| 层 | 内容 | 文件 |
|---|---|---|
| 引导 | 512 字节 MBR、INT13h LBA 扩展读、Stage2、A20、GDT、保护模式切换 | `boot/boot.S`、`boot/stage2.S` |
| 内存探测 | INT15h E820 探测、AH=88h 回退、类型识别、排序、合并、统计 | `boot/stage2.S`、`kernel/e820.c` |
| 物理内存 | 页帧位图分配器、单页/连续页分配释放、保留区、自检 | `kernel/pmm.c` |
| 控制台 | VGA 80x25 文本模式、颜色、滚动、光标、printf 子集 | `kernel/console.c` |
| 运行库 | memcpy/memset/memmove/str*/utoa/itoa | `kernel/string.c` |

---

## 二、构建

### 2.1 环境要求

| 工具 | 路径 | 说明 |
|---|---|---|
| GCC | `D:\develop\gcc\bin\gcc.exe` | TDM-GCC 10.3.0，同时充当汇编器（GAS） |
| LD | `D:\develop\gcc\bin\ld.exe` | GNU ld 2.36.1 |
| objcopy | `D:\develop\gcc\bin\objcopy.exe` | 生成扁平二进制 |
| Python | `python`（PATH 中） | 运行构建脚本 |

> **不使用 NASM**：NASM 2.16.01 在含中文的路径下会因编码问题报
> `unable to open output file`，因此统一改用 GAS（`.S` 文件，AT&T 语法）。

### 2.2 构建命令

```powershell
python "D:\新系统开发需求文档\项目代码\xos\build\build.py"
```

预期输出：

```
[1/4] 构建 MBR
  boot.bin    : 512 B  (OK)
[2/4] 构建 Stage2
  stage2.bin  : 528 B  (上限 16384 B)
[3/4] 构建内核
  kernel.bin  : 12412 B  (上限 65536 B)
[4/4] 生成磁盘镜像
  xos.img     : 10485760 B  (10 MB)
构建成功
```

### 2.3 构建产物的硬校验

| 产物 | 约束 | 校验方式 |
|---|---|---|
| `build/boot.bin` | **必须恰好 512 字节** | 构建脚本强校验，不符即中止 |
| 引导魔数 | 偏移 510-511 必须为 `55 AA` | 构建脚本 + `objdump` 复核 |
| `build/stage2.bin` | ≤ 16384 字节（32 扇区） | 构建脚本强校验 |
| `build/kernel.bin` | ≤ 65536 字节（128 扇区） | 构建脚本强校验 |

---

## 三、磁盘布局

| LBA | 内容 | 大小 |
|---|---|---|
| 0 | MBR | 512 B |
| 1 – 32 | Stage2 | ≤ 16 KB |
| 33 – 160 | 内核扁平二进制 | ≤ 64 KB |
| 161 – | 未使用（预留） | 剩余 |

---

## 四、在 VirtualBox 中运行

```powershell
$vb = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$b  = "D:\新系统开发需求文档\项目代码\xos\build"

# 1. 把原始镜像转换为 VDI
& $vb convertfromraw "$b\xos.img" "$b\xos.vdi" --format VDI

# 2. 挂载到虚拟机的 IDE 主盘
& $vb storageattach "XOS" --storagectl "IDE Controller" --port 0 --device 0 --type hdd --medium "$b\xos.vdi"

# 3. 启动
& $vb startvm "XOS" --type headless
```

虚拟机 `XOS` 已配置为：128MB 内存、BIOS 固件、从硬盘启动。

---

## 五、运行结果（实测）

VirtualBox 128MB 内存下的实际控制台输出：

```
XOS Booting...
XOS Stage2
======================================================
  XOS - eXperimental Operating System   v0.1.0
  Memory Subsystem Bring-up (Milestone M1)
======================================================

[1/3] Detecting physical memory (E820)...
  No  Base              Length            Type
  --- ----------------- ----------------- ---------------
  0   0x00000000        0x0009FC00        Usable
  1   0x0009FC00        0x00000400        Reserved
  ...
[2/3] Computing memory statistics...
  Entries (merged)     : 8
  Total address space  : 131016 KB
  Usable memory        : 130623 KB  (32655 pages)
  Reserved memory      : 329 KB
  ACPI reclaimable     : 64 KB
  Usable below 1MB     : 639 KB
  Highest address      : 0x8000000

[3/3] Initializing page frame allocator...
  Page size            : 4096 bytes
  Managed pages        : 32768  (128 MB)
  Reserved pages       : 304
  Used pages           : 272
  Free pages           : 32496  (126 MB)

[SELFTEST] Running PMM self-test...
  PASS: all 8 test cases succeeded.

Memory subsystem bring-up complete. System halted.
```

---

## 六、目录结构

```
xos/
├── boot/
│   ├── boot.S        MBR 主引导记录（512 字节）
│   ├── boot.ld       MBR 链接脚本
│   ├── stage2.S      第二阶段引导（E820 / A20 / GDT / 保护模式）
│   └── stage2.ld     Stage2 链接脚本
├── include/
│   ├── types.h       基础类型、宏、位操作
│   ├── string.h      字符串与内存操作
│   ├── console.h     VGA 文本控制台
│   ├── e820.h        E820 内存映射
│   ├── pmm.h         物理内存管理器
│   └── stdarg.h      变参支持（编译器内建封装）
├── kernel/
│   ├── entry.S       内核入口（.bss 清零 → 调用 kmain）
│   ├── linker.ld     内核链接脚本（0x10000）
│   ├── console.c     控制台实现
│   ├── string.c      运行库实现
│   ├── e820.c        内存探测与统计实现
│   ├── pmm.c         页帧分配器实现
│   └── kmain.c       内核主入口与自检调度
└── build/
    ├── build.py      构建脚本
    ├── boot.bin      MBR 产物
    ├── stage2.bin    Stage2 产物
    ├── kernel.bin    内核产物
    └── xos.img       可引导磁盘镜像（10 MB）
```

---

## 七、构建工具链的关键约束（踩坑记录）

1. **必须用 objcopy，不能用 `ld --oformat binary`**
   `ld` 的目标是 PE，`--oformat binary` 会报
   `cannot perform PE operations on non PE output file`。
   正确路径：`ld -m i386pe` → `objcopy -O binary`。

2. **必须用链接脚本丢弃 `.reloc` / `.idata` 等节区**
   mingw 的 ld 会自动生成 `.reloc` 重定位节，混入扁平二进制会让
   MBR 变成 1040 字节。三份链接脚本都已在 `/DISCARD/` 中丢弃。

3. **不要用 64 位整数除法**
   `-nostdlib` 下 `u64 / 10` 会产生 `__udivdi3` 未定义引用。
   容量统计统一用 `>> 10` 换算 KB 并截断为 `u32`。

4. **段寄存器不能直接参与 `add`**
   `addw %ax, %es` 是非法指令，必须经通用寄存器中转。

5. **必须保存 BIOS 启动盘号 DL**
   `INT15h E820` 会用 `EDX` 装载 `'SMAP'` 魔数，把 `DL` 覆盖成 `0x50`，
   导致后续 `INT13h` 用无效驱动器号读盘失败。引导程序必须在入口处
   立即保存 `DL`，每次磁盘调用前恢复。

---

## 八、下一步（M2）

- 分页机制：页目录/页表建立、开启 CR0.PG、线性地址映射
- 内核堆：基于页分配器的 `kmalloc` / `kfree`
- 中断与异常：IDT、ISR 桩、PIC 重映射、时钟中断
