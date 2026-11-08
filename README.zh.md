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
| 显示与界面帧节奏 | **PARTIAL** | 1080 x 2340 面板时序被接受，CoreAnimation 在需要时以 90 fps 绘制（连续四个 5 秒窗口 452、451、451、451 帧，中位帧间隔 11.1 ms）。日常场景现在请求 90 Hz（`fb_hz=90`）：本面板自己的 timing-switch 序列（F0 page 0、本模式的 B4 表、2f、26）按设备树要求以 LP 模式被接受，帧缓冲重新公布速率。面板自身的扫描率在 THYME 上仍无法回读（TE 焊盘读不到脉冲、INTF 块不安全读取），所以真正的 90 Hz 面板刷新尚未被客观证实 |
| CPU / 调频 | **PASS** | 7 个 CPU 启动；三个 EPSS 域分别达到 1804 / 2419 / 3187 MHz |
| GPU（Mesa/FD650 OpenGL） | **PASS** | 硬件 `hakios-gl` 帧，使用中不回落 `softpipe`；标准无线回归中 110 帧 |
| Metal | **PARTIAL / 阻塞** | `hakios-metal` 可构建、探测通过，但第一个 render pass 仍会卡死，因此 Metal/CoreAnimation 保持关闭。上游的 Metal-on-Vulkan/Turnip 路径已合并，但仍是可选且未在 THYME 上验证 |
| 触摸、按键、手势 | **PASS** | FTS521 触摸、按键、点击/拖动/手势；`errors=0` |
| 亮度 | **PARTIAL** | DSI DCS 亮度命令被确认；自动亮度依赖环境光传感器 |
| Wi-Fi（QCA6390） | **PASS** | 扫描、2.4/5 GHz WPA2 关联、四次握手、DHCP、DNS/TLS/Safari；关闭/打开恢复 |
| 蓝牙（QCA6390，3 Mbaud） | **PASS**（发现、配对、连接、A2DP）；HFP/SCO、AVRCP、麦克风和自动重连仍未验证 | H4 传输、560 次 LE report、无传输故障；QCA responder 先于 `bluetoothd` 启动 |
| 音频 | 扬声器和麦克风 **PASS**；振动 **PARTIAL** | 扬声器/麦克风通路有 1 kHz 声学/录音验证；振动线圈和 BEMF 已验证，iOS 触感反馈尚未 |
| 电池、充电、温控 | **PASS** | 电量/电流/充电状态；30 分钟充电保持；温控降频和有序返回 |
| 相机 | **PARTIAL** | 两颗相机都以 `cam=5 cam_real=1` 接入 `/dev/hakios-camera`（在日常启动参数里，相机图标常在）。S5KHMX 跑模块的 mode 3 全画幅 3008×2256，经 CSIPHY 3、CSID480、IFE-lite，10–25 帧/秒（AE 按光线请求更短帧长，最短 39 ms；两个 DMA bank，一读一帧，不冻结也不撕裂；AE 改变帧长时，等待取新旧帧长较长者，旧的、更长的 bank 不会按新的短时钟去读）。帧头标注像素对应的实际曝光，AE 约五帧收敛；切换相机只给一颗传感器上电（实测 1.7–2.4 s）。广角镜头马达是 **AKM AK7377**（I2C 0x18，位置寄存器 0x00，12 位）：开流时在模块位置附近爬坡，邻域平坦才扫全行程，冰箱上的 QA 标签与 Android 同场景裁切对照可辨（`logs/label-compare-af3.jpg`）。行程没有峰值（暗场景或空白墙面，梯度全是噪声）时，回到上次搜索停下的位置而不是停在噪声挑出的点——切回后摄的第二次会话以前就是这样变糊的；画面比停下的场景明显更清晰时重新搜索；全黑帧不参与测量。前摄 S5K3T2 跑 2592×1940（CSIPHY 4、CSID-lite 1），曝光经 SMIA group hold 中途生效（回读 0x618，亮度随增益变化）；帧长下限 1100 行（48 ms，约 20 帧/秒），模块没有对焦马达，帧头报告无对焦位置，所以前摄开启时不再去驱动后摄马达。灰世界/中性格混合、1.5% 红色校准和 1.18 饱和度负责前摄颜色（厂商调校的色彩矩阵是单位矩阵，没有可用的标定），部分灯光下仍偏冷；108 MP 全分辨率静态照片尚未实现 |
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
2. **相机** —— 后摄全画幅 3008×2256 已经通过 `/dev/hakios-camera` 进入
   Camera.app（10–25 帧/秒），曝光/增益来自采集栈，颜色来自手机自己的色彩
   矩阵和镜头阴影；前摄 2592×1940 经 CSIPHY 4、CSID-lite 1 送入同一个图，
   切换只上电一颗传感器。自动对焦在全行程无峰值时回到上次对好的位置，画面
   出现细节会重新搜索。接下来是前摄颜色（厂商调校没有色彩矩阵）和 108 MP
   全分辨率静态照片。
3. **蜂窝** —— 剩余最大的模块：MPSS、QMI 和 telephony 栈。
4. **持久化存储与独立安装器** —— 采用受保护的写入路径和从 flash RAM 启动的
   设计；在明确的授权与恢复门禁完成前不碰 Android 分区。

## 已验证的 RAM 基线（2026-11-09）

| 产物 | SHA-256 |
|---|---|
| `hakios-kernel` | `c329d30a6f7930ee5e6f06addc4914b98483c4b02570e202372eaec47adbaf1f` |
| `out/thyme-boot/root-wirelessfull.apfs` | `8f5bc2aaa51f57beef172359ffb13e3625b45f50e0369bf19e9cea47279f9683` |
| `devices/thyme/prebuilt/system.apfs` | `4f0f3f8b1ba017ed96016883127761ea65c7d070be791fb49789c2fdace5ab48` |
| `devices/thyme/prebuilt/xnu-usb.img` | `2d662c991a5abacc9f229d3b9d556665959fbda16ec2887f357b7dea56400e10` |

标准冷启动门禁是 `logs/014626-run.log`：两次 Wi-Fi 关联与 DHCP
`192.168.0.114`、Safari 打开、设置里的蓝牙页显示本机以 "HakiOS Mi 10S"
可被发现并正在扫描、`hakios-gl` 帧、无 panic
（`logs/wirelessregress-wifi.png`、`logs/wirelessregress-bt.png`）。日常场景
现在向面板请求 90 Hz（`fb_hz=90`）：本面板自己的 timing-switch 序列
（F0 page 0、本模式的 B4 表、2f、26——J2 节点的，不是 J1S 节点更长的那条）
按设备树要求以 LP 模式发送，帧缓冲重新公布被接受的速率，帧间隔落在
11 ms 节拍上。内核同时是 servreg 应答那版（`8befdf8`）：确认通知器的状态
指示、读取保护域失败原因、SNS 客户端请求字节与手机 HAL 完全一致；传感器
数据流仍未打通（`devices/thyme/docs/sensors-status.md`）。更早的门禁
`logs/184735-run.log` 仍是相机对焦、帧节奏、拍照累加器与 locator 那版
（`938bed9`）的参考。90 fps 帧节奏检查是 `logs/115200-run.log`，
60 Hz 长跑是 `logs/035725-run.log`。`devices/thyme/ram-baseline.conf` 是这些哈希的机器
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
`devices/thyme/`。上游 main `63fc2b4` 已合并；Metal-on-Vulkan/Turnip 路径
是可选功能（`TURNIP=1 M2V=1`、`metal_m2v=1`），不属于日用镜像。设备专属
修改不会推送到上游。

## 仓库与致谢

| 仓库 | 内容 |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | **公共部分**：内核移植、所有公共驱动、引导加载器、用户空间、工具、本文档以及 `devices/qemu` |
| [`hakios-metal/`](hakios-metal) | 通过内核的 GPU 驱动，在 Adreno 上实现 Metal 的命令模型 |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G：defconfig、状态、UEFI 平台、固件脚本及其 SoC 层（Qualcomm SC7280 系列） |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20（MediaTek Helio G85）：实验性移植，有自己的引导加载器。不在本仓库的清单中 |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | 启动引导加载器的固件：带有 HakiOS 启动路径、横幅和名称的 Mu-Silicium，以及 HakiOS 的 U-Boot |
| [hackios-web](https://github.com/oosexgang/hackios-web) | 网站 |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | 旧仓库：只为保留链接而存在的指向。用户空间现在在这里的 `userland/` |

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
