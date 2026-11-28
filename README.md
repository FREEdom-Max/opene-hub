# OpenE hub

[English](README.en.md)

**OpenE hub** 是面向 **华为 MateBook E 2023（Intel x64）** 的 Windows 11 硬件控制工具。在任务栏托盘即可切换电源模式、查看电池和配件状态，并打开主窗口调整充电保护和屏幕设置。

## 适用设备

- **电脑**　华为 MateBook E 2023（`DRR-WXX`），Intel Core i5-1230U 或 i7-1260U，Windows 11 x64。已记录的真机验证环境为 i5-1230U、BIOS 1.29。
- **配件**　华为 M-Pencil 手写笔和 RX0I 分体式智能磁吸键盘。

请保留原装华为驱动和华为电脑管家的安装文件。充电保护、屏幕校色、配件状态及部分传感器读取依赖其中的组件。可以在 OpenE hub 的「服务」页停用或恢复电脑管家的常驻业务服务。

## 主要功能

- **电源与性能**　同步 Windows 电源模式，分别设置 Intel P 核和 E 核的频率上限，适用于插电和电池供电。启用 OpenE hub 硬件服务并停用华为电脑管家的电源服务后，`Fn + P` 可循环切换三档模式，在屏幕上显示提示，并在最佳性能档联动华为高能模式。
- **电池与传感器**　查看充放电功率、电量、电池健康度和循环次数，按需读取风扇转速与 CPU 温度。支持智能充电和 50% 至 100% 的六档手动充电上限。
- **屏幕**　切换内屏 60 Hz / 120 Hz 刷新率和原生、sRGB、Display P3 色域，调整自然色彩显示、色温与护眼模式。
- **手写笔与键盘**　查看连接状态、电量和固件版本，控制磁吸键盘分离后的无线连接。
- **托盘与服务**　左键点击托盘图标打开快捷面板，右键菜单可打开主窗口。「服务」页可停用或恢复华为电脑管家的常驻业务服务，同时保留触控、手写笔及硬件快捷键所需的底层服务。

| Windows 电源模式 | P 核上限 | E 核上限 |
| --- | ---: | ---: |
| 最佳能效（含节能模式） | 2.0 GHz | 1.5 GHz |
| 平衡 | 2.8 GHz | 2.2 GHz |
| 最佳性能 | 不限频 | 不限频 |

退出 OpenE hub 时，程序会还原启动前的频率设置，并关闭由它启用的华为高能模式。

## 构建与安装

准备 **Visual Studio 2022 或 Build Tools**、MSVC x64 工具链、MSBuild 和 **Windows SDK 10.0.26100**。构建时由 MSBuild 还原 NuGet 依赖。

### 生成 MSI 安装包

另需 **WiX Toolset v5**，以及 `WixToolset.UI.wixext` 和 `WixToolset.Util.wixext` 扩展。在仓库根目录运行以下命令。

```powershell
.\scripts\pack-msi-x64.ps1
```

脚本会先构建程序，再生成 `build\OpenEHubSetup-x64.msi`。运行 MSI 时需要管理员权限来注册硬件服务。安装后可从开始菜单启动。通过 Windows「设置 → 应用」卸载时，安装程序会移除硬件服务，并恢复此前由 OpenE hub 停用的华为后台服务。

预编译安装包发布后可从这里下载。 [GitHub Releases](https://github.com/FREEdom-Max/opene-hub/releases)

### 从源码直接运行

双击 [opene-hub.cmd](opene-hub.cmd)，脚本会在需要时自动构建。也可以在仓库根目录运行以下命令。

```powershell
.\scripts\build-settings-x64.ps1
.\build\settings-x64\openehub.exe
```

首次启动后可在任务栏托盘找到 OpenE hub。启用风扇转速、温度和充电保护等硬件功能时，程序可能请求管理员权限来安装硬件服务。

## 诊断与源码

- [硬件诊断脚本](scripts/Test-MateBookE.ps1) 可只读检查电池、风扇转速和温度接口。
- [Tools/EGoTouchSettings](Tools/EGoTouchSettings) 包含 WinUI 3 主程序、硬件服务及配件辅助程序。
- [scripts](scripts) 包含 x64 构建、MSI 打包和诊断脚本。[docs](docs) 收录技术笔记。

## 致谢与许可证

OpenE hub 派生自 [OpenEGo Hub](https://github.com/NihilDigit/openego-hub)，后者基于 [EGoTouchRev](https://github.com/awarson2233/EGoTouchRev)。开发过程中也参考了 [MateBook-E-Pen](https://github.com/eiyooooo/MateBook-E-Pen)、[goodies](https://github.com/matebook-e-go/goodies)、[EgoTools](https://github.com/SaKongA/EgoTools) 和 [HuaweiPenEraserService](https://github.com/qwqVictor/HuaweiPenEraserService)。

本项目采用 [MIT 许可证](LICENSE)。再分发源码或二进制文件时，请随附适用的版权和许可声明，详见 [第三方声明](THIRD-PARTY-NOTICES.md)。OpenE hub 是独立社区项目，与华为无官方关联。
