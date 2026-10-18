# Femto Mega Python 验收测试

这套实现使用官方 `pyorbbecsdk` 的 `v2-main` API，与目录中的 C++ 版本相互独立。支持 USB 枚举，也支持 Femto Mega 通过 IP/8090 连接。

## 安装

Windows x64 建议使用 Python 3.10–3.12：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r .\python\requirements.txt
```

官方 PyPI 包名称是 `pyorbbecsdk2`，Python 导入名仍然是 `pyorbbecsdk`。当前官方 `v2-main` 的 Python 包版本是 `2.1.2`；文档中出现的 `2.7.6` 是其内置的底层 Orbbec SDK 版本，不是可供 `pip` 安装的 Python 包版本。
首次在Windows使用时，按官方说明完成一次系统环境设置：

```powershell
$SdkPackage = python -c "import pyorbbecsdk, os; print(os.path.dirname(pyorbbecsdk.__file__))"
python "$SdkPackage\shared\setup_env.py"
```

## 快速检查

USB：

```powershell
python .\python\femto_mega_acceptance.py survey --out .\evidence-python\00-survey
```

网络连接：

```powershell
python .\python\femto_mega_acceptance.py survey --ip 192.168.1.10 --port 8090 `
  --out .\evidence-python\00-survey
```

完整跑完验收表中的 14 个深度 Profile、4K30、IMU 和 HDR 查询：

```powershell
.\scripts\run_acceptance_plan_python.ps1 -EvidenceRoot .\evidence-python
```

网络相机使用：

```powershell
.\scripts\run_acceptance_plan_python.ps1 -IpAddress 192.168.1.10 -Port 8090
```

## 1 m 相对精度

把平面漫反射板从深度参考面准确放置在 1000 mm：

```powershell
python .\python\femto_mega_acceptance.py stream --sensor depth `
  --width 640 --height 576 --fps 30 --seconds 0 --frames 300 `
  --roi 100 --valid-ratio 0.95 --reference-mm 1000 `
  --out .\evidence-python\04-accuracy-1m
```

结果位于 `stream_summary.json`，包括每帧中心 ROI 中位数的均值、相对误差及 `meets_0_15_percent_limit`。

## 工作距离

在每个规定距离放置经校准的平面板，执行 100 帧测试。例如 WFOV unbinned 的 250 mm 近端：

```powershell
python .\python\femto_mega_acceptance.py stream --sensor depth `
  --width 1024 --height 1024 --fps 15 --seconds 0 --frames 100 `
  --roi 100 --valid-ratio 0.95 --reference-mm 250 `
  --out .\evidence-python\03-range-WFOV-unbinned-250mm
```

`meets_95_of_100_stability_rule` 表示满足中心 ROI 非零像素比例不小于 95% 的帧是否达到 95%。

## HDR与功耗

HDR只查询能力，不写设置：

```powershell
python .\python\femto_mega_acceptance.py hdr --set query --out .\evidence-python\07-hdr-query
```

显式开关HDR：

```powershell
python .\python\femto_mega_acceptance.py hdr --set on  --out .\evidence-python\07-hdr-on
python .\python\femto_mega_acceptance.py hdr --set off --out .\evidence-python\07-hdr-off
```

功耗测试先制造10分钟深度、彩色和IMU负载：

```powershell
python .\python\femto_mega_acceptance.py load --seconds 600 --out .\evidence-python\08-power-load
python .\tools\analyze_power_csv.py meter.csv --out .\evidence-python\08-power-analysis.json
```

瓦特值必须来自外部校准功率计。若最高像素率的深度和彩色 Profile 不能同时启动，先运行 `survey`，再通过 `--depth-profile-index` 和 `--color-profile-index` 指定兼容组合。

## 输出文件

- `video_profiles.csv`：实际分辨率、FPS、格式、内参、FOV及阈值判断。
- `contract_profile_matrix.csv`：验收表要求的Profile是否真实存在。
- `frames.csv`：逐帧序号、时间戳、ROI有效率和深度中位数。
- `stream_summary.json`：实际FPS、掉帧、稳定率和精度。
- `imu_frames.csv` / `imu_summary.json`：IMU原始值、实测Hz和帧序号缺口。
- `supported_properties.csv` / `hdr_summary.json`：设备属性及RGB HDR支持状态。

## 官方依据

- <https://github.com/orbbec/pyorbbecsdk/tree/v2-main>
- <https://github.com/orbbec/pyorbbecsdk/blob/v2-main/stubs/pyorbbecsdk.pyi>
- <https://github.com/orbbec/pyorbbecsdk/tree/v2-main/examples>
