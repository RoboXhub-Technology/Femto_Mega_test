// Femto Mega acceptance test utility based on the public Orbbec SDK v2 C++ API.
// It does not modify firmware. The only writable device operation is the explicit
// "hdr --set on|off" command.

#include <libobsensor/ObSensor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Args {
    std::string command;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
};

struct ProfileInfo {
    OBSensorType sensorType = OB_SENSOR_UNKNOWN;
    uint32_t index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    OBFormat format = OB_FORMAT_UNKNOWN;
    std::string formatName;
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double hfov = 0.0;
    double vfov = 0.0;
    bool hasIntrinsics = false;
};

struct RoiStats {
    size_t total = 0;
    size_t valid = 0;
    double validRatio = 0.0;
    double medianMm = std::numeric_limits<double>::quiet_NaN();
};

struct FrameStats {
    uint64_t count = 0;
    uint64_t timeouts = 0;
    uint64_t estimatedDrops = 0;
    uint64_t firstIndex = 0;
    uint64_t lastIndex = 0;
    uint64_t firstTimestampUs = 0;
    uint64_t lastTimestampUs = 0;
};

std::string jsonEscape(const std::string &s) {
    std::ostringstream out;
    for(unsigned char c : s) {
        switch(c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if(c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
            }
            else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

std::string csvEscape(const std::string &s) {
    if(s.find_first_of(",\"\r\n") == std::string::npos) {
        return s;
    }
    std::string escaped = s;
    size_t pos = 0;
    while((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.insert(pos, 1, '"');
        pos += 2;
    }
    return '"' + escaped + '"';
}

std::string nowTag() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return out.str();
}

Args parseArgs(int argc, char **argv) {
    Args args;
    if(argc > 1) {
        args.command = argv[1];
    }
    for(int i = 2; i < argc; ++i) {
        std::string token = argv[i];
        if(token.rfind("--", 0) != 0) {
            throw std::runtime_error("Unexpected positional argument: " + token);
        }
        if(i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            args.values[token] = argv[++i];
        }
        else {
            args.flags.insert(token);
        }
    }
    return args;
}

std::string getString(const Args &args, const std::string &key, const std::string &fallback = "") {
    const auto it = args.values.find(key);
    return it == args.values.end() ? fallback : it->second;
}

int getInt(const Args &args, const std::string &key, int fallback) {
    const auto text = getString(args, key);
    return text.empty() ? fallback : std::stoi(text);
}

double getDouble(const Args &args, const std::string &key, double fallback) {
    const auto text = getString(args, key);
    return text.empty() ? fallback : std::stod(text);
}

fs::path outputDir(const Args &args, const std::string &prefix) {
    const auto requested = getString(args, "--out");
    fs::path out = requested.empty() ? fs::path("results") / (prefix + "_" + nowTag()) : fs::path(requested);
    fs::create_directories(out);
    return out;
}

std::string formatDouble(double value, int precision = 6) {
    if(!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::shared_ptr<ob::Device> openDevice(const Args &args, std::shared_ptr<ob::Context> &context) {
    context = std::make_shared<ob::Context>();
    auto devices = context->queryDeviceList();
    if(!devices || devices->getCount() == 0) {
        throw std::runtime_error("No Orbbec device found. Connect Femto Mega and retry.");
    }
    const int index = getInt(args, "--device-index", 0);
    if(index < 0 || static_cast<uint32_t>(index) >= devices->getCount()) {
        throw std::runtime_error("--device-index is outside the enumerated device list");
    }
    return devices->getDevice(static_cast<uint32_t>(index));
}

std::string sensorName(OBSensorType type) {
    return ob::TypeHelper::convertOBSensorTypeToString(type);
}

std::string formatName(OBFormat format) {
    return ob::TypeHelper::convertOBFormatTypeToString(format);
}

std::string modeName(uint32_t width, uint32_t height) {
    if(width == 1024 && height == 1024) return "WFOV unbinned";
    if(width == 512 && height == 512) return "WFOV binned";
    if(width == 640 && height == 576) return "NFOV unbinned";
    if(width == 320 && height == 288) return "NFOV binned";
    return "other";
}

std::pair<double, double> requiredFov(OBSensorType type, uint32_t width, uint32_t height) {
    if(type == OB_SENSOR_COLOR && width == 3840 && height == 2160) return {80.0, 51.0};
    if(type == OB_SENSOR_DEPTH) {
        if((width == 1024 && height == 1024) || (width == 512 && height == 512)) return {120.0, 120.0};
        if((width == 640 && height == 576) || (width == 320 && height == 288)) return {75.0, 65.0};
    }
    return {0.0, 0.0};
}

std::vector<ProfileInfo> enumerateVideoProfiles(const std::shared_ptr<ob::Device> &device, OBSensorType type) {
    std::vector<ProfileInfo> result;
    auto sensor = device->getSensor(type);
    if(!sensor) {
        return result;
    }
    auto list = sensor->getStreamProfileList();
    for(uint32_t i = 0; i < list->getCount(); ++i) {
        auto base = list->getProfile(i);
        if(!base || !base->is<ob::VideoStreamProfile>()) {
            continue;
        }
        auto profile = base->as<ob::VideoStreamProfile>();
        ProfileInfo p;
        p.sensorType = type;
        p.index = i;
        p.width = profile->getWidth();
        p.height = profile->getHeight();
        p.fps = profile->getFps();
        p.format = profile->getFormat();
        p.formatName = formatName(p.format);
        try {
            const auto intr = profile->getIntrinsic();
            p.fx = intr.fx;
            p.fy = intr.fy;
            p.cx = intr.cx;
            p.cy = intr.cy;
            p.hasIntrinsics = intr.fx > 0.0f && intr.fy > 0.0f;
            if(p.hasIntrinsics) {
                p.hfov = 2.0 * std::atan(static_cast<double>(intr.width) / (2.0 * intr.fx)) * 180.0 / kPi;
                p.vfov = 2.0 * std::atan(static_cast<double>(intr.height) / (2.0 * intr.fy)) * 180.0 / kPi;
            }
        }
        catch(const std::exception &) {
            p.hasIntrinsics = false;
        }
        result.push_back(p);
    }
    return result;
}

std::shared_ptr<ob::VideoStreamProfile> selectVideoProfile(const std::shared_ptr<ob::Device> &device, OBSensorType type, const Args &args) {
    auto sensor = device->getSensor(type);
    if(!sensor) {
        throw std::runtime_error("Requested sensor is not present on the device");
    }
    auto list = sensor->getStreamProfileList();
    const int requestedIndex = getInt(args, "--profile-index", -1);
    if(requestedIndex >= 0) {
        if(static_cast<uint32_t>(requestedIndex) >= list->getCount()) {
            throw std::runtime_error("--profile-index is outside the sensor profile list");
        }
        return list->getProfile(static_cast<uint32_t>(requestedIndex))->as<ob::VideoStreamProfile>();
    }

    const int width = getInt(args, "--width", -1);
    const int height = getInt(args, "--height", -1);
    const int fps = getInt(args, "--fps", -1);
    const std::string requestedFormat = getString(args, "--format");
    for(uint32_t i = 0; i < list->getCount(); ++i) {
        auto base = list->getProfile(i);
        if(!base || !base->is<ob::VideoStreamProfile>()) continue;
        auto p = base->as<ob::VideoStreamProfile>();
        if(width >= 0 && static_cast<int>(p->getWidth()) != width) continue;
        if(height >= 0 && static_cast<int>(p->getHeight()) != height) continue;
        if(fps >= 0 && static_cast<int>(p->getFps()) != fps) continue;
        if(!requestedFormat.empty() && formatName(p->getFormat()) != requestedFormat) continue;
        return p;
    }
    throw std::runtime_error("No stream profile matches the requested width/height/fps/format. Run survey first.");
}

RoiStats computeDepthRoi(const std::shared_ptr<ob::DepthFrame> &frame, int requestedRoi) {
    RoiStats stats;
    if(!frame || frame->getDataSize() < frame->getWidth() * frame->getHeight() * sizeof(uint16_t)) {
        return stats;
    }
    const int width = static_cast<int>(frame->getWidth());
    const int height = static_cast<int>(frame->getHeight());
    const int roi = std::max(1, std::min({requestedRoi, width, height}));
    const int x0 = (width - roi) / 2;
    const int y0 = (height - roi) / 2;
    const auto *pixels = reinterpret_cast<const uint16_t *>(frame->getData());
    const double scale = frame->getValueScale();
    std::vector<double> values;
    values.reserve(static_cast<size_t>(roi) * static_cast<size_t>(roi));
    for(int y = y0; y < y0 + roi; ++y) {
        for(int x = x0; x < x0 + roi; ++x) {
            const uint16_t raw = pixels[static_cast<size_t>(y) * width + x];
            ++stats.total;
            if(raw != 0) {
                ++stats.valid;
                values.push_back(raw * scale);
            }
        }
    }
    stats.validRatio = stats.total ? static_cast<double>(stats.valid) / stats.total : 0.0;
    if(!values.empty()) {
        const size_t mid = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
        stats.medianMm = values[mid];
        if(values.size() % 2 == 0) {
            const auto lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
            stats.medianMm = (lower + stats.medianMm) / 2.0;
        }
    }
    return stats;
}

void updateFrameStats(FrameStats &stats, const std::shared_ptr<ob::Frame> &frame) {
    const uint64_t index = frame->getIndex();
    const uint64_t ts = frame->getTimeStampUs();
    if(stats.count == 0) {
        stats.firstIndex = index;
        stats.firstTimestampUs = ts;
    }
    else if(index > stats.lastIndex + 1) {
        stats.estimatedDrops += index - stats.lastIndex - 1;
    }
    stats.lastIndex = index;
    stats.lastTimestampUs = ts;
    ++stats.count;
}

double measuredFps(const FrameStats &stats) {
    if(stats.count < 2 || stats.lastTimestampUs <= stats.firstTimestampUs) return 0.0;
    return (stats.count - 1) * 1000000.0 / static_cast<double>(stats.lastTimestampUs - stats.firstTimestampUs);
}

void saveFirstFrame(const fs::path &out, const std::shared_ptr<ob::Frame> &frame, const std::string &fmt) {
    std::string ext = ".bin";
    if(fmt == "MJPG" || fmt == "MJPEG") ext = ".jpg";
    if(frame->getType() == OB_FRAME_DEPTH) ext = ".raw";
    std::ofstream raw(out / ("first_frame" + ext), std::ios::binary);
    raw.write(reinterpret_cast<const char *>(frame->getData()), frame->getDataSize());
}

void writeDeviceJson(std::ostream &json, const std::shared_ptr<ob::Device> &device) {
    const auto info = device->getDeviceInfo();
    json << "  \"device\": {\n"
         << "    \"name\": \"" << jsonEscape(info->getName()) << "\",\n"
         << "    \"serial_number\": \"" << jsonEscape(info->getSerialNumber()) << "\",\n"
         << "    \"firmware_version\": \"" << jsonEscape(info->getFirmwareVersion()) << "\",\n"
         << "    \"hardware_version\": \"" << jsonEscape(info->getHardwareVersion()) << "\",\n"
         << "    \"connection_type\": \"" << jsonEscape(info->getConnectionType()) << "\",\n"
         << "    \"ip_address\": \"" << jsonEscape(info->getIpAddress()) << "\",\n"
         << "    \"vid\": " << info->getVid() << ",\n"
         << "    \"pid\": " << info->getPid() << "\n"
         << "  }";
}

bool profileExists(const std::vector<ProfileInfo> &profiles, int width, int height, int fps, std::vector<std::string> *formats = nullptr) {
    bool exists = false;
    for(const auto &p : profiles) {
        if(static_cast<int>(p.width) == width && static_cast<int>(p.height) == height && static_cast<int>(p.fps) == fps) {
            exists = true;
            if(formats) formats->push_back(p.formatName);
        }
    }
    if(formats) {
        std::sort(formats->begin(), formats->end());
        formats->erase(std::unique(formats->begin(), formats->end()), formats->end());
    }
    return exists;
}

void runSurvey(const Args &args) {
    const fs::path out = outputDir(args, "survey");
    std::shared_ptr<ob::Context> context;
    auto device = openDevice(args, context);

    const auto depthProfiles = enumerateVideoProfiles(device, OB_SENSOR_DEPTH);
    const auto colorProfiles = enumerateVideoProfiles(device, OB_SENSOR_COLOR);

    std::ofstream profilesCsv(out / "video_profiles.csv");
    profilesCsv << "sensor,profile_index,mode,width,height,fps,format,fx,fy,cx,cy,hfov_deg,vfov_deg,required_hfov_deg,required_vfov_deg,fov_pass\n";
    for(const auto &group : {depthProfiles, colorProfiles}) {
        for(const auto &p : group) {
            profilesCsv << csvEscape(sensorName(p.sensorType)) << ',' << p.index << ',' << csvEscape(modeName(p.width, p.height)) << ',' << p.width << ','
                        << p.height << ',' << p.fps << ',' << csvEscape(p.formatName) << ',';
            const auto fovRequirement = requiredFov(p.sensorType, p.width, p.height);
            if(p.hasIntrinsics) {
                profilesCsv << p.fx << ',' << p.fy << ',' << p.cx << ',' << p.cy << ',' << p.hfov << ',' << p.vfov;
            }
            else {
                profilesCsv << ",,,,,";
            }
            profilesCsv << ',';
            if(fovRequirement.first > 0.0) {
                profilesCsv << fovRequirement.first << ',' << fovRequirement.second << ','
                            << (p.hasIntrinsics && p.hfov >= fovRequirement.first && p.vfov >= fovRequirement.second ? "true" : "false");
            }
            else {
                profilesCsv << ",,";
            }
            profilesCsv << '\n';
        }
    }

    std::ofstream propertiesCsv(out / "supported_properties.csv");
    propertiesCsv << "index,id,name,type,permission\n";
    const int propertyCount = device->getSupportedPropertyCount();
    for(int i = 0; i < propertyCount; ++i) {
        const auto item = device->getSupportedProperty(static_cast<uint32_t>(i));
        propertiesCsv << i << ',' << static_cast<int>(item.id) << ',' << csvEscape(item.name ? item.name : "") << ',' << static_cast<int>(item.type) << ','
                      << static_cast<int>(item.permission) << '\n';
    }

    struct RequiredProfile { const char *mode; int width; int height; int fps; };
    const std::vector<RequiredProfile> requiredDepth = {
        {"WFOV unbinned", 1024, 1024, 5}, {"WFOV unbinned", 1024, 1024, 15},
        {"WFOV binned", 512, 512, 5}, {"WFOV binned", 512, 512, 15}, {"WFOV binned", 512, 512, 25}, {"WFOV binned", 512, 512, 30},
        {"NFOV unbinned", 640, 576, 5}, {"NFOV unbinned", 640, 576, 15}, {"NFOV unbinned", 640, 576, 25}, {"NFOV unbinned", 640, 576, 30},
        {"NFOV binned", 320, 288, 5}, {"NFOV binned", 320, 288, 15}, {"NFOV binned", 320, 288, 25}, {"NFOV binned", 320, 288, 30}
    };
    std::ofstream matrix(out / "contract_profile_matrix.csv");
    matrix << "item,mode,width,height,fps,available,formats\n";
    bool allDepth = true;
    for(const auto &r : requiredDepth) {
        std::vector<std::string> formats;
        const bool available = profileExists(depthProfiles, r.width, r.height, r.fps, &formats);
        allDepth = allDepth && available;
        std::ostringstream joined;
        for(size_t i = 0; i < formats.size(); ++i) {
            if(i) joined << ';';
            joined << formats[i];
        }
        matrix << "depth," << r.mode << ',' << r.width << ',' << r.height << ',' << r.fps << ',' << (available ? "true" : "false") << ','
               << csvEscape(joined.str()) << '\n';
    }
    std::vector<std::string> rgbFormats;
    const bool rgb4k30 = profileExists(colorProfiles, 3840, 2160, 30, &rgbFormats);
    std::ostringstream rgbJoined;
    for(size_t i = 0; i < rgbFormats.size(); ++i) {
        if(i) rgbJoined << ';';
        rgbJoined << rgbFormats[i];
    }
    matrix << "color,4K,3840,2160,30," << (rgb4k30 ? "true" : "false") << ',' << csvEscape(rgbJoined.str()) << '\n';

    bool hasAccel = false;
    bool hasGyro = false;
    std::ofstream imuProfiles(out / "imu_profiles.csv");
    imuProfiles << "sensor,profile_index,sample_rate,full_scale_range\n";
    auto sensorList = device->getSensorList();
    for(uint32_t s = 0; s < sensorList->getCount(); ++s) {
        const OBSensorType type = sensorList->getSensorType(s);
        if(type != OB_SENSOR_ACCEL && type != OB_SENSOR_GYRO) continue;
        hasAccel = hasAccel || type == OB_SENSOR_ACCEL;
        hasGyro = hasGyro || type == OB_SENSOR_GYRO;
        auto sensor = sensorList->getSensor(type);
        auto list = sensor->getStreamProfileList();
        for(uint32_t i = 0; i < list->getCount(); ++i) {
            auto base = list->getProfile(i);
            if(type == OB_SENSOR_ACCEL) {
                auto p = base->as<ob::AccelStreamProfile>();
                imuProfiles << "ACCEL," << i << ',' << csvEscape(ob::TypeHelper::convertOBIMUSampleRateTypeToString(p->getSampleRate())) << ','
                            << static_cast<int>(p->getFullScaleRange()) << '\n';
            }
            else {
                auto p = base->as<ob::GyroStreamProfile>();
                imuProfiles << "GYRO," << i << ',' << csvEscape(ob::TypeHelper::convertOBIMUSampleRateTypeToString(p->getSampleRate())) << ','
                            << static_cast<int>(p->getFullScaleRange()) << '\n';
            }
        }
    }

    const bool hdrRead = device->isPropertySupported(OB_PROP_COLOR_HDR_BOOL, OB_PERMISSION_READ);
    const bool hdrWrite = device->isPropertySupported(OB_PROP_COLOR_HDR_BOOL, OB_PERMISSION_WRITE);
    bool hdrCurrent = false;
    if(hdrRead) hdrCurrent = device->getBoolProperty(OB_PROP_COLOR_HDR_BOOL);

    std::ofstream json(out / "survey_summary.json");
    json << "{\n"
         << "  \"generated_at\": \"" << nowTag() << "\",\n"
         << "  \"sdk_version\": \"" << ob::Version::getMajor() << '.' << ob::Version::getMinor() << '.' << ob::Version::getPatch()
         << jsonEscape(ob::Version::getStageVersion() ? ob::Version::getStageVersion() : "") << "\",\n";
    writeDeviceJson(json, device);
    json << ",\n"
         << "  \"contract_capabilities\": {\n"
         << "    \"all_required_depth_profiles_listed\": " << (allDepth ? "true" : "false") << ",\n"
         << "    \"rgb_3840x2160_30_listed\": " << (rgb4k30 ? "true" : "false") << ",\n"
         << "    \"color_hdr_read_supported\": " << (hdrRead ? "true" : "false") << ",\n"
         << "    \"color_hdr_write_supported\": " << (hdrWrite ? "true" : "false") << ",\n"
         << "    \"color_hdr_current\": " << (hdrRead ? (hdrCurrent ? "true" : "false") : "null") << ",\n"
         << "    \"accelerometer_present\": " << (hasAccel ? "true" : "false") << ",\n"
         << "    \"gyroscope_present\": " << (hasGyro ? "true" : "false") << "\n"
         << "  },\n"
         << "  \"files\": [\"video_profiles.csv\", \"contract_profile_matrix.csv\", \"imu_profiles.csv\", \"supported_properties.csv\"]\n"
         << "}\n";
    std::cout << "Survey complete: " << out.string() << std::endl;
}

void runStream(const Args &args) {
    const std::string requestedSensor = getString(args, "--sensor", "depth");
    const OBSensorType sensorType = requestedSensor == "color" ? OB_SENSOR_COLOR : OB_SENSOR_DEPTH;
    const OBFrameType frameType = sensorType == OB_SENSOR_COLOR ? OB_FRAME_COLOR : OB_FRAME_DEPTH;
    const fs::path out = outputDir(args, requestedSensor);
    std::shared_ptr<ob::Context> context;
    auto device = openDevice(args, context);
    auto profile = selectVideoProfile(device, sensorType, args);

    auto config = std::make_shared<ob::Config>();
    config->enableStream(profile);
    config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION);
    ob::Pipeline pipeline(device);
    pipeline.start(config);

    const int seconds = getInt(args, "--seconds", 60);
    const int requestedFrames = getInt(args, "--frames", 0);
    if(seconds <= 0 && requestedFrames <= 0) {
        throw std::runtime_error("At least one of --seconds or --frames must be greater than zero");
    }
    const int roiSize = getInt(args, "--roi", 100);
    const double referenceMm = getDouble(args, "--reference-mm", 0.0);
    const double validThreshold = getDouble(args, "--valid-ratio", 0.95);
    FrameStats frameStats;
    std::vector<double> depthMedians;
    uint64_t validRoiFrames = 0;
    bool firstSaved = false;

    std::ofstream csv(out / "frames.csv");
    csv << "sequence,frame_index,timestamp_us,system_timestamp_us,width,height,format,roi_valid_ratio,roi_median_mm\n";
    const auto wallStart = std::chrono::steady_clock::now();
    while(true) {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        if(seconds > 0 && elapsed >= seconds) break;
        if(requestedFrames > 0 && static_cast<int>(frameStats.count) >= requestedFrames) break;
        auto frameset = pipeline.waitForFrameset(2000);
        if(!frameset) {
            ++frameStats.timeouts;
            continue;
        }
        auto frame = frameset->getFrame(frameType);
        if(!frame) continue;
        auto video = frame->as<ob::VideoFrame>();
        updateFrameStats(frameStats, frame);
        RoiStats roi;
        if(sensorType == OB_SENSOR_DEPTH) {
            roi = computeDepthRoi(frame->as<ob::DepthFrame>(), roiSize);
            if(std::isfinite(roi.medianMm)) depthMedians.push_back(roi.medianMm);
            if(roi.validRatio >= validThreshold) ++validRoiFrames;
        }
        csv << frameStats.count << ',' << frame->getIndex() << ',' << frame->getTimeStampUs() << ',' << frame->getSystemTimeStampUs() << ','
            << video->getWidth() << ',' << video->getHeight() << ',' << csvEscape(formatName(frame->getFormat())) << ',';
        if(sensorType == OB_SENSOR_DEPTH) csv << roi.validRatio << ',' << (std::isfinite(roi.medianMm) ? std::to_string(roi.medianMm) : "");
        else csv << ',';
        csv << '\n';
        if(!firstSaved && args.flags.count("--save-first-frame")) {
            saveFirstFrame(out, frame, formatName(frame->getFormat()));
            firstSaved = true;
        }
    }
    pipeline.stop();
    const double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    const double fps = measuredFps(frameStats);
    const double dmean = depthMedians.empty() ? std::numeric_limits<double>::quiet_NaN()
                                               : std::accumulate(depthMedians.begin(), depthMedians.end(), 0.0) / depthMedians.size();
    const double relativeError = referenceMm > 0.0 && std::isfinite(dmean) ? std::abs(dmean - referenceMm) / referenceMm * 100.0
                                                                          : std::numeric_limits<double>::quiet_NaN();
    const double stableFrameRatio = frameStats.count ? static_cast<double>(validRoiFrames) / frameStats.count : 0.0;

    std::ofstream json(out / "stream_summary.json");
    json << "{\n"
         << "  \"sensor\": \"" << requestedSensor << "\",\n"
         << "  \"profile\": {\"width\": " << profile->getWidth() << ", \"height\": " << profile->getHeight() << ", \"fps\": " << profile->getFps()
         << ", \"format\": \"" << jsonEscape(formatName(profile->getFormat())) << "\"},\n"
         << "  \"requested_duration_s\": " << seconds << ",\n"
         << "  \"requested_frames\": " << requestedFrames << ",\n"
         << "  \"wall_duration_s\": " << formatDouble(wallSeconds, 3) << ",\n"
         << "  \"frames_received\": " << frameStats.count << ",\n"
         << "  \"timeouts\": " << frameStats.timeouts << ",\n"
         << "  \"estimated_index_gaps\": " << frameStats.estimatedDrops << ",\n"
         << "  \"measured_fps_from_device_timestamps\": " << formatDouble(fps, 3);
    if(sensorType == OB_SENSOR_DEPTH) {
        json << ",\n  \"roi_size\": " << roiSize
             << ",\n  \"roi_valid_ratio_threshold\": " << formatDouble(validThreshold, 6)
             << ",\n  \"frames_meeting_valid_ratio\": " << validRoiFrames
             << ",\n  \"stable_frame_ratio\": " << formatDouble(stableFrameRatio, 6)
             << ",\n  \"meets_95_of_100_stability_rule\": " << (stableFrameRatio >= 0.95 ? "true" : "false")
             << ",\n  \"frames_with_depth_median\": " << depthMedians.size()
             << ",\n  \"mean_of_frame_medians_mm\": " << formatDouble(dmean, 3)
             << ",\n  \"reference_distance_mm\": " << (referenceMm > 0.0 ? formatDouble(referenceMm, 3) : "null")
             << ",\n  \"relative_error_percent\": " << formatDouble(relativeError, 6)
             << ",\n  \"meets_0_15_percent_limit\": " << (std::isfinite(relativeError) ? (relativeError <= 0.15 ? "true" : "false") : "null");
    }
    json << "\n}\n";
    std::cout << "Stream test complete: " << out.string() << std::endl;
}

void runImu(const Args &args) {
    const fs::path out = outputDir(args, "imu");
    std::shared_ptr<ob::Context> context;
    auto device = openDevice(args, context);
    if(!device->getSensor(OB_SENSOR_ACCEL) || !device->getSensor(OB_SENSOR_GYRO)) {
        throw std::runtime_error("Accelerometer or gyroscope sensor is not present");
    }
    auto config = std::make_shared<ob::Config>();
    config->enableAccelStream();
    config->enableGyroStream();
    config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ALL_TYPE_FRAME_REQUIRE);
    ob::Pipeline pipeline(device);
    pipeline.start(config);
    const int seconds = getInt(args, "--seconds", 60);
    FrameStats accelStats;
    FrameStats gyroStats;
    std::ofstream csv(out / "imu_frames.csv");
    csv << "sensor,frame_index,timestamp_us,system_timestamp_us,x,y,z,temperature_c\n";
    const auto start = std::chrono::steady_clock::now();
    while(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        auto frameset = pipeline.waitForFrameset(2000);
        if(!frameset) {
            ++accelStats.timeouts;
            ++gyroStats.timeouts;
            continue;
        }
        auto accelRaw = frameset->getFrame(OB_FRAME_ACCEL);
        if(accelRaw) {
            auto frame = accelRaw->as<ob::AccelFrame>();
            updateFrameStats(accelStats, frame);
            const auto v = frame->getValue();
            csv << "ACCEL," << frame->getIndex() << ',' << frame->getTimeStampUs() << ',' << frame->getSystemTimeStampUs() << ',' << v.x << ',' << v.y << ','
                << v.z << ',' << frame->getTemperature() << '\n';
        }
        auto gyroRaw = frameset->getFrame(OB_FRAME_GYRO);
        if(gyroRaw) {
            auto frame = gyroRaw->as<ob::GyroFrame>();
            updateFrameStats(gyroStats, frame);
            const auto v = frame->getValue();
            csv << "GYRO," << frame->getIndex() << ',' << frame->getTimeStampUs() << ',' << frame->getSystemTimeStampUs() << ',' << v.x << ',' << v.y << ','
                << v.z << ',' << frame->getTemperature() << '\n';
        }
    }
    pipeline.stop();
    std::ofstream json(out / "imu_summary.json");
    json << "{\n"
         << "  \"duration_s\": " << seconds << ",\n"
         << "  \"accelerometer\": {\"frames\": " << accelStats.count << ", \"measured_hz\": " << formatDouble(measuredFps(accelStats), 3)
         << ", \"estimated_index_gaps\": " << accelStats.estimatedDrops << "},\n"
         << "  \"gyroscope\": {\"frames\": " << gyroStats.count << ", \"measured_hz\": " << formatDouble(measuredFps(gyroStats), 3)
         << ", \"estimated_index_gaps\": " << gyroStats.estimatedDrops << "}\n"
         << "}\n";
    std::cout << "IMU test complete: " << out.string() << std::endl;
}

void runHdr(const Args &args) {
    const fs::path out = outputDir(args, "hdr");
    std::shared_ptr<ob::Context> context;
    auto device = openDevice(args, context);
    const bool canRead = device->isPropertySupported(OB_PROP_COLOR_HDR_BOOL, OB_PERMISSION_READ);
    const bool canWrite = device->isPropertySupported(OB_PROP_COLOR_HDR_BOOL, OB_PERMISSION_WRITE);
    const std::string action = getString(args, "--set", "query");
    bool before = false;
    bool after = false;
    if(canRead) before = device->getBoolProperty(OB_PROP_COLOR_HDR_BOOL);
    if(action != "query") {
        if(action != "on" && action != "off") throw std::runtime_error("--set must be query, on, or off");
        if(!canWrite) throw std::runtime_error("OB_PROP_COLOR_HDR_BOOL is not writable on this device/firmware");
        device->setBoolProperty(OB_PROP_COLOR_HDR_BOOL, action == "on");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if(canRead) after = device->getBoolProperty(OB_PROP_COLOR_HDR_BOOL);
    std::ofstream json(out / "hdr_summary.json");
    json << "{\n"
         << "  \"property\": \"OB_PROP_COLOR_HDR_BOOL\",\n"
         << "  \"read_supported\": " << (canRead ? "true" : "false") << ",\n"
         << "  \"write_supported\": " << (canWrite ? "true" : "false") << ",\n"
         << "  \"requested_action\": \"" << action << "\",\n"
         << "  \"value_before\": " << (canRead ? (before ? "true" : "false") : "null") << ",\n"
         << "  \"value_after\": " << (canRead ? (after ? "true" : "false") : "null") << "\n"
         << "}\n";
    std::cout << "HDR query/test complete: " << out.string() << std::endl;
}

std::shared_ptr<ob::VideoStreamProfile> highestPixelRateProfile(const std::shared_ptr<ob::Device> &device, OBSensorType type, int explicitIndex) {
    auto sensor = device->getSensor(type);
    if(!sensor) return nullptr;
    auto list = sensor->getStreamProfileList();
    if(explicitIndex >= 0) {
        if(static_cast<uint32_t>(explicitIndex) >= list->getCount()) throw std::runtime_error("Explicit profile index is out of range");
        return list->getProfile(static_cast<uint32_t>(explicitIndex))->as<ob::VideoStreamProfile>();
    }
    std::shared_ptr<ob::VideoStreamProfile> best;
    uint64_t bestRate = 0;
    for(uint32_t i = 0; i < list->getCount(); ++i) {
        auto base = list->getProfile(i);
        if(!base || !base->is<ob::VideoStreamProfile>()) continue;
        auto p = base->as<ob::VideoStreamProfile>();
        const uint64_t rate = static_cast<uint64_t>(p->getWidth()) * p->getHeight() * p->getFps();
        if(rate > bestRate) {
            bestRate = rate;
            best = p;
        }
    }
    return best;
}

void runLoad(const Args &args) {
    const fs::path out = outputDir(args, "power_load");
    std::shared_ptr<ob::Context> context;
    auto device = openDevice(args, context);
    auto depth = highestPixelRateProfile(device, OB_SENSOR_DEPTH, getInt(args, "--depth-profile-index", -1));
    auto color = highestPixelRateProfile(device, OB_SENSOR_COLOR, getInt(args, "--color-profile-index", -1));
    if(!depth || !color) throw std::runtime_error("Depth or color sensor/profile is unavailable");
    auto config = std::make_shared<ob::Config>();
    config->enableStream(depth);
    config->enableStream(color);
    bool imuEnabled = false;
    if(device->getSensor(OB_SENSOR_ACCEL) && device->getSensor(OB_SENSOR_GYRO)) {
        config->enableAccelStream();
        config->enableGyroStream();
        imuEnabled = true;
    }
    config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION);
    ob::Pipeline pipeline(device);
    pipeline.start(config);
    const int seconds = getInt(args, "--seconds", 600);
    std::map<OBFrameType, FrameStats> stats;
    const auto start = std::chrono::steady_clock::now();
    while(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        auto frameset = pipeline.waitForFrameset(2000);
        if(!frameset) continue;
        for(OBFrameType type : {OB_FRAME_DEPTH, OB_FRAME_COLOR, OB_FRAME_ACCEL, OB_FRAME_GYRO}) {
            auto frame = frameset->getFrame(type);
            if(frame) updateFrameStats(stats[type], frame);
        }
    }
    pipeline.stop();
    std::ofstream json(out / "power_load_summary.json");
    json << "{\n"
         << "  \"duration_s\": " << seconds << ",\n"
         << "  \"note\": \"This command creates camera workload only; watts must come from an external calibrated meter.\",\n"
         << "  \"depth_profile\": {\"width\": " << depth->getWidth() << ", \"height\": " << depth->getHeight() << ", \"fps\": " << depth->getFps()
         << ", \"format\": \"" << jsonEscape(formatName(depth->getFormat())) << "\"},\n"
         << "  \"color_profile\": {\"width\": " << color->getWidth() << ", \"height\": " << color->getHeight() << ", \"fps\": " << color->getFps()
         << ", \"format\": \"" << jsonEscape(formatName(color->getFormat())) << "\"},\n"
         << "  \"imu_enabled\": " << (imuEnabled ? "true" : "false") << ",\n"
         << "  \"frames\": {\"depth\": " << stats[OB_FRAME_DEPTH].count << ", \"color\": " << stats[OB_FRAME_COLOR].count
         << ", \"accel\": " << stats[OB_FRAME_ACCEL].count << ", \"gyro\": " << stats[OB_FRAME_GYRO].count << "}\n"
         << "}\n";
    std::cout << "Power-load run complete: " << out.string() << std::endl;
}

void printHelp() {
    std::cout
        << "Femto Mega acceptance utility\n\n"
        << "Commands:\n"
        << "  survey [--device-index 0] [--out DIR]\n"
        << "  stream --sensor depth|color [--profile-index N | --width W --height H --fps F [--format NAME]]\n"
        << "         [--seconds 60] [--frames N] [--roi 100] [--valid-ratio 0.95]\n"
        << "         [--reference-mm 1000] [--save-first-frame] [--out DIR]\n"
        << "  imu [--seconds 60] [--out DIR]\n"
        << "  hdr [--set query|on|off] [--out DIR]\n"
        << "  load [--seconds 600] [--depth-profile-index N] [--color-profile-index N] [--out DIR]\n\n"
        << "Run survey first. It writes profile indices and the contract capability matrix.\n";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const Args args = parseArgs(argc, argv);
        if(args.command.empty() || args.command == "help" || args.command == "--help") {
            printHelp();
            return args.command.empty() ? 1 : 0;
        }
        ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
        if(args.command == "survey") runSurvey(args);
        else if(args.command == "stream") runStream(args);
        else if(args.command == "imu") runImu(args);
        else if(args.command == "hdr") runHdr(args);
        else if(args.command == "load") runLoad(args);
        else throw std::runtime_error("Unknown command: " + args.command);
        return 0;
    }
    catch(const ob::Error &e) {
        std::cerr << "Orbbec SDK error\nfunction: " << e.getFunction() << "\nargs: " << e.getArgs() << "\nmessage: " << e.what() << "\nstatus: "
                  << e.getStatus() << "\ntype: " << e.getExceptionType() << std::endl;
        return 2;
    }
    catch(const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 3;
    }
}
