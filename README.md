<p align="center">
  <img src="Assets/brand/openego-hub-256.png" alt="OpenE hub" width="88">
</p>

<h1 align="center">OpenE hub</h1>

<p align="center">面向 HUAWEI MateBook E 2023（Intel x64）的轻量级硬件控制中心</p>

<p align="center">
  <a href="#硬件支持"><img src="https://img.shields.io/badge/Platform-Windows%2011%20x64-317AF7?style=flat-square&labelColor=334155" alt="Platform"></a>
  <a href="https://github.com/NihilDigit/openego-hub"><img src="https://img.shields.io/badge/Forked%20from-OpenEGo%20Hub-64748B?style=flat-square&labelColor=334155" alt="Forked from OpenEGo Hub"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/FREEdom-Max/opene-hub?style=flat-square&label=License&labelColor=334155&color=64748B" alt="License"></a>
</p>

<p align="center"><sub><strong>中文</strong> | <a href="README.en.md">English</a></sub></p>

**OpenE hub** 派生自 [OpenEGo Hub](https://github.com/NihilDigit/openego-hub)（MateBook E Go · ARM64），专为 **HUAWEI MateBook E 2023（`DRR-WXX`，Intel x64）** 重新适配与扩展。在停用华为电脑管家常驻后台服务的前提下，提供电池与充电保护、Intel 大小核分核频限联动、`Fn + P` 高能模式接管与 Fluent HUD、风扇/温度按需监测、屏幕色彩与刷新率切换，以及手写笔和智能磁吸键盘状态管理。

> [!IMPORTANT]
> 系统中需保留原装华为驱动与华为电脑管家安装目录。屏幕校色、配件状态、充电保护与 BIOS/EC 传感器均需按需调用电脑管家附带的本地 DLL，彻底卸载电脑管家会导致对应功能不可用；电脑管家的常驻后台服务可在「服务」页一键停用，无需后台常驻。

---

## 主要功能

- **概览与零轮询托盘面板**：汇总主机硬件信息、实时充放电功率/电压/容量/循环次数与健康度，按需读取风扇转速与 CPU 温度，显示手写笔与磁吸键盘电量。首次启动默认静默驻留任务栏托盘，左键弹出快捷控制面板，再次启动或右键菜单打开主窗口。
- **三档电源模式 × 分核频限 × 华为高能模式联动**：
  - 运行期间自动将 Windows 三种电源模式（插电与电池通用）双向绑定到 Intel 混合架构频率上限：
    - **最佳能效（含节能模式）**：P 核 `2.0 GHz` · E 核 `1.5 GHz`（保持 Windows 默认 EPP）
    - **平衡**：P 核 `2.8 GHz` · E 核 `2.2 GHz`
    - **最佳性能**：不限频，并自动联动开启华为 BIOS/EC **高能模式**
  - 无论在 Windows 设置、托盘快捷面板、主窗口还是快捷键切档，均实时双向同步；退出、关机或卸载时自动还原系统默认频限并关闭高能模式。
- **`Fn + P` 快捷键接管与 Fluent HUD**：
  - 停用华为电脑管家服务后，`OpenEHubHwService` 通过零轮询 WMI `OemWMIEvent` 自动接管 `Fn + P` 快捷键，实现「最佳能效 → 平衡 → 最佳性能（高能模式）」三档循环切换。
  - 切换时在屏幕顶部居中显示自适应明暗主题的 **Fluent 亚克力 HUD**，并在切档瞬间定向压制华为 `OSD_Daemon.exe` 重复弹窗，不影响音量、亮度、麦克风、Wi-Fi 等其他快捷键 OSD。
- **电池与充电保护**：集成 Windows 11 节能模式与电源模式切换，通过独立本地硬件服务 `OpenEHubHwService` 按需同步华为智能充电与六档手动充电上限（50%–100%）。
- **屏幕与色彩**：切换内屏刷新率（120 Hz / 60 Hz）、出厂校准色域（原生 / sRGB / Display P3）、自然色彩显示、色温与护眼模式。
- **手写笔与智能磁吸键盘**：采用事件驱动与重叠 I/O（overlapped IPC）的独立宿主进程（`openehubPen.exe`、`openehubKeyboard.exe`），监测 HUAWEI M-Pencil 与分体式智能磁吸键盘的连接、吸附、充放电、电量及固件版本，支持控制键盘分离无线连接开关，并在接入时弹出顶部状态提示。
- **服务管理**：一键停用或恢复华为电脑管家 6 项常驻业务服务（`MBAMainService`、`HwPCClientService`、`HiConnectivityService`、`HwDistributedMainService`、`MouseCrossingDaemon`、`WUCSProxy`），同时保留触控/笔与硬件快捷键底层服务（`HuaweiThpService`、`OSD_Daemon`）。卸载时自动恢复原状。

---

## 硬件支持

- **主机**：HUAWEI MateBook E 2023（`DRR-WXX`，Intel Core i5-1230U / i7-1260U，Windows 11 x64；已在 i5-1230U / BIOS 1.29 真机验证）
- **键盘**：HUAWEI 智能磁吸键盘（RX0I 分体式）
- **手写笔**：HUAWEI M-Pencil

仓库内置的只读硬件诊断脚本 [scripts/Test-MateBookE.ps1](scripts/Test-MateBookE.ps1) 可在不改动系统设置的前提下验证本机电池、风扇转速与温度接口。

---

## 安装与使用

### 方式一：MSI 标准安装（推荐）

1. 使用 `scripts/pack-msi-x64.ps1` 构建或从 Releases 获取 `OpenEHubSetup-x64.msi`。
2. 运行安装程序（注册硬件服务 `OpenEHubHwService` 需管理员权限）。
3. 安装后通过开始菜单或任务栏托盘图标使用；可在 Windows「设置 → 应用」中一键完整卸载，卸载时自动移除硬件服务并还原此前停用的华为后台服务与系统电源设置。

### 方式二：便携直接运行

- 双击仓库根目录的 [opene-hub.cmd](opene-hub.cmd)（未编译时自动触发构建），或直接运行 `build/settings-x64/openehub.exe`。

---

## 源码构建

### 环境要求

- Windows 11 x64
- Visual Studio 2022（MSVC x64 工具链与 MSBuild）
- Windows SDK `10.0.26100`
- NuGet CLI
- WiX Toolset v5（仅构建 MSI 安装包时需要）

### 编译与打包

```powershell
# 1. 构建 x64 主程序、硬件服务与配件宿主（输出至 build\settings-x64）
.\scripts\build-settings-x64.ps1

# 2. 打包标准 MSI 安装程序（输出至 build\OpenEHubSetup-x64.msi）
.\scripts\pack-msi-x64.ps1
```

---

## 项目结构

- `Tools/EGoTouchSettings/`：WinUI 3 主程序（`openehub.exe`）、托盘与 Fluent HUD、分核频限绑定器（`MateBookPowerFreqBinder.h`）、硬件服务（`MateBookHw.cpp` → `openehubHw.exe` / `OpenEHubHwService`）及事件驱动配件宿主（`openehubPen.exe`、`openehubKeyboard.exe`）。
- `scripts/`：x64 构建脚本（`build-settings-x64.ps1`）、WiX MSI 打包脚本（`pack-msi-x64.ps1` / `OpenEHubSetup-x64.wxs`）与只读诊断脚本（`Test-MateBookE.ps1`）。
- `Common/`：跨进程共享内存通道与公共定义。
- `docs/`：键盘 MCU、手写笔与华为组件协议分析文档。
- `EGoTouchService/`、`hal/`：继承自上游 OpenEGo Hub 的 ARM64 服务与硬件抽象层代码。

---

## 致谢

本项目派生自 **[OpenEGo Hub](https://github.com/NihilDigit/openego-hub)**（面向 MateBook E Go ARM64，其本身派生自 **[EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)**，MIT License，© Detach2233），完整第三方版权声明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

开发与逆向过程中参考了以下社区项目：

- [OpenEGo Hub](https://github.com/NihilDigit/openego-hub)（by NihilDigit）
- [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)（by eiyooooo）
- [goodies](https://github.com/matebook-e-go/goodies)（by dantmnf）
- [EgoTools](https://github.com/SaKongA/EgoTools)（by SaKongA）
- [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)（by qwqVictor）

---

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。任何形式的源码或二进制分发均须保留 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) 中的版权声明。

---

## 免责声明

- **商标与产权**：本项目与 HUAWEI 及其他相关权利方无任何官方关联或授权。提及的产品名称、商标与公司名称均归各自权利人所有。
- **第三方资产**：程序在运行时按需从本机已安装的华为电脑管家目录读取设备渲染图与组件能力，未检测到时自动回退至内置图标；受版权保护的原厂素材不包含在本仓库或安装包中。
- **风险提示**：本项目涉及底层硬件接口、WMI/EC 交互、电源策略与系统服务管理，仅用于互操作性研究与技术交流。使用本软件产生的硬件故障、数据丢失或系统不稳定风险由用户自行承担，作者及贡献者不承担相关法律责任。
