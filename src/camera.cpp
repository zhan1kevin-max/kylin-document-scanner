#include "camera.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace scan {
namespace {
using Clock = std::chrono::steady_clock;
constexpr qint64 MaxPixels = 64000000;

#ifdef Q_OS_LINUX
int callIoctl(int fd, unsigned long request, void *argument) {
    int result;
    do { result = ::ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result;
}

QString failureText(const QString &operation, int errorNumber = errno) {
    if (errorNumber == EACCES || errorNumber == EPERM)
        return operation + QStringLiteral("：没有摄像头访问权限，请检查设备权限或 video 用户组。");
    if (errorNumber == EBUSY)
        return operation + QStringLiteral("：设备正被其他程序占用，请关闭占用摄像头的程序。");
    if (errorNumber == ENODEV || errorNumber == ENXIO || errorNumber == ENOENT)
        return operation + QStringLiteral("：设备已断开，请重新连接高拍仪。");
    return operation + QStringLiteral("：") + QString::fromLocal8Bit(std::strerror(errorNumber));
}

class CameraFailure : public std::runtime_error {
public:
    explicit CameraFailure(const QString &message)
        : std::runtime_error(message.toUtf8().constData()), message_(message) {}
    QString message() const { return message_; }
private:
    QString message_;
};

struct FileDescriptor {
    int value = -1;
    explicit FileDescriptor(int fd) : value(fd) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
};

bool supportedPixelFormat(quint32 code) {
    return code == V4L2_PIX_FMT_MJPEG || code == V4L2_PIX_FMT_JPEG ||
           code == V4L2_PIX_FMT_YUYV || code == V4L2_PIX_FMT_UYVY ||
           code == V4L2_PIX_FMT_RGB24 || code == V4L2_PIX_FMT_BGR24;
}

QString fourccName(quint32 value) {
    QByteArray code;
    for (int i = 0; i < 4; ++i) code.append(char((value >> (i * 8)) & 0xff));
    return QString::fromLatin1(code);
}

QSize jpegDimensions(const uchar *data, size_t length) {
    // Read SOF dimensions before decoding. A corrupt frame must not make the
    // JPEG decoder allocate an image larger than the negotiated capture size.
    if (length < 4 || data[0] != 0xff || data[1] != 0xd8) return {};
    size_t position = 2;
    while (position < length) {
        if (data[position++] != 0xff) return {};
        while (position < length && data[position] == 0xff) ++position;
        if (position >= length) return {};
        const uchar marker = data[position++];
        if (marker == 0xda || marker == 0xd9) return {}; // SOS/EOI before SOF
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
        if (position + 2 > length) return {};
        const size_t segmentLength = (size_t(data[position]) << 8) | data[position + 1];
        if (segmentLength < 2 || segmentLength > length - position) return {};
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (segmentLength < 8) return {};
            return QSize((int(data[position + 5]) << 8) | data[position + 6],
                         (int(data[position + 3]) << 8) | data[position + 4]);
        }
        position += segmentLength;
    }
    return {};
}

bool queryCaptureDevice(int fd, v4l2_capability *capability = nullptr) {
    v4l2_capability caps{};
    if (callIoctl(fd, VIDIOC_QUERYCAP, &caps) < 0) return false;
    const quint32 flags = (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
    if (capability) *capability = caps;
    // UVC devices use the single-planar capture API. Metadata-only nodes are excluded.
    return (flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING);
}

QString readSmallFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.read(512)).trimmed();
}

QString usbIdentity(const QString &videoName) {
    QString path = QFileInfo(QStringLiteral("/sys/class/video4linux/") + videoName + QStringLiteral("/device")).canonicalFilePath();
    for (int depth = 0; depth < 10 && !path.isEmpty() && path != QStringLiteral("/"); ++depth) {
        const QString vendor = readSmallFile(path + QStringLiteral("/idVendor"));
        const QString product = readSmallFile(path + QStringLiteral("/idProduct"));
        if (!vendor.isEmpty() && !product.isEmpty()) return vendor + QLatin1Char(':') + product;
        path = QFileInfo(path).dir().absolutePath();
    }
    return {};
}

QVector<CameraFormat> enumerateFormats(int fd) {
    QVector<CameraFormat> result;
    auto append = [&](quint32 width, quint32 height, quint32 code) {
        if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
            qint64(width) * height > MaxPixels) return;
        if ((code == V4L2_PIX_FMT_YUYV || code == V4L2_PIX_FMT_UYVY) && (width % 2)) return;
        for (const CameraFormat &entry : result)
            if (entry.width == int(width) && entry.height == int(height) && entry.fourcc == code) return;
        result.append({int(width), int(height), code,
                       QStringLiteral("%1 × %2 · %3").arg(width).arg(height).arg(fourccName(code))});
    };
    for (quint32 i = 0; i < 64; ++i) {
        v4l2_fmtdesc description{};
        description.index = i;
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (callIoctl(fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (!supportedPixelFormat(description.pixelformat)) continue;
        bool foundSize = false;
        for (quint32 j = 0; j < 256; ++j) {
            v4l2_frmsizeenum size{};
            size.index = j;
            size.pixel_format = description.pixelformat;
            if (callIoctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
            foundSize = true;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                append(size.discrete.width, size.discrete.height, description.pixelformat);
            } else {
                const auto &s = size.stepwise;
                append(s.min_width, s.min_height, description.pixelformat);
                append(s.max_width, s.max_height, description.pixelformat);
                const QSize preferred[] = {{640, 480}, {1280, 720}, {1280, 960}, {1920, 1080}, {2560, 1920}};
                for (const QSize &candidate : preferred) {
                    const quint32 width = quint32(candidate.width()), height = quint32(candidate.height());
                    if (width >= s.min_width && width <= s.max_width && height >= s.min_height && height <= s.max_height &&
                        ((width - s.min_width) % std::max(1u, s.step_width) == 0) &&
                        ((height - s.min_height) % std::max(1u, s.step_height) == 0))
                        append(width, height, description.pixelformat);
                }
                break;
            }
        }
        // Old drivers may only implement G_FMT. Advertise that actual format, never a guessed size.
        if (!foundSize) {
            v4l2_format format{};
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (callIoctl(fd, VIDIOC_G_FMT, &format) == 0 && format.fmt.pix.pixelformat == description.pixelformat)
                append(format.fmt.pix.width, format.fmt.pix.height, format.fmt.pix.pixelformat);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const CameraFormat &a, const CameraFormat &b) {
        const qint64 pa = qint64(a.width) * a.height, pb = qint64(b.width) * b.height;
        if (pa != pb) return pa > pb;
        const bool am = a.fourcc == V4L2_PIX_FMT_MJPEG || a.fourcc == V4L2_PIX_FMT_JPEG;
        const bool bm = b.fourcc == V4L2_PIX_FMT_MJPEG || b.fourcc == V4L2_PIX_FMT_JPEG;
        return am && !bm;
    });
    return result;
}

bool equalFormat(const CameraFormat &a, const CameraFormat &b) {
    return a.width == b.width && a.height == b.height && a.fourcc == b.fourcc;
}

class Stream {
public:
    explicit Stream(int fd) : fd_(fd) {}
    ~Stream() { stop(); }
    void stop() noexcept {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            callIoctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (const auto &buffer : buffers_)
            if (buffer.address != MAP_FAILED) ::munmap(buffer.address, buffer.length);
        buffers_.clear();
        if (allocated_) {
            v4l2_requestbuffers request{};
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            callIoctl(fd_, VIDIOC_REQBUFS, &request);
            allocated_ = false;
        }
    }

    void start(const CameraFormat &requested) {
        stop();
        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = quint32(requested.width);
        format.fmt.pix.height = quint32(requested.height);
        format.fmt.pix.pixelformat = requested.fourcc;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (callIoctl(fd_, VIDIOC_S_FMT, &format) < 0) throw CameraFailure(failureText(QStringLiteral("设置视频格式失败")));
        const auto &p = format.fmt.pix;
        if (p.width != quint32(requested.width) || p.height != quint32(requested.height) || p.pixelformat != requested.fourcc)
            throw CameraFailure(QStringLiteral("设备未接受指定格式：请求 %1，实际返回 %2 × %3 · %4。请重新选择实际可用的分辨率。")
                                    .arg(requested.description).arg(p.width).arg(p.height).arg(fourccName(p.pixelformat)));
        current_ = requested;
        bytesPerLine_ = p.bytesperline;

        v4l2_requestbuffers request{};
        request.count = 3;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (callIoctl(fd_, VIDIOC_REQBUFS, &request) < 0) throw CameraFailure(failureText(QStringLiteral("分配视频缓冲失败")));
        allocated_ = true;
        if (request.count < 2 || request.count > 16) throw CameraFailure(QStringLiteral("设备返回不支持的缓冲数量。"));
        quint64 totalBytes = 0;
        for (quint32 index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (callIoctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) throw CameraFailure(failureText(QStringLiteral("查询视频缓冲失败")));
            totalBytes += buffer.length;
            if (buffer.length == 0 || totalBytes > 512ull * 1024 * 1024)
                throw CameraFailure(QStringLiteral("视频缓冲过大，请选择较低分辨率。"));
            void *address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
            if (address == MAP_FAILED) throw CameraFailure(failureText(QStringLiteral("映射视频缓冲失败")));
            buffers_.push_back({address, buffer.length});
            if (callIoctl(fd_, VIDIOC_QBUF, &buffer) < 0) throw CameraFailure(failureText(QStringLiteral("提交视频缓冲失败")));
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (callIoctl(fd_, VIDIOC_STREAMON, &type) < 0) throw CameraFailure(failureText(QStringLiteral("启动视频流失败")));
        streaming_ = true;
    }

    // Discard queued frames from before the shutter without resetting exposure,
    // format, mappings or the stream. The fd is explicitly nonblocking.
    void discardReady() {
        for (size_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (callIoctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN) return;
                throw CameraFailure(failureText(QStringLiteral("清理过期视频帧失败")));
            }
            requeue(buffer);
        }
    }

    // Return true only for a valid, complete frame. When the preview consumer
    // is busy, keep the device queue moving without decoding or copying pixels.
    bool read(std::atomic_bool &stopRequested, CapturedFrame *output) {
        pollfd descriptor{fd_, POLLIN, 0};
        int status;
        do { status = ::poll(&descriptor, 1, 200); } while (status < 0 && errno == EINTR && !stopRequested.load());
        if (stopRequested.load() || status == 0) return false;
        if (status < 0) throw CameraFailure(failureText(QStringLiteral("等待视频帧失败")));
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw CameraFailure(QStringLiteral("高拍仪视频流已断开，请检查 USB 连接后重新打开设备。"));
        if (!(descriptor.revents & POLLIN)) return false;
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (callIoctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) return false;
            throw CameraFailure(failureText(QStringLiteral("读取视频帧失败")));
        }
        if (buffer.index >= buffers_.size()) throw CameraFailure(QStringLiteral("设备返回了无效视频缓冲索引。"));
        const auto &mapped = buffers_[buffer.index];
        bool valid = !(buffer.flags & V4L2_BUF_FLAG_ERROR) && buffer.bytesused > 0 && buffer.bytesused <= mapped.length;
        if (valid && output) {
            const auto *data = static_cast<const uchar *>(mapped.address);
            const int width = current_.width, height = current_.height;
            output->size = QSize(width, height);
            if (current_.fourcc == V4L2_PIX_FMT_MJPEG || current_.fourcc == V4L2_PIX_FMT_JPEG) {
                // Some devices append padding after EOI; trim it from the stored JPEG.
                size_t end = buffer.bytesused;
                while (end >= 2 && (data[end - 2] != 0xff || data[end - 1] != 0xd9)) --end;
                valid = end >= 4 && jpegDimensions(data, end) == output->size;
                if (valid) output->jpeg = QByteArray(reinterpret_cast<const char *>(data), int(end));
            } else {
                output->pixels = readRaw(data, buffer.bytesused);
                valid = !output->pixels.isNull();
            }
        }
        requeue(buffer);
        return valid;
    }

private:
    void requeue(v4l2_buffer &buffer) {
        if (callIoctl(fd_, VIDIOC_QBUF, &buffer) < 0)
            throw CameraFailure(failureText(QStringLiteral("归还视频缓冲失败")));
    }

    QImage readRaw(const uchar *data, size_t size) const {
        const int width = current_.width, height = current_.height;
        const bool packedYuv = current_.fourcc == V4L2_PIX_FMT_YUYV || current_.fourcc == V4L2_PIX_FMT_UYVY;
        const size_t minimumStride = size_t(width) * (packedYuv ? 2u : 3u);
        const size_t stride = std::max<size_t>(minimumStride, bytesPerLine_);
        if (size < stride * size_t(height - 1) + minimumStride) return {};
        cv::Mat input(height, width, packedYuv ? CV_8UC2 : CV_8UC3, const_cast<uchar *>(data), stride);
        cv::Mat rgb;
        if (current_.fourcc == V4L2_PIX_FMT_YUYV) cv::cvtColor(input, rgb, cv::COLOR_YUV2RGB_YUY2);
        else if (current_.fourcc == V4L2_PIX_FMT_UYVY) cv::cvtColor(input, rgb, cv::COLOR_YUV2RGB_UYVY);
        else if (current_.fourcc == V4L2_PIX_FMT_BGR24) cv::cvtColor(input, rgb, cv::COLOR_BGR2RGB);
        else rgb = input;
        return QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step), QImage::Format_RGB888).copy();
    }

    struct Buffer { void *address; size_t length; };
    int fd_;
    bool streaming_ = false;
    bool allocated_ = false;
    CameraFormat current_;
    quint32 bytesPerLine_ = 0;
    std::vector<Buffer> buffers_;
};
#endif
} // namespace

struct Camera::Impl {
    explicit Impl(Camera *camera) : owner(camera) {}
    Camera *owner;
    std::thread worker;
    std::atomic_bool stopRequested{false};
    std::atomic_bool running{false};
    std::atomic_bool capturePending{false};
    std::atomic_bool previewQueued{false};
    std::atomic<quint64> generation{0};

    template <class Action> void post(quint64 expectedGeneration, Action action) {
        QMetaObject::invokeMethod(owner, [this, expectedGeneration, action]() {
            if (generation.load() == expectedGeneration) action();
        }, Qt::QueuedConnection);
    }

    void publishPreview(QImage image, quint64 expectedGeneration) {
        // Coalesce UI delivery: a blocked UI must not accumulate unlimited video frames.
        if (previewQueued.exchange(true)) return;
        QMetaObject::invokeMethod(owner, [this, image, expectedGeneration]() {
            previewQueued.store(false);
            if (generation.load() == expectedGeneration && running.load()) emit owner->preview(image);
        }, Qt::QueuedConnection);
    }

#ifdef Q_OS_LINUX
    void run(const QString &device, CameraFormat captureFormat, quint64 expectedGeneration) {
        try {
            FileDescriptor fd(::open(QFile::encodeName(device).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
            if (fd.value < 0) throw CameraFailure(failureText(QStringLiteral("打开高拍仪失败")));
            if (!queryCaptureDevice(fd.value))
                throw CameraFailure(QStringLiteral("所选设备不是支持 V4L2 流式采集的摄像头节点。"));
            const auto available = enumerateFormats(fd.value);
            if (captureFormat.width <= 0 || captureFormat.height <= 0 ||
                qint64(captureFormat.width) * captureFormat.height > MaxPixels)
                throw CameraFailure(QStringLiteral("请选择有效的拍摄分辨率。"));
            const auto selected = std::find_if(available.cbegin(), available.cend(), [&](const CameraFormat &f) { return equalFormat(f, captureFormat); });
            if (selected == available.cend()) throw CameraFailure(QStringLiteral("所选拍摄格式已不可用，请刷新设备和分辨率列表。"));
            captureFormat = *selected;
            Stream stream(fd.value);
            stream.start(captureFormat);
            if (stopRequested.load()) return;
            running.store(true);
            const QString description = QStringLiteral("已连接：%1 · 实时取景").arg(captureFormat.description);
            post(expectedGeneration, [this, description]() { emit owner->opened(description); });
            auto lastFrame = Clock::now();
            auto lastPreview = lastFrame - std::chrono::seconds(1);
            bool awaitingShutter = false;
            while (!stopRequested.load()) {
                if (capturePending.exchange(false)) {
                    stream.discardReady();
                    awaitingShutter = true;
                }
                const bool wantPreview = !previewQueued.load() &&
                    Clock::now() - lastPreview >= std::chrono::milliseconds(55);
                CapturedFrame frame;
                if (!stream.read(stopRequested, (wantPreview || awaitingShutter) ? &frame : nullptr)) {
                    if (Clock::now() - lastFrame > std::chrono::seconds(8))
                        throw CameraFailure(QStringLiteral("连续 8 秒没有有效视频画面，请检查连接或降低分辨率。"));
                    continue;
                }
                lastFrame = Clock::now();
                if (stopRequested.load()) break;
                if (awaitingShutter) {
                    awaitingShutter = false;
                    // Legacy consumers can still request an uncompressed full-size QImage.
                    // The normal desktop pipeline gets the compressed native exposure.
                    QImage legacy;
                    if (owner->receivers(SIGNAL(captured(QImage))) > 0) legacy = frame.decode();
                    post(expectedGeneration, [this, frame, legacy]() {
                        emit owner->frameCaptured(frame);
                        if (!legacy.isNull()) emit owner->captured(legacy);
                    });
                }
                if (wantPreview) {
                    auto small = frame.preview(1152);
                    if (stopRequested.load()) break;
                    publishPreview(std::move(small), expectedGeneration);
                    lastPreview = lastFrame;
                }
            }
        } catch (const CameraFailure &failure) {
            if (!stopRequested.load()) {
                const QString message = failure.message();
                post(expectedGeneration, [this, message]() { emit owner->error(message); });
            }
        } catch (const cv::Exception &failure) {
            if (!stopRequested.load()) {
                const QString message = QStringLiteral("视频解码失败：") + QString::fromUtf8(failure.what());
                post(expectedGeneration, [this, message]() { emit owner->error(message); });
            }
        } catch (const std::exception &failure) {
            if (!stopRequested.load()) {
                const QString message = QStringLiteral("摄像头错误：") + QString::fromUtf8(failure.what());
                post(expectedGeneration, [this, message]() { emit owner->error(message); });
            }
        }
        running.store(false);
        capturePending.store(false);
        post(expectedGeneration, [this]() { emit owner->stopped(); });
    }
#endif
};

Camera::Camera(QObject *parent) : QObject(parent), impl_(new Impl(this)) {
    qRegisterMetaType<QImage>("QImage");
    qRegisterMetaType<scan::CapturedFrame>("scan::CapturedFrame");
    qRegisterMetaType<scan::CameraFormat>("scan::CameraFormat");
}
Camera::~Camera() {
    // Destruction must not call UI slots on a partly destroyed parent window.
    disconnect(this, nullptr, nullptr, nullptr);
    stop();
}

QVector<CameraDevice> Camera::devices() {
    QVector<CameraDevice> result;
#ifdef Q_OS_LINUX
    struct Candidate { CameraDevice device; bool deli; };
    QVector<Candidate> candidates;
    const QDir directory(QStringLiteral("/sys/class/video4linux"));
    for (const QString &node : directory.entryList({QStringLiteral("video*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString path = QStringLiteral("/dev/") + node;
        const QString identity = usbIdentity(node);
        FileDescriptor fd(::open(QFile::encodeName(path).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
        QString name;
        if (fd.value < 0) {
            if (errno != EACCES && errno != EPERM && errno != EBUSY) continue;
            name = readSmallFile(directory.absoluteFilePath(node + QStringLiteral("/name"))) + QStringLiteral("（无法访问，请检查权限或占用）");
        } else {
            v4l2_capability capability{};
            if (!queryCaptureDevice(fd.value, &capability)) continue;
            name = QString::fromLocal8Bit(reinterpret_cast<const char *>(capability.card), int(strnlen(reinterpret_cast<const char *>(capability.card), sizeof(capability.card))));
        }
        if (name.isEmpty()) name = QStringLiteral("USB 摄像头");
        name += QStringLiteral(" · ") + path;
        if (!identity.isEmpty()) name += QStringLiteral(" · USB ") + identity;
        candidates.append({{path, name}, identity.compare(QStringLiteral("2e5a:1719"), Qt::CaseInsensitive) == 0});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.deli && !b.deli; });
    for (const Candidate &candidate : candidates) result.append(candidate.device);
#endif
    return result;
}

QVector<CameraFormat> Camera::formats(const QString &deviceId) {
#ifdef Q_OS_LINUX
    FileDescriptor fd(::open(QFile::encodeName(deviceId).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (fd.value >= 0 && queryCaptureDevice(fd.value)) return enumerateFormats(fd.value);
#else
    Q_UNUSED(deviceId)
#endif
    return {};
}

void Camera::start(const QString &deviceId, const CameraFormat &captureFormat) {
    stop();
    impl_->stopRequested.store(false);
    impl_->capturePending.store(false);
#ifdef Q_OS_LINUX
    const quint64 generation = impl_->generation.load();
    impl_->worker = std::thread([this, deviceId, captureFormat, generation] { impl_->run(deviceId, captureFormat, generation); });
#else
    Q_UNUSED(deviceId)
    Q_UNUSED(captureFormat)
    emit error(QStringLiteral("设备采集仅支持 Linux V4L2。此 Windows 开发版本可用于导入图片、处理与导出验证。"));
    emit stopped();
#endif
}

void Camera::stop() {
    const bool hadWorker = impl_->worker.joinable();
    impl_->generation.fetch_add(1);
    impl_->stopRequested.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->running.store(false);
    impl_->capturePending.store(false);
    if (hadWorker) emit stopped();
}

void Camera::capture() {
    if (!impl_->running.load()) {
        emit error(QStringLiteral("请先连接高拍仪并等待实时预览。"));
        return;
    }
    impl_->capturePending.store(true); // bounded queue: at most one pending shutter request
}

bool Camera::isRunning() const { return impl_->running.load(); }
} // namespace scan
