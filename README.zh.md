# HakiOS on Xiaomi Mi 10S (thyme)

[English](README.md) · 中文 · [Українська](README.uk.md)

**这是 HakiOS 的 THYME 设备分支。** 它把 Apple 开源的 Darwin 内核（XNU
xnu-8796.121.2）运行在小米 10S（SM8250 / 骁龙 870）上，默认从内存启动，
不修改 Android 系统和用户数据。本分支保留在真机上验证过的 THYME 驱动和
设备修复，并有选择地合并上游的公共实现。

带完整证据的权威状态见
[`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md)，本页只是摘要。
只有在该构建下、在真实的小米 10S 上观察到的结果才记为 `PASS`，仅编译通过
不算。

## 状态一览

| 项目 | 状态 | 已验证内容 |
|---|---|---|
| 启动到 SpringBoard | **PASS** | launchd、backboardd、SpringBoard 以及设置/界面栈在冷启动 RAM 运行中到达主屏幕 |
| 90 Hz 显示与界面帧率 | **PASS** | 1080 x 2340 面板时序被接受；CoreAnimation 跟随面板以 90 fps 绘制（连续四个 5 秒窗口 452、451、451、451 帧，中位帧间隔 11.1 ms） |
| CPU / 调频 | **PASS** | 7 个 CPU 启动；三个 EPSS 域分别达到 1804 / 2419 / 3187 MHz |
| GPU（Mesa/FD650 OpenGL） | **PASS** | 硬件 `hakios-gl` 帧，使用中不回落 `softpipe`；标准无线回归中 735 帧 |
| Metal | **PARTIAL / 阻塞** | `hakios-metal` 可构建、探测通过，但第一个 render pass 仍会卡死，因此 Metal/CoreAnimation 保持关闭。上游的 Metal-on-Vulkan/Turnip 路径已合并，但仍是可选且未在 THYME 上验证 |
| 触摸、按键、手势 | **PASS** | FTS521 触摸、按键、点击/拖动/手势；`errors=0` |
| 亮度 | **PARTIAL** | DSI DCS 亮度命令被确认；自动亮度依赖环境光传感器 |
| Wi-Fi（QCA6390） | **PASS** | 扫描、2.4/5 GHz WPA2 关联、四次握手、DHCP、DNS/TLS/Safari；关闭/打开恢复 |
| 蓝牙（QCA6390，3 Mbaud） | **PASS**（发现、配对、连接、A2DP）；HFP/SCO、AVRCP、麦克风和自动重连仍未验证 | H4 传输、85 次 LE report、无传输故障；QCA responder 先于 `bluetoothd` 启动 |
| 音频 | 扬声器和麦克风 **PASS**；振动 **PARTIAL** | 扬声器/麦克风通路有 1 kHz 声学/录音验证；振动线圈和 BEMF 已验证，iOS 触感反馈尚未 |
| 电池、充电、温控 | **PASS** | 电量/电流/充电状态；30 分钟充电保持；温控降频和有序返回 |
| 相机 | **PARTIAL** | S5KHMX 上电/模式、CSID/IFE 探测通过；手电筒端到端可用；合成帧可到 AVFoundation；后置相机实际拍照仍失败 |
| 传感器（加速度、环境光、距离） | **进行中** | SLPI 传感器核心已能加载签名固件、退出复位并暴露 QRTR 服务；SNS 客户端和 HID 传感器服务正在接入。iOS 目前还拿不到传感器数据 |
| 蜂窝（基带、SIM、通话/数据） | **FAIL** | MPSS/telephony 栈尚未实现 |
| 持久化存储 | **TODO** | root 和用户数据仍在内存中；受保护的持久化设计尚未接受 |
| 无根包管理器（Sileo） | **PARTIAL** | 离线 dpkg 安装/卸载和 Sileo 渲染通过；在线仓库安装未验证 |
| 中文界面 | **PASS** | 简体/繁体资源和 PingFang 正常；设置语言切换后整页中文 |

## 当前推进

1. **传感器** —— SLPI 固件（`slpi.mdt`、`slpi.b00..b20`）已从手机 Android
   中提取，经 TrustZone 认证并启动；GLINK `IPCRTR` 通道和 QRTR 服务列表
   已建立。下一步是 SNS 客户端协议（先加速度计）、通过 FastRPC 提供传感器
   registry，以及给 `backboardd` 的 IOHID 传感器服务。
2. **后置相机实际拍照** —— S5KHMX 传感器和 CSID/IFE 通路已探测；缺的是完整
   的 CSI/ISP 采集并送入 `/dev/hakios-camera` 用户空间路径。
3. **蜂窝** —— 剩余最大的模块：MPSS、QMI 和 telephony 栈。
4. **持久化存储与独立安装器** —— 采用受保护的写入路径和从 flash RAM 启动的
   设计；在明确的授权与恢复门禁完成前不碰 Android 分区。

## 已验证的 RAM 基线（2026-11-05）

| 产物 | SHA-256 |
|---|---|
| `hakios-kernel` | `97fcecd0be88ffae68ae52faeee51969cb289d9da58357e4c019992be4214610` |
| `out/thyme-boot/root-wirelessfull.apfs` | `b7e0f382b240add0356c954259a28a7383ca653f58e5b471f6f331f3cfba2cf4` |
| `devices/thyme/prebuilt/system.apfs` | `4f0f3f8b1ba017ed96016883127761ea65c7d070be791fb49789c2fdace5ab48` |
| `devices/thyme/prebuilt/xnu-usb.img` | `2d662c991a5abacc9f229d3b9d556665959fbda16ec2887f357b7dea56400e10` |

标准冷启动门禁是 `logs/114117-run.log`：两次 Wi-Fi 关联与 DHCP
`192.168.0.102`、Safari TLS、85 次 LE report（15 个有名称）、735 个
`hakios-gl` 帧、零 H4 故障、无 panic。90 Hz 界面检查是
`logs/115200-run.log`。`devices/thyme/ram-baseline.conf` 是这些哈希的机器
校验副本；`devices/thyme/ram-regress.sh` 会拒绝不一致的字节，除非显式指定
新候选。

## 构建与运行

开发环境以 WSL2 Ubuntu 为主：源码、Git、编译缓存和日志尽量放在 ext4，
Windows 只用于必要的 USB/fastboot 操作。正常测试路径不刷写任何分区：内核和
用户空间加载到内存，运行结束后手机返回 Android。

```sh
make thyme_defconfig
make kernel JOBS=16
devices/thyme/build-root.sh
devices/thyme/ram-regress.sh
```

厂商固件、Darwin 用户空间和大型 system image 都是本地文件，已被 Git 忽略；
需要从手机和 IPSW 中提取什么，见
[`devices/thyme/README.md`](devices/thyme/README.md)；硬件证据、测试方法和
未完成项见 [`devices/thyme/CHECKLIST.md`](devices/thyme/CHECKLIST.md)。

## 上游

本分支跟踪
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) 的公共
内核、引导加载器、用户空间和工具，把 THYME 专属工作放在
`devices/thyme/`。上游 main `87bf485` 已合并；Metal-on-Vulkan/Turnip 路径
是可选功能（`TURNIP=1 M2V=1`、`metal_m2v=1`），不属于日用镜像。设备专属
修改不会推送到上游。

## 仓库与致谢

| 仓库 | 内容 |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | 上游公共内核、加载器、用户空间和文档 |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | Adreno 上的 Metal 命令模型 |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | 启动加载器的 UEFI |

本仓库不包含任何 Apple 的软件或厂商固件。Darwin 用户空间由用户从自己获取
的 IPSW 中布置；固件来自用户自己的手机。Apple 的服务（激活、Apple ID、
iCloud、App Store）不受支持，也不是目标。

XNU 补丁及其派生文件使用 Apple Public Source License 2.0
（[LICENSE.APSL](LICENSE.APSL)），其余使用 MIT（[LICENSE](LICENSE)），
以文件头为准。第三方声明：[NOTICE](NOTICE)、
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)、
[COMPLIANCE.md](COMPLIANCE.md)；分发边界见 [LEGAL.md](LEGAL.md)。以上不构成
法律意见。

HakiOS 是独立研究项目，**与 Apple 无关联、未获其背书**；“Darwin”、“iPhone”、
“XNU”和“Apple”仅用于说明代码所对接的对象。

感谢上游 HakiOS 团队，以及公开逆向 SLPI/SNS 和 FastRPC 协议的人们（参见
[`tools/`](tools) 与 `devices/thyme/CHECKLIST.md` 中的传感器笔记）。
