# TrayS 兼容性维护说明

这份 fork 面向 Windows 10/11 的 x86 与 x64 桌面系统，重点覆盖近年的 AMD Ryzen 平台。它记录的是代码层面的防护和已验证边界，不等同于在所有硬件上的实机认证。

## ARM / ARM64 边界

当前 solution 只提供 `Win32` 和 `x64` 配置，**没有原生 ARM64 或 ARM64EC 构建**。Windows on ARM64 可以尝试运行 x64 包，但那是 Windows 的 x64 模拟层，不代表 TrayS 已经原生适配 ARM；CPU 温度是否可见取决于设备固件是否暴露 ACPI 热区，AMD ADL/NVIDIA NVAPI 也未必存在于 ARM 设备上。不要把 x64/x86 包重命名成 ARM64 包，也不要为了补齐传感器而安装第三方内核驱动。

真正的 ARM64 支持需要新的 ARM64 构建、可用的传感器/厂商 DLL、C++/CLI 或 IPC 边界改造，以及在真实 ARM 设备上的睡眠唤醒、Explorer、DPI、更新回滚和安全软件回归；这些工作尚未完成，因此维护版会明确把原生 ARM64 标记为未支持。

程序启动前会通过 `RtlGetVersion` 检查系统版本；Windows 7/8/8.1 不在维护范围内，程序会显示提示并退出，不会创建共享内存、修改 Explorer、加载温度 DLL 或访问显卡接口。Windows 10（build 10240 及以上）和 Windows 11 才是当前支持目标。

设置窗口已移除旧论坛和旧作者信息，项目链接指向本 fork，另一个链接指向本文件。显示风格选项与实现保持一致：默认、透明渐变、DWM 模糊和亚克力；实际效果取决于 DWM/Explorer、系统主题和当前 Windows 版本。Windows 11 的任务栏图标布局可能由系统 Shell 接管，因此位置选项会按检测到的 Shell 结构禁用，而不是声称玻璃或亚克力完全不可用。

## 本次维护内容

- 硬件监控包装层升级到 `LibreHardwareMonitorLib` 0.9.4（`net472`/I386 IL 程序集，可供 Win32 与 x64 的 C++/CLI 包装层引用），并随附它运行时需要的 `HidSharp` 2.1.0 程序集。该版本保留了旧包装层使用的 `Computer`、`IComputer`、`IHardware` 和 `ISensor` API，并包含较新的 Ryzen/AMD 识别修复。
- `GetTemperature` 的所有输出先初始化为 `-1`；硬件初始化失败、传感器 `Nullable<float>` 没有值、硬盘索引超出范围时均返回缺省值，不再解引用空指针或越界迭代器。
- 托管硬件遍历按计算机、硬件集合和单个子硬件节点分层捕获异常；导出的 `GetTemperature` 也将更新、映射读取和数值转换置于同一 C ABI 异常边界。AMD 固件切换、设备热插拔或某个传感器提供者短暂故障时，其余硬件仍可继续刷新，失败字段显示为不可用。
- 温度读数仅接受有限的 `-50–255°C` 值；GPU 与硬盘负载另行限定为 `0–100%`，温度平均值和导出前数值均会再次检查。这样可阻止传感器返回的 NaN、无穷或异常哨兵值进入 TrayS 的整数显示字段。
- 禁止温度 DLL 失败后自动加载 WinRing0。TrayS 已移除自己的 WinRing0 模块句柄、驱动文件、旧 import library 和 AMD/Intel 固定 PCI/MSR 回退路径；NVIDIA/AMD 显卡厂商 API 仍作为可选补充路径。由于 LHM 0.9.4 自身仍内嵌 WinRing0，安全默认值是在所有 CPU 平台都不加载 LHM，因此不会因 TrayS 启动而安装或打开该内核驱动；只有用户明确设置 `TRAYS_ENABLE_LHM=1` 才会启用 LHM（旧的 `TRAYS_ENABLE_LHM_AMD=1` 仅作为 AMD 测试兼容别名），这表示用户自行承担驱动兼容性风险。
- 默认 CPU 温度路径改为 Windows/ACPI Thermal Zone 的 PDH 只读计数器：优先读取 `High Precision Temperature`，再兼容普通 `Temperature`，遍历可用热区并以最高的合理值作为平台/封装温度。该路径由 Windows 与固件提供，不读 MSR、不读 PCI 配置空间、不安装 WinRing0/PawnIO，也不调用 AMD/Intel 调频或电压接口；因此 AMD Ryzen、Intel Core/Xeon 以及没有厂商专用传感器的机器都可以安全尝试。某些固件不公开热区时会返回不可用，不会阻止 GPU、磁盘或其他监控继续运行。
- PDH 句柄、函数指针和计数器状态均经过检查；缺失的性能计数器会降级为不可用，不再向 `lodctr` 发起隐式系统修改。
- 任务栏图标位置改为一次性移动，取消逐像素 `SetWindowPos` 动画；查找 Explorer 任务栏改为有限重试，避免 Explorer 重启时永久阻塞或造成高 CPU。
- 行情请求使用 System32 中的 `winhttp.dll`，完整验证导出函数、设置 5 秒解析/连接/发送/接收超时，并把响应限制为 4096 字节。请求主机和行情标识符经白名单校验，解析在两个价格字段都成功后才提交，网络故障保留上一次有效价格。
- NVIDIA 的物理 GPU 句柄缓存按 `NVAPI_MAX_PHYSICAL_GPUS` 分配，匹配 `NvAPI_EnumPhysicalGPUs` 的最大写入量；温度结构和返回数量均经过检查，修复原先固定四个句柄可能造成的越界写入。
- 配置文件读取改为临时结构 + 完整长度/版本校验，刷新间隔限制在 100–5000 ms，默认值从 11 ms 调整为 400 ms；退出时先等待工作线程，超时才使用最后手段终止。
- Visual Studio solution 只保留实际支持的 x86/x64 配置，移除了把 ARM/ARM64/Any CPU 错误映射到 x64 的条目；两个项目的输出目录统一到 `Bin\\$(Platform)\\$(Configuration)`，并建立 TrayS 对监控 DLL 项目的构建依赖。

默认安全发布包只包含 `TrayS.exe` 和文档，因此不会把仍含旧 WinRing0 字符串的第三方 LHM 程序集交给安全软件扫描；TrayS 内置的 AMD/NVIDIA 用户态 GPU 温度接口仍可用。GPU 路径只调用 NVIDIA NVAPI 温度读取和 AMD ADL 温度读取，遍历同一厂商的全部物理适配器并取最高有效读数；不调用风扇、功耗、频率、电压或其他写入接口。混合 AMD+iGPU/NVIDIA 独显、多 NVIDIA 卡、虚拟显示适配器或某一厂商 DLL 缺失时，失败只影响对应适配器，其他路径继续工作。需要在隔离环境测试 LHM 时，才使用 `-IncludeLhm` 生成实验包，该包必须同时包含 `OpenHardwareMonitorApi.dll`、`LibreHardwareMonitorLib.dll` 和 `HidSharp.dll`；缺少后者时，监控 DLL 会在创建 `Computer` 时降级为无温度输出。静态检查脚本会验证这些程序集的版本和 PE 架构。

## 仍需明确的限制

`0.9.4` 是为了保持现有 net472 C++/CLI 包装层和 Win32 构建可用而选择的过渡版本。它本身仍包含旧版 WinRing0 后端；因此本次修改通过默认禁用 LHM，消除了 TrayS 正常启动时触发该后端的路径，但不能宣称 DLL 内部已经删除所有第三方驱动代码。默认 CPU 温度不依赖 LHM，而是尽力使用 Windows/ACPI 热区；只有在 ACPI 不可用、且用户明确设置 `TRAYS_ENABLE_LHM=1` 时才会启用 LHM（旧 AMD 别名仅用于兼容测试），并应在隔离环境观察安全软件、睡眠唤醒和重启行为。更长期的方向是迁移到 PawnIO 版本的 LHM（0.9.5 或更高版本），但 PawnIO 同样涉及内核驱动安装，必须单独完成签名、权限和实机回归后才能改变默认策略。

本机已使用仓库内固定的 VS 2022 Build Tools（MSBuild 17.14、MSVC v143）完成 `Release|x64` 与 `Release|Win32` 构建，静态检查和链接均通过，生成的安全包不包含 LHM/WinRing0/PawnIO/Ols。MSBuild 日志中曾出现 C++/CLI 尝试加载 `System.Core, Version=3.5.0.0` 的非致命提示，但最终构建为 0 个警告、0 个错误。尚未在本机启动维护版 TrayS 做真实硬件温度采样，因此 AMD、Intel、多显卡和不同 Windows build 的实机回归仍需在隔离环境完成；请使用管理员权限按下表回归：

| 平台/场景 | 需要确认的行为 |
| --- | --- |
| AMD Ryzen 5000/7000/8000 移动或桌面 | 默认不加载 LHM/WinRing0；先尝试 Windows/ACPI 平台温度，再读取 AMD ADL/NVIDIA NVAPI 显卡温度，失败时只显示不可用 |
| Intel 近代桌面/移动 | 默认不加载 LHM；先尝试 Windows/ACPI 平台温度，PDH、显卡厂商 API 和硬盘监控仍应正常，显式启用 LHM 后再单独做驱动回归 |
| 多 AMD/NVIDIA 适配器、混合显卡、虚拟显示适配器 | 只读枚举所有物理适配器，过滤不可用适配器并取最高有效 GPU 温度；不改变驱动状态，单个适配器失败不影响其他适配器 |
| Windows 10 22H2、Windows 11 23H2/24H2 | 任务栏重启、多显示器、DPI 缩放、全屏窗口切换后窗口可恢复，Explorer CPU 不持续升高 |
| 损坏/旧版 `TrayS.dat` | 程序使用默认配置启动，不因短文件或非法盘符崩溃 |

## 可重复的静态检查

在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate-compatibility.ps1
```

脚本会解析两个 `.vcxproj`、检查 solution 配置、验证监控程序集的 PE 架构/版本，并反射检查包装层依赖的托管 API。它不能替代 VS 编译或真实硬件回归。

## 构建和打包

在安装了 VS 2022 C++/CLI 工具链的 Windows 机器上，可以从仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1
```

默认会构建 `Release|x64`，并在仓库内生成 `dist\TrayS_1.5.0_x64\` 与对应的 zip。x86 构建使用 `-Platform Win32 -PackageName TrayS_1.5.0_x86`。安全包只复制新构建的 `TrayS.exe` 和兼容性说明；旧的 WinRing0 文件、旧 import library、LHM 程序集和个人配置不会自动进入包。若要在隔离环境测试 LHM，显式增加 `-IncludeLhm`；若确实需要迁移配置，可显式提供 `-ConfigSourceDirectory`，脚本只会读取其中的 `TrayS.dat` 与 `TrayS.xml`。

如果本机没有 MSBuild，可以把 VS 2022 Build Tools 安装到仓库内的 `.buildtools` 目录。构建完成后，先运行 `tools\uninstall-build-tools.ps1`，让官方 Visual Studio Installer 完成卸载并清理该目录，再删除整个仓库目录；直接删除 `.buildtools` 会留下安装器注册信息和缓存，不应作为卸载步骤。

项目内的 `tools\install-build-tools.ps1` 会通过微软官方 `winget` 源安装所需的 C++、MSVC v143、Windows SDK 与 C++/CLI 组件，并使用 `--nocache` 减少安装包缓存。该脚本必须在管理员 PowerShell 中运行。
