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
| 相机与闪光灯 | **PARTIAL** | 前后摄均出真图。两轮对照 RAM 测试的后摄平均帧交付间隔由 86–90 ms 降至 62–69 ms。后摄跟手性、前摄清晰度、彩块和取景闪屏仍在验收。 |
| 蜂窝与通话 | **PASS** | SDX55M（`pcie2_mhi=2` 原生 `AppleBaseband`/`CommCenter`）、4G LTE 双栈与 VoLTE（`Mav17`） |
| GPS 与日常定位 | **PASS** | 用户在真机确认定位和地图图层刷新。开阔天空下的纯卫星 3D 定位单独验收。 |
| Wi-Fi（QCA6390） | **PASS** | 2.4/5 GHz WPA2、DHCP/TLS、国家码（`CN`）、65 s 锁屏唤醒、AP 踢下线与断电自动重连 |
| 蓝牙（QCA6390） | **PASS** | 3 Mbaud H4、已实测正常连接蓝牙耳机（A2DP 音频播放）与其它手机配对、BlueTool 休眠 |
| 真机调试与 USB | **PARTIAL** | WSL2 和 Windows 原生 OpenSSH 已验证。WinUSB 开发接口可经 WSL2 读取真实型号与 SoC。`hdb --usb` 可读取当次 Wi-Fi 地址并校验 SSH 主机密钥，软件重连后也已验证。爱思助手原生识别、配对和其他管理服务待验收。见[真机调试说明](devices/thyme/docs/debug.md)。 |
| 电池与温控 | **PASS** | 电量读取、`bq25970` 与 `fsa4480` 10 s 遥测、USB 充电、温控限频与低电量保护正常 |
| 传感器（SLPI） | **PASS** | LSM6DSO、50 Hz `mag_cal` 指南针、前 `TCS3701` + 后 `TSL2540` 光感、气压计、计步与朝向 |
| CDSP / 红外 / RTC / WDT | **PASS** | `CDSP` TZ 鉴权启动、SMP2P 就绪、GLINK 协商、FastRPC 能力查询、guest 会话挂接与释放、原厂独立计算进程创建、进程内 ASID 查询、反向监听器注册与进程释放、`/dev/hakios-ir`（38 kHz NEC）、SPMI RTC 校时、TRNG、APSS 硬件看门狗 |
| NFC 与视频 VPU | **PARTIAL** | `/dev/hakios-nfc` NCI 2.0 握手就绪；Venus 固件准备与 MVS0C 核心时钟和 AXI0 门控可逆启停已验证，VPU 硬解待通 |
| 中文界面与日常应用 | **PASS** | 开机默认简体中文界面、苹方字体及拼音输入法。百度地图需单独安装测试。 |
| Sileo 与巨魔商店 | **PASS** | Sileo（11 个国内源）、TrollStore（预装 `ldid`）、TrollApps（6 个国内源）开箱即用 |
| 持久化存储 | **TODO** | 默认纯内存启动。`modem_a` 只读 VFAT 就绪，按 GPT 生成的 HFSX 数据卷通过 QEMU 重启保留测试，[真机安装与持久化](docs/install-safe.md)待验收 |

## 快速启动

Windows、Linux、macOS 都可以直接通过 USB 启动 HakiOS。**HakiOS 从 RAM 运行，重启手机即回到 Android。**

先准备：

- 一台已解锁 Bootloader 的小米 10S（`thyme`），手机开在 Android，打开 USB 调试并接受本机授权。使用数据线，电量至少 40%，电池温度低于 45 °C。
- Python 3.8 或更新版本。在 Windows 上从 [Python 官网](https://www.python.org/downloads/windows/) 安装。Linux 或 macOS 上可用包管理器安装 `python3`。
- [下载项目 ZIP](https://github.com/printlndarling/hakios-thyme/archive/refs/heads/main.zip) 并解压，或克隆仓库。把同一次构建的 `xnu-usb.img`、`hakios-kernel`、`root.apfs`、`system.apfs` 放在仓库内的 `thyme-bundle` 目录中。首次运行时，脚本会将缺失的 [Google Platform-Tools](https://developer.android.com/tools/releases/platform-tools) 下载到本地缓存。

### Windows

双击 `devices\thyme\ram-boot.bat`，或在解压后的目录打开 PowerShell 执行：

```powershell
.\devices\thyme\ram-boot.bat
```

若 Android 可见但进入 Fastboot 后失联，请检查 Windows USB 驱动和数据线。

### Linux

```sh
sh devices/thyme/ram-boot.sh
```

出现 USB 权限错误时，先为 Android/Fastboot 配置本发行版的 udev 规则。

### macOS

```sh
sh devices/thyme/ram-boot.sh
```

脚本使用原生 macOS Platform-Tools。传输镜像时请保持 USB 连接。

### 首次自动下载镜像

如果你有获授权的镜像下载地址，在启动命令后加上 `--bundle-url https://your-host/thyme-ram-boot.json`。脚本自动下载并校验每个镜像的 SHA-256，此后复用校验通过的缓存。清单格式见 [跨平台启动说明](devices/thyme/docs/ram-boot.md)。

目前项目没有发布公共的完整镜像包。请用自备 IPSW 和从自有设备取得的固件生成镜像，或从你获授权的私有 HTTPS 清单下载。

### 在 Linux 上准备镜像

先安装构建依赖并按 [设备文档](devices/thyme/README.md) 准备本地固件和 UEFI 输入。Linux 实机、Linux 虚拟机和 WSL2 都可以构建。构建完成后可在原目录启动，或将四个镜像复制到其他启动主机的 `thyme-bundle`。

```sh
make doctor
make thyme_defconfig
make unpack IPSW=/path/to/iPhone10,3_16.5_Restore.ipsw
make kernel JOBS=8
SYSTEM=1 SCENE=daily devices/thyme/build-root.sh
devices/thyme/build-loader.sh
```

可加 `--check` 在连接手机前检查输入和载荷大小。构建输出位置、清单及异常恢复步骤见 [跨平台启动说明](devices/thyme/docs/ram-boot.md)。

## 后续重点

- **定位与蜂窝完善**：室外开阔天空下验证 `umi_gnssd` 3D 卫星定位注入，按需补充短信测试。
- **视频硬解与 USB 设备连接**：推进 `pil-venus` VPU 硬解、配对与信任、标准设备管理服务，以及爱思助手和 iTunes 的原生识别。
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
