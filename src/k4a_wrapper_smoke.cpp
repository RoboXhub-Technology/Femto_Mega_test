#include <k4a/k4a.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 60;
    const uint32_t count = k4a_device_get_installed_count();
    std::cout << "installed_device_count=" << count << '\n';
    if(count == 0) return 2;

    k4a_device_t device = nullptr;
    if(K4A_FAILED(k4a_device_open(K4A_DEVICE_DEFAULT, &device))) {
        std::cerr << "k4a_device_open failed\n";
        return 3;
    }

    size_t serialSize = 0;
    k4a_device_get_serialnum(device, nullptr, &serialSize);
    std::string serial(serialSize ? serialSize : 1, '\0');
    if(serialSize && K4A_SUCCEEDED(k4a_device_get_serialnum(device, &serial[0], &serialSize))) {
        if(!serial.empty() && serial.back() == '\0') serial.pop_back();
        std::cout << "serial_number=" << serial << '\n';
    }

    const auto version = k4a_device_get_version(device);
    std::cout << "rgb_firmware=" << version.rgb.major << '.' << version.rgb.minor << '.' << version.rgb.iteration << '\n';
    std::cout << "depth_firmware=" << version.depth.major << '.' << version.depth.minor << '.' << version.depth.iteration << '\n';

    k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
    config.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
    config.color_resolution = K4A_COLOR_RESOLUTION_2160P;
    config.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
    config.camera_fps = K4A_FRAMES_PER_SECOND_30;
    config.synchronized_images_only = true;

    if(K4A_FAILED(k4a_device_start_cameras(device, &config))) {
        std::cerr << "k4a_device_start_cameras failed\n";
        k4a_device_close(device);
        return 4;
    }

    uint64_t captures = 0;
    uint64_t depthImages = 0;
    uint64_t colorImages = 0;
    const auto start = std::chrono::steady_clock::now();
    while(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        k4a_capture_t capture = nullptr;
        const auto result = k4a_device_get_capture(device, &capture, 2000);
        if(result == K4A_WAIT_RESULT_TIMEOUT) continue;
        if(result != K4A_WAIT_RESULT_SUCCEEDED) {
            std::cerr << "k4a_device_get_capture failed\n";
            break;
        }
        ++captures;
        k4a_image_t depth = k4a_capture_get_depth_image(capture);
        if(depth) {
            ++depthImages;
            k4a_image_release(depth);
        }
        k4a_image_t color = k4a_capture_get_color_image(capture);
        if(color) {
            ++colorImages;
            k4a_image_release(color);
        }
        k4a_capture_release(capture);
    }

    k4a_device_stop_cameras(device);
    k4a_device_close(device);
    std::cout << "duration_s=" << seconds << '\n'
              << "captures=" << captures << '\n'
              << "depth_images=" << depthImages << '\n'
              << "color_images=" << colorImages << '\n';
    return captures > 0 && depthImages > 0 && colorImages > 0 ? 0 : 5;
}
