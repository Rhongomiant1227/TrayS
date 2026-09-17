# TrayS 维护交接说明

这份文件面向后续维护 agent。当前仓库是 `Rhongomiant1227/TrayS` fork 的 Windows 10/11 维护版，当前产品版本为 **1.5.0**。目标是保留 TrayS 的任务栏监控功能，同时让默认发布包在 AMD、Intel、多显卡和较新的 Windows Shell 上安全降级。

## 当前基线

- 目标平台：Windows 10（build 10240 及以上）和 Windows 11；构建配置只有 `Release|x64`、`Release|Win32` 及对应 Debug 配置。
- 默认 UAC：`asInvoker`。程序不主动提权，不安装服务、计划任务或驱动。
- 启动顺序：先用 `RtlGetVersion` 检查 Windows 10+，旧系统只显示提示并退出；随后才创建进程映射、查找 Explorer、读取配置和初始化监控接口。
- 设置 UI：链接指向本 fork 和 `COMPATIBILITY.md`，不再保留旧论坛链接；风格选项对应 `ACCENT_DISABLED`、透明渐变、DWM 模糊和亚克力。
- 更新机制：设置中提供“自动获取更新”（默认开启）和“检查更新”按钮。更新只访问 GitHub HTTPS API/Release，按 `TrayS_<版本>_<架构>.zip` 精确选择资产，并要求 SHA-256、版本和 PE 架构全部匹配；下载、校验和替换在独立线程/临时 PowerShell helper 中完成，不打开浏览器、不加载驱动。更新失败保留旧 EXE。
- ARM 边界：当前只维护 Win32/x64。Windows on ARM64 可尝试 x64 模拟运行，但不等同于原生 ARM64；不要添加把 ARM64 错误映射到 x64 的 solution 配置，也不要把现有包重命名成 ARM64。原生 ARM64 需要重新处理 C++/CLI、传感器程序集和 AMD/NVIDIA 厂商 DLL，并经过真实设备回归。
- 版本信息：资源文件 `TrayS/TrayS.rc` 中为 `1.5.0.0`。`TRAYSAVE` 原始结构保持兼容，当前数据版本仍为 `116`，不要仅因改 UI 就递增它。

## 温度与显卡安全边界

1. CPU 默认先尝试 Windows/ACPI Thermal Zone 的 PDH 只读计数器（高精度计数器优先，普通计数器其次）。这是平台/封装温度，某些固件不公开热区时返回不可用，不应把不可用当成 0°C。
2. 默认不加载 `OpenHardwareMonitorApi.dll`，因为随附的 LHM 0.9.4 仍可能包含 WinRing0 后端。只有显式设置 `TRAYS_ENABLE_LHM=1` 才启用它；旧的 `TRAYS_ENABLE_LHM_AMD=1` 仅作为 AMD 测试兼容别名。默认发布包不带这些托管 DLL。
3. AMD GPU 只使用 `atiadlxx.dll`/`atiadlxy.dll` 的 ADL 只读温度接口；NVIDIA GPU 只使用 `nvapi64.dll`/`nvapi.dll` 的 NVAPI 温度接口。两条路径都逐卡枚举，单卡失败不能阻塞其他卡，也不能调用风扇、频率、电压、功耗或超频接口。
4. 严禁恢复 `WinRing0x32.sys`、`WinRing0x64.sys`、Ols/MSR/PCI 直读、PawnIO 自动安装，或任何会重载显示驱动、改变 BIOS/服务状态的测试。
5. 释放温度 DLL 前必须取得 `g_temperatureLock` 独占锁；ACPI 查询的生命周期由 `g_thermalPdhLock` 管理。若移动线程或新增硬件后端，先审查锁顺序和退出等待。

## Windows 10/11 UI 边界

- 透明、模糊、亚克力是否可见取决于 DWM、系统主题、Explorer 状态和具体 Windows build；不能把某个 build 的失败写成所有 Win11 都不支持。
- Windows 11 的任务栏图标区域可能由 `Windows.UI.Composition.DesktopWindowContentBridge` 接管。检测到该 Shell 结构时，位置单选项会禁用；这是避免反复移动 Explorer 控件，不是驱动或系统修改。
- Explorer 重启、多显示器、DPI 缩放和全屏切换是重点回归场景。不要用循环逐像素移动窗口，也不要把刷新间隔恢复到旧版的 11 ms；当前默认值为 400 ms，合法范围 100–5000 ms。

## 构建与静态检查

Build Tools 保存在仓库目录 `F:\trayS\.buildtools`，用户要求在明确说“卸载”之前保留它。默认构建和打包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -Platform x64 -Configuration Release -PackageName TrayS_1.5.0_x64 -Force
```

生成物应位于 `F:\trayS\dist\`。安全默认包只放 `TrayS.exe`、`COMPATIBILITY.md`、`PACKAGE.txt` 和 `SHA256SUMS.txt`，不得包含 `.buildtools`、个人 `TrayS.dat`、WinRing0/Ols/PawnIO 或 LHM DLL。需要隔离测试 LHM 时才显式使用 `-IncludeLhm`，该包不应作为默认 Release。

Release 资产名称必须包含产品名和架构，例如 `TrayS_1.5.0_x64.zip`、`TrayS_1.5.0_x86.zip`；不要恢复旧的 `_x64_ALL_...` 匿名命名。

当前基线已经在本机完成 `Release|x64` 和 `Release|Win32` 构建，链接结果为 0 个警告、0 个错误；静态检查也已通过。MSBuild 日志中的 `System.Core, Version=3.5.0.0` C++/CLI 加载提示不影响最终产物。真实硬件温度采样尚未执行，后续 agent 必须在隔离环境中验证 AMD、Intel、混合 AMD/NVIDIA 和多显卡场景，不能通过启动当前 TrayS 来替代验证。

构建后至少运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
git diff --check
```

如果 VS/MSBuild 不可用，记录为“未完成编译验证”，不要用启动程序来替代静态检查，也不要为了测试而加载驱动。

## 未来机型适配清单

新增机型或传感器后，先收集 Windows build、CPU/GPU 型号、是否混合显卡、是否有虚拟显示适配器、ACPI PDH 计数器名称和值、Explorer 任务栏类名和 DPI 缩放。优先增加只读、可失败、逐设备隔离的探测；不要把一个厂商的失败变成全局退出条件。任何需要内核驱动、管理员权限、MSR/PCI 访问或改变显卡状态的方案，都必须单独评审，不能直接进入默认包。

发布前检查 Git 状态，确认没有把 `.buildtools`、`Bin` 中的中间文件、用户配置和实验包提交到仓库。代码推送到 `origin`（`https://github.com/Rhongomiant1227/TrayS.git`）后，再把不含旧驱动的版本化 ZIP 上传到 GitHub Release，并在 Release 说明中列出 SHA-256 和“LHM/WinRing0 未包含”的事实。
