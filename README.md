# ✦ TrayS / 现在就要清爽的任务栏

> 一只安静住在任务栏里的小工具：看流量、看占用、看温度，也给老项目做一次认真而克制的现代化维护。
>
> **这不是把旧代码重新染个颜色。** 这是一个面向 Windows 10/11、AMD/Intel、多显卡和更严格安全边界的维护型 fork。

<p align="center">
  <img alt="TrayS" src="TrayS/TrayS.ico" width="96">
</p>

<p align="center">
  <a href="https://github.com/Rhongomiant1227/TrayS/releases/latest"><strong>下载最新 Release</strong></a>
  ·
  <a href="COMPATIBILITY.md">兼容性说明</a>
  ·
  <a href="AGENT_HANDOFF.md">维护交接</a>
</p>

## 🌙 这是什么项目？

TrayS 是一个运行在 Windows 任务栏附近的轻量监控工具，可以显示网络流量、系统占用、温度、磁盘和行情等信息，并提供透明、模糊、亚克力等任务栏风格选项。

本仓库来自上游项目 [cgbsmy/TrayS](https://github.com/cgbsmy/TrayS) 的 fork，由 [Rhongomiant1227/TrayS](https://github.com/Rhongomiant1227/TrayS) 继续维护。上游版本已经比较老，面对新一代 AMD 平台、混合显卡、Windows 11 Shell 和现代安全软件时，容易出现兼容性或告警问题；本 fork 的目标是保留 TrayS 的轻巧体验，同时把高风险、过时和容易误伤系统的路径收紧。

当前维护版本：**TrayS 1.5.0**

## ✨ 现在有什么不一样？

### 🧊 温度监控：优先安全降级

- CPU 温度默认优先尝试 Windows/ACPI Thermal Zone 的只读 PDH 路径，支持 `High Precision Temperature` 和普通 `Temperature` 计数器。
- AMD Ryzen、Intel Core/Xeon 等平台只要固件向 Windows 暴露热区，就可以尝试读取；没有热区时显示不可用，不会为了“必须有数字”去读 MSR、PCI 配置空间或安装驱动。
- AMD 显卡使用 ADL 只读温度接口，NVIDIA 显卡使用 NVAPI 只读温度接口；逐卡枚举，混合 AMD+iGPU/NVIDIA、多 NVIDIA 卡或虚拟显示适配器时，单个设备失败不会拖垮其他设备。
- 风扇、频率、电压、功耗、超频和驱动重载接口不在默认监控路径中。
- LHM/WinRing0/PawnIO 相关路径默认关闭。旧版 LHM 程序集仍可能包含旧驱动代码，只有明确设置 `TRAYS_ENABLE_LHM=1` 才会尝试加载，因此安全包不会在正常启动时安装或打开这些内核组件。

### 🛡️ 安全边界：少一点“神秘操作”

- 默认以 `asInvoker` 运行，不主动申请管理员权限。
- 默认不安装服务、不创建计划任务、不修改 BIOS、不重载 AMD/NVIDIA 显示驱动。
- DLL 从明确的应用目录或 Windows System32 加载，避免当前目录 DLL 劫持。
- 网络请求使用系统 `winhttp.dll`，限制主机、HTTPS、超时和响应大小；行情与更新失败时保留旧数据或静默降级。
- 配置和网络、进程、传感器数据边界都做了长度、数量、索引、有限数值和异常检查。
- 删除旧 WinRing0/Ols 文件、旧硬件直读路径和过时的论坛/破解提示；不会因为一个传感器失败就让整个程序退出。

### 🎛️ Windows 10/11 UI：功能和界面终于对得上

- 启动前明确检查 Windows 10 及以上版本；Windows 7/8/8.1 不在本维护版范围内。
- 设置窗口保留原生 Win32 风格，减少额外依赖；默认、透明、模糊、亚克力选项与实际实现一致。
- Win11 任务栏布局由系统 Shell 接管的场景会按实际能力降级，而不是强行改写 Shell。
- 更新了版本标识、维护说明链接，并移除已经失效的旧系统/旧论坛文案。

### 🔄 应用内更新：不用跳浏览器

设置里有两个选项：

- **自动获取更新**：默认开启；启动后延迟检查，之后以较长间隔检查，不会高频访问 GitHub。
- **检查更新**：手动触发一次后台检查，界面不会被网络请求卡住。

更新流程会从固定的 GitHub HTTPS API 查询最新 Release，严格选择当前版本和架构对应的资产，例如：

```text
TrayS_1.5.0_x64.zip
TrayS_1.5.0_x86.zip
```

下载包在安装前会检查 Release digest、本地 SHA-256、版本资源和 PE 架构。程序会先退出，再由临时 helper 完成替换并重启；失败时保留旧 EXE，不会把半个更新包覆盖到正在运行的程序上。更新过程不打开浏览器，也不调用第三方下载器。

## 📦 下载和使用

从 [Releases](https://github.com/Rhongomiant1227/TrayS/releases) 选择对应架构：

- 大多数 Intel/AMD 电脑：`TrayS_<版本>_x64.zip`
- 只有确实运行 32 位 Windows 时：`TrayS_<版本>_x86.zip`

解压后直接运行 `TrayS.exe` 即可。默认安全包不包含个人配置，配置文件会在程序目录旁按需创建：

```text
TrayS.exe
TrayS.dat             # 原有设置，保持旧结构兼容
TrayS.update.dat      # 更新开关，默认不存在即视为开启
```

如果安全软件提示风险，请先核对 Release 的 SHA-256 和包内 `SHA256SUMS.txt`，不要为了绕过告警而关闭系统防护。默认包不包含 LHM、WinRing0、PawnIO、Ols 或 `.sys` 驱动文件；如果仍有告警，应把告警路径和行为交给安全软件分析，而不是盲目放行。

## 🧰 从源码构建

要求：Windows 10/11、Visual Studio 2022 Build Tools、MSVC v143、Windows SDK 和 C++/CLI 组件。仓库可把 Build Tools 放在 `.buildtools`，这样构建环境和项目一起保存；删除项目时再按 [COMPATIBILITY.md](COMPATIBILITY.md) 的卸载说明处理。

只做静态兼容性检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
```

构建并生成带产品名的安全 Release 包：

```powershell
# x64
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\package-release.ps1 `
  -Platform x64 -Configuration Release `
  -PackageName TrayS_1.5.0_x64 -Force

# x86
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\package-release.ps1 `
  -Platform Win32 -Configuration Release `
  -PackageName TrayS_1.5.0_x86 -Force
```

默认包只包含 `TrayS.exe`、`COMPATIBILITY.md`、`PACKAGE.txt` 和 `SHA256SUMS.txt`。它不会把 `.buildtools`、`Bin` 中间文件、个人 `TrayS.dat`、WinRing0/Ols/PawnIO 或 LHM DLL 打进去。`-IncludeLhm` 只用于隔离测试，不是默认发布方案。

## 🔍 维护版详细改进清单

这次 fork 不是单点修补，主要改动可以按下面几条线理解：

| 方向 | 改进 |
| --- | --- |
| 系统范围 | 明确 Windows 10/11 目标；删除旧系统映射和错误的 Any CPU 映射。 |
| CPU 温度 | ACPI/PDH 只读热区、有限值检查、失败可用性降级；不再依赖 WinRing0 才能启动。 |
| AMD GPU | ADL 动态加载、活动适配器过滤、逐卡只读温度读取。 |
| NVIDIA GPU | NVAPI 物理 GPU 数组按 API 最大值分配，逐卡读取并限制传感器范围。 |
| 混合显卡 | AMD+iGPU/NVIDIA、多 NVIDIA、虚拟适配器和 DLL 缺失时互不拖累。 |
| LHM 边界 | LHM 作为显式 opt-in；默认包不携带仍可能包含旧驱动后端的托管 DLL。 |
| DLL 安全 | System32/应用目录限定加载，降低 DLL 搜索路径劫持风险。 |
| 网络 | WinHTTP HTTPS、固定主机、请求白名单、5 秒超时、响应大小上限。 |
| 配置 | 保持 `TRAYSAVE` 固定布局和版本兼容；更新开关单独原子写入。 |
| 任务栏 UI | Win11 Shell 能力检测、DPI 和 Explorer 重启边界收紧；取消高成本逐像素移动。 |
| 线程退出 | 正常等待工作线程，避免用 `TerminateThread` 留下锁或第三方 DLL 状态。 |
| 应用更新 | 后台检查、精确资产名、SHA-256、版本/架构校验、失败回滚和重启。 |
| 发布 | x64/x86 包改为 `TrayS_<版本>_<架构>.zip`，包内附校验清单和兼容性说明。 |

更完整的边界、已知限制和回归表请看 [COMPATIBILITY.md](COMPATIBILITY.md)。后续维护 agent 的上下文、禁止事项和交接入口在 [AGENT_HANDOFF.md](AGENT_HANDOFF.md)。

## 🧪 测试边界

当前完成的是静态检查、x64/x86 Release 编译、PE 架构检查、包内容检查和 Release digest 校验；没有启动维护版 TrayS，没有加载温度 DLL，没有重载 AMD/NVIDIA 驱动，也没有做可能导致关机、卡死或驱动冲突的压力测试。

真实硬件回归仍需要在可恢复的隔离环境中逐项完成，尤其是：

- AMD Ryzen 不同世代的 ACPI 热区暴露；
- Intel 移动/桌面平台的热区和睡眠唤醒；
- AMD+iGPU/NVIDIA 独显混合场景；
- 多 NVIDIA 卡、虚拟显示适配器和 Explorer 重启；
- 更新失败、网络中断、磁盘空间不足和旧配置迁移。

没有实机证据的项目不会在这里被写成“已认证”。

## 💌 贡献和反馈

欢迎提交 issue 或 pull request。请尽量附上：Windows 版本、CPU/GPU 型号、x64/x86 环境、是否为混合显卡、复现步骤和日志。不要上传个人 `TrayS.dat`、GitHub token 或完整的安全软件敏感报告。

## 📜 许可与致谢

本仓库保留上游 TrayS 的历史代码和许可信息。感谢上游作者提供最初的任务栏监控工具，也感谢 LibreHardwareMonitor、HidSharp、AMD ADL 与 NVIDIA NVAPI 生态提供可用的用户态接口。第三方组件的许可和版本信息请以仓库中的对应文件为准。
