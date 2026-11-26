# HakiOS — 小米 10S（`thyme`）移植版

[English](README.md) · 中文 · [Українська](README.uk.md)

**本项目为 [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) 的小米 10S（`thyme`，骁龙 870 / SM8250-AC）长期维护分支。**
系统默认以 RAM 方式引导 Apple 开源 Darwin/XNU 内核（`xnu-8796.121.2`），不刷写、不修改原有 Android 分区与用户数据；保留经真机验证的 `thyme` 专属驱动与修复，并选择性合并上游通用更新。

> 下表所有 `PASS` 均经小米 10S 真机实测验证。完整测试日志、构建校验和及排查记录请见 [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md)。

## 目标硬件

| 模块 | 规格 |
|---|---|
| SoC / GPU | 高通骁龙 870（`SM8250-AC`）· Adreno 650 |
| 基带 / GNSS | 外挂高通骁龙 X55（`SDX55M`，PCIe2 / MHI） |
| 屏幕 / 触控 | 6.67 英寸 1080 × 2340 90 Hz AMOLED · FocalTech FTS521 |
| 无线连接 | 高通 QCA6390（Wi-Fi 6 + 蓝牙 5.1） |
| 音频 / 马达 | 双 Cirrus Logic CS35L41 立体声功放 · AW8697 线性马达 |
| 相机 | 后置 1.08 亿像素三星 S5KHMX + AK7377 对焦马达 · 前置 2000 万 S5K3T2 |

## 硬件与功能状态

| 功能模块 | 状态 | 实测摘要 |
|---|---|---|
| 系统启动与桌面 | **PASS** | RAM 冷启动至 `launchd`、`backboardd`、SpringBoard 与设置 |
| CPU / 调频 | **PASS** | 7 核在线，EPSS 三丛集频率达 1.80 / 2.42 / 3.19 GHz |
| 显示与刷新率 | **PASS** | 1080 × 2340 OLED，真硬件 90 Hz（`INTF_1` TE 同步零漏扫）、DCS 亮度调节与锁屏灭背光 |
| GPU（OpenGL） | **PASS** | Adreno 650 经 Mesa/Freedreno 硬件渲染（`hakios-gl`） |
| Metal | **PARTIAL** | `hakios-metal` 可编译；系统 `MTLDevice` / VectorKit / ANGLE 应用待打通 |
| 触控与按键 | **PASS** | FTS521 多点触控、手势、电源键与音量键正常 |
| 音频与触感 | **PASS** | 双 CS35L41 独立左右声道扬声器、双麦克风、AW8697 CoreHaptics 正常 |
| 相机与闪光灯 | **PASS** | 后摄 27 MP S5KHMX + AK7377 自动对焦、前摄 S5K3T2、镜头切换与 PM8150L 闪光灯 |
| 蜂窝与通话 | **PASS** | SDX55M 4G LTE 双栈数据与双向 VoLTE 语音（当前 `Mav17` 仅 4G，5G 需 `Mav20`） |
| 定位与 GNSS | **PARTIAL** | 设置开关、Wi-Fi 定位（百度地图实测精准蓝点）与 SDX55M QMI LOC 卫星流正常；室外 3D 定位待测 |
| Wi-Fi（QCA6390） | **PASS** | 2.4/5 GHz WPA2、DHCP/TLS、国家码（`CN`）、65 s 锁屏休眠唤醒、AP 踢下线与断电自动重连 |
| 蓝牙（QCA6390） | **PARTIAL** | 3 Mbaud H4、扫描配对、A2DP 播放、BlueTool 休眠与 PlayAndRecord HFP/A2DP 会话正常；耳机 HFP/AVRCP 实测待补 |
| 电池与温控 | **PASS** | 电量/电流读取、USB 充电、温控限频与低电量保护正常 |
| 传感器（SLPI） | **PASS** | LSM6DSO 加速度/陀螺仪、AK0991x 磁力计、TCS3701 屏下光感已接入 backboardd HID，BMP285 气压计在线（距离为 ADSP 超声波） |
| 中文界面 | **PASS** | 开机默认简体中文界面、苹方字体与简体拼音键盘 |
| Sileo 与巨魔商店 | **PASS** | Sileo（11 个国内/无根源）、TrollStore（预装 `ldid`）、TrollApps（6 个国内源）开箱即用 |
| 持久化存储 | **TODO** | 默认纯内存启动；受保护的闪存持久化与独立安装流程尚未启用 |

## 后续重点

- **定位与蜂窝完善**：室外开阔天空下验证 `umi_gnssd` 3D 卫星定位注入，按需补充短信测试。
- **蓝牙体验与 USB 设备连接**：实测蓝牙耳机 HFP/SCO 与 AVRCP；推进 SM8250 `DWC3` USB Gadget / `usbmux` / `lockdownd`（用于电脑端爱思助手 / iTunes 连接）。
- **图形栈与持久化**：推进系统级 `MTLDevice` 支持以及安全可控的持久化存储方案。

## 构建与运行（RAM 启动）

开发环境位于 **WSL2 Ubuntu** 原生 ext4 文件系统（Windows 仅用于 USB `adb`/`fastboot`）。常规测试全程在内存中运行，不刷写分区，测试结束后自动重启回 Android。

```sh
make thyme_defconfig
make kernel JOBS=16
SCENE=wirelessfull devices/thyme/build-root.sh
devices/thyme/ram-regress.sh
```

- **固件与 IPSW 准备**：厂商固件与 Darwin 用户空间均为本地文件且已忽略，准备步骤见 [`devices/thyme/README.md`](devices/thyme/README.md)。
- **蜂窝 / GNSS 启动**：请使用 `XL_BOOT_IMAGE=devices/thyme/prebuilt/xnu-usb.img`（支持 1023 字符启动参数与 `/baseband` 设备树节点）。
- **详细记录与基线**：完整真机证据见 [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md) 与 [`devices/thyme/ram-baseline.conf`](devices/thyme/ram-baseline.conf)。

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

- **不包含 Apple 或厂商闭源固件**：用户空间由用户自行提供的 IPSW 解包，固件从用户自有设备提取。不支持且不以 Apple 服务（激活、Apple ID、iCloud、App Store / FairPlay DRM）为目标。
- **开源协议**：XNU 补丁及其派生代码遵循 [APSL 2.0](LICENSE.APSL)，其余原创代码遵循 [MIT](LICENSE)。详见 [NOTICE](NOTICE)、[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、[COMPLIANCE.md](COMPLIANCE.md) 与 [LEGAL.md](LEGAL.md)。
- **免责声明**：本项目为独立研究项目，**与 Apple 无任何关联或背书关系**。
- **致谢**：感谢 HakiOS 上游团队以及高通 SLPI/QMI 开源逆向社区。
