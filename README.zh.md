# HakiOS — 小米 10S（`thyme`）移植版

[English](README.md) · 中文 · [Українська](README.uk.md)

**本项目为 [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) 的小米 10S（`thyme`，骁龙 870 / SM8250-AC）长期维护分支。**
系统默认以 RAM 方式引导 Apple 开源 Darwin/XNU 内核（`xnu-8796.121.2`），不刷写、不修改原有 Android 分区与用户数据；保留经真机验证的 `thyme` 专属驱动与修复，并选择性合并上游通用更新。

> 下表所有 `PASS` 均经小米 10S 真机实测验证。完整逐项清单请见 [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md)。

## 硬件与功能状态

| 功能模块 | 状态 | 实测摘要 |
|---|---|---|
| 系统启动与桌面 | **PASS** | `SCENE=daily` 独立脱线启动（`boot-hakios-ram.bat`）至 `launchd`、`backboardd` 与 SpringBoard |
| CPU 与动态调频 | **PASS** | 7 核在线；200 ms 运行时 EPSS 调频（最高 1.80 / 2.42 / 3.19 GHz）与温控限频 |
| 显示与刷新率 | **PASS** | 1080 × 2340 OLED，真硬件 90 Hz（`INTF_1` TE 同步零漏扫）、DCS 亮度与锁屏灭背光 |
| GPU（OpenGL）与调频 | **PASS** | Adreno 650 经 Mesa/Freedreno 硬件渲染 + GMU HFI 运行时调频（305–587 MHz） |
| Metal | **PARTIAL** | `hakios-metal` 可编译（已修 `metal_apps=1` `backboardd` 豁免）；系统 `MTLDevice` 稳定性待测 |
| 触控与按键 | **PASS** | FTS521 多点触控、手势、电源键与音量键正常 |
| 音频与触感 | **PASS** | 双 CS35L41 立体声+听筒（15 s 防休眠恢复）、语音备忘录零断流双麦、AW8697 CoreHaptics |
| 相机与闪光灯 | **PASS** | 后摄 1080p ~51 fps S5KHMX + AK7377 对焦、前摄 S5K3T2、PM8150L 闪光灯（OIS `0x1c` 应答已验证） |
| 蜂窝与通话 | **PASS** | SDX55M（`pcie2_mhi=2` 原生 `AppleBaseband`/`CommCenter`）、4G LTE 双栈与 VoLTE（`Mav17`） |
| 定位与 GNSS | **PARTIAL** | 设置开关、Wi-Fi 定位（百度地图实测蓝点）与 SDX55M QMI LOC 卫星流正常；室外 3D 定位待测 |
| Wi-Fi（QCA6390） | **PASS** | 2.4/5 GHz WPA2、DHCP/TLS、国家码（`CN`）、65 s 锁屏唤醒、AP 踢下线与断电自动重连 |
| 蓝牙（QCA6390） | **PASS** | 3 Mbaud H4、已实测正常连接蓝牙耳机（A2DP 音频播放）与其它手机配对、BlueTool 休眠 |
| 电池与温控 | **PASS** | 电量读取、`bq25970` 与 `fsa4480` 10 s 遥测、USB 充电、温控限频与低电量保护正常 |
| 传感器（SLPI） | **PASS** | LSM6DSO、50 Hz `mag_cal` 指南针、前 `TCS3701` + 后 `TSL2540` 光感、气压计、计步与朝向 |
| CDSP / 红外 / RTC / WDT | **PASS** | `CDSP` TZ 鉴权启动与关闭、`/dev/hakios-ir`（38 kHz NEC）、SPMI RTC 校时、TRNG、APSS 硬件看门狗 |
| NFC 与视频 VPU | **PARTIAL** | `/dev/hakios-nfc` NCI 2.0 握手就绪；UFS `modem_a` 加载 `venus.mdt` 与 TZ `MEM_SETUP` 已过，VPU 解码待通 |
| 中文界面与日常应用 | **PASS** | 开机默认简体中文界面、苹方字体、拼音输入法与预装百度地图 |
| Sileo 与巨魔商店 | **PASS** | Sileo（11 个国内源）、TrollStore（预装 `ldid`）、TrollApps（6 个国内源）开箱即用 |
| 持久化存储 | **TODO** | 默认纯内存启动（`modem_a` 只读 VFAT 就绪）；受保护的闪存读写持久化尚未启用 |

## 快速开始（Quick Start）

全程**仅在内存中引导运行**，不刷写、不改动手机任何分区；重启后直接回到原 Android 系统。

### 准备条件

- **手机**：已解锁 Bootloader 的小米 10S（`thyme`），开启 USB 调试，电量 **≥ 40%**。
- **主机环境**：Linux 或 **WSL2 Ubuntu** 原生 ext4 文件系统（Windows 仅负责 `adb`/`fastboot` USB 连接）。
- **本地资源（仓库不包含闭源固件）**：
  - 自行准备的 `iPhone10,3` iOS 16.5 IPSW（用于解包 Darwin 用户空间）。
  - 从本机提取的厂商固件（`fw-*`），提取方式见 [`devices/thyme/README.md`](devices/thyme/README.md)。
  - 可选：在 `fw-wifi/network` 写入两行 Wi-Fi 名称与密码，开机即可自动连网。

### 1. 拉取代码并初始化配置

```sh
git clone --recursive https://github.com/printlndarling/hakios-thyme.git
cd hakios-thyme
make doctor
make thyme_defconfig
```

### 2. 解包用户空间并构建最新稳定镜像

```sh
# 1) 解包 iPhone10,3 iOS 16.5 IPSW 到 sysimage/（仅需一次）
make unpack IPSW=/path/to/iPhone10,3_16.5_Restore.ipsw

# 2) 构建 XNU 内核（hakios-kernel）
make kernel JOBS=$(nproc)

# 3) 构建完整系统镜像（userland/system.apfs）与日常内存根镜像（userland/root.apfs）
SYSTEM=1 SCENE=daily devices/thyme/build-root.sh
# （后续仅更新内存根镜像时只需运行：SCENE=daily devices/thyme/build-root.sh）
```

### 3. 一键 RAM 启动到小米 10S

```sh
export DEVICE=thyme
export XL_BOOT_IMAGE=devices/thyme/prebuilt/xnu-usb.img
export XL_RAMDISK=userland/root.apfs
export XL_CMDLINE="-v debug=0x8 rd=md0 $(tr -d '\n' < devices/thyme/ram.args) pcie2_test=1 pcie2_bhi=2 ufs=2 pcie2_msi=1 pcie2_sid=5 pcie2_mhi=2 pcie2_nr=0"

# 进入内存 UEFI 引导器（仅 fastboot boot，不刷机）
./xl enter

# 上传常驻系统镜像 userland/system.apfs 到内存（约 2 分钟，后续重启内核无需重复上传）
./xl ramdisk userland/system.apfs

# 方式 A — 日常独立启动（启动确认后自动脱离电脑，可拔掉数据线）
XL_SECS=0 XL_DETACH=1 ./xl run

# 方式 B — 限时测试启动（保存 logs/<时间>-run.log 并自动重启回 Android 充电）
# XL_SECS=120 XL_REBOOT_AFTER=1 ./xl run
```

> **提示**：完整硬件清单见 [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md)，回归基线见 [`devices/thyme/ram-baseline.conf`](devices/thyme/ram-baseline.conf)。随时可执行 `./xl reboot` 或长按电源键重启回原 Android。

## 后续重点

- **定位与蜂窝完善**：室外开阔天空下验证 `umi_gnssd` 3D 卫星定位注入，按需补充短信测试。
- **视频硬解与 USB 设备连接**：打通 `pil-venus` VPU 硬件解码及 SM8250 `DWC3` USB Gadget / `usbmux` / `lockdownd`（用于电脑端爱思助手 / iTunes 连接）。
- **图形栈与持久化**：推进系统级 `MTLDevice` 支持以及安全可控的持久化存储方案。

## 上游与相关仓库

本分支跟踪 [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) 的通用内核、引导器、用户空间与工具链更新，小米 10S 专属适配集中维护于 `devices/thyme/`。

| 仓库 | 说明 |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | 上游公共内核移植、驱动、引导器、用户空间与工具 |
| [`hakios-metal/`](hakios-metal) | 基于内核 GPU 驱动的 Adreno Metal 命令层 |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G（SC7280）设备适配 |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | HakiOS 所用 Mu-Silicium UEFI 与 U-Boot 引导固件 |
| [hackios-web](https://github.com/oosexgang/hackios-web) | 项目官网 |

## 法律声明与致谢

- **不包含 Apple 或厂商闭源固件**：用户空间由用户自行提供的 IPSW 解包，固件从用户自有设备提取。上游公开实验与 Apple ID、App Store、FairPlay、WeatherKit 的真机验收状态见 [兼容性记录](devices/thyme/UPSTREAM.md#apple-services-compatibility-review)。
- **开源协议**：XNU 补丁及其派生代码遵循 [APSL 2.0](LICENSE.APSL)，其余原创代码遵循 [MIT](LICENSE)。详见 [NOTICE](NOTICE)、[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、[COMPLIANCE.md](COMPLIANCE.md) 与 [LEGAL.md](LEGAL.md)。
- **免责声明**：本项目为独立研究项目，**与 Apple 无任何关联或背书关系**。
- **致谢**：感谢 HakiOS 上游团队以及高通 SLPI/QMI 开源逆向社区。
