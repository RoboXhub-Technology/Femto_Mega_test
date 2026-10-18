#!/usr/bin/env python3
"""Orbbec Femto Mega acceptance tests using the official pyorbbecsdk v2 API.

The program records raw CSV evidence and machine-readable JSON summaries.  It
does not change camera settings except when ``hdr --set on|off`` is requested.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import fmean
from typing import Any, Callable, Iterable

try:
    import numpy as np
    from pyorbbecsdk import (  # type: ignore
        Config,
        Context,
        OBFormat,
        OBFrameAggregateOutputMode,
        OBPermissionType,
        OBPropertyID,
        OBSensorType,
        Pipeline,
        convert_format_to_string,
        convert_imu_sample_rate_to_string,
        convert_sensor_type_to_string,
        get_version,
    )
except ImportError as exc:  # pragma: no cover - requires SDK runtime
    raise SystemExit(
        "Python dependencies are not installed. Run: "
        "python -m pip install -r python/requirements.txt"
    ) from exc


REQUIRED_DEPTH_PROFILES = (
    ("WFOV unbinned", 1024, 1024, 5),
    ("WFOV unbinned", 1024, 1024, 15),
    ("WFOV binned", 512, 512, 5),
    ("WFOV binned", 512, 512, 15),
    ("WFOV binned", 512, 512, 25),
    ("WFOV binned", 512, 512, 30),
    ("NFOV unbinned", 640, 576, 5),
    ("NFOV unbinned", 640, 576, 15),
    ("NFOV unbinned", 640, 576, 25),
    ("NFOV unbinned", 640, 576, 30),
    ("NFOV binned", 320, 288, 5),
    ("NFOV binned", 320, 288, 15),
    ("NFOV binned", 320, 288, 25),
    ("NFOV binned", 320, 288, 30),
)


@dataclass
class ProfileInfo:
    index: int
    sensor: str
    width: int
    height: int
    fps: int
    format_name: str
    profile: Any
    fx: float | None = None
    fy: float | None = None
    cx: float | None = None
    cy: float | None = None
    hfov_deg: float | None = None
    vfov_deg: float | None = None


@dataclass
class FrameStats:
    count: int = 0
    timeouts: int = 0
    estimated_drops: int = 0
    first_index: int | None = None
    last_index: int | None = None
    first_timestamp_us: int | None = None
    last_timestamp_us: int | None = None

    def update(self, frame: Any) -> None:
        index = int(frame.get_index())
        timestamp_us = int(frame.get_timestamp_us())
        if self.count == 0:
            self.first_index = index
            self.first_timestamp_us = timestamp_us
        elif self.last_index is not None and index > self.last_index + 1:
            self.estimated_drops += index - self.last_index - 1
        self.last_index = index
        self.last_timestamp_us = timestamp_us
        self.count += 1

    def measured_fps(self) -> float:
        if (
            self.count < 2
            or self.first_timestamp_us is None
            or self.last_timestamp_us is None
            or self.last_timestamp_us <= self.first_timestamp_us
        ):
            return 0.0
        return (self.count - 1) * 1_000_000.0 / (
            self.last_timestamp_us - self.first_timestamp_us
        )


def enum_name(value: Any) -> str:
    return str(getattr(value, "name", value))


def format_name(value: Any) -> str:
    try:
        return str(convert_format_to_string(value))
    except Exception:
        return enum_name(value)


def safe_call(call: Callable[[], Any], default: Any = "") -> Any:
    try:
        return call()
    except Exception:
        return default


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def output_dir(requested: str | None, prefix: str) -> Path:
    target = (
        Path(requested)
        if requested
        else Path("results") / f"{prefix}_{datetime.now():%Y%m%d_%H%M%S}"
    )
    target.mkdir(parents=True, exist_ok=True)
    return target.resolve()


def add_connection_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device-index", type=int, default=0)
    parser.add_argument("--ip", help="Femto Mega network address; omit for USB enumeration")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--out")


def open_device(args: argparse.Namespace) -> tuple[Any, Any]:
    context = Context()
    if args.ip:
        device = context.create_net_device(args.ip, args.port)
        if device is None:
            raise RuntimeError(f"Unable to open network device {args.ip}:{args.port}")
        return context, device
    devices = context.query_devices()
    count = int(devices.get_count())
    if count == 0:
        raise RuntimeError("No Orbbec device found")
    if args.device_index < 0 or args.device_index >= count:
        raise RuntimeError(f"--device-index must be between 0 and {count - 1}")
    return context, devices.get_device_by_index(args.device_index)


def mode_name(width: int, height: int) -> str:
    return {
        (1024, 1024): "WFOV unbinned",
        (512, 512): "WFOV binned",
        (640, 576): "NFOV unbinned",
        (320, 288): "NFOV binned",
    }.get((width, height), "other")


def required_fov(sensor: str, width: int, height: int) -> tuple[float, float] | None:
    if sensor == "color" and (width, height) == (3840, 2160):
        return 80.0, 51.0
    if sensor == "depth":
        if (width, height) in {(1024, 1024), (512, 512)}:
            return 120.0, 120.0
        if (width, height) in {(640, 576), (320, 288)}:
            return 75.0, 65.0
    return None


def calculate_fov(width: int, height: int, fx: float, fy: float) -> tuple[float, float]:
    if fx <= 0 or fy <= 0:
        raise ValueError("Invalid focal length")
    return (
        math.degrees(2.0 * math.atan(width / (2.0 * fx))),
        math.degrees(2.0 * math.atan(height / (2.0 * fy))),
    )


def enumerate_video_profiles(pipeline: Any, sensor_type: Any, sensor_name: str) -> list[ProfileInfo]:
    result: list[ProfileInfo] = []
    profile_list = pipeline.get_stream_profile_list(sensor_type)
    for index in range(int(profile_list.get_count())):
        base = profile_list.get_stream_profile_by_index(index)
        if not base.is_video_stream_profile():
            continue
        profile = base.as_video_stream_profile()
        item = ProfileInfo(
            index=index,
            sensor=sensor_name,
            width=int(profile.get_width()),
            height=int(profile.get_height()),
            fps=int(profile.get_fps()),
            format_name=format_name(profile.get_format()),
            profile=profile,
        )
        try:
            intr = profile.get_intrinsic()
            item.fx, item.fy = float(intr.fx), float(intr.fy)
            item.cx, item.cy = float(intr.cx), float(intr.cy)
            fov_width = int(intr.width) or item.width
            fov_height = int(intr.height) or item.height
            item.hfov_deg, item.vfov_deg = calculate_fov(
                fov_width, fov_height, item.fx, item.fy
            )
        except Exception:
            pass
        result.append(item)
    return result


def profile_row(item: ProfileInfo) -> dict[str, Any]:
    requirement = required_fov(item.sensor, item.width, item.height)
    fov_pass: bool | None = None
    if requirement and item.hfov_deg is not None and item.vfov_deg is not None:
        fov_pass = item.hfov_deg >= requirement[0] and item.vfov_deg >= requirement[1]
    return {
        "sensor": item.sensor,
        "profile_index": item.index,
        "mode": mode_name(item.width, item.height) if item.sensor == "depth" else "4K" if (item.width, item.height) == (3840, 2160) else "other",
        "width": item.width,
        "height": item.height,
        "fps": item.fps,
        "format": item.format_name,
        "fx": item.fx,
        "fy": item.fy,
        "cx": item.cx,
        "cy": item.cy,
        "hfov_deg": item.hfov_deg,
        "vfov_deg": item.vfov_deg,
        "required_hfov_deg": requirement[0] if requirement else None,
        "required_vfov_deg": requirement[1] if requirement else None,
        "fov_pass": fov_pass,
    }


def write_csv(path: Path, rows: Iterable[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def profile_matches(
    profile: ProfileInfo, width: int, height: int, fps: int, requested_format: str | None
) -> bool:
    return (
        profile.width == width
        and profile.height == height
        and profile.fps == fps
        and (not requested_format or profile.format_name.upper() == requested_format.upper())
    )


def select_video_profile(
    pipeline: Any,
    sensor_type: Any,
    sensor_name: str,
    width: int | None,
    height: int | None,
    fps: int | None,
    requested_format: str | None,
    profile_index: int | None,
) -> ProfileInfo:
    profiles = enumerate_video_profiles(pipeline, sensor_type, sensor_name)
    if profile_index is not None:
        for item in profiles:
            if item.index == profile_index:
                return item
        raise RuntimeError(f"Profile index {profile_index} is not a video profile")
    if width is None or height is None or fps is None:
        raise RuntimeError("Specify --profile-index or all of --width --height --fps")
    matches = [
        item
        for item in profiles
        if profile_matches(item, width, height, fps, requested_format)
    ]
    if not matches:
        raise RuntimeError("No exact profile match; run survey and do not substitute another mode")
    if sensor_name == "depth" and not requested_format:
        preference = {"Y16": 0, "Z16": 1, "RLE": 2}
        matches.sort(key=lambda item: preference.get(item.format_name.upper(), 99))
    return matches[0]


def command_survey(args: argparse.Namespace) -> None:
    out = output_dir(args.out, "survey")
    _, device = open_device(args)
    pipeline = Pipeline(device)
    depth = enumerate_video_profiles(pipeline, OBSensorType.DEPTH_SENSOR, "depth")
    color = enumerate_video_profiles(pipeline, OBSensorType.COLOR_SENSOR, "color")
    rows = [profile_row(item) for item in depth + color]
    profile_fields = list(rows[0].keys()) if rows else [
        "sensor", "profile_index", "mode", "width", "height", "fps", "format",
        "fx", "fy", "cx", "cy", "hfov_deg", "vfov_deg", "required_hfov_deg",
        "required_vfov_deg", "fov_pass",
    ]
    write_csv(out / "video_profiles.csv", rows, profile_fields)

    matrix_rows: list[dict[str, Any]] = []
    for mode, width, height, fps in REQUIRED_DEPTH_PROFILES:
        matches = [p for p in depth if profile_matches(p, width, height, fps, None)]
        matrix_rows.append({
            "item": "depth", "mode": mode, "width": width, "height": height,
            "fps": fps, "available": bool(matches),
            "formats": ";".join(sorted({p.format_name for p in matches})),
        })
    rgb_matches = [p for p in color if profile_matches(p, 3840, 2160, 30, None)]
    matrix_rows.append({
        "item": "color", "mode": "4K", "width": 3840, "height": 2160,
        "fps": 30, "available": bool(rgb_matches),
        "formats": ";".join(sorted({p.format_name for p in rgb_matches})),
    })
    write_csv(
        out / "contract_profile_matrix.csv",
        matrix_rows,
        ["item", "mode", "width", "height", "fps", "available", "formats"],
    )

    property_rows: list[dict[str, Any]] = []
    for index in range(int(device.get_support_property_count())):
        item = device.get_supported_property(index)
        property_rows.append({
            "index": index,
            "id": int(item.id.value),
            "name": item.name,
            "type": enum_name(item.type),
            "permission": enum_name(item.permission),
        })
    write_csv(
        out / "supported_properties.csv",
        property_rows,
        ["index", "id", "name", "type", "permission"],
    )

    imu_rows: list[dict[str, Any]] = []
    has_accel = False
    has_gyro = False
    sensors = device.get_sensor_list()
    for index in range(int(sensors.get_count())):
        sensor = sensors.get_sensor_by_index(index)
        sensor_type = sensor.get_type()
        if sensor_type not in (OBSensorType.ACCEL_SENSOR, OBSensorType.GYRO_SENSOR):
            continue
        has_accel |= sensor_type == OBSensorType.ACCEL_SENSOR
        has_gyro |= sensor_type == OBSensorType.GYRO_SENSOR
        profiles = sensor.get_stream_profile_list()
        for profile_index in range(int(profiles.get_count())):
            base = profiles.get_stream_profile_by_index(profile_index)
            if sensor_type == OBSensorType.ACCEL_SENSOR:
                profile = base.as_accel_stream_profile()
                full_scale = enum_name(profile.get_full_scale_range())
                label = "ACCEL"
            else:
                profile = base.as_gyro_stream_profile()
                full_scale = enum_name(profile.get_full_scale_range())
                label = "GYRO"
            imu_rows.append({
                "sensor": label,
                "profile_index": profile_index,
                "sample_rate": convert_imu_sample_rate_to_string(profile.get_sample_rate()),
                "full_scale_range": full_scale,
            })
    write_csv(
        out / "imu_profiles.csv",
        imu_rows,
        ["sensor", "profile_index", "sample_rate", "full_scale_range"],
    )

    hdr_id = OBPropertyID.OB_PROP_COLOR_HDR_BOOL
    hdr_read = bool(device.is_property_supported(hdr_id, OBPermissionType.PERMISSION_READ))
    hdr_write = bool(device.is_property_supported(hdr_id, OBPermissionType.PERMISSION_WRITE))
    info = device.get_device_info()
    summary = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "pyorbbecsdk_version": get_version(),
        "device": {
            "name": info.get_name(),
            "serial_number": info.get_serial_number(),
            "firmware_version": info.get_firmware_version(),
            "hardware_version": safe_call(info.get_hardware_version),
            "connection_type": safe_call(info.get_connection_type),
            "ip_address": safe_call(info.get_device_ip_address),
            "vid": info.get_vid(),
            "pid": info.get_pid(),
        },
        "contract_capabilities": {
            "all_required_depth_profiles_listed": all(row["available"] for row in matrix_rows[:-1]),
            "rgb_3840x2160_30_listed": bool(rgb_matches),
            "color_hdr_read_supported": hdr_read,
            "color_hdr_write_supported": hdr_write,
            "color_hdr_current": device.get_bool_property(hdr_id) if hdr_read else None,
            "accelerometer_present": has_accel,
            "gyroscope_present": has_gyro,
        },
        "files": [
            "video_profiles.csv", "contract_profile_matrix.csv", "imu_profiles.csv",
            "supported_properties.csv",
        ],
    }
    write_json(out / "survey_summary.json", summary)
    print(f"Survey complete: {out}")


def depth_roi(frame: Any, roi_size: int) -> tuple[float, float | None]:
    width, height = int(frame.get_width()), int(frame.get_height())
    expected = width * height
    data = np.asarray(frame.get_data(), dtype=np.uint8)
    if data.nbytes < expected * 2:
        raise RuntimeError(
            f"Depth buffer is compressed or incomplete ({data.nbytes} bytes); select Y16/Z16"
        )
    raw = np.frombuffer(data, dtype=np.uint16, count=expected).reshape(height, width)
    roi = max(1, min(roi_size, width, height))
    x0, y0 = (width - roi) // 2, (height - roi) // 2
    crop = raw[y0 : y0 + roi, x0 : x0 + roi]
    valid = crop[crop != 0]
    valid_ratio = float(valid.size / crop.size)
    if valid.size == 0:
        return valid_ratio, None
    return valid_ratio, float(np.median(valid) * float(frame.get_depth_scale()))


def save_first_frame(out: Path, frame: Any, sensor: str, fmt: str) -> str:
    extension = ".raw"
    if sensor == "color":
        extension = {
            "MJPG": ".jpg", "MJPEG": ".jpg", "H264": ".h264", "H265": ".h265",
            "HEVC": ".h265",
        }.get(fmt.upper(), ".raw")
    name = f"first_frame{extension}"
    (out / name).write_bytes(np.asarray(frame.get_data(), dtype=np.uint8).tobytes())
    write_json(out / "first_frame_metadata.json", {
        "file": name,
        "sensor": sensor,
        "width": int(frame.get_width()),
        "height": int(frame.get_height()),
        "format": fmt,
        "bytes": int(frame.get_data_size()),
    })
    return name


def command_stream(args: argparse.Namespace) -> None:
    if args.seconds <= 0 and args.frames <= 0:
        raise RuntimeError("At least one of --seconds or --frames must be greater than zero")
    if args.max_wall_seconds <= 0:
        raise RuntimeError("--max-wall-seconds must be greater than zero")
    out = output_dir(args.out, args.sensor)
    _, device = open_device(args)
    pipeline = Pipeline(device)
    sensor_type = OBSensorType.COLOR_SENSOR if args.sensor == "color" else OBSensorType.DEPTH_SENSOR
    selected = select_video_profile(
        pipeline, sensor_type, args.sensor, args.width, args.height, args.fps,
        args.format, args.profile_index,
    )
    config = Config()
    config.enable_stream(selected.profile)
    config.set_frame_aggregate_output_mode(OBFrameAggregateOutputMode.ANY_SITUATION)
    pipeline.start(config)
    stats = FrameStats()
    medians: list[float] = []
    valid_roi_frames = 0
    first_saved = False
    started = time.monotonic()
    frame_csv = (out / "frames.csv").open("w", encoding="utf-8-sig", newline="")
    writer = csv.writer(frame_csv)
    writer.writerow([
        "sequence", "frame_index", "timestamp_us", "system_timestamp_us", "width",
        "height", "format", "roi_valid_ratio", "roi_median_mm",
    ])
    try:
        while True:
            elapsed = time.monotonic() - started
            if args.seconds > 0 and elapsed >= args.seconds:
                break
            if args.seconds <= 0 and elapsed >= args.max_wall_seconds:
                break
            if args.frames > 0 and stats.count >= args.frames:
                break
            frameset = pipeline.wait_for_frames(2000)
            if frameset is None:
                stats.timeouts += 1
                continue
            frame = frameset.get_color_frame() if args.sensor == "color" else frameset.get_depth_frame()
            if frame is None:
                continue
            stats.update(frame)
            fmt = format_name(frame.get_format())
            valid_ratio: float | None = None
            roi_median: float | None = None
            if args.sensor == "depth":
                valid_ratio, roi_median = depth_roi(frame, args.roi)
                if valid_ratio >= args.valid_ratio:
                    valid_roi_frames += 1
                if roi_median is not None:
                    medians.append(roi_median)
            writer.writerow([
                stats.count, frame.get_index(), frame.get_timestamp_us(),
                frame.get_system_timestamp_us(), frame.get_width(), frame.get_height(), fmt,
                valid_ratio, roi_median,
            ])
            if args.save_first_frame and not first_saved:
                save_first_frame(out, frame, args.sensor, fmt)
                first_saved = True
    finally:
        frame_csv.close()
        pipeline.stop()
    wall_seconds = time.monotonic() - started
    dmean = fmean(medians) if medians else None
    relative_error = (
        abs(dmean - args.reference_mm) / args.reference_mm * 100.0
        if dmean is not None and args.reference_mm > 0
        else None
    )
    stable_ratio = valid_roi_frames / stats.count if stats.count else 0.0
    summary: dict[str, Any] = {
        "sensor": args.sensor,
        "profile": {
            "profile_index": selected.index, "width": selected.width,
            "height": selected.height, "fps": selected.fps, "format": selected.format_name,
        },
        "requested_duration_s": args.seconds,
        "requested_frames": args.frames,
        "completed_requested_frames": args.frames <= 0 or stats.count >= args.frames,
        "max_wall_seconds": args.max_wall_seconds,
        "wall_duration_s": wall_seconds,
        "frames_received": stats.count,
        "timeouts": stats.timeouts,
        "estimated_index_gaps": stats.estimated_drops,
        "measured_fps_from_device_timestamps": stats.measured_fps(),
    }
    if args.sensor == "depth":
        summary.update({
            "roi_size": args.roi,
            "roi_valid_ratio_threshold": args.valid_ratio,
            "frames_meeting_valid_ratio": valid_roi_frames,
            "stable_frame_ratio": stable_ratio,
            "meets_95_of_100_stability_rule": stable_ratio >= 0.95,
            "frames_with_depth_median": len(medians),
            "mean_of_frame_medians_mm": dmean,
            "reference_distance_mm": args.reference_mm if args.reference_mm > 0 else None,
            "relative_error_percent": relative_error,
            "meets_0_15_percent_limit": relative_error <= 0.15 if relative_error is not None else None,
        })
    write_json(out / "stream_summary.json", summary)
    print(f"Stream test complete: {out}")


def command_imu(args: argparse.Namespace) -> None:
    out = output_dir(args.out, "imu")
    _, device = open_device(args)
    device.get_sensor(OBSensorType.ACCEL_SENSOR)
    device.get_sensor(OBSensorType.GYRO_SENSOR)
    pipeline = Pipeline(device)
    config = Config()
    config.enable_accel_stream()
    config.enable_gyro_stream()
    config.set_frame_aggregate_output_mode(OBFrameAggregateOutputMode.FULL_FRAME_REQUIRE)
    pipeline.start(config)
    accel, gyro = FrameStats(), FrameStats()
    started = time.monotonic()
    with (out / "imu_frames.csv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["sensor", "frame_index", "timestamp_us", "system_timestamp_us", "x", "y", "z", "temperature_c"])
        try:
            while time.monotonic() - started < args.seconds:
                frameset = pipeline.wait_for_frames(2000)
                if frameset is None:
                    accel.timeouts += 1
                    gyro.timeouts += 1
                    continue
                accel_frame = frameset.get_accel_frame()
                if accel_frame is not None:
                    accel.update(accel_frame)
                    value = accel_frame.get_value()
                    writer.writerow(["ACCEL", accel_frame.get_index(), accel_frame.get_timestamp_us(), accel_frame.get_system_timestamp_us(), value.x, value.y, value.z, accel_frame.get_temperature()])
                gyro_frame = frameset.get_gyro_frame()
                if gyro_frame is not None:
                    gyro.update(gyro_frame)
                    value = gyro_frame.get_value()
                    writer.writerow(["GYRO", gyro_frame.get_index(), gyro_frame.get_timestamp_us(), gyro_frame.get_system_timestamp_us(), value.x, value.y, value.z, gyro_frame.get_temperature()])
        finally:
            pipeline.stop()
    write_json(out / "imu_summary.json", {
        "duration_s": args.seconds,
        "accelerometer": {"frames": accel.count, "measured_hz": accel.measured_fps(), "estimated_index_gaps": accel.estimated_drops, "timeouts": accel.timeouts},
        "gyroscope": {"frames": gyro.count, "measured_hz": gyro.measured_fps(), "estimated_index_gaps": gyro.estimated_drops, "timeouts": gyro.timeouts},
    })
    print(f"IMU test complete: {out}")


def command_hdr(args: argparse.Namespace) -> None:
    out = output_dir(args.out, "hdr")
    _, device = open_device(args)
    property_id = OBPropertyID.OB_PROP_COLOR_HDR_BOOL
    can_read = bool(device.is_property_supported(property_id, OBPermissionType.PERMISSION_READ))
    can_write = bool(device.is_property_supported(property_id, OBPermissionType.PERMISSION_WRITE))
    before = device.get_bool_property(property_id) if can_read else None
    if args.set != "query":
        if not can_write:
            raise RuntimeError("OB_PROP_COLOR_HDR_BOOL is not writable on this firmware")
        device.set_bool_property(property_id, args.set == "on")
        time.sleep(0.3)
    after = device.get_bool_property(property_id) if can_read else None
    write_json(out / "hdr_summary.json", {
        "property": "OB_PROP_COLOR_HDR_BOOL",
        "read_supported": can_read,
        "write_supported": can_write,
        "requested_action": args.set,
        "value_before": before,
        "value_after": after,
    })
    print(f"HDR query/test complete: {out}")


def highest_pixel_rate(profiles: list[ProfileInfo], explicit_index: int | None) -> ProfileInfo:
    if explicit_index is not None:
        for profile in profiles:
            if profile.index == explicit_index:
                return profile
        raise RuntimeError(f"Profile index {explicit_index} not found")
    return max(profiles, key=lambda item: item.width * item.height * item.fps)


def command_load(args: argparse.Namespace) -> None:
    out = output_dir(args.out, "power_load")
    _, device = open_device(args)
    pipeline = Pipeline(device)
    depth = highest_pixel_rate(
        enumerate_video_profiles(pipeline, OBSensorType.DEPTH_SENSOR, "depth"),
        args.depth_profile_index,
    )
    color = highest_pixel_rate(
        enumerate_video_profiles(pipeline, OBSensorType.COLOR_SENSOR, "color"),
        args.color_profile_index,
    )
    config = Config()
    config.enable_stream(depth.profile)
    config.enable_stream(color.profile)
    imu_enabled = True
    try:
        device.get_sensor(OBSensorType.ACCEL_SENSOR)
        device.get_sensor(OBSensorType.GYRO_SENSOR)
        config.enable_accel_stream()
        config.enable_gyro_stream()
    except Exception:
        imu_enabled = False
    config.set_frame_aggregate_output_mode(OBFrameAggregateOutputMode.ANY_SITUATION)
    pipeline.start(config)
    counts = {"depth": FrameStats(), "color": FrameStats(), "accel": FrameStats(), "gyro": FrameStats()}
    started = time.monotonic()
    try:
        while time.monotonic() - started < args.seconds:
            frameset = pipeline.wait_for_frames(2000)
            if frameset is None:
                continue
            for name, getter in (
                ("depth", frameset.get_depth_frame), ("color", frameset.get_color_frame),
                ("accel", frameset.get_accel_frame), ("gyro", frameset.get_gyro_frame),
            ):
                frame = getter()
                if frame is not None:
                    counts[name].update(frame)
    finally:
        pipeline.stop()
    write_json(out / "power_load_summary.json", {
        "duration_s": args.seconds,
        "note": "Camera workload only; watts require an external calibrated meter.",
        "depth_profile": profile_row(depth),
        "color_profile": profile_row(color),
        "imu_enabled": imu_enabled,
        "frames": {name: stats.count for name, stats in counts.items()},
    })
    print(f"Power-load run complete: {out}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Femto Mega acceptance tests with pyorbbecsdk v2")
    subparsers = parser.add_subparsers(dest="command", required=True)

    survey = subparsers.add_parser("survey", help="Enumerate device capabilities, profiles, FOV and properties")
    add_connection_arguments(survey)
    survey.set_defaults(func=command_survey)

    stream = subparsers.add_parser("stream", help="Measure a depth or color stream")
    add_connection_arguments(stream)
    stream.add_argument("--sensor", choices=("depth", "color"), default="depth")
    stream.add_argument("--profile-index", type=int)
    stream.add_argument("--width", type=int)
    stream.add_argument("--height", type=int)
    stream.add_argument("--fps", type=int)
    stream.add_argument("--format")
    stream.add_argument("--seconds", type=float, default=60.0)
    stream.add_argument("--frames", type=int, default=0)
    stream.add_argument("--max-wall-seconds", type=float, default=300.0, help="Safety timeout for frame-count-only tests")
    stream.add_argument("--roi", type=int, default=100)
    stream.add_argument("--valid-ratio", type=float, default=0.95)
    stream.add_argument("--reference-mm", type=float, default=0.0)
    stream.add_argument("--save-first-frame", action="store_true")
    stream.set_defaults(func=command_stream)

    imu = subparsers.add_parser("imu", help="Measure accelerometer and gyroscope delivery rate")
    add_connection_arguments(imu)
    imu.add_argument("--seconds", type=float, default=60.0)
    imu.set_defaults(func=command_imu)

    hdr = subparsers.add_parser("hdr", help="Query or explicitly switch the RGB HDR property")
    add_connection_arguments(hdr)
    hdr.add_argument("--set", choices=("query", "on", "off"), default="query")
    hdr.set_defaults(func=command_hdr)

    load = subparsers.add_parser("load", help="Create a maximum-stream workload for power-meter testing")
    add_connection_arguments(load)
    load.add_argument("--seconds", type=float, default=600.0)
    load.add_argument("--depth-profile-index", type=int)
    load.add_argument("--color-profile-index", type=int)
    load.set_defaults(func=command_load)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        args.func(args)
        return 0
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
