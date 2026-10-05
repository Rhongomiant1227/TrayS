# TrayS 资源与内存审计

本审计针对 `xiaoxin-duet` 兼容性增强版本，覆盖原生 Win32 任务栏窗口、监控数据线程、温度模块、网络快照、行情线程和更新模块。审计的目标是确认资源在重复刷新、动态模块切换和正常退出时有明确的所有权与释放路径。

## 静态检查

- `clang-tidy` 对 `TrayS/TrayS.cpp`、`TrayS/Function.cpp` 和 `TrayS/Update.cpp` 运行了 `NewDeleteLeaks`、`unix.Malloc` 和 `core.NullDereference` 检查，没有报告。
- 以 `TRAYS_ENABLE_LEGACY_SERVICE=1` 再次检查 `Function.cpp`，同一组分析器没有报告；该路径的原有编译警告仍然来自旧的 Windows API typedef 和历史注释，不是本次新增资源问题。
- `validate-compatibility.ps1`、`git diff --check` 和便携式 x64 构建均通过。
- 生成的 EXE 为 `IMAGE_FILE_MACHINE_AMD64`、Windows GUI 子系统，并且没有导入 LLVM-MinGW 的运行库 DLL。

## 已修正的生命周期问题

- 行情线程停止后，调用新的 `UnloadWinHttp()`，在清除函数表后主动卸载动态加载的 `winhttp.dll`。调用发生在线程句柄等待成功之后，避免其他线程仍在执行函数指针时卸载模块。
- NVAPI 初始化现在记录初始化状态，并在释放温度模块前成对调用 `NvAPI_Unload`；重复开启/关闭温度采样不会把 NVAPI 的引用计数留在进程中。
- 默认关闭的旧服务路径现在关闭进程快照、用户令牌、复制令牌、进程令牌、`CreateProcessAsUser` 返回的进程/线程句柄，并通过 `DestroyEnvironmentBlock` 释放环境块。服务事件在 `ServiceMain` 返回前关闭。
- 旧服务的 `EmptyProcessMemory` 路径在打开外部进程时关闭句柄。
- 进程退出时映射视图、文件映射句柄和桌面 DC 都先判断有效性再释放；网络快照继续在 `iphlpapi.dll` 仍加载且持有网络锁时释放 `GetIfTable2` 的结果。

## 运行压力检查

在本机 Windows 11 build 22631 上，从仓库内直接启动：

```text
dist/TrayS-compat-win11-x64/TrayS-compat-win11-x64.exe
```

程序持续运行约 20 分钟，任务栏 ARGB/`WM_PAINT` 日志持续更新，日志中的绘制计数达到 4,440。读取 TrayS 进程的 `PrivatePageCount`、工作集、句柄数、线程数，以及 `GetGuiResources` 返回的 GDI/USER 对象数，得到以下范围：

| 指标 | 观测范围 | 结论 |
| --- | ---: | --- |
| Private Bytes | 约 3.0–3.1 MB | 在稳定范围内上下波动，没有单调增长 |
| Working Set | 约 3.5 MB | 在稳定范围内上下波动 |
| Handles | 297 | 稳定 |
| Threads | 6–8 | 稳定 |
| GDI objects | 16 | 稳定 |
| USER objects | 22 | 稳定 |

另一次连续采样中，句柄数保持 297，GDI 保持 16，USER 保持 22；私有字节只在 3,052–3,124 KB 之间变化。关闭测试进程后，`_TrayS_` 互斥体被释放，没有残留 TrayS 进程。

## 审计边界

这些结果证明默认便携式 Win32 路径在当前 Windows 版本和当前硬件上的长时间刷新没有发现持续增长，但不能替代所有硬件和驱动组合的认证。当前机器没有可用的 AMD ADL 或 NVIDIA NVAPI 温度 DLL，因此厂商 GPU 温度初始化/卸载路径只能通过静态分析和无模块降级路径验证。带 C++/CLI 的 MSVC Release 需要在安装 Visual Studio Build Tools 的环境中重新构建；本目录的 LLVM-MinGW 构建用于原生兼容性验证。

没有使用驱动安装、WinRing0、Ols、PawnIO 或 Windows Driver Verifier。若在其他电脑上启用可选硬件监控，建议按相同指标观察一段时间，并在启用/关闭温度监控、重启 Explorer、反复打开设置窗口后确认资源数回到稳定区间。
