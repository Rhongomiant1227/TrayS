# TrayS 维护交接说明

这份文件面向后续维护 agent。当前仓库是 `Rhongomiant1227/TrayS` fork 的 Windows 10/11 维护版，当前产品版本为 **1.7.2**。目标是保留 TrayS 的任务栏监控功能，同时让发布包在 AMD、Intel、多显卡和较新的 Windows Shell 上安全降级。

## 当前基线

- 目标平台：Windows 10（build 10240 及以上）和 Windows 11；构建配置只有 `Release|x64`、`Release|Win32` 及对应 Debug 配置。
- 默认 UAC：`asInvoker`。程序不主动提权，不安装服务、计划任务或驱动。
- 启动顺序：先用 `RtlGetVersion` 检查 Windows 10+，旧系统只显示提示并退出；随后才创建进程映射、查找 Explorer、读取配置和初始化监控接口。
- 设置 UI：链接指向本 fork 和 `COMPATIBILITY.md`，不再保留旧论坛链接；风格选项对应 `ACCENT_DISABLED`、透明渐变、DWM 模糊和亚克力。
- 更新机制：设置中提供“自动获取更新”（默认开启）和“检查更新”按钮。更新只访问 GitHub HTTPS API/Release，按 `TrayS_<版本>_<架构>.zip` 精确选择资产，并要求 SHA-256、版本和 PE 架构全部匹配；下载、校验和替换在独立线程/临时 PowerShell helper 中完成，不打开浏览器、不加载驱动。更新失败保留旧 EXE。
- ARM 边界：当前只维护 Win32/x64。Windows on ARM64 可尝试 x64 模拟运行，但不等同于原生 ARM64；不要添加把 ARM64 错误映射到 x64 的 solution 配置，也不要把现有包重命名成 ARM64。原生 ARM64 需要重新处理 C++/CLI、传感器程序集和 AMD/NVIDIA 厂商 DLL，并经过真实设备回归。
- 版本信息：资源文件 `TrayS/TrayS.rc` 中为 `1.7.2.0`。`TRAYSAVE` 原始结构保持兼容，当前数据版本仍为 `116`，不要仅因改 UI 就递增它。资源文件保持 UTF-16 LE 编码，C++ 源文件保持 UTF-8。

## 温度与显卡安全边界

1. CPU 优先读 LHM 的硬件封装温度（AMD Tctl/Tdie 优先），不可用时回退 Windows/ACPI Thermal Zone 的 PDH 只读计数器（高精度计数器优先）。热区未必对应 CPU，某些固件不公开热区时返回不可用，不应把不可用当成 0°C。
2. 只有 PawnIO 设备已经安装且可访问时，TrayS 才加载随包提供的 LibreHardwareMonitor 0.9.6。官方 PawnIO 2.2.0 的设备 ACL 只允许 SYSTEM 与提升权限的管理员，普通权限返回 Win32 5。设置必须提示管理员权限，不能误报需要重装。LHM CPU 硬件组必须在 `Computer::Open()` 前启用。TrayS 不安装、不启动或捆绑 PawnIO 驱动，禁止回退 WinRing0、Ols 或自行读 MSR/PCI。
3. AMD GPU 只使用 `atiadlxx.dll`/`atiadlxy.dll` 的 ADL 只读温度接口；NVIDIA GPU 只使用 `nvapi64.dll`/`nvapi.dll` 的 NVAPI 温度接口。两条路径都逐卡枚举，单卡失败不能阻塞其他卡，也不能调用风扇、频率、电压、功耗或超频接口。
4. 严禁恢复 `WinRing0x32.sys`、`WinRing0x64.sys`、Ols/MSR/PCI 直读、PawnIO 自动安装，或任何会重载显示驱动、改变 BIOS/服务状态的测试。
5. 释放温度 DLL 前必须取得 `g_temperatureLock` 独占锁；ACPI 查询的生命周期由 `g_thermalPdhLock` 管理。若移动线程或新增硬件后端，先审查锁顺序和退出等待。

## Windows 10/11 UI 边界

- 透明、模糊、亚克力是否可见取决于 DWM、系统主题、Explorer 状态和具体 Windows build；不能把某个 build 的失败写成所有 Win11 都不支持。
- Windows 11 的任务栏图标区域可能由 `Windows.UI.Composition.DesktopWindowContentBridge` 接管。检测到该 Shell 结构时，位置单选项会禁用；这是避免反复移动 Explorer 控件，不是驱动或系统修改。
- Explorer 重启、多显示器、DPI 缩放和全屏切换是重点回归场景。不要用循环逐像素移动窗口，也不要把刷新间隔恢复到旧版的 11 ms；当前默认值为 400 ms，合法范围 100–5000 ms。

## 构建与静态检查

Build Tools 保存在仓库目录 `.buildtools`，用户要求在明确说“卸载”之前保留它。默认构建和打包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -Platform x64 -Configuration Release -PackageName TrayS_1.7.2_x64 -Force
```

当前机器没有 MSBuild 时，使用项目内 LLVM-MinGW 生成原生 Win32 兼容性验证 EXE：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-portable-compat-exe.ps1 -Force
```

它输出 `dist\TrayS-compat-win11-x64\TrayS-compat-win11-x64.exe` 和对应的 `dist\TrayS-compat-win11-x64.zip`，中间文件放在 `.build-native\`，并静态链接 LLVM C++ 运行库，因而可以脱离 `.buildtools` 目录直接启动。该构建不包含 C++/CLI 的 LibreHardwareMonitor 包装层；完整安全 Release 仍按上面的 `package-release.ps1` 使用 MSVC/C++/CLI 工具链生成。

生成物应位于仓库内的 `dist\`。标准包包含 TrayS EXE、LHM 0.9.6 的架构匹配程序集、所需运行依赖、兼容说明和许可文件；不得包含 `.buildtools`、个人 `TrayS.dat`、WinRing0/Ols/PawnIO 驱动或任何 `.sys` 文件。更新器对包内程序和运行库整体暂存、校验和回滚。

兼容性 Release 同时附带 `MEMORY_AUDIT.md`，记录动态模块、句柄、GDI/USER 对象和运行压力检查的边界。

Release 资产名称必须包含产品名和架构，例如 `TrayS_1.7.2_x64.zip`、`TrayS_1.7.2_x86.zip`；不要恢复旧的 `_x64_ALL_...` 匿名命名。

本机现有仓库内 MSBuild 和 LLVM-MinGW，并具备完整的 MSVC/C++/CLI 构建工具。此前 x64/Win32 Release 已以 0 个警告、0 个错误构建。旧硬件上的 ACPI/UI 测试记录不能作为更换 CPU 后的温度实测依据。

当前 CPU 为 AMD Ryzen 9 9955HX，主板 TOPC YUNIK ITX WIFI D5；ACPI 没有可用温度实例。用户已单独授权并安装官方签名 PawnIO 2.2.0。用户在管理员 Windows PowerShell 运行 `diagnose-cpu-temperature.ps1 -RequireTemperature`，得到 5/5 有效 Tctl/Tdie（52.375–55.125°C）和两个 CCD 读数。普通权限打开设备返回 Win32 5。DLL 能加载但读数为 0 不属于硬件测试通过；使用 `test-lhm-wrapper.ps1 -RequireCpuTemperature -SampleCount 5` 和 native 集成测试进一步验证程序路径。

构建后至少运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
git diff --check
```

如果 VS/MSBuild 不可用，保持 C++/CLI Release 的限制说明，并使用 `build-portable-compat-exe.ps1` 验证原生 Win32 路径；不要为了测试而加载驱动。

## 未来机型适配清单

新增机型或传感器后，先收集 Windows build、CPU/GPU 型号、是否混合显卡、是否有虚拟显示适配器、ACPI PDH 计数器名称和值、Explorer 任务栏类名和 DPI 缩放。优先增加只读、可失败、逐设备隔离的探测；不要把一个厂商的失败变成全局退出条件。任何需要内核驱动、管理员权限、MSR/PCI 访问或改变显卡状态的方案，都必须单独评审，不能直接进入默认包。

发布前检查 Git 状态，确认没有把 `.buildtools`、`Bin` 中的中间文件、用户配置和实验包提交到仓库。代码推送到 `origin`（`https://github.com/Rhongomiant1227/TrayS.git`）后，再把不含旧驱动的版本化 ZIP 上传到 GitHub Release，并在 Release 说明中列出 SHA-256、LHM 已包含和 WinRing0/PawnIO 内核驱动未包含的事实。
