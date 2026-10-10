# TrayS 兼容性维护说明

这份 fork 面向 Windows 10/11 的 x86 与 x64 桌面系统，重点覆盖近年的 AMD Ryzen 平台。它记录的是代码层面的防护和已验证边界，不等同于在所有硬件上的实机认证。

## ARM / ARM64 边界

当前 solution 只提供 `Win32` 和 `x64` 配置，**没有原生 ARM64 或 ARM64EC 构建**。Windows on ARM64 可以尝试运行 x64 包，但那是 Windows 的 x64 模拟层，不代表 TrayS 已经原生适配 ARM；CPU 温度是否可见取决于设备固件是否暴露 ACPI 热区，AMD ADL/NVIDIA NVAPI 也未必存在于 ARM 设备上。不要把 x64/x86 包重命名成 ARM64 包，也不要为了补齐传感器而安装第三方内核驱动。

真正的 ARM64 支持需要新的 ARM64 构建、可用的传感器/厂商 DLL、C++/CLI 或 IPC 边界改造，以及在真实 ARM 设备上的睡眠唤醒、Explorer、DPI、更新回滚和安全软件回归；这些工作尚未完成，因此维护版会明确把原生 ARM64 标记为未支持。

程序启动前会通过 `RtlGetVersion` 检查系统版本；Windows 7/8/8.1 不在维护范围内，程序会显示提示并退出，不会创建共享内存、修改 Explorer、加载温度 DLL 或访问显卡接口。Windows 10（build 10240 及以上）和 Windows 11 才是当前支持目标。

设置窗口已移除旧论坛和旧作者信息，项目链接指向本 fork，另一个链接指向本文件。显示风格选项与实现保持一致：默认、透明渐变、DWM 模糊和亚克力；实际效果取决于 DWM/Explorer、系统主题和当前 Windows 版本。Windows 11 的任务栏图标布局可能由系统 Shell 接管，因此位置选项会按检测到的 Shell 结构禁用，而不是声称玻璃或亚克力完全不可用。

## 本次维护内容

- 硬件监控包装层升级到 `LibreHardwareMonitorLib` 0.9.6（`net472`，分别使用 x86 与 x64 程序集），并包含运行依赖。该版本识别 Zen 5，支持 Ryzen 9 9955HX 的 CPU 温度传感器。
- `GetTemperature` 的所有输出先初始化为 `-1`；硬件初始化失败、传感器 `Nullable<float>` 没有值、硬盘索引超出范围时均返回缺省值，不再解引用空指针或越界迭代器。
- 托管硬件遍历按计算机、硬件集合和单个子硬件节点分层捕获异常；导出的 `GetTemperature` 也将更新、映射读取和数值转换置于同一 C ABI 异常边界。AMD 固件切换、设备热插拔或某个传感器提供者短暂故障时，其余硬件仍可继续刷新，失败字段显示为不可用。
- 温度读数仅接受有限的 `-50–255°C` 值；GPU 与硬盘负载另行限定为 `0–100%`，温度平均值和导出前数值均会再次检查。这样可阻止传感器返回的 NaN、无穷或异常哨兵值进入 TrayS 的整数显示字段。
- TrayS 已移除自己的 WinRing0 模块句柄、驱动文件、旧 import library 和 AMD/Intel 固定 PCI/MSR 回退路径；NVIDIA/AMD 显卡厂商 API 仍作为独立的只读温度路径。LHM 0.9.6 使用 PawnIO 后端，TrayS 只在 PawnIO 设备已经安装且可访问时加载它。TrayS 不安装、不启动或捆绑 PawnIO 驱动，也不会回退到 WinRing0。
- CPU 温度优先使用已经可访问的 LHM 硬件传感器：AMD `Core (Tctl/Tdie)` / `Core (Tdie)` 优先作为 CPU 封装读数，传感器缺失时再平均有效 CPU 温度值。LHM CPU 组会在 `Computer::Open()` 前显式启用。硬件读数不可用时回退 Windows/ACPI Thermal Zone 的只读 PDH 计数器（优先 `High Precision Temperature`，再兼容普通 `Temperature`）。ACPI 与 PawnIO 都不可用时显示不可用；设置窗口区分 PawnIO 缺失和访问权限不足。
- PDH 句柄、函数指针和计数器状态均经过检查；缺失的性能计数器会降级为不可用，不再向 `lodctr` 发起隐式系统修改。
- 任务栏图标位置改为一次性移动，取消逐像素 `SetWindowPos` 动画；查找 Explorer 任务栏改为有限重试，避免 Explorer 重启时永久阻塞或造成高 CPU。
- Explorer 的任务栏子窗口查找使用有界的 `EnumChildWindows` 类名枚举，兼容 Windows 11 的 XAML/Composition 中间层；当 `FindWindowEx` 无法返回实际任务列表句柄时，监控窗口仍能定位和刷新。
- 行情请求使用 System32 中的 `winhttp.dll`，完整验证导出函数、设置 5 秒解析/连接/发送/接收超时，并把响应限制为 4096 字节。请求主机和行情标识符经白名单校验，解析在两个价格字段都成功后才提交，网络故障保留上一次有效价格。
- NVIDIA 的物理 GPU 句柄缓存按 `NVAPI_MAX_PHYSICAL_GPUS` 分配，匹配 `NvAPI_EnumPhysicalGPUs` 的最大写入量；温度结构和返回数量均经过检查，修复原先固定四个句柄可能造成的越界写入。
- 配置文件读取改为临时结构 + 完整长度/版本校验，刷新间隔限制在 100–5000 ms，默认值从 11 ms 调整为 400 ms；退出时先等待工作线程，超时才使用最后手段终止。
- Visual Studio solution 只保留实际支持的 x86/x64 配置，移除了把 ARM/ARM64/Any CPU 错误映射到 x64 的条目；两个项目的输出目录统一到 `Bin\\$(Platform)\\$(Configuration)`，并建立 TrayS 对监控 DLL 项目的构建依赖。

标准发布包包含 LHM 0.9.6 x86/x64 对应程序集、必要依赖、第三方许可和源代码链接；不包含任何 `.sys` 驱动。LHM 只有在 PawnIO 设备已经可访问时才会加载。AMD/NVIDIA 用户态 GPU 温度接口仍可独立工作：它们只调用 NVAPI/ADL 温度读取，逐卡枚举并取最高有效值，不调用风扇、功耗、频率、电压或其他写入接口。混合 AMD+iGPU/NVIDIA 独显、多 NVIDIA 卡、虚拟显示适配器或某一厂商 DLL 缺失时，失败只影响对应适配器。静态检查会验证 LHM x86/x64 版本、PE 架构、PawnIO 门控和发布依赖清单。

## 仍需明确的限制

Ryzen 9 9955HX 的 LHM 传感器路径依赖单独安装的 PawnIO 软件和已经可访问的 `\\?\GLOBALROOT\Device\PawnIO` 设备。官方 PawnIO 2.2.0 的设备访问控制只允许 SYSTEM 和提升权限的管理员，因此“已安装驱动”不等同于“普通启动可读”。发布包不会捆绑或安装该驱动，也不会修改它的访问控制；普通权限下继续尝试 ACPI 温度路径。

1.7.1 曾在本工作目录使用 VS 2022 Build Tools / MSVC v143 完成 x64 与 Win32 Release 构建，并运行静态兼容性校验和更新器多文件安装/回滚模拟。该测试不安装、不启动 PawnIO，也不加载 WinRing0。

本机 AMD Ryzen 9 9955HX / TOPC YUNIK ITX WIFI D5 没有可用的 ACPI 温度实例。用户安装官方 PawnIO 2.2.0 后，在管理员 Windows PowerShell 中运行 LHM 0.9.6 诊断，连续 5 次读到有效的 Tctl/Tdie（52.375–55.125°C）、CCD1 和 CCD2 温度。普通权限打开同一设备返回 Win32 5。此结果确认本机的 LHM 传感器和权限边界，不代表其他机型已经认证。AMD、Intel、多显卡和不同 Windows build 的完整实机回归仍需逐项完成：

| 平台/场景 | 需要确认的行为 |
| --- | --- |
| AMD Ryzen 5000/7000/8000 移动或桌面 | PawnIO 已安装且管理员启动时验证 LHM 封装读数；普通启动验证 ACPI 回退和权限提示，失败时只显示不可用 |
| Intel 近代桌面/移动 | 验证 LHM CPU Package、ACPI 回退、睡眠唤醒以及独立的显卡厂商 API；不恢复 WinRing0 |
| 多 AMD/NVIDIA 适配器、混合显卡、虚拟显示适配器 | 只读枚举所有物理适配器，过滤不可用适配器并取最高有效 GPU 温度；不改变驱动状态，单个适配器失败不影响其他适配器 |
| Windows 10 22H2、Windows 11 23H2/24H2 | 任务栏重启、多显示器、DPI 缩放、全屏窗口切换后窗口可恢复，Explorer CPU 不持续升高 |
| 损坏/旧版 `TrayS.dat` | 程序使用默认配置启动，不因短文件或非法盘符崩溃 |

## CPU 温度设置

1. 安装 [官方签名的 PawnIO](https://github.com/namazso/PawnIO.Setup/releases)。TrayS 不会代为安装或捆绑驱动。
2. 通过设置窗口的“退出”按钮关闭正在运行的 TrayS。
3. 右键 `TrayS.exe`，选择“以管理员身份运行”，接受 Windows UAC 提示。
4. 在设置中勾选“系统监视”和“显示温度”。9955HX 显示 CPU 的 Tctl/Tdie；不应把 CCD 温度的平均值当成整个 CPU 的封装温度。

默认开机启动使用普通权限，因此下次登录后需要手动按上述方式启动，才能访问 PawnIO 硬件传感器。TrayS 保持 `asInvoker`，不自动提权或创建提升权限的计划任务。设置提示“请以管理员身份运行”时，重装 PawnIO 没有帮助；提示需要安装 PawnIO 时，再检查驱动是否存在。

开发环境中可使用只读脚本区分驱动、LHM 和程序加载问题。以下命令要求管理员 Windows PowerShell，且已构建 x64 Release：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\diagnose-cpu-temperature.ps1 -Platform x64 -SampleCount 5 -RequireTemperature
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\test-lhm-wrapper.ps1 -Platform x64 -SampleCount 5 -RequireCpuTemperature
```

`test-lhm-wrapper.ps1` 不带 `-RequireCpuTemperature` 时仅检查 DLL 加载，不能据此声称已读到真实 CPU 温度。

## 可重复的静态检查

在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
```

脚本会解析两个 `.vcxproj`、检查 solution 配置、验证监控程序集的 PE 架构/版本，并反射检查包装层依赖的托管 API。它不能替代 VS 编译或真实硬件回归。

没有 MSBuild 时，可运行项目内的原生兼容性构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-portable-compat-exe.ps1 -Force
```

它只使用 `.buildtools\llvm-mingw`，把中间文件写入 `.build-native\`，输出 `dist\TrayS-compat-win11-x64\TrayS-compat-win11-x64.exe` 及其构建说明。删除整个仓库会同时删除这套工具和中间文件；该构建不包含 LibreHardwareMonitor 或任何内核驱动。

## 只读环境诊断

如果程序在某台机器上没有出现在任务栏或温度栏为空，可在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\diagnose-compatibility.ps1
```

该脚本只读取 Windows/Explorer 的版本和窗口类、ACPI Thermal Zone 计数器、已安装的厂商 DLL、仓库监控程序集和 MSBuild 位置，不启动 TrayS，不加载 LHM/WinRing0/PawnIO，也不修改系统。若 `EnumChildWindows` 能找到 `MSTaskSwWClass`/`MSTaskListWClass`，而 `FindWindowEx direct` 对应项为空，说明 Explorer 的实际层级超出了旧的直接子窗口假设；维护版会使用后代枚举路径。若高精度热区计数器有有效值，CPU 温度路径可用；`TRAYSAVE` 的 `bMonitorTemperature` 默认仍为关闭，需在设置中启用后才会采样。Intel 集成显卡或虚拟显示适配器没有 ADL/NVAPI 时，GPU 温度属于不可用能力，不应把 `0` 当成真实温度。

资源生命周期审计和运行压力采样记录在 [MEMORY_AUDIT.md](MEMORY_AUDIT.md)，其中包含动态模块、线程、句柄、GDI/USER 对象和网络快照的清理边界。

## 构建和打包

在安装了 VS 2022 C++/CLI 工具链的 Windows 机器上，可以从仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1
```

默认会构建 `Release|x64`，并在仓库内生成 `dist\TrayS_1.7.2_x64\` 与对应的 zip。x86 构建使用 `-Platform Win32 -PackageName TrayS_1.7.2_x86`。标准包复制新构建的 `TrayS.exe`、架构对应的 LHM 程序集、运行依赖和许可文档；不会复制旧 WinRing0 文件、旧 import library、PawnIO 驱动或个人配置。若确实需要迁移配置，可显式提供 `-ConfigSourceDirectory`，脚本只会读取其中的 `TrayS.dat` 与 `TrayS.xml`。

如果本机没有 MSBuild，可以把 VS 2022 Build Tools 安装到仓库内的 `.buildtools` 目录。构建完成后，先运行 `tools\uninstall-build-tools.ps1`，让官方 Visual Studio Installer 完成卸载并清理该目录，再删除整个仓库目录；直接删除 `.buildtools` 会留下安装器注册信息和缓存，不应作为卸载步骤。

项目内的 `tools\install-build-tools.ps1` 会通过微软官方 `winget` 源安装所需的 C++、MSVC v143、Windows SDK 与 C++/CLI 组件，并使用 `--nocache` 减少安装包缓存。该脚本必须在管理员 PowerShell 中运行。
