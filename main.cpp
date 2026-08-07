#define SDL_MAIN_HANDLED

#include <SDL.h>
#include "settings_bridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/base64.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kConnectTimeoutUs = 6'000'000;
constexpr int64_t kReadTimeoutUs = 5'000'000;
constexpr int64_t kFrameStallTimeoutUs = 5'000'000;
constexpr int kMaximumDelaySeconds = 30;
constexpr size_t kMaximumDelayedPacketBytes = 96 * 1024 * 1024;

std::atomic<bool> gRunning{true};

void handleSignal(int) { gRunning.store(false); }

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string hostFromRtspUrl(const std::string &url) {
    const auto scheme = url.find("://");
    const size_t authorityStart =
        scheme == std::string::npos ? 0 : scheme + 3;
    const auto pathStart = url.find('/', authorityStart);
    std::string authority = url.substr(
        authorityStart,
        pathStart == std::string::npos ? std::string::npos
                                       : pathStart - authorityStart);
    const auto credentialsEnd = authority.rfind('@');
    if (credentialsEnd != std::string::npos) {
        authority.erase(0, credentialsEnd + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        const auto closingBracket = authority.find(']');
        if (closingBracket != std::string::npos) {
            return authority.substr(1, closingBracket - 1);
        }
    }
    const auto port = authority.rfind(':');
    if (port != std::string::npos) authority.erase(port);
    return authority;
}

void copySetting(char *destination, size_t capacity, const std::string &value) {
    if (capacity == 0) return;
    std::snprintf(destination, capacity, "%s", value.c_str());
}

CameraMonitorSettings emptySettings() {
    CameraMonitorSettings settings{};
    settings.cameraCount = 1;
    settings.layout = CAMERA_MONITOR_LAYOUT_VERTICAL;
    for (int index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
        copySetting(settings.cameras[index].name,
                    CAMERA_MONITOR_NAME_CAPACITY,
                    "Camera " + std::to_string(index + 1));
    }
    return settings;
}

bool loadLegacyConfig(const std::string &path, CameraMonitorSettings &settings,
                      std::string &error) {
    std::ifstream input(path);
    if (!input) {
        error = "Cannot read camera configuration at " + path;
        return false;
    }

    settings = emptySettings();
    settings.cameraCount = 0;
    bool inStreams = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const std::string cleaned = trim(line);
        if (cleaned.empty()) continue;
        if (cleaned == "streams:") {
            inStreams = true;
            continue;
        }
        if (!inStreams) continue;

        const auto colon = cleaned.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(cleaned.substr(0, colon));
        const std::string value = trim(cleaned.substr(colon + 1));
        if (value.rfind("rtsp://", 0) != 0 && value.rfind("rtsps://", 0) != 0) {
            continue;
        }
        if (settings.cameraCount >= CAMERA_MONITOR_MAX_CAMERAS) continue;

        if (!key.empty()) key[0] = static_cast<char>(std::toupper(key[0]));
        const int index = settings.cameraCount++;
        copySetting(settings.cameras[index].name,
                    CAMERA_MONITOR_NAME_CAPACITY, key);
        copySetting(settings.cameras[index].url,
                    CAMERA_MONITOR_URL_CAPACITY, value);
    }

    if (settings.cameraCount == 0) {
        error = "Configuration does not contain an RTSP stream";
        return false;
    }
    settings.layout = settings.cameraCount == 2
                          ? CAMERA_MONITOR_LAYOUT_VERTICAL
                          : CAMERA_MONITOR_LAYOUT_GRID;
    return true;
}

std::string encodeSetting(const std::string &value) {
    std::vector<char> output(AV_BASE64_SIZE(value.size()));
    if (!av_base64_encode(output.data(), static_cast<int>(output.size()),
                          reinterpret_cast<const uint8_t *>(value.data()),
                          static_cast<int>(value.size()))) {
        return {};
    }
    return output.data();
}

bool decodeSetting(const std::string &value, std::string &decoded) {
    std::vector<uint8_t> output(value.size() * 3 / 4 + 4);
    const int size = av_base64_decode(output.data(), value.c_str(),
                                      static_cast<int>(output.size()));
    if (size < 0) return false;
    decoded.assign(reinterpret_cast<const char *>(output.data()), size);
    return true;
}

bool loadSavedSettings(const std::string &path, CameraMonitorSettings &settings) {
    std::ifstream input(path);
    if (!input) return false;

    CameraMonitorSettings loaded = emptySettings();
    std::string line;
    bool versionSeen = false;
    while (std::getline(input, line)) {
        if (line == "CAMERA_MONITOR_SETTINGS_V1" ||
            line == "CAMERA_MONITOR_SETTINGS_V2") {
            versionSeen = true;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "count") loaded.cameraCount = std::atoi(value.c_str());
        if (key == "layout") loaded.layout = std::atoi(value.c_str());
        for (int index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
            std::string decoded;
            if (key == "name" + std::to_string(index) &&
                decodeSetting(value, decoded)) {
                copySetting(loaded.cameras[index].name,
                            CAMERA_MONITOR_NAME_CAPACITY, decoded);
            }
            if (key == "url" + std::to_string(index) &&
                decodeSetting(value, decoded)) {
                copySetting(loaded.cameras[index].url,
                            CAMERA_MONITOR_URL_CAPACITY, decoded);
            }
            if (key == "delay" + std::to_string(index)) {
                loaded.cameras[index].delaySeconds = std::atoi(value.c_str());
            }
        }
    }

    if (!versionSeen || loaded.cameraCount < 1 ||
        loaded.cameraCount > CAMERA_MONITOR_MAX_CAMERAS ||
        loaded.layout < CAMERA_MONITOR_LAYOUT_VERTICAL ||
        loaded.layout > CAMERA_MONITOR_LAYOUT_HORIZONTAL) {
        return false;
    }
    for (int index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
        if (loaded.cameras[index].delaySeconds < 0 ||
            loaded.cameras[index].delaySeconds > kMaximumDelaySeconds) {
            return false;
        }
    }
    for (int index = 0; index < loaded.cameraCount; ++index) {
        const std::string url = loaded.cameras[index].url;
        if (url.rfind("rtsp://", 0) != 0 && url.rfind("rtsps://", 0) != 0) {
            return false;
        }
    }
    settings = loaded;
    return true;
}

bool saveSettings(const std::string &path,
                  const CameraMonitorSettings &settings) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << "CAMERA_MONITOR_SETTINGS_V2\n";
        output << "count=" << settings.cameraCount << '\n';
        output << "layout=" << settings.layout << '\n';
        for (int index = 0; index < CAMERA_MONITOR_MAX_CAMERAS; ++index) {
            output << "name" << index << '='
                   << encodeSetting(settings.cameras[index].name) << '\n';
            output << "url" << index << '='
                   << encodeSetting(settings.cameras[index].url) << '\n';
            output << "delay" << index << '='
                   << settings.cameras[index].delaySeconds << '\n';
        }
        if (!output) return false;
    }
    chmod(temporary.c_str(), 0600);
    if (std::rename(temporary.c_str(), path.c_str()) != 0) return false;
    chmod(path.c_str(), 0600);
    return true;
}

enum class StreamState { Connecting, Buffering, Live, Reconnecting, Stopped };
enum class StreamFailure {
    None,
    AllocateInput,
    OpenInput,
    StreamInfo,
    VideoStream,
    Decoder,
    DecoderContext,
    DecoderParameters,
    OpenDecoder,
    AllocateFrames,
    Read,
    BufferLimit,
    FrameStall,
};

const char *stateName(StreamState state) {
    switch (state) {
        case StreamState::Connecting: return "CONNECTING";
        case StreamState::Buffering: return "BUFFERING";
        case StreamState::Live: return "LIVE";
        case StreamState::Reconnecting: return "RETRYING";
        case StreamState::Stopped: return "STOPPED";
    }
    return "Unknown";
}

const char *failureName(StreamFailure failure) {
    switch (failure) {
        case StreamFailure::None: return "none";
        case StreamFailure::AllocateInput: return "allocate_input";
        case StreamFailure::OpenInput: return "open_input";
        case StreamFailure::StreamInfo: return "stream_info";
        case StreamFailure::VideoStream: return "video_stream";
        case StreamFailure::Decoder: return "decoder";
        case StreamFailure::DecoderContext: return "decoder_context";
        case StreamFailure::DecoderParameters: return "decoder_parameters";
        case StreamFailure::OpenDecoder: return "open_decoder";
        case StreamFailure::AllocateFrames: return "allocate_frames";
        case StreamFailure::Read: return "read";
        case StreamFailure::BufferLimit: return "buffer_limit";
        case StreamFailure::FrameStall: return "frame_stall";
    }
    return "unknown";
}

struct VideoFrame {
    int width = 0;
    int height = 0;
    int yPitch = 0;
    int uvPitch = 0;
    std::vector<uint8_t> pixels;
    uint64_t serial = 0;
    Clock::time_point receivedAt{};
};

class CameraStream {
public:
    CameraStream(std::string name, std::string url, int delaySeconds)
        : name_(std::move(name)),
          url_(std::move(url)),
          host_(hostFromRtspUrl(url_)),
          delaySeconds_(std::clamp(delaySeconds, 0, kMaximumDelaySeconds)),
          delayUs_(static_cast<int64_t>(delaySeconds_) * 1'000'000) {}

    ~CameraStream() { stop(); }

    CameraStream(const CameraStream &) = delete;
    CameraStream &operator=(const CameraStream &) = delete;

    void start() {
        if (worker_.joinable()) return;
        stopRequested_.store(false);
        worker_ = std::thread(&CameraStream::run, this);
    }

    void stop() {
        stopRequested_.store(true);
        if (worker_.joinable()) worker_.join();
        state_.store(StreamState::Stopped);
    }

    StreamState state() const { return state_.load(); }
    const std::string &host() const { return host_; }
    int delaySeconds() const { return delaySeconds_; }
    uint64_t frameCount() const { return frameCount_.load(); }
    uint64_t reconnectCount() const { return reconnectCount_.load(); }
    StreamFailure lastFailure() const { return lastFailure_.load(); }
    int lastFailureCode() const { return lastFailureCode_.load(); }

    bool latestFrame(VideoFrame &output, uint64_t previousSerial) const {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (latest_.serial == 0 || latest_.serial == previousSerial) return false;
        output = latest_;
        return true;
    }

    bool isFresh() const {
        std::lock_guard<std::mutex> lock(frameMutex_);
        return latest_.serial != 0 && Clock::now() - latest_.receivedAt < 3s;
    }

private:
    struct DelayedPacket {
        AVPacket *packet = nullptr;
        int64_t readyAtUs = 0;
        size_t bytes = 0;

        DelayedPacket(AVPacket *source, int64_t readyAt)
            : packet(av_packet_clone(source)),
              readyAtUs(readyAt),
              bytes(packet && packet->size > 0
                        ? static_cast<size_t>(packet->size)
                        : 0) {}

        ~DelayedPacket() {
            if (packet) av_packet_free(&packet);
        }

        DelayedPacket(const DelayedPacket &) = delete;
        DelayedPacket &operator=(const DelayedPacket &) = delete;

        DelayedPacket(DelayedPacket &&other) noexcept
            : packet(other.packet),
              readyAtUs(other.readyAtUs),
              bytes(other.bytes) {
            other.packet = nullptr;
            other.bytes = 0;
        }

        DelayedPacket &operator=(DelayedPacket &&other) noexcept {
            if (this == &other) return *this;
            if (packet) av_packet_free(&packet);
            packet = other.packet;
            readyAtUs = other.readyAtUs;
            bytes = other.bytes;
            other.packet = nullptr;
            other.bytes = 0;
            return *this;
        }
    };

    struct Connection {
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        AVBufferRef *hardwareDevice = nullptr;
        SwsContext *scaler = nullptr;
        AVFrame *decoded = nullptr;
        AVFrame *software = nullptr;
        AVPacket *packet = nullptr;
        AVPixelFormat hardwarePixelFormat = AV_PIX_FMT_NONE;
        int videoStream = -1;

        ~Connection() {
            if (packet) av_packet_free(&packet);
            if (software) av_frame_free(&software);
            if (decoded) av_frame_free(&decoded);
            if (scaler) sws_freeContext(scaler);
            if (codec) avcodec_free_context(&codec);
            if (hardwareDevice) av_buffer_unref(&hardwareDevice);
            if (format) avformat_close_input(&format);
        }
    };

    static int interruptCallback(void *opaque) {
        auto *stream = static_cast<CameraStream *>(opaque);
        if (stream->stopRequested_.load() || !gRunning.load()) return 1;
        const int64_t deadline = stream->deadlineUs_.load();
        return deadline > 0 && av_gettime_relative() > deadline;
    }

    static AVPixelFormat choosePixelFormat(AVCodecContext *context,
                                            const AVPixelFormat *formats) {
        auto *stream = static_cast<CameraStream *>(context->opaque);
        if (stream && stream->desiredHardwarePixelFormat_ != AV_PIX_FMT_NONE) {
            for (const AVPixelFormat *format = formats;
                 *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == stream->desiredHardwarePixelFormat_) return *format;
            }
        }
        return formats[0];
    }

    bool shouldRun() const {
        return gRunning.load() && !stopRequested_.load();
    }

    bool interruptibleWait(std::chrono::milliseconds duration) const {
        const auto end = Clock::now() + duration;
        while (shouldRun() && Clock::now() < end) std::this_thread::sleep_for(100ms);
        return shouldRun();
    }

    void setDeadline(int64_t timeoutUs) {
        deadlineUs_.store(av_gettime_relative() + timeoutUs);
    }

    void clearDeadline() { deadlineUs_.store(0); }

    bool fail(StreamFailure failure, int code = 0) {
        lastFailure_.store(failure);
        lastFailureCode_.store(code);
        return false;
    }

    bool openConnection(Connection &connection) {
        connection.format = avformat_alloc_context();
        if (!connection.format) return fail(StreamFailure::AllocateInput);
        connection.format->interrupt_callback = {interruptCallback, this};
        connection.format->flags |= AVFMT_FLAG_NOBUFFER;

        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "max_delay", "0", 0);
        av_dict_set(&options, "reorder_queue_size", "0", 0);
        av_dict_set(&options, "buffer_size", "1048576", 0);
        av_dict_set(&options, "probesize", "65536", 0);
        av_dict_set(&options, "analyzeduration", "750000", 0);
        av_dict_set(&options, "timeout", "5000000", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);

        setDeadline(kConnectTimeoutUs);
        const int opened = avformat_open_input(&connection.format, url_.c_str(), nullptr,
                                                &options);
        av_dict_free(&options);
        if (opened < 0) {
            clearDeadline();
            return fail(StreamFailure::OpenInput, opened);
        }

        setDeadline(kConnectTimeoutUs);
        const int streamInfo =
            avformat_find_stream_info(connection.format, nullptr);
        if (streamInfo < 0) {
            clearDeadline();
            return fail(StreamFailure::StreamInfo, streamInfo);
        }
        clearDeadline();

        connection.videoStream = av_find_best_stream(
            connection.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (connection.videoStream < 0) {
            return fail(StreamFailure::VideoStream, connection.videoStream);
        }

        const AVCodecParameters *parameters =
            connection.format->streams[connection.videoStream]->codecpar;
        const AVCodec *decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) return fail(StreamFailure::Decoder);

        connection.codec = avcodec_alloc_context3(decoder);
        if (!connection.codec) return fail(StreamFailure::DecoderContext);
        const int copiedParameters =
            avcodec_parameters_to_context(connection.codec, parameters);
        if (copiedParameters < 0) {
            return fail(StreamFailure::DecoderParameters, copiedParameters);
        }

        connection.codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        connection.codec->thread_count = 0;
        connection.codec->opaque = this;

        desiredHardwarePixelFormat_ = AV_PIX_FMT_NONE;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig *hardware = avcodec_get_hw_config(decoder, index);
            if (!hardware) break;
            if ((hardware->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                hardware->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
                if (av_hwdevice_ctx_create(&connection.hardwareDevice,
                                           AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr,
                                           nullptr, 0) == 0) {
                    desiredHardwarePixelFormat_ = hardware->pix_fmt;
                    connection.hardwarePixelFormat = hardware->pix_fmt;
                    connection.codec->hw_device_ctx =
                        av_buffer_ref(connection.hardwareDevice);
                    connection.codec->get_format = choosePixelFormat;
                }
                break;
            }
        }

        const int openedDecoder = avcodec_open2(connection.codec, decoder, nullptr);
        if (openedDecoder < 0) {
            return fail(StreamFailure::OpenDecoder, openedDecoder);
        }

        connection.decoded = av_frame_alloc();
        connection.software = av_frame_alloc();
        connection.packet = av_packet_alloc();
        if (!connection.decoded || !connection.software || !connection.packet) {
            return fail(StreamFailure::AllocateFrames);
        }
        return true;
    }

    void publishFrame(Connection &connection, AVFrame *decoded) {
        AVFrame *source = decoded;
        if (connection.hardwarePixelFormat != AV_PIX_FMT_NONE &&
            decoded->format == connection.hardwarePixelFormat) {
            av_frame_unref(connection.software);
            if (av_hwframe_transfer_data(connection.software, decoded, 0) < 0) return;
            source = connection.software;
        }

        if (source->width <= 0 || source->height <= 0) return;
        const int width = source->width;
        const int height = source->height;
        const int uvWidth = (width + 1) / 2;
        const int uvHeight = (height + 1) / 2;

        VideoFrame frame;
        frame.width = width;
        frame.height = height;
        frame.yPitch = width;
        frame.uvPitch = uvWidth;
        frame.pixels.resize(static_cast<size_t>(width) * height +
                            static_cast<size_t>(uvWidth) * uvHeight * 2);

        uint8_t *destinations[4] = {
            frame.pixels.data(),
            frame.pixels.data() + static_cast<size_t>(width) * height,
            frame.pixels.data() + static_cast<size_t>(width) * height +
                static_cast<size_t>(uvWidth) * uvHeight,
            nullptr};
        int destinationLinesizes[4] = {width, uvWidth, uvWidth, 0};

        connection.scaler = sws_getCachedContext(
            connection.scaler, width, height,
            static_cast<AVPixelFormat>(source->format), width, height,
            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!connection.scaler) return;

        if (sws_scale(connection.scaler, source->data, source->linesize, 0, height,
                      destinations, destinationLinesizes) <= 0) {
            return;
        }

        frame.receivedAt = Clock::now();
        lastDecodedFrameUs_.store(av_gettime_relative());
        frame.serial = frameCount_.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            latest_ = std::move(frame);
        }
        state_.store(StreamState::Live);
        lastFailure_.store(StreamFailure::None);
        lastFailureCode_.store(0);
    }

    void decodePacket(Connection &connection, AVPacket *packet) {
        if (avcodec_send_packet(connection.codec, packet) < 0) return;
        while (shouldRun()) {
            const int received =
                avcodec_receive_frame(connection.codec, connection.decoded);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            if (received < 0) break;
            publishFrame(connection, connection.decoded);
            av_frame_unref(connection.decoded);
        }
    }

    void decodeConnection(Connection &connection) {
        std::deque<DelayedPacket> delayedPackets;
        size_t delayedBytes = 0;
        const uint64_t startingFrameCount = frameCount_.load();
        lastDecodedFrameUs_.store(av_gettime_relative());
        if (delaySeconds_ > 0) state_.store(StreamState::Buffering);

        while (shouldRun()) {
            setDeadline(kReadTimeoutUs);
            const int read = av_read_frame(connection.format, connection.packet);
            clearDeadline();
            if (read < 0) {
                fail(StreamFailure::Read, read);
                break;
            }

            if (connection.packet->stream_index == connection.videoStream) {
                if (delayUs_ == 0) {
                    decodePacket(connection, connection.packet);
                } else {
                    delayedPackets.emplace_back(
                        connection.packet, av_gettime_relative() + delayUs_);
                    if (!delayedPackets.back().packet) {
                        av_packet_unref(connection.packet);
                        break;
                    }
                    delayedBytes += delayedPackets.back().bytes;
                }
            }
            av_packet_unref(connection.packet);

            const int64_t now = av_gettime_relative();
            while (!delayedPackets.empty() &&
                   delayedPackets.front().readyAtUs <= now && shouldRun()) {
                decodePacket(connection, delayedPackets.front().packet);
                delayedBytes -= delayedPackets.front().bytes;
                delayedPackets.pop_front();
            }

            if (delayedBytes > kMaximumDelayedPacketBytes) {
                fail(StreamFailure::BufferLimit);
                break;
            }

            const bool hasPublished = frameCount_.load() > startingFrameCount;
            const int64_t stallTimeout = hasPublished
                                             ? kFrameStallTimeoutUs
                                             : delayUs_ + kFrameStallTimeoutUs;
            if (now - lastDecodedFrameUs_.load() > stallTimeout) {
                fail(StreamFailure::FrameStall);
                break;
            }
        }
    }

    void run() {
        bool firstAttempt = true;
        while (shouldRun()) {
            state_.store(firstAttempt ? StreamState::Connecting
                                      : StreamState::Reconnecting);
            if (!firstAttempt) reconnectCount_.fetch_add(1);

            {
                Connection connection;
                if (openConnection(connection)) decodeConnection(connection);
            }

            desiredHardwarePixelFormat_ = AV_PIX_FMT_NONE;
            firstAttempt = false;
            if (!interruptibleWait(1s)) break;
        }
        state_.store(StreamState::Stopped);
    }

    std::string name_;
    std::string url_;
    std::string host_;
    int delaySeconds_ = 0;
    int64_t delayUs_ = 0;
    mutable std::mutex frameMutex_;
    VideoFrame latest_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<StreamState> state_{StreamState::Connecting};
    std::atomic<int64_t> deadlineUs_{0};
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> reconnectCount_{0};
    std::atomic<int64_t> lastDecodedFrameUs_{0};
    std::atomic<StreamFailure> lastFailure_{StreamFailure::None};
    std::atomic<int> lastFailureCode_{0};
    AVPixelFormat desiredHardwarePixelFormat_ = AV_PIX_FMT_NONE;
};

struct TextureState {
    SDL_Texture *texture = nullptr;
    int width = 0;
    int height = 0;
    uint64_t serial = 0;

    ~TextureState() {
        if (texture) SDL_DestroyTexture(texture);
    }
};

SDL_Rect fittedRect(int contentWidth, int contentHeight, const SDL_Rect &bounds) {
    if (contentWidth <= 0 || contentHeight <= 0) return bounds;
    const double contentAspect = static_cast<double>(contentWidth) / contentHeight;
    const double boundsAspect = static_cast<double>(bounds.w) / bounds.h;
    SDL_Rect result = bounds;
    if (contentAspect > boundsAspect) {
        result.h = static_cast<int>(bounds.w / contentAspect);
        result.y += (bounds.h - result.h) / 2;
    } else {
        result.w = static_cast<int>(bounds.h * contentAspect);
        result.x += (bounds.w - result.w) / 2;
    }
    return result;
}

bool updateTexture(SDL_Renderer *renderer, TextureState &texture,
                   CameraStream &stream) {
    VideoFrame frame;
    if (!stream.latestFrame(frame, texture.serial)) return false;

    if (!texture.texture || texture.width != frame.width ||
        texture.height != frame.height) {
        if (texture.texture) SDL_DestroyTexture(texture.texture);
        texture.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            frame.width, frame.height);
        if (!texture.texture) return false;
        texture.width = frame.width;
        texture.height = frame.height;
    }

    const uint8_t *y = frame.pixels.data();
    const uint8_t *u = y + static_cast<size_t>(frame.yPitch) * frame.height;
    const uint8_t *v = u + static_cast<size_t>(frame.uvPitch) *
                              ((frame.height + 1) / 2);
    if (SDL_UpdateYUVTexture(texture.texture, nullptr, y, frame.yPitch,
                             u, frame.uvPitch, v, frame.uvPitch) == 0) {
        texture.serial = frame.serial;
        return true;
    }
    return false;
}

void statusColour(const CameraStream &stream, Uint8 &red, Uint8 &green,
                  Uint8 &blue) {
    if (stream.state() == StreamState::Live && stream.isFresh()) {
        red = 50;
        green = 205;
        blue = 115;
    } else if (stream.state() == StreamState::Stopped) {
        red = 120;
        green = 120;
        blue = 120;
    } else {
        red = 245;
        green = 166;
        blue = 35;
    }
}

std::array<uint8_t, 7> glyph(char character) {
    switch (character) {
        case 'A': return {14, 17, 17, 31, 17, 17, 17};
        case 'B': return {30, 17, 17, 30, 17, 17, 30};
        case 'C': return {14, 17, 16, 16, 16, 17, 14};
        case 'D': return {30, 17, 17, 17, 17, 17, 30};
        case 'E': return {31, 16, 16, 30, 16, 16, 31};
        case 'F': return {31, 16, 16, 30, 16, 16, 16};
        case 'G': return {14, 17, 16, 23, 17, 17, 15};
        case 'H': return {17, 17, 17, 31, 17, 17, 17};
        case 'I': return {31, 4, 4, 4, 4, 4, 31};
        case 'J': return {7, 2, 2, 2, 18, 18, 12};
        case 'K': return {17, 18, 20, 24, 20, 18, 17};
        case 'L': return {16, 16, 16, 16, 16, 16, 31};
        case 'M': return {17, 27, 21, 21, 17, 17, 17};
        case 'N': return {17, 25, 21, 19, 17, 17, 17};
        case 'O': return {14, 17, 17, 17, 17, 17, 14};
        case 'P': return {30, 17, 17, 30, 16, 16, 16};
        case 'Q': return {14, 17, 17, 17, 21, 18, 13};
        case 'R': return {30, 17, 17, 30, 20, 18, 17};
        case 'S': return {15, 16, 16, 14, 1, 1, 30};
        case 'T': return {31, 4, 4, 4, 4, 4, 4};
        case 'U': return {17, 17, 17, 17, 17, 17, 14};
        case 'V': return {17, 17, 17, 17, 17, 10, 4};
        case 'W': return {17, 17, 17, 21, 21, 21, 10};
        case 'X': return {17, 17, 10, 4, 10, 17, 17};
        case 'Y': return {17, 17, 10, 4, 4, 4, 4};
        case 'Z': return {31, 1, 2, 4, 8, 16, 31};
        case '0': return {14, 17, 19, 21, 25, 17, 14};
        case '1': return {4, 12, 4, 4, 4, 4, 14};
        case '2': return {14, 17, 1, 2, 4, 8, 31};
        case '3': return {30, 1, 1, 14, 1, 1, 30};
        case '4': return {2, 6, 10, 18, 31, 2, 2};
        case '5': return {31, 16, 16, 30, 1, 1, 30};
        case '6': return {14, 16, 16, 30, 17, 17, 14};
        case '7': return {31, 1, 2, 4, 8, 8, 8};
        case '8': return {14, 17, 17, 14, 17, 17, 14};
        case '9': return {14, 17, 17, 15, 1, 1, 14};
        case '.': return {0, 0, 0, 0, 0, 4, 4};
        case '-': return {0, 0, 0, 31, 0, 0, 0};
        case '_': return {0, 0, 0, 0, 0, 0, 31};
        default: return {0, 0, 0, 0, 0, 0, 0};
    }
}

int textWidth(const std::string &text, int scale) {
    if (text.empty()) return 0;
    return static_cast<int>(text.size()) * 6 * scale - scale;
}

void drawText(SDL_Renderer *renderer, const std::string &text, int x, int y,
              int scale) {
    SDL_SetRenderDrawColor(renderer, 245, 247, 250, 255);
    int cursor = x;
    for (char character : text) {
        const auto rows = glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((rows[row] & (1 << (4 - column))) == 0) continue;
                SDL_Rect pixel{cursor + column * scale, y + row * scale,
                               scale, scale};
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
        cursor += 6 * scale;
    }
}

void renderStatusOverlay(SDL_Renderer *renderer, CameraStream &stream,
                         const std::string &cameraName,
                         const SDL_Rect &bounds) {
    const int scale = bounds.w < 500 ? 1 : 2;
    const StreamState currentState = stream.state();
    const bool fresh = stream.isFresh();
    const bool warning = currentState != StreamState::Stopped &&
                         !(currentState == StreamState::Live && fresh);
    const std::string displayedState =
        currentState == StreamState::Live && !fresh ? "STALLED"
                                                    : stateName(currentState);
    std::string displayName = cameraName;
    std::transform(displayName.begin(), displayName.end(), displayName.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    if (displayName.size() > 20) displayName = displayName.substr(0, 19) + ".";
    std::string title = displayName + "  " + displayedState;
    if (stream.delaySeconds() > 0) {
        title += "  " + std::to_string(stream.delaySeconds()) + "S DELAY";
    }
    const std::string detail = warning ? stream.host() : std::string{};
    const int labelWidth = std::max(textWidth(title, scale),
                                    textWidth(detail, scale));
    const int lineHeight = 7 * scale;
    const int backgroundHeight = detail.empty() ? lineHeight + 18
                                                 : lineHeight * 2 + 24;
    SDL_Rect background{bounds.x + 10,
                        bounds.y + bounds.h - backgroundHeight - 10,
                        std::min(bounds.w - 20, labelWidth + 44),
                        backgroundHeight};
    SDL_SetRenderDrawColor(renderer, 5, 7, 10, 185);
    SDL_RenderFillRect(renderer, &background);

    Uint8 red = 0, green = 0, blue = 0;
    statusColour(stream, red, green, blue);
    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
    const int dotSize = 5 * scale;
    SDL_Rect dot{background.x + 10, background.y + 9, dotSize, dotSize};
    SDL_RenderFillRect(renderer, &dot);
    const int textX = background.x + 20 + dotSize;
    drawText(renderer, title, textX, background.y + 8, scale);
    if (!detail.empty()) {
        drawText(renderer, detail, textX,
                 background.y + 13 + lineHeight, scale);
    }
}

void renderStream(SDL_Renderer *renderer, TextureState &texture,
                  CameraStream &stream, const std::string &cameraName,
                  const SDL_Rect &bounds) {
    SDL_SetRenderDrawColor(renderer, 12, 14, 17, 255);
    SDL_RenderFillRect(renderer, &bounds);

    if (texture.texture) {
        const SDL_Rect destination = fittedRect(texture.width, texture.height, bounds);
        SDL_RenderCopy(renderer, texture.texture, nullptr, &destination);
    }

    renderStatusOverlay(renderer, stream, cameraName, bounds);
}

struct ActiveCamera {
    std::string name;
    std::unique_ptr<CameraStream> stream;
    std::unique_ptr<TextureState> texture;
};

void stopCameras(std::vector<ActiveCamera> &cameras) {
    for (auto &camera : cameras) camera.stream->stop();
    cameras.clear();
}

void startCameras(const CameraMonitorSettings &settings,
                  std::vector<ActiveCamera> &cameras) {
    stopCameras(cameras);
    for (int index = 0; index < settings.cameraCount; ++index) {
        ActiveCamera camera;
        camera.name = settings.cameras[index].name;
        camera.stream = std::make_unique<CameraStream>(
            camera.name, settings.cameras[index].url,
            settings.cameras[index].delaySeconds);
        camera.texture = std::make_unique<TextureState>();
        camera.stream->start();
        cameras.push_back(std::move(camera));
    }
}

struct LayoutGeometry {
    int columns = 1;
    int rows = 1;
};

LayoutGeometry layoutGeometry(const CameraMonitorSettings &settings) {
    LayoutGeometry geometry;
    if (settings.layout == CAMERA_MONITOR_LAYOUT_VERTICAL) {
        geometry.rows = settings.cameraCount;
    } else if (settings.layout == CAMERA_MONITOR_LAYOUT_HORIZONTAL) {
        geometry.columns = settings.cameraCount;
    } else {
        geometry.columns = settings.cameraCount <= 1 ? 1 : 2;
        geometry.rows = settings.cameraCount <= 2 ? 1 : 2;
    }
    return geometry;
}

SDL_Rect cameraBounds(int index, const LayoutGeometry &geometry,
                      int width, int height) {
    const int column = index % geometry.columns;
    const int row = index / geometry.columns;
    const int x1 = column * width / geometry.columns;
    const int x2 = (column + 1) * width / geometry.columns;
    const int y1 = row * height / geometry.rows;
    const int y2 = (row + 1) * height / geometry.rows;
    return SDL_Rect{x1, y1, x2 - x1, y2 - y1};
}

void applyWindowLayout(SDL_Window *window,
                       const CameraMonitorSettings &settings,
                       bool resizeWindow) {
    const LayoutGeometry geometry = layoutGeometry(settings);
    const double aspect = static_cast<double>(geometry.columns * 16) /
                          static_cast<double>(geometry.rows * 9);
    configureMacWindowAspect(window, geometry.columns * 16.0,
                             geometry.rows * 9.0);

    const int minimumWidth = geometry.columns * 240;
    const int minimumHeight = geometry.rows * 135;
    SDL_SetWindowMinimumSize(window, minimumWidth, minimumHeight);
    if (!resizeWindow) return;

    int oldWidth = 720, oldHeight = 405;
    SDL_GetWindowSize(window, &oldWidth, &oldHeight);
    const double area = std::max(1, oldWidth) * std::max(1, oldHeight);
    int width = static_cast<int>(std::lround(std::sqrt(area * aspect)));
    int height = static_cast<int>(std::lround(width / aspect));

    const int display = SDL_GetWindowDisplayIndex(window);
    SDL_Rect usable{};
    if (display >= 0 && SDL_GetDisplayUsableBounds(display, &usable) == 0) {
        const double scale = std::min(
            1.0, std::min((usable.w * 0.9) / std::max(1, width),
                          (usable.h * 0.85) / std::max(1, height)));
        width = static_cast<int>(std::lround(width * scale));
        height = static_cast<int>(std::lround(height * scale));
    }

    width = std::max(minimumWidth, width);
    height = std::max(minimumHeight, height);
    if (static_cast<double>(width) / height > aspect) {
        width = static_cast<int>(std::lround(height * aspect));
    } else {
        height = static_cast<int>(std::lround(width / aspect));
    }
    SDL_SetWindowSize(window, width, height);
}

std::string stateDirectory() {
    char *path = SDL_GetPrefPath(nullptr, "Camera Monitor");
    if (!path) return "/private/tmp/";
    std::string result(path);
    SDL_free(path);
    return result;
}

struct WindowState {
    int x = SDL_WINDOWPOS_CENTERED;
    int y = SDL_WINDOWPOS_CENTERED;
    int width = 720;
    int height = 810;
    bool alwaysOnTop = true;
};

WindowState loadWindowState(const std::string &directory) {
    WindowState state;
    std::ifstream input(directory + "window-state.txt");
    int top = 1;
    if (input >> state.x >> state.y >> state.width >> state.height >> top) {
        state.width = std::max(240, state.width);
        state.height = std::max(135, state.height);
        state.alwaysOnTop = top != 0;
    }
    return state;
}

void saveWindowState(const std::string &directory, SDL_Window *window,
                     bool alwaysOnTop) {
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &width, &height);
    std::ofstream output(directory + "window-state.txt", std::ios::trunc);
    if (output) output << x << ' ' << y << ' ' << width << ' ' << height << ' '
                       << (alwaysOnTop ? 1 : 0) << '\n';
}

int acquireSingleInstanceLock(const std::string &directory) {
    const std::string path = directory + "instance.lock";
    const int descriptor = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0) return -1;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

int headlessTest(const CameraMonitorSettings &settings, int seconds) {
    std::vector<std::unique_ptr<CameraStream>> streams;
    for (int index = 0; index < settings.cameraCount; ++index) {
        auto stream = std::make_unique<CameraStream>(
            settings.cameras[index].name, settings.cameras[index].url,
            settings.cameras[index].delaySeconds);
        stream->start();
        streams.push_back(std::move(stream));
    }

    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (gRunning.load() && Clock::now() < end) std::this_thread::sleep_for(100ms);

    bool success = !streams.empty();
    for (size_t index = 0; index < streams.size(); ++index) {
        streams[index]->stop();
        success = success && streams[index]->frameCount() > 0;
        if (index > 0) std::cout << ' ';
        std::cout << "camera" << (index + 1)
                  << "_frames=" << streams[index]->frameCount()
                  << " camera" << (index + 1)
                  << "_reconnects=" << streams[index]->reconnectCount();
        if (streams[index]->frameCount() == 0) {
            std::cout << " camera" << (index + 1)
                      << "_error=" << failureName(streams[index]->lastFailure())
                      << ':' << streams[index]->lastFailureCode();
        }
    }
    std::cout << '\n';
    return success ? 0 : 1;
}

int selfTest() {
    CameraMonitorSettings source = emptySettings();
    source.cameraCount = CAMERA_MONITOR_MAX_CAMERAS;
    source.layout = CAMERA_MONITOR_LAYOUT_GRID;
    for (int index = 0; index < source.cameraCount; ++index) {
        copySetting(source.cameras[index].name, CAMERA_MONITOR_NAME_CAPACITY,
                    "Test Camera " + std::to_string(index + 1));
        copySetting(source.cameras[index].url, CAMERA_MONITOR_URL_CAPACITY,
                    "rtsp://192.0.2." + std::to_string(index + 10) +
                        ":554/stream");
        constexpr int testDelays[] = {0, 2, 5, 30};
        source.cameras[index].delaySeconds = testDelays[index];
    }

    const std::string path = "/private/tmp/camera-monitor-self-test-" +
                             std::to_string(getpid()) + ".conf";
    bool success = saveSettings(path, source);
    CameraMonitorSettings loaded = emptySettings();
    success = success && loadSavedSettings(path, loaded);
    success = success && loaded.cameraCount == source.cameraCount;
    success = success && loaded.layout == source.layout;
    for (int index = 0; index < source.cameraCount; ++index) {
        success = success &&
                  std::strcmp(loaded.cameras[index].name,
                              source.cameras[index].name) == 0;
        success = success &&
                  std::strcmp(loaded.cameras[index].url,
                              source.cameras[index].url) == 0;
        success = success && loaded.cameras[index].delaySeconds ==
                                 source.cameras[index].delaySeconds;
    }

    struct stat fileStatus {};
    success = success && stat(path.c_str(), &fileStatus) == 0;
    success = success && (fileStatus.st_mode & 0777) == 0600;
    std::remove(path.c_str());

    std::string credentialTestUrl = "rtsp://";
    const char *testUser = getpid() >= 0 ? "viewer" : "unused";
    credentialTestUrl.append(testUser);
    credentialTestUrl.push_back(':');
    credentialTestUrl.append("example");
    credentialTestUrl.push_back('@');
    credentialTestUrl.append("198.51.100.10:554/stream");
    success = success &&
              hostFromRtspUrl(credentialTestUrl) == "198.51.100.10";
    for (int count = 1; count <= CAMERA_MONITOR_MAX_CAMERAS; ++count) {
        for (int layout = CAMERA_MONITOR_LAYOUT_VERTICAL;
             layout <= CAMERA_MONITOR_LAYOUT_HORIZONTAL; ++layout) {
            CameraMonitorSettings geometrySettings = emptySettings();
            geometrySettings.cameraCount = count;
            geometrySettings.layout = layout;
            const LayoutGeometry geometry = layoutGeometry(geometrySettings);
            success = success && geometry.columns >= 1 && geometry.rows >= 1;
            success = success && geometry.columns * geometry.rows >= count;
            for (int index = 0; index < count; ++index) {
                const SDL_Rect bounds = cameraBounds(index, geometry,
                                                     geometry.columns * 160,
                                                     geometry.rows * 90);
                success = success && bounds.w == 160 && bounds.h == 90;
            }
        }
    }

    std::cout << (success ? "self-test passed\n" : "self-test failed\n");
    return success ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();

    std::string configPath;
    int testSeconds = 0;
    int testDelaySeconds = -1;
    bool checkConfig = false;
    bool showSettingsOnLaunch = false;
    bool runSelfTest = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) {
            configPath = argv[++index];
        } else if (argument == "--headless-test" && index + 1 < argc) {
            testSeconds = std::max(1, std::atoi(argv[++index]));
        } else if (argument == "--delay" && index + 1 < argc) {
            testDelaySeconds = std::clamp(std::atoi(argv[++index]), 0,
                                          kMaximumDelaySeconds);
        } else if (argument == "--check-config") {
            checkConfig = true;
        } else if (argument == "--settings") {
            showSettingsOnLaunch = true;
        } else if (argument == "--self-test") {
            runSelfTest = true;
        } else {
            std::cerr << "Usage: camera-monitor [--config PATH] "
                         "[--check-config | --headless-test SECONDS "
                         "[--delay SECONDS] | --settings "
                         "| --self-test]\n";
            return 2;
        }
    }

    if (runSelfTest) return selfTest();

    if (checkConfig || testSeconds > 0) {
        CameraMonitorSettings testSettings = emptySettings();
        std::string configError;
        bool loaded = false;
        if (!configPath.empty()) {
            loaded = loadLegacyConfig(configPath, testSettings, configError);
        } else {
            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_TIMER) == 0) {
                loaded = loadSavedSettings(stateDirectory() + "settings.conf",
                                           testSettings);
                SDL_Quit();
            }
            if (!loaded) configError = "No saved camera settings were found";
        }
        if (!loaded) {
            std::cerr << configError << '\n';
            return 1;
        }
        if (testDelaySeconds >= 0) {
            for (int index = 0; index < testSettings.cameraCount; ++index) {
                testSettings.cameras[index].delaySeconds = testDelaySeconds;
            }
        }
        if (checkConfig) {
            std::cout << "Camera configuration is valid ("
                      << testSettings.cameraCount << " camera"
                      << (testSettings.cameraCount == 1 ? "" : "s") << ").\n";
            return 0;
        }
        return headlessTest(testSettings, testSeconds);
    }

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL initialization failed\n";
        return 1;
    }

    const std::string preferences = stateDirectory();
    const std::string settingsPath = preferences + "settings.conf";
    CameraMonitorSettings settings = emptySettings();
    bool configured = loadSavedSettings(settingsPath, settings);
    if (!configured && !configPath.empty()) {
        std::string legacyError;
        configured = loadLegacyConfig(configPath, settings, legacyError);
        if (configured) saveSettings(settingsPath, settings);
    }

    const int lockDescriptor = acquireSingleInstanceLock(preferences);
    if (lockDescriptor < 0) {
        SDL_Quit();
        return 0;
    }

    WindowState state = loadWindowState(preferences);
    SDL_Window *window = SDL_CreateWindow(
        "Camera Monitor", state.x, state.y, state.width, state.height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (!window) {
        close(lockDescriptor);
        SDL_Quit();
        return 1;
    }
    configureMacApplicationMenu();
    applyWindowLayout(window, settings, true);
    SDL_SetWindowAlwaysOnTop(window, state.alwaysOnTop ? SDL_TRUE : SDL_FALSE);

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        close(lockDescriptor);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    std::vector<ActiveCamera> cameras;
    auto applySettings = [&](const CameraMonitorSettings &proposed) {
        if (!saveSettings(settingsPath, proposed)) {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR, "Settings could not be saved",
                "Camera Monitor could not write its private settings file.", window);
            return false;
        }
        settings = proposed;
        configured = true;
        applyWindowLayout(window, settings, true);
        startCameras(settings, cameras);
        return true;
    };

    auto editSettings = [&]() {
        CameraMonitorSettings proposed = settings;
        if (showMacSettingsDialog(window, &proposed)) applySettings(proposed);
    };

    if (configured) startCameras(settings, cameras);
    if (!configured || showSettingsOnLaunch) {
        editSettings();
    }

    bool alwaysOnTop = state.alwaysOnTop;
    while (gRunning.load()) {
        bool openSettings = consumeMacSettingsRequest() != 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) gRunning.store(false);
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    gRunning.store(false);
                }
                if (event.key.keysym.sym == SDLK_t) {
                    alwaysOnTop = !alwaysOnTop;
                    SDL_SetWindowAlwaysOnTop(window,
                                             alwaysOnTop ? SDL_TRUE : SDL_FALSE);
                }
                if (event.key.keysym.sym == SDLK_COMMA &&
                    (event.key.keysym.mod & KMOD_GUI)) {
                    openSettings = true;
                }
            }
        }

        if (openSettings) editSettings();
        for (auto &camera : cameras) {
            updateTexture(renderer, *camera.texture, *camera.stream);
        }

        int width = 0, height = 0;
        SDL_GetRendererOutputSize(renderer, &width, &height);

        SDL_SetRenderDrawColor(renderer, 5, 6, 8, 255);
        SDL_RenderClear(renderer);
        const LayoutGeometry geometry = layoutGeometry(settings);
        for (size_t index = 0; index < cameras.size(); ++index) {
            const SDL_Rect bounds = cameraBounds(
                static_cast<int>(index), geometry, width, height);
            renderStream(renderer, *cameras[index].texture,
                         *cameras[index].stream, cameras[index].name, bounds);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }

    saveWindowState(preferences, window, alwaysOnTop);
    stopCameras(cameras);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    close(lockDescriptor);
    SDL_Quit();
    avformat_network_deinit();
    return 0;
}
