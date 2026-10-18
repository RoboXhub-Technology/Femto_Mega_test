# Femto Mega — OrbbecSDK-K4A-Wrapper 兼容性复测

本目录是独立的 C++17 测试工程，只验证 Femto Mega 对 `OrbbecSDK-K4A-Wrapper` 传感器接口的支持情况，不链接 `k4abt`，也不测试人体或骨架识别。

## 目录说明

- `src/main.cpp`：枚举、标定、控制项、RGB/深度同步与单流、IMU 测试。
- `run_test.ps1`：配置、编译并运行 quick/full 测试；新结果默认写入 `results/current`。
- `compare_results.ps1`：把另一台相机的 full JSON 与本机基准逐项比较。
- `results/reference_CL3N241005X_2026-10-18`：2026-10-18 的原始基准 JSON，不会被运行脚本覆盖。
- `SHA256SUMS.txt`：源码、脚本和基准结果的 SHA-256 清单。
- `build`：本机生成目录，已忽略，不属于可移植证据。

## 本机实际测试记录

参考测试于 2026-10-18 在 Windows 上完成，环境如下：

| 项目 | 实际值 |
|---|---|
| 相机 | Orbbec Femto Mega |
| 序列号 | `CL3N241005X` |
| 固件 | `1.3.0` |
| CMake | `4.4.4` |
| 编译器 | Visual Studio 2022 Build Tools x64，MSBuild `17.14.60` |
| Wrapper | 预编译 `OrbbecSDK-K4A-Wrapper v2.0.12` |
| Wrapper 配套运行库 | `OrbbecSDK.dll 2.5.5` |
| 原测试目录 | `D:\Femto_mega\k4a_wrapper_compat_test` |
| Wrapper 根目录 | `D:\Femto_mega\OrbbecSDK-K4A-Wrapperp` |

本机当时实际执行的是：

```powershell
cd D:\Femto_mega\k4a_wrapper_compat_test
cmake --preset vs2022-x64
cmake --build --preset release
powershell -ExecutionPolicy Bypass -File .\run_test.ps1 -Full -Frames 5
```

已完成结果：设备枚举/打开、序列号与固件读取、4 种深度模式标定及 transformation 创建、RGB 控制项查询、RGB+Depth 同步流、14 个要求的深度分辨率/帧率组合、4K RGB 5/15/25 fps、IMU，均为 PASS。每个流配置采集 5/5 有效帧且时间戳单调。

注意：本机 Wrapper 测试没有宣称 4K@30 fps 或 RGB HDR 通过。4K 仅测试并通过 5/15/25 fps；HDR 被明确跳过，因为该 Wrapper 版本只为 Femto Bolt 映射 HDR 属性。更直接的 pyorbbecsdk 调查同样显示本机设备没有列出 4K@30，且 `OB_PROP_COLOR_HDR_BOOL` 不可读、不可写。

## 在另一台电脑上复现

先安装 CMake 3.20+ 和 Visual Studio 2022 Build Tools，并勾选“使用 C++ 的桌面开发”。关闭 OrbbecViewer、K4A Viewer 和其他正在占用相机的程序，然后取得与测试匹配的预编译 OrbbecSDK-K4A-Wrapper 包。

### 必须修改或指定的地方

通常不需要修改 `src/main.cpp`。程序不会硬编码序列号、固件号或设备结果。复现者只需提供自己的 Wrapper 路径，并在多相机场景选择设备索引：

```powershell
cd D:\Master\Orbbec_Femto_Mega_test_code\k4a_wrapper_compat_test
powershell -ExecutionPolicy Bypass -File .\run_test.ps1 `
  -Full -Frames 5 -Device 0 `
  -K4AWrapperRoot "D:\SDK\OrbbecSDK-K4A-Wrapper"
```

需要按实际电脑修改的只有：

1. `-K4AWrapperRoot`：改成预编译 Wrapper 包根目录；该目录必须含 `include\k4a\k4a.h`、`lib\k4a.lib` 和 `bin\k4a.dll`。
2. `-Device`：只有接入多台设备时才修改。先从 `0` 开始，范围为 0–31。
3. `-Frames`：用于控制每个配置至少取得多少帧；与基准严格对比时保持 `5`。
4. 可选 `-Output`：指定候选 JSON，例如 `-Output results\current\camera_B_full.json`。

也可设置环境变量后运行：

```powershell
$env:K4A_WRAPPER_ROOT = 'D:\SDK\OrbbecSDK-K4A-Wrapper'
powershell -ExecutionPolicy Bypass -File .\run_test.ps1 -Full -Frames 5 -Device 0
```

或直接传给 CMake：

```powershell
cmake --preset vs2022-x64 -DK4A_WRAPPER_ROOT="D:\SDK\OrbbecSDK-K4A-Wrapper"
cmake --build --preset release
```

如果使用 VS Code，在“终端 → 运行任务”选择 `K4A Wrapper: Full Test`，并在提示框中输入自己的 Wrapper 根目录。默认值是本机参考路径，别的电脑必须改掉。

不要把另行安装的 `C:\Program Files\OrbbecSDK ...\OrbbecSDK.dll` 覆盖到测试程序旁边。Wrapper 发布包中的 `k4a.dll`、`OrbbecSDK.dll` 和 `extensions` 是配套组合，混用版本会让结果失去可比性。程序会在 JSON 中记录实际加载 DLL 的完整路径。

## 与本机结果比较

复测完成后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\compare_results.ps1 `
  -Candidate .\results\current\k4a_wrapper_full.json `
  -Output .\results\current\comparison.json
```

脚本以测试名称为键，对所有 `required: true` 项比较 PASS/FAIL，并同时显示参考与候选的序列号、固件和 overall 状态。另一台相机的序列号应当不同，这正是独立复测的正常证据；要求相同的是测试集合、分辨率/帧率配置和必测项状态。

用下列命令验证本目录中的基准文件没有在整合后被改写：

```powershell
Get-Content .\SHA256SUMS.txt
Get-FileHash .\results\reference_CL3N241005X_2026-10-18\*.json -Algorithm SHA256
```

SHA-256 可以证明文件相对于清单是否发生变化；“另一台相机重新运行并得到可对照的原始 JSON”提供可复现性证据。两者都不等同于带可信时间戳的数字签名，因此不能单独提供密码学意义上的作者身份或采集时间证明。需要更强审计链时，应同时保存相机序列号照片、运行屏幕录像、系统时间和第三方签名/时间戳。

## 退出码和范围

- `0`：所有必测项通过。
- `1`：至少一个必测项失败。
- `2`：未发现设备或无法打开设备。
- `3`：结果文件无法写入。
- `64`：命令行参数错误。

本工程不测试录制回放、麦克风、多机外部同步、网络带宽压力或人体骨架识别。JSON 中的标定 FOV 是由内参按针孔模型计算的近似值；对广角畸变镜头不能替代厂家标称值或物理角度标定。
