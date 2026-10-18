#include <k4a/k4a.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct TestResult
{
    std::string name;
    std::string status;
    std::string details;
    bool required;
};

struct Report
{
    std::vector<TestResult> tests;
    uint32_t device_count = 0;
    int device_index = 0;
    std::string serial_number;
    std::string firmware_version;
    std::string k4a_module_path;
    std::string orbbec_module_path;

    void add(const std::string &name, bool passed, const std::string &details, bool required = true)
    {
        tests.push_back({ name, passed ? "PASS" : "FAIL", details, required });
        std::cout << '[' << (passed ? "PASS" : "FAIL") << "] " << name;
        if (!details.empty())
        {
            std::cout << " - " << details;
        }
        std::cout << '\n';
    }

    void info(const std::string &name, const std::string &details)
    {
        tests.push_back({ name, "INFO", details, false });
        std::cout << "[INFO] " << name;
        if (!details.empty())
        {
            std::cout << " - " << details;
        }
        std::cout << '\n';
    }

    bool passed() const
    {
        return std::none_of(tests.begin(), tests.end(), [](const TestResult &r) {
            return r.required && r.status == "FAIL";
        });
    }
};

struct StreamCase
{
    std::string name;
    k4a_color_resolution_t color_resolution;
    k4a_depth_mode_t depth_mode;
    k4a_fps_t fps;
    int expected_color_width;
    int expected_color_height;
    int expected_depth_width;
    int expected_depth_height;
    bool synchronized;
};

static std::string json_escape(const std::string &value)
{
    std::ostringstream out;
    for (unsigned char c : value)
    {
        switch (c)
        {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
            {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
            }
            else
            {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

#ifdef _WIN32
static std::string utf8_from_wide(const std::wstring &value)
{
    if (value.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

static std::string loaded_module_path(const wchar_t *module_name)
{
    HMODULE module = GetModuleHandleW(module_name);
    if (module == nullptr)
    {
        return "not loaded";
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return "path unavailable";
    }
    return utf8_from_wide(std::wstring(buffer.data(), length));
}
#else
static std::string loaded_module_path(const wchar_t *)
{
    return "module path reporting is only implemented on Windows";
}
#endif

static std::string version_string(const k4a_version_t &version)
{
    std::ostringstream out;
    out << version.major << '.' << version.minor << '.' << version.iteration;
    return out.str();
}

static std::string get_serial(k4a_device_t device)
{
    size_t size = 0;
    if (k4a_device_get_serialnum(device, nullptr, &size) != K4A_BUFFER_RESULT_TOO_SMALL || size == 0)
    {
        return {};
    }
    std::vector<char> buffer(size, '\0');
    if (k4a_device_get_serialnum(device, buffer.data(), &size) != K4A_BUFFER_RESULT_SUCCEEDED)
    {
        return {};
    }
    return std::string(buffer.data());
}

static const char *control_name(k4a_color_control_command_t command)
{
    switch (command)
    {
    case K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE: return "exposure";
    case K4A_COLOR_CONTROL_BRIGHTNESS: return "brightness";
    case K4A_COLOR_CONTROL_CONTRAST: return "contrast";
    case K4A_COLOR_CONTROL_SATURATION: return "saturation";
    case K4A_COLOR_CONTROL_SHARPNESS: return "sharpness";
    case K4A_COLOR_CONTROL_WHITEBALANCE: return "white_balance";
    case K4A_COLOR_CONTROL_GAIN: return "gain";
    case K4A_COLOR_CONTROL_POWERLINE_FREQUENCY: return "powerline_frequency";
    default: return "unknown";
    }
}

static bool check_calibration(k4a_device_t device,
                              k4a_depth_mode_t depth_mode,
                              const std::string &mode_name,
                              Report &report)
{
    k4a_calibration_t calibration{};
    const k4a_result_t result = k4a_device_get_calibration(
        device, depth_mode, K4A_COLOR_RESOLUTION_1080P, &calibration);
    if (result != K4A_RESULT_SUCCEEDED)
    {
        report.add("calibration." + mode_name, false, "k4a_device_get_calibration failed");
        return false;
    }

    const auto &depth = calibration.depth_camera_calibration;
    const auto &p = depth.intrinsics.parameters.param;
    constexpr double radians_to_degrees = 57.295779513082320876;
    double hfov = 0.0;
    double vfov = 0.0;
    if (p.fx > 0.0f && p.fy > 0.0f)
    {
        hfov = (std::atan(p.cx / p.fx) + std::atan((depth.resolution_width - p.cx) / p.fx)) *
               radians_to_degrees;
        vfov = (std::atan(p.cy / p.fy) + std::atan((depth.resolution_height - p.cy) / p.fy)) *
               radians_to_degrees;
    }

    std::ostringstream details;
    details << depth.resolution_width << 'x' << depth.resolution_height
            << ", fx=" << std::fixed << std::setprecision(2) << p.fx
            << ", fy=" << p.fy
            << ", approximate_fov=" << hfov << "x" << vfov << " deg";
    report.add("calibration." + mode_name, true, details.str());

    k4a_transformation_t transformation = k4a_transformation_create(&calibration);
    const bool transformation_ok = transformation != nullptr;
    report.add("transformation.create." + mode_name,
               transformation_ok,
               transformation_ok ? "handle created" : "returned null");
    if (transformation != nullptr)
    {
        k4a_transformation_destroy(transformation);
    }
    return transformation_ok;
}

static bool run_stream_case(k4a_device_t device,
                            const StreamCase &test,
                            int requested_frames,
                            Report &report)
{
    k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
    config.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
    config.color_resolution = test.color_resolution;
    config.depth_mode = test.depth_mode;
    config.camera_fps = test.fps;
    config.synchronized_images_only = test.synchronized;

    if (k4a_device_start_cameras(device, &config) != K4A_RESULT_SUCCEEDED)
    {
        report.add("stream." + test.name, false, "k4a_device_start_cameras failed");
        return false;
    }

    int valid_frames = 0;
    int first_color_width = 0;
    int first_color_height = 0;
    int first_depth_width = 0;
    int first_depth_height = 0;
    uint64_t previous_color_timestamp = 0;
    uint64_t previous_depth_timestamp = 0;
    bool timestamps_monotonic = true;
    const int max_attempts = std::max(requested_frames * 3, requested_frames + 3);

    for (int attempt = 0; attempt < max_attempts && valid_frames < requested_frames; ++attempt)
    {
        k4a_capture_t capture = nullptr;
        const k4a_wait_result_t wait_result = k4a_device_get_capture(device, &capture, 3000);
        if (wait_result == K4A_WAIT_RESULT_TIMEOUT)
        {
            continue;
        }
        if (wait_result != K4A_WAIT_RESULT_SUCCEEDED || capture == nullptr)
        {
            break;
        }

        bool frame_ok = true;
        k4a_image_t color = k4a_capture_get_color_image(capture);
        k4a_image_t depth = k4a_capture_get_depth_image(capture);

        if (test.expected_color_width > 0)
        {
            frame_ok = frame_ok && color != nullptr;
            if (color != nullptr)
            {
                const int width = k4a_image_get_width_pixels(color);
                const int height = k4a_image_get_height_pixels(color);
                first_color_width = width;
                first_color_height = height;
                frame_ok = frame_ok && width == test.expected_color_width && height == test.expected_color_height;
                const uint64_t timestamp = k4a_image_get_device_timestamp_usec(color);
                timestamps_monotonic = timestamps_monotonic &&
                                       (previous_color_timestamp == 0 || timestamp > previous_color_timestamp);
                previous_color_timestamp = timestamp;
            }
        }

        if (test.expected_depth_width > 0)
        {
            frame_ok = frame_ok && depth != nullptr;
            if (depth != nullptr)
            {
                const int width = k4a_image_get_width_pixels(depth);
                const int height = k4a_image_get_height_pixels(depth);
                first_depth_width = width;
                first_depth_height = height;
                frame_ok = frame_ok && width == test.expected_depth_width && height == test.expected_depth_height;
                const uint64_t timestamp = k4a_image_get_device_timestamp_usec(depth);
                timestamps_monotonic = timestamps_monotonic &&
                                       (previous_depth_timestamp == 0 || timestamp > previous_depth_timestamp);
                previous_depth_timestamp = timestamp;
            }
        }

        if (frame_ok)
        {
            ++valid_frames;
        }
        if (color != nullptr)
        {
            k4a_image_release(color);
        }
        if (depth != nullptr)
        {
            k4a_image_release(depth);
        }
        k4a_capture_release(capture);
    }

    k4a_device_stop_cameras(device);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    const bool passed = valid_frames >= requested_frames && timestamps_monotonic;
    std::ostringstream details;
    details << "valid_frames=" << valid_frames << '/' << requested_frames;
    if (test.expected_color_width > 0)
    {
        details << ", color=" << first_color_width << 'x' << first_color_height;
    }
    if (test.expected_depth_width > 0)
    {
        details << ", depth=" << first_depth_width << 'x' << first_depth_height;
    }
    details << ", timestamps_monotonic=" << (timestamps_monotonic ? "true" : "false");
    report.add("stream." + test.name, passed, details.str());
    return passed;
}

static bool check_imu(k4a_device_t device, int requested_samples, Report &report)
{
    if (k4a_device_start_imu(device) != K4A_RESULT_SUCCEEDED)
    {
        report.add("imu", false, "k4a_device_start_imu failed");
        return false;
    }

    int valid_samples = 0;
    k4a_imu_sample_t last{};
    for (int attempt = 0; attempt < requested_samples * 4 && valid_samples < requested_samples; ++attempt)
    {
        if (k4a_device_get_imu_sample(device, &last, 1000) == K4A_WAIT_RESULT_SUCCEEDED)
        {
            ++valid_samples;
        }
    }
    k4a_device_stop_imu(device);

    const bool passed = valid_samples >= requested_samples;
    std::ostringstream details;
    details << "samples=" << valid_samples << '/' << requested_samples
            << ", temperature=" << std::fixed << std::setprecision(2) << last.temperature
            << ", accel_timestamp_us=" << last.acc_timestamp_usec
            << ", gyro_timestamp_us=" << last.gyro_timestamp_usec;
    report.add("imu", passed, details.str());
    return passed;
}

static void check_color_controls(k4a_device_t device, Report &report)
{
    const std::vector<k4a_color_control_command_t> commands = {
        K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE,
        K4A_COLOR_CONTROL_BRIGHTNESS,
        K4A_COLOR_CONTROL_CONTRAST,
        K4A_COLOR_CONTROL_SATURATION,
        K4A_COLOR_CONTROL_SHARPNESS,
        K4A_COLOR_CONTROL_WHITEBALANCE,
        K4A_COLOR_CONTROL_GAIN,
        K4A_COLOR_CONTROL_POWERLINE_FREQUENCY,
    };

    for (const auto command : commands)
    {
        bool supports_auto = false;
        int32_t minimum = 0;
        int32_t maximum = 0;
        int32_t step = 0;
        int32_t default_value = 0;
        k4a_color_control_mode_t default_mode = K4A_COLOR_CONTROL_MODE_MANUAL;

        const k4a_result_t capability_result = k4a_device_get_color_control_capabilities(
            device, command, &supports_auto, &minimum, &maximum, &step, &default_value, &default_mode);
        if (capability_result != K4A_RESULT_SUCCEEDED)
        {
            report.info(std::string("color_control.") + control_name(command), "not exposed by Wrapper/device");
            continue;
        }

        int32_t current_value = 0;
        k4a_color_control_mode_t current_mode = K4A_COLOR_CONTROL_MODE_MANUAL;
        const bool readable = k4a_device_get_color_control(device, command, &current_mode, &current_value) ==
                              K4A_RESULT_SUCCEEDED;
        std::ostringstream details;
        details << "range=" << minimum << ".." << maximum << ", step=" << step
                << ", default=" << default_value << ", auto=" << (supports_auto ? "true" : "false");
        if (readable)
        {
            details << ", current=" << current_value
                    << ", mode=" << (current_mode == K4A_COLOR_CONTROL_MODE_AUTO ? "auto" : "manual");
        }
        else
        {
            details << ", current=unreadable";
        }
        report.add(std::string("color_control.") + control_name(command), readable, details.str(), false);
    }

    // Deliberately do not call K4A_COLOR_CONTROL_HDR here. In Wrapper v2-main
    // the implementation maps HDR only for Femto Bolt. Femto Mega does not
    // expose OB_PROP_COLOR_HDR_BOOL, and old Wrapper releases may return
    // success with undefined capability fields for non-Bolt devices.
    report.info("color_control.hdr",
                "skipped: Wrapper maps HDR only for Femto Bolt; not a Femto Mega compatibility criterion");
}

static bool write_report(const fs::path &path, const Report &report, bool full_mode, int frames)
{
    std::error_code error;
    if (path.has_parent_path())
    {
        fs::create_directories(path.parent_path(), error);
        if (error)
        {
            std::cerr << "Cannot create result directory: " << error.message() << '\n';
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        std::cerr << "Cannot open result file: " << path << '\n';
        return false;
    }

    output << "{\n"
           << "  \"test_scope\": \"OrbbecSDK-K4A-Wrapper sensor compatibility; no body tracking\",\n"
           << "  \"mode\": \"" << (full_mode ? "full" : "quick") << "\",\n"
           << "  \"frames_per_stream_case\": " << frames << ",\n"
           << "  \"device_index\": " << report.device_index << ",\n"
           << "  \"device_count\": " << report.device_count << ",\n"
           << "  \"serial_number\": \"" << json_escape(report.serial_number) << "\",\n"
           << "  \"firmware_version\": \"" << json_escape(report.firmware_version) << "\",\n"
           << "  \"loaded_k4a_dll\": \"" << json_escape(report.k4a_module_path) << "\",\n"
           << "  \"loaded_orbbecsdk_dll\": \"" << json_escape(report.orbbec_module_path) << "\",\n"
           << "  \"overall_pass\": " << (report.passed() ? "true" : "false") << ",\n"
           << "  \"tests\": [\n";
    for (size_t i = 0; i < report.tests.size(); ++i)
    {
        const auto &test = report.tests[i];
        output << "    {\"name\": \"" << json_escape(test.name)
               << "\", \"status\": \"" << test.status
               << "\", \"required\": " << (test.required ? "true" : "false")
               << ", \"details\": \"" << json_escape(test.details) << "\"}";
        output << (i + 1 == report.tests.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return true;
}

static void print_usage(const char *program)
{
    std::cout << "Usage: " << program
              << " [--quick|--full] [--device INDEX] [--frames N] [--output FILE]\n";
}

int main(int argc, char **argv)
{
    bool full_mode = false;
    int device_index = 0;
    int frames = 5;
    fs::path output_path = fs::path("results") / "k4a_wrapper_quick.json";

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--full")
        {
            full_mode = true;
            output_path = fs::path("results") / "k4a_wrapper_full.json";
        }
        else if (argument == "--quick")
        {
            full_mode = false;
        }
        else if (argument == "--device" && i + 1 < argc)
        {
            device_index = std::stoi(argv[++i]);
        }
        else if (argument == "--frames" && i + 1 < argc)
        {
            frames = std::max(1, std::stoi(argv[++i]));
        }
        else if (argument == "--output" && i + 1 < argc)
        {
            output_path = argv[++i];
        }
        else if (argument == "--help" || argument == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            print_usage(argv[0]);
            return 64;
        }
    }

    Report report;
    report.device_index = device_index;
    report.device_count = k4a_device_get_installed_count();
    report.k4a_module_path = loaded_module_path(L"k4a.dll");
    report.orbbec_module_path = loaded_module_path(L"OrbbecSDK.dll");
    report.info("runtime.k4a_dll", report.k4a_module_path);
    report.info("runtime.orbbecsdk_dll", report.orbbec_module_path);

    if (report.device_count == 0 || device_index < 0 || static_cast<uint32_t>(device_index) >= report.device_count)
    {
        std::ostringstream details;
        details << "device_count=" << report.device_count << ", requested_index=" << device_index;
        report.add("device.enumerate", false, details.str());
        write_report(output_path, report, full_mode, frames);
        return 2;
    }
    report.add("device.enumerate", true, "device_count=" + std::to_string(report.device_count));

    k4a_device_t device = nullptr;
    if (k4a_device_open(static_cast<uint32_t>(device_index), &device) != K4A_RESULT_SUCCEEDED || device == nullptr)
    {
        report.add("device.open", false, "k4a_device_open failed");
        write_report(output_path, report, full_mode, frames);
        return 2;
    }
    report.add("device.open", true, "index=" + std::to_string(device_index));

    report.k4a_module_path = loaded_module_path(L"k4a.dll");
    report.orbbec_module_path = loaded_module_path(L"OrbbecSDK.dll");
    report.serial_number = get_serial(device);
    report.add("device.serial", !report.serial_number.empty(), report.serial_number);

    const int pid = k4a_device_get_pid(device);
    report.info("device.pid", std::to_string(pid));

    k4a_hardware_version_t hardware_version{};
    if (k4a_device_get_version(device, &hardware_version) == K4A_RESULT_SUCCEEDED)
    {
        report.firmware_version = version_string(hardware_version.depth);
        report.add("device.firmware", true, report.firmware_version);
    }
    else
    {
        report.add("device.firmware", false, "k4a_device_get_version failed");
    }

    check_calibration(device, K4A_DEPTH_MODE_NFOV_2X2BINNED, "nfov_2x2binned", report);
    check_calibration(device, K4A_DEPTH_MODE_NFOV_UNBINNED, "nfov_unbinned", report);
    check_calibration(device, K4A_DEPTH_MODE_WFOV_2X2BINNED, "wfov_2x2binned", report);
    check_calibration(device, K4A_DEPTH_MODE_WFOV_UNBINNED, "wfov_unbinned", report);

    check_color_controls(device, report);

    std::vector<StreamCase> stream_cases = {
        { "sync_nfov_unbinned_1080p_25fps", K4A_COLOR_RESOLUTION_1080P,
          K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_25, 1920, 1080, 640, 576, true },
        { "color_2160p_25fps", K4A_COLOR_RESOLUTION_2160P,
          K4A_DEPTH_MODE_OFF, K4A_FRAMES_PER_SECOND_25, 3840, 2160, 0, 0, false },
        { "depth_nfov_2x2binned_30fps", K4A_COLOR_RESOLUTION_OFF,
          K4A_DEPTH_MODE_NFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_30, 0, 0, 320, 288, false },
        { "depth_wfov_2x2binned_30fps", K4A_COLOR_RESOLUTION_OFF,
          K4A_DEPTH_MODE_WFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_30, 0, 0, 512, 512, false },
        { "depth_wfov_unbinned_15fps", K4A_COLOR_RESOLUTION_OFF,
          K4A_DEPTH_MODE_WFOV_UNBINNED, K4A_FRAMES_PER_SECOND_15, 0, 0, 1024, 1024, false },
    };

    if (full_mode)
    {
        stream_cases = {
            { "sync_nfov_unbinned_1080p_25fps", K4A_COLOR_RESOLUTION_1080P,
              K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_25, 1920, 1080, 640, 576, true },
            { "color_2160p_5fps", K4A_COLOR_RESOLUTION_2160P,
              K4A_DEPTH_MODE_OFF, K4A_FRAMES_PER_SECOND_5, 3840, 2160, 0, 0, false },
            { "color_2160p_15fps", K4A_COLOR_RESOLUTION_2160P,
              K4A_DEPTH_MODE_OFF, K4A_FRAMES_PER_SECOND_15, 3840, 2160, 0, 0, false },
            { "color_2160p_25fps", K4A_COLOR_RESOLUTION_2160P,
              K4A_DEPTH_MODE_OFF, K4A_FRAMES_PER_SECOND_25, 3840, 2160, 0, 0, false },
            { "depth_nfov_2x2binned_5fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_5, 0, 0, 320, 288, false },
            { "depth_nfov_2x2binned_15fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_15, 0, 0, 320, 288, false },
            { "depth_nfov_2x2binned_25fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_25, 0, 0, 320, 288, false },
            { "depth_nfov_2x2binned_30fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_30, 0, 0, 320, 288, false },
            { "depth_nfov_unbinned_5fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_5, 0, 0, 640, 576, false },
            { "depth_nfov_unbinned_15fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_15, 0, 0, 640, 576, false },
            { "depth_nfov_unbinned_25fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_25, 0, 0, 640, 576, false },
            { "depth_nfov_unbinned_30fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_NFOV_UNBINNED, K4A_FRAMES_PER_SECOND_30, 0, 0, 640, 576, false },
            { "depth_wfov_2x2binned_5fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_5, 0, 0, 512, 512, false },
            { "depth_wfov_2x2binned_15fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_15, 0, 0, 512, 512, false },
            { "depth_wfov_2x2binned_25fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_25, 0, 0, 512, 512, false },
            { "depth_wfov_2x2binned_30fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_2X2BINNED, K4A_FRAMES_PER_SECOND_30, 0, 0, 512, 512, false },
            { "depth_wfov_unbinned_5fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_UNBINNED, K4A_FRAMES_PER_SECOND_5, 0, 0, 1024, 1024, false },
            { "depth_wfov_unbinned_15fps", K4A_COLOR_RESOLUTION_OFF,
              K4A_DEPTH_MODE_WFOV_UNBINNED, K4A_FRAMES_PER_SECOND_15, 0, 0, 1024, 1024, false },
        };
    }

    for (const auto &test : stream_cases)
    {
        run_stream_case(device, test, frames, report);
    }

    check_imu(device, frames, report);
    k4a_device_close(device);

    if (!write_report(output_path, report, full_mode, frames))
    {
        return 3;
    }

    std::cout << "\nResult: " << (report.passed() ? "PASS" : "FAIL")
              << "\nJSON: " << fs::absolute(output_path).string() << '\n';
    return report.passed() ? 0 : 1;
}

