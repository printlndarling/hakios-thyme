# HackiOS → 小米10S(thyme)移植方案

> 生成:2026-10-25。基于 DBC1349/hackios-mi10ultra(cas 移植真机验证版)的全部交接文档整理。
> 三个仓库已克隆到 `F:\DSH`:hackios-mi10ultra、hackios-xnu-kernel-umi、hackios-xnu-userland-umi。

## 0. 一句话评估

**可行性高,是 cas 移植流程的高复用重演**。thyme(小米10S,SD870 = SM8250-AC / kona)与已验证的 cas(小米10U,SD865+)、驱动来源 umi(小米10,SD865)同 SoC 平台、同 GPU、同分辨率。真正的新增工作 = 从 thyme 真机提取素材(DTB / XBL 驱动 / 固件)+ 照 cas 的做法搭 thymePkg UEFI。唯一可能要写新驱动的点是触摸芯片(见 §2)。

## 1. 项目认知与安全边界

- 启动链:`fastboot boot <boot.img>`(纯内存启动,**不写任何分区**)→ Mu-Silicium UEFI → xnuloader → XNU(iOS 16.5 内核)→ iOS 16.5 用户空间(system.apfs 经 WinUsb 送入 RAM)。
- 重启即回 Android;研究用途;iOS 系统文件与厂商固件不进仓库、不再分发。

## 2. 硬件对比与移植要点

| 项 | umi(小米10,驱动来源) | cas(小米10U)✅真机已验证 | thyme(小米10S)目标 | 对 thyme 的意义 |
|---|---|---|---|---|
| SoC | SM8250 kona | SM8250 kona | SM8250-AC kona | 同平台,XNU 平台层/GIC/SMP 全复用 |
| GPU | Adreno 650 | Adreno 650(✅ Mesa FD650) | Adreno 650 | 驱动+固件名(a650_*)直接复用 |
| 屏幕 | 1080×2340 90Hz | 1080×2340 120Hz(实测 TE 60.3Hz 可用) | 1080×2340 90Hz | 同 umi,面板差异小 |
| 触摸 | ST FTS521 @I2C 0x49,GPIO36/37/38/39 | 同 umi(✅ 零错误) | **待 DTB 验证**(预期同 umi 系 FTS) | 唯一可能写驱动的点 |
| WiFi | QCA6390 PCIe | QCA6390(❌ cas cmd-db 缺 smpf2 轨) | QCA6390(umi 同源,cmd-db 预期有 smpf2) | **WiFi 可能开箱即通,比 cas 好** |
| 内存 | 8/12GB | 6GB(实测含 0xBBB00000 69MB DDR 空洞) | 8/12GB | 仅日志显示不同,不影响布局 |
| 音频/触觉 | — | 驱动固件就位未启用 | 哈曼卡顿四扬声器(与 umi 不同) | 反正未启用,不阻塞 |

**SoC 公共地址(cas 实测,thyme 预期相同,起不来再校)**:
FD_BASE `0x9FC00000` / FD_SIZE `0x300000`;PAYLOAD_BASE `0xF0000000`(64MB);FB_BASE `0x9C000000`(Display Reserved);GIC dist `0x17A00000` / redist `0x17A60000`;8 核,MPIDR shift 8,小核掩码 0xf。

## 3. 前提条件(用户侧,缺一不可)

1. **小米10S 已解锁 bootloader**(`fastboot getvar unlocked` 为 yes;解锁会清数据)。
2. **安装 WSL2**——本机(当前这台 Windows)**尚未安装 WSL**!管理员 PowerShell 运行 `wsl --install -d Ubuntu`,装完重启。建议 `C:\Users\<用户>\.wslconfig` 写 `[wsl2]` `vmIdleTimeout=-1`(否则空闲 60s 杀构建进程,cas 团队踩过的坑),内存给 ≥12GB。
3. USB **3.0** 口 + 可靠数据线(USB 2.0 口 loader 枚举失败)。
4. 建议 thyme 刷 OrangeFox/TWRP recovery(或已 root 的类原生系统),素材提取需要 adb root。
5. 磁盘:WSL 内约需 30GB+(IPSW 7.4GB、Mu-Silicium 含子模块 3.6GB、xnu 构建树、system.apfs 2.4GB);F: 剩余 150GB 足够。
6. 网络可达 Apple CDN(IPSW 下载)与 GitHub。

## 4. 分阶段施工计划

### 阶段 A:thyme 真机素材采集(需要手机,recovery/adb root)

| 素材 | 命令/方法 | 用途 |
|---|---|---|
| `thyme.dtb` | `adb pull /sys/firmware/fdt` | 设备档案 + UEFI DTB + **验证触摸芯片** |
| XBL 镜像 | recovery 下 `dd if=/dev/block/by-name/xbl_config(及 xbl)of=/sdcard/xbl.img` | UEFIReader 提取 90 个 DXE 驱动 |
| GPU 固件 | `/vendor/firmware/` 或官方线刷包:`a650_gmu.bin`、`a650_sqe.fw`、`a650_zap.mdt`+b00..b02 | GPU 硬件渲染 |
| WiFi 固件 | `/vendor/firmware/wlan/qca6390/` 全套 ~54 文件 | WiFi |
| ADSP 固件 | modem/dsp 分区或 `/vendor/firmware/adsp.*` | 音频(未启用,顺手留) |
| 触觉固件 | `aw8697_haptic.bin`(thyme 若型号不同以实测为准) | 触觉(未启用) |

> cas 当时 xbl_4/xbl_5 分区里只有 boot 子集驱动,完整 DXE 集是另找的;thyme 同样优先 `xbl_config`,不够就从官方线刷包(thyme 最新 MIUI/HyperOS 包)的 `xbl.img`/`abl.img` 提。工具:WOA-Project/UEFIReader(Windows GUI)+ Silicium 5 个标准 patch。

### 阶段 B:WSL 环境搭建 + QEMU 校准(强烈建议先做)

照 `mi10ultra/docs/HackiOS-cas-工程交接.md` §5:

```sh
# 工具链:theos + iPhoneOS16.5.sdk + cctools(或直接用 LLVM ld64.lld——cas 最终方案)
# 依赖:git curl tar xz make clang flex bison m4 perl python3 autoconf automake \
#       uuid-dev libcrypt-dev libblocksruntime-dev libyaml-dev meson ninja cmake python3-mako 7z lz4 dtc qemu-system-arm
cd hackios-xnu-kernel-umi && bash common/setup.sh
bash ios165/fetch.sh        # 下载 iPhone X iOS16.5 IPSW(6GB)
bash common/build.sh qemu-virt && bash common/run.sh qemu-virt
# 里程碑:QEMU 里见到黑底白苹果 Logo = 整条编译/打包/启动链验证通过
```

### 阶段 C:thymePkg UEFI(Mu-Silicium)

cas 最终验证的结构(照抄,不要从 redwoodPkg 起步):

1. `uefi/apply.sh` 克隆 Mu-Silicium(约 3.6GB,含子模块;断链就手动 `git apply uefi/mu-silicium.patch` + `cp -r uefi/files/. .`)。
2. **从 umiPkg 模板复制 thymePkg**(MemoryMapLib/ConfigurationMapLib/BootManagerLib 取 umi 结构,配置值用 thyme 自己 `uefiplat.cfg` 的)。注意 cas 的教训:**用 alioth 的 ConfigurationMapLib 配 kona 驱动必失败**。
3. `Binaries/thyme/`:UEFIReader 从 thyme XBL 提取的驱动替换;平台无关件(MdeModulePkg/ArmPkg/FatPkg/MiToken/XBLCore)复用。
4. **TzDxeLA.efi 必须打补丁:0xd469 处 0→1**(不打则 UEFI 启动 2 秒设备消失、6 秒回 fastboot)。thyme 的该驱动哈希若与 umi 补丁前一致,同款补丁直接可用;不一致则人工确认该字节含义。
5. 内存映射先用 kona/cas 值:FD 0x9FC00000/0x300000、PAYLOAD 0xF0000000、FB 0x9C000000。
6. `uefi/files/Resources/Configs/thyme.toml` + `Resources/DTBs/thyme.dtb`(真机 DTB)。`build_uefi.py -d thyme -r RELEASE` 是动态发现,无需注册。

### 阶段 D:loader + boot.img

```sh
common/new-device.sh thyme /path/to/thyme.dtb   # 生成 common/devices/thyme.conf + thyme.platform.c
# 预期填入(同 cas):GIC 0x17A00000/0x17A60000、8 核、shift 8、small 0xf、FB_BASE 0x9C000000
# loader 照 mi10ultra/loader/build.sh:aarch64 freestanding clang + lld-link /base:0xA1000000
# mkxnuimg.py 打包 cas-touch2.img 同款 boot.img(UEFI + loader + thyme.dtb)
```

loader 保持上报 **board=umi**(触摸/按键驱动的门控是 `/chosen/hackios-board == "umi"`)。

### 阶段 E:内核 + userland + payload

```sh
# 内核(先直接用 umi prebuilt 试;要改驱动时自编译):
cd hackios-xnu-kernel-umi && bash xnu/apply.sh && bash xnu-deps/setup.sh
THEOS=~/theos ./xnu-build.sh -j$(nproc) -k
# 产出 xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837

# userland(必须 DEVICE=umi,否则 WiFi 固件不进镜像):
cd userland && env DEVICE=umi GL=1 WIFID=1 REST=1 ADSP= RC_JOBS=rc.jobs.umi-boot bash build.sh
# 产出 root.apfs(小镜像)+ system.apfs(2.4GB);GL 需 Mesa(ld64.lld 链接)

# payload:
python3 xnuloader/mkpayload.py -o out/run-payload.bin \
  kernel=<内核> ramdisk=userland/root.apfs \
  --cmdline "loader_return_after=900 sb_delay=8 bootcpu=7 cpus=7 umi_touch=1 umi_keys=1 gpu=1 wifi=1 fb_scan=1 cpufreq=1"
```

### 阶段 F:真机 bring-up 里程碑(每步可验证)

| 里程碑 | 判据 |
|---|---|
| M1 loader 活着 | `fastboot boot thyme-boot.img` 后 loader 屏幕 + `./xl log` USB 日志;记下 "display W x H at ..." 回填 fb_base |
| M2 内核在跑 | `-v` 启动文本出现在日志 |
| M3 iOS 桌面 | SpringBoard 主屏(M1–M3 无 GPU 固件也可达) |
| M4 触摸 | `UMI FTS: poll=... errors=0`;**若 thyme 触摸不是 FTS521**:从 DTB 读 compatible/引脚,照 `umi_touch.c`(I2C/FTS)或 `hk_goodix.c`(SPI/Goodix)写 thyme 驱动,门控加 board 或新启动参数 |
| M5 GPU | 送入 fw-gpu + `gpu=1`:`adreno: THE GPU STAYS ON`、Mesa FD650、587MHz |
| M6 WiFi | `wifi=1` 直接试;thyme 的 cmd-db 大概率含 smpf2(umi 同源),若仍 0x13 枚举失败,照 cas 方案用 `patch_rpmh_mirror.py` 日志镜像内核读出实际轨名,改 `xnu/files/bsd/dev/umi_rpmh.c` 的 `rails[]` |

运行操作(Windows 侧,照 mi10ultra/scripts/windows/):`winfbd3.ps1` 常驻 daemon → `ramdisk` → `stage system.apfs` → `stage run-payload.bin` → `run` → `getvar result` 回 `OKAYrunning 0 0`。

## 5. 直接继承的踩坑清单(cas 全部踩过,全部可复现)

1. TzDxeLA 0xd469=1 补丁,缺了 UEFI 秒崩回 fastboot。
2. 包结构 = umiPkg 模板 + 本机驱动 + 本机 uefiplat.cfg 值;结构不匹配必失败。
3. **必须接 USB 3.0 口**。
4. WinUsb daemon 保持常驻、句柄不关;`err=203` = daemon 句柄失效,重启 daemon 即可,不是 USB 坏。
5. GPU 跑过一轮后二次 bring-up 卡 step 20 → 长按电源冷启动(自建 loader 缺 GPU 归还复位)。
6. WSL 空闲回收杀进程 → `.wslconfig` 设 `vmIdleTimeout=-1`;后台任务用前台长 timeout 或 wrapper 内 nohup。
7. 内核改完编译"不生效" → `xnu-src/` 不在 git 内:先 `git checkout -- .` → 重新 `git apply hackios.patch` → `cp -a xnu/files/. xnu-src/`。
8. Mesa/userland 链接用 **LLVM `ld64.lld`**(cctools 的 ld64 不开 TAPI_SUPPORT 读不了 .tbd)。
9. 日志只收 32KB:驱动里照 `hk_adreno.c` 的 `say()` 用 `loader_log_putc` 逐字符输出。
10. PowerShell 5.1 脚本纯 ASCII,禁中文;复杂命令写脚本文件再执行,不要内联。
11. 传给 WSL 的脚本已是 LF,不要再跑删 `\r` 的 sed(会把字母 r 删光)。
12. 7z 需要 ≥23 版本;QEMU 用 AAVMF_CODE.fd(64MB pflash),不能用 2MB 的 QEMU_EFI.fd。

## 6. 当前状态(2026-10-25)

| 项 | 状态 |
|---|---|
| 三个仓库克隆到 F:\DSH | ✅ |
| 移植方案(本文档) | ✅ |
| WSL2 安装 | ✅ 2026-10-25 完成(WSL 3.0.3 + Ubuntu 26.04.1,root 默认用户,虚拟盘在 F:\WSL\Ubuntu,已配 .wslconfig:vmIdleTimeout=-1 / 10GB 内存 / 镜像网络,apt 清华源) |
| **阶段 B:构建环境 + QEMU 校准** | ✅ 2026-10-25 完成。WSL /root/hackios/hackios-xnu-kernel-umi(kernel 6264ebf + userland ee787f6,与 cas 发布同提交);依赖全装(7z 26.00/clang 21/QEMU 10.2);IPSW 5.7GB 拉取完;root.apfs(4,194,304B)+ system.apfs(2,492,198,912B)与 cas 字节数一致;loader 现编译成功;**QEMU 内 iOS 16.5 启动到 SpringBoard**("Successfully spawned SpringBoard[8]"、SystemShell connected),截屏像素级验证=黑底白苹果 Logo(1125×2436)。注意:run.sh 需 `QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd`(启动脚本 /root/hackios/run-qemu.sh 已封装) |
| **阶段 A:真机素材采集** | ✅ 2026-10-25 完成(手机已连,bootloader 实际解锁=fastboot getvar unlocked yes,系统为第三方 HyperOS4 移植包 props 伪装 green;经用户在 Magisk 授权 shell 后 root 提取)。素材在 `F:\DSH\thyme-assets\`:**thyme.dtb**(933KB;**触摸 fts@49 = ST FTS,I2C 0x49,IRQ GPIO39/RESET GPIO38,与 umi 完全一致→umi_touch.c 零改动可用**)、**xbl_a/xbl_config_a/abl_a**(ELF,UEFIReader 提取驱动的原料)、GPU 固件(a650 全套 7 文件)、ADSP(36 文件)、触觉 aw8697、触摸生产限制 CSV、props/cmdline。缺:WiFi 固件(amss/bdwban 在本机非标准位置不存在,从官方 HyperOS 1 线刷包取,M6 里程碑才需要)。 |
| Mu-Silicium(阶段 C 前置) | ⏳ 克隆中(uefi/apply.sh;注意实际路径是 hackios-xnu-kernel-umi/Mu-Silicium/,子模块进行中) |
| **阶段 C:thymePkg UEFI** | ✅ 2026-10-25 完成。umi 流程(`uefi/umi/`,BASE 42fe5637):extract.py 从 thyme XBL 提取 147 个 FFS/90 个驱动(GUID 与 alioth 模板全匹配)+ RawFiles 56 个(SecParti.cfg/面板 XML 全在);TzDxeLA 手动打 0xd469 补丁(revision 与 umi 不同,字节语义一致);DEBUG 版 SILICIUM_UEFI.fd 恰好 0x300000。坑:DeviceBuild.py 必须在 Mu-Silicium 目录下跑(build.sh 有路径 bug);mu_nasm 需假 extdep(verify=version 匹配)+ 装 mono;子模块经 gh-proxy.com 镜像拉取;mkxnuimg.py 的 umi DTB 守卫已放行 thyme。 |
| **真机 bring-up** | ✅ **2026-10-25 达成 M1+M2+M3**:`fastboot boot out/umi-boot.img` → loader 活(xnuloader v0.5,display 1080×2340@0x9C000000,platform umi,与 umi 作者首亮输出逐字一致)→ XNU 跑通(bsd_init done → launchd,90 秒干净返回)→ **iOS 16.5 到 SpringBoard**("System shell com.apple.springboard finished startup after 12.30s",137 进程,1.3MB 日志,截屏 2.3MB PNG 在 thyme-assets/ios-screen.png)。首次失败教训:cmdline 加 fb_scan=1 会导致看门狗复位回安卓(手机无损);cas 原版配置(无 fb_scan、无 gpu)成功。cmdline 注意:手机流程 ramdisk=md0 由 loader 自动补,**用户 cmdline 里不能带 rd=**(白名单拒绝)。操作序列:adb reboot bootloader → fastboot boot umi-boot.img → `fastboot oem ramdisk` → `stage system.apfs`(2.4GB≈96s)→ `stage payload` → `oem run`;日志=`oem log`+`get_staged`;截屏=`getvar screen`→`oem screenshot`→`get_staged raw`→xl 的 BGRX→PNG 转换器。payload=mkpayload.py(kernel+root.apfs+cmdline)。 |
| 剩余里程碑 | ✅ M4 触摸(2026-10-15 前述)✅ **M5 GPU 硬件渲染 2026-10-25 15:47 达成**:`adreno: THE GPU STAYS ON` + GMU 587MHz + `hk-gl: OpenGL ES 3.2 Mesa 25.0.7; FD650`(CoreAnimation 走 freedreno 硬件渲染)。**关键修复:thyme 的 zap 内存区域在 0x8c41a000(umi 是 0x8681a000),hk_adreno.c 的 ZAP_MEMORY_UMI 必须改成 thyme 地址**,否则 TrustZone 在 step 19 PAS 认证时挂死(需要冷启动恢复)。诊断模式(`adreno=22 adreno_pause=1000 adreno_fine=1 adreno_return=1`,无需系统镜像)是验证利器;挂死内核后 loader 可能存活但需冷启动才能再跑。userland GL 构建坑:cctools ld64 读不了 .tbd → 用 cas 的 lld-clang-wrapper(F:\DSH\hackios-mi10ultra\mi10ultra\patches\lld-clang-wrapper,注意去 CRLF);**固件目录必须在仓库根的 fw-gpu/(不是上级目录)**,构建日志确认 "fw/: N files"。⏳ ~~M6 WiFi~~ → **M6 WiFi 2026-10-25 16:20 主体达成**:①ath11k 驱动链编好(mk-ath11k.sh:Linux 7.0.14 + FreeBSD 15.1 LinuxKPI → ath11k.o 链入内核,内核 15,733,176B sha8268600f;mk-wlfw.sh 36 表);②**WiFi 固件在手机的 firmware 分区(sde5→/vendor/firmware_mnt/image/qca6390/,51 文件 7.8MB,之前搜不到是因为独立 vfat 分区挂载)**,注意 adb cat 会 CRLF 污染,必须 `adb exec-out su -c 'cat ...'`;③PCIe 诊断 errno=0(**thyme cmd-db 有 smpf2**,cas 的 0x13 问题不存在);④全量:amss20.bin SBL 下载+校准+重启 ✓、QMI 消息流 ✓、**radio 注册(2.4/5/6GHz 全频段)**、wifid 运行 ✓。遗留:radio 无 MAC 地址(persist 分区的 wlan_mac.bin 未提取,`wifi: 0: the driver asks for the radio's address: none was given`)+ ath11k 探测 firmware-2.bin 缺失(疑似良性)+ 加入网络/上网待验证(上游 umi 自己也标注 "being verified"——已达到上游同等水平)。下一步可选:提取 wlan_mac.bin、试 iOS 设置里扫描/加入、如需 firmware-2.bin 从 ath11k 固件仓补。 |

**内核编译链(2026-10-25 已跑通,坑全记录)**:theos(sparse)+ iPhoneOS16.5.sdk(theos/sdks 里是**目录**不是 tar 包,sparse-checkout set iPhoneOS16.5.sdk)+ cctools-port(target=arm64-apple-darwin14,需先源码编 libdispatch——swift-5.10 分支,CC=clang,去掉 -Werror)+ **cctools 装完必须给所有工具补短名链接**(setup.sh 和 make 期望 ld/libtool/strip 等裸名,只有 arm64-apple-darwin14-* 前缀名会踩 libtool -static 被解析到 GNU libtool、strip 剥 Mach-O 失败等坑)+ libncurses-dev(tcsh)+ libtool-bin+ 全局 git 走 gh-proxy.com 镜像。产物:15,253,400 字节内核(sha 78488800…)。WSL 镜像网络跑几小时后会劣化(TLS 截断),`wsl --shutdown` 重启即愈。

> **WSL 使用提示**:终端输 `wsl` 即以 root 进入 Ubuntu。构建务必在 WSL 自身文件系统内进行(如 `/root/hackios`),**不要**在 `/mnt/f/` 下编译(9p 挂载极慢且符号链接/权限有问题);F:\DSH 的仓库可 `cp -r` 进去或重新 clone。WSL 内 GitHub 可直连,也可用代理 `127.0.0.1:7890`(镜像网络模式已启用)。`F:\DSH\_wsl-setup\wsl.msi`(350MB)是安装包留档,可删。

## 7. 参考路径

- cas 交接文档(最详细,施工时通读):`F:\DSH\hackios-mi10ultra\mi10ultra\docs\HackiOS-cas-工程交接.md`
- cas 成功报告 + 踩坑:`同目录下 SUCCESS-iOS-on-cas-2026-10-24.md`、`SUCCESS-GPU-on-cas-2026-10-24.md`
- 通用移植指南:`F:\DSH\hackios-mi10ultra\docs\PORTING.md`
- umi 驱动源码:`F:\DSH\hackios-xnu-kernel-umi\xnu\files\bsd\dev\`(umi_touch.c / umi_rpmh.c / hk_adreno.c 等)
