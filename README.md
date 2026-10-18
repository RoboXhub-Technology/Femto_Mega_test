# Orbbec Femto Mega 验收测试工具

本目录与《Orbbec Femto Mega 3D 深度相机验收记录表》一一对应，用 Orbbec SDK v2 采集设备实际值并保存 CSV/JSON 原始证据。主程序不依赖 OpenCV，适用 Windows 10/11 和 Linux。

## 能够测量的验收项

| 验收项 | 命令 | 主要输出 |
|---|---|---|
| 深度 FOV | `survey` | 每个 Depth profile 的 fx/fy/cx/cy、HFOV、VFOV |
| 深度分辨率@帧率 | `survey` + `stream` | 合同配置能力矩阵、60 s 实测 FPS、超时与帧序号缺口 |
| 工作距离 | `stream --sensor depth` | 中心 ROI 有效比例和每帧深度中位数 |
| 1 m 相对精度 | `stream --reference-mm 1000 --frames 300` | Dmean、相对误差、是否不大于 0.15% |
| RGB 3840 x 2160 @ 30 fps | `survey` + `stream --sensor color` | profile 支持状态、实测 FPS、原始首帧 |
| RGB FOV | `survey` | 3840 x 2160 profile 内参与 HFOV/VFOV |
| RGB HDR | `hdr` | `OB_PROP_COLOR_HDR_BOOL` 读写支持、开关前后值 |
| 最大功耗 | `load` + `tools/analyze_power_csv.py` | 10 min 最高载取流记录、功率计 CSV 的 Pavg/Pmax |
| Orbbec SDK | `survey` + `stream` | SDK/固件/序列号/连接方式和取流结果 |
| K4A Wrapper | `k4a_wrapper_smoke` | 设备枚举、固件和 4K RGB + NFOV Depth 同步取流 |
| IMU | `imu` | 加速度/角速度原始 CSV、实测 Hz、帧序号缺口 |

注：功耗值必须由外部校准功率计得到。SDK 只能创建稳定的高负载场景，不能替代 DC/USB-C/PoE 功率计。

## 依赖

- CMake 3.15+
- 支持 C++17 的编译器（Windows 建议 Visual Studio 2019/2022 x64）
- Orbbec SDK v2 预编译包。Femto Mega 建议固件版本以 SDK 当前支持列表为准，在验收证据中记录实际 SDK 和固件版本。
- 可选：OrbbecSDK-K4A-Wrapper，用于编译 K4A smoke test。

官方依据：

- [OrbbecSDK v2 开源仓库](https://github.com/orbbec/OrbbecSDK_v2)
- [OrbbecSDK v2 安装与开发指南](https://github.com/orbbec/OrbbecSDK_v2/blob/main/docs/tutorial/installation_and_development_guide.md)
- [OrbbecSDK v2 官方示例](https://github.com/orbbec/OrbbecSDK_v2/tree/main/examples)
- [OrbbecSDK-K4A-Wrapper](https://github.com/orbbec/OrbbecSDK-K4A-Wrapper/tree/v2-main)

## Windows 编译

1. 下载并解压 Orbbec SDK v2 预编译包。
2. 在 PowerShell 运行：

```powershell
.\scripts\build_windows.ps1 -OrbbecSdkRoot "D:\SDK\OrbbecSDK_v2"
```

脚本会在 `build\Release` 或当前生成器的对应目录生成 `femto_mega_acceptance.exe`。SDK 包内的 `OrbbecSDK.dll` 及 `extensions` 目录必须按 SDK 官方指南放到可执行文件可访问位置。

如需 K4A：

```powershell
.\scripts\build_windows.ps1 `
  -OrbbecSdkRoot "D:\SDK\OrbbecSDK_v2" `
  -BuildK4A `
  -K4AWrapperRoot "D:\SDK\OrbbecSDK-K4A-Wrapper"
```

## 建议验收顺序

### 1 预热和基础调查

相机稳定供电并预热不少于 10 min，关闭 D2C、点云和非必要后处理，然后：

```powershell
.\femto_mega_acceptance.exe survey --out evidence\00-survey
```

先检查：

- `survey_summary.json`：型号、序列号、固件、SDK 版本、HDR/IMU 能力摘要。
- `contract_profile_matrix.csv`：验收表要求的 14 个深度组合及 4K30 RGB 是否在 SDK 列表中。
- `video_profiles.csv`：实际 profile index、格式、内参和 FOV。
- `imu_profiles.csv`：可用采样率和量程。

### 2 自动跑合同流配置

```powershell
.\scripts\run_acceptance_plan.ps1 `
  -ExePath .\femto_mega_acceptance.exe `
  -EvidenceRoot .\evidence `
  -SecondsPerMode 60
```

脚本会按验收表中的所有深度分辨率/FPS、4K30 RGB 和 IMU 顺序取流。某个 profile 未提供时，该命令失败即是应当保留的验收结果，不应用相近配置替代。

### 3 1 m 精度

将平面漫反射板从相机深度参考面精确放置在 1000 mm，再运行：

```powershell
.\femto_mega_acceptance.exe stream --sensor depth --width 640 --height 576 --fps 30 `
  --frames 300 --seconds 0 --roi 100 --reference-mm 1000 --valid-ratio 0.95 `
  --out evidence\04-accuracy-1m
```

`stream_summary.json` 中会给出 Dmean、相对误差和 `meets_0_15_percent_limit`。

### 4 工作距离

每次将测试板放到经校准的距离，运行 `scripts/measure_depth_point.ps1`。示例：

```powershell
.\scripts\measure_depth_point.ps1 -ExePath .\femto_mega_acceptance.exe `
  -ModeName "WFOV-unbinned" -Width 1024 -Height 1024 -Fps 15 `
  -DistanceMm 250 -EvidenceRoot .\evidence
```

以 ROI 非零比例不小于 95%，且 100 帧中不少于 95 帧达到该比例作为建议判据。

### 5 RGB HDR

```powershell
.\femto_mega_acceptance.exe hdr --set query --out evidence\07-hdr-query
.\femto_mega_acceptance.exe hdr --set on    --out evidence\07-hdr-on
.\femto_mega_acceptance.exe hdr --set off   --out evidence\07-hdr-off
```

`on/off` 是显式写设备属性操作；若只需读能力，只运行 `query`。HDR 画质证据仍应在同一高反差场景中分别保存开/关原图。

### 6 功耗

先启动 10 min 高负载：

```powershell
.\femto_mega_acceptance.exe load --seconds 600 --out evidence\08-power-load
```

同时用外部校准功率计导出 CSV，然后：

```powershell
python .\tools\analyze_power_csv.py meter.csv --out evidence\08-power-analysis.json
```

如仪表没有功率列，但有电压和电流列，脚本会计算 `P=V*I`。表头不是默认名称时，使用 `--power-column`、`--voltage-column`、`--current-column` 指定。

## 证据可追溯性

- 每次命令使用独立 `--out` 目录，名称建议为“序号-项目-模式-日期”。
- 保留 `frames.csv`/`imu_frames.csv`、JSON 摘要和首帧原始数据，不要只保留截图。
- 建议同时保存 OrbbecViewer 设备信息页和仪器校准证书。
- 程序估算的“掉帧”来自帧序号缺口，应结合 SDK 日志和主机负载复核。

详细映射见 [验收项目与代码映射](docs/验收项目与代码映射.md)。
