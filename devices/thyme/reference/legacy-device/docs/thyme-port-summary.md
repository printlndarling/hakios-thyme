# HackiOS on Xiaomi 10S (thyme) — 移植成功总结

> 2026-10-25,单日完成。从 DBC1349/hackios-mi10ultra(cas/小米10U 真机验证版)移植到小米10S(thyme/SD870)。

## 最终战果

| 里程碑 | 状态 | 关键证据 |
|---|---|---|
| M1 UEFI + loader | ✅ | `xnuloader v0.5 / display 1080x2340 at 9c000000 / platform umi` |
| M2 XNU 内核 | ✅ | Darwin 22.5.0 (xnu-8796.121.2),7 核 PSCI,bsd_init 完整 |
| M3 iOS 16.5 桌面 | ✅ | `System shell "com.apple.springboard" finished startup after 12.30s`,137 进程 |
| M4 触摸 | ✅ | `UMI FTS: poll=24375 events=2211 errors=0 irq=1`(自编译内核,prebuilt 无驱动) |
| M5 GPU 硬件渲染 | ✅ | `THE GPU STAYS ON` + GMU 587MHz + `Mesa 25.0.7; FD650`(CoreAnimation 硬件渲染) |
| M6 WiFi | ✅ | PCIe errno=0 → amss20 SBL → QMI → radio 注册(en0)→ **iOS 扫描到网络**(用户实机确认) |
| M7 蓝牙 | 侦察完毕,缓行 | QCA6390 BT 半区,SlimBus 总线(btfmslim_slave, elemental-addr 00 01 20 02 17 02),RESET GPIO21/SW_CTRL GPIO124,5 路供电——上游无任何基础,需自研 SlimBus 控制器+BTFM+iOS 桥接(周级) |

## 产物清单(F:\DSH\thyme-assets\)

| 文件 | 说明 |
|---|---|
| thyme.dtb | 真机设备树(933KB) |
| partitions/xbl_a、xbl_config_a、abl_a | 引导链 ELF(UEFI 驱动提取源) |
| firmware/fw-gpu/ | a650 全套(GMU/SQE/zap) |
| firmware/fw-wifi/qca6390/ | 51 文件(amss20/m3/bd_*/regdb),来自手机 firmware 分区 sde5 |
| firmware/fw-haptic/ | aw8697 |
| firmware/adsp/ | ADSP 36 文件 |
| umi-boot.img | **thyme 的启动镜像**(thyme UEFI + loader,可反复 fastboot boot) |
| thyme-wifi-mac.bin | **最终 payload**(内核 8268600f + Mesa root.apfs + 全部固件 + wifi_mac) |
| thyme-gpu1/thyme-diag2/thyme-wifi-diag/full | 过程 payload(诊断模式各有用途) |
| wlan_mac.bin→wifi_mac=02:00:00:00:00:01 | WiFi MAC(来自 HyperOS 运行时 /sys/class/net/wlan0/address) |
| ios-screen/gpu-screen/wifi-screen/touch-screen.png | 各里程碑截屏 |

## 复现流程(WiFi 全量启动)

```
adb reboot bootloader
fastboot boot F:/DSH/thyme-assets/umi-boot.img        # 内存启动 thyme UEFI+loader
fastboot oem ramdisk                                   # 申请常驻窗口
fastboot stage F:/DSH/thyme-assets/system.apfs         # 2.4GB,~96s
fastboot stage F:/DSH/thyme-assets/thyme-wifi-mac.bin  # 35MB payload
fastboot oem run                                       # 点火;iOS 30 分钟自动返回
# 日志:fastboot oem log && fastboot get_staged <file>
# 截屏:fastboot getvar screen → oem screenshot → get_staged <raw>(BGRX 1080x2340 stride4320)
```

## thyme 相对 umi 的全部改动(共 5 处代码/配置)

1. **zap 内存地址**:`hk_adreno.c` ZAP_MEMORY_UMI `0x8681a000`→`0x8c41a000`(thyme DTB pil_gpu_region;不改则 TZ 认证挂死)
2. **DTB**:全部用 thyme.dtb(触摸 FTS@0x49 与 umi 同款,零驱动改动)
3. **UEFI 驱动**:从 thyme 自己的 xbl 提取(90 个,extract.py GUID 全匹配)+ RawFiles 56 个手动拷(extract.py 因 TzDxeLA 版本哈希不同早退)
4. **WiFi MAC**:cmdline `wifi_mac=02:00:00:00:00:01`(HyperOS 无 persist/wlan_mac.bin,运行时接口取)
5. **显示 TE 同步(hk_display.c,治撕裂)**:thyme 的面板是 autorefresh(0x2b4 bit31)但 TE config(0x288=0x180088)缺 umi 校准要求的 BIT(20)|BIT(23) → command 校准失败 → fb_scan 同步整体搁置 → GPU 撕裂。且 thyme 的 0x298/0x2b0 计数器**不按帧回绕**(自由行计数,~7μs/行),umi 的"回绕测帧周期"法失效。修复:校准改用**行进速率 × 2340 行构造帧基准**(60.334Hz,与 cas 的 60.316Hz 一致),idle 窗口取帧尾 scan.lines-100,成功打出 "staged presentation enabled"。诊断手段:临时 printf 采样 TE 计数器(测得 +9 行/50μs、无回绕)是破案关键。

## 今天的全部关键坑(按时间序)

| # | 坑 | 解法 |
|---|---|---|
| 1 | `wsl --install` 官方下载器卡死 | 手动下 MSI(350MB,代理)+ 静默安装 |
| 2 | MSI 截断(1620) | curl 走代理完整下载 |
| 3 | run.sh 找不到 edk2 固件 | AAVMF 环境变量封装 |
| 4 | Magisk 拒绝 shell | 用户在管理器里手动放行 |
| 5 | fastboot fetch 不支持 | Magisk root dd 提取 |
| 6 | 子模块 24KB/s | gh-proxy.com 镜像(gh-proxy 或 ghfast.top,失效互相备份) |
| 7 | WSL 镜像网络数小时后 TLS 截断 | `wsl --shutdown` 重启即愈 |
| 8 | DeviceBuild.py 路径 bug | 必须在 Mu-Silicium 目录内跑 |
| 9 | mu_nasm 要 Mono | 假 extdep(verify 只查 version)+装 mono 双保险 |
| 10 | extract.py TzDxeLA 哈希不符早退,RawFiles 没拷 | 手动 cp work/umi-firmware/extracted/RawFiles |
| 11 | mkxnuimg umi DTB 守卫 | patch 放行 thyme model |
| 12 | **cmdline 不能带 rd=**(loader 自己补 rd=md0) | 去掉即可 |
| 13 | fb_scan=1 无 GPU 时看门狗复位 | cas 原版 cmdline 成功;GPU 固件就位后 fb_scan 可用 |
| 14 | prebuilt 内核无触摸驱动 | 完整内核编译链(下条) |
| 15 | 内核编译链:cctools 裸名缺失/libdispatch/tcsh/MacOSX SDK 是目录 | 见方案文档"内核编译链"段 |
| 16 | cctools ld64 无 TAPI(-lSystem 失败) | cas 的 lld-clang-wrapper(**必须去 CRLF**) |
| 17 | **fw-gpu 放错层级**(build.sh 的 ../fw-gpu=仓库根) | 放 hackios-xnu-kernel-umi/fw-gpu/ |
| 18 | GPU step19 TZ 挂死 | 坑 1 的 zap 地址 + 冷启动清 TZ(长按电源 10-15s) |
| 19 | 诊断模式是利器 | GPU:adreno=22 adreno_pause=1000 adreno_fine=1 adreno_return=1;WiFi:umi_wifi_step=2 umi_wifi_return=1(均免系统镜像、失败自动返回) |
| 20 | WiFi 固件搜不到 | 在独立 firmware 分区(sde5);adb cat 会 CRLF 污染,用 exec-out |
| 21 | WiFi 扫描空 | radio 无 MAC→cmdline wifi_mac= 补上 |

## 蓝牙侦察记录(供未来开工)

- 硬件:QCA6390 BT 半区;**SlimBus 总线**(非 UART):DT 节点 `qca,qca6390`(RESET GPIO0x15、SW_CTRL GPIO0x7c、5 路供电)+ `qcom,btfmslim_slave`(elemental-addr `00 01 20 02 17 02`)
- 参考源码:Linux `drivers/slimbus/`(控制器)+ 高通供应商内核 `btfm_slim`/`btobex`;SlimBus 控制器在 Qualcomm 平台是 NGD(generic slimbus device)
- iOS 侧:BluetoothStack 需要的驱动接口未研究;HKAudio/SoundWire 那套(umi 音频已用)可能共享部分 slimbus 基础设施,值得先读 `hk_sound.c`/`umi_audio` 相关
- 建议路线:①SlimBus NGD 控制器探测(复用音频的 slimbus 基础,若有)→ ②BTFM slim slave 探测+版本读 → ③HCI over slimbus → ④iOS BluetoothStack 桥
