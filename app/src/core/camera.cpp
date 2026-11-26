#include "camera.h"

// Adapted from LibreDrop (src/core/camera.cpp): the ffmpeg and V4L2 backends are LibreDrop's, with the goniometer's
// camera settings left out; listing reads names without opening devices; the demo camera replaces LibreDrop's
// simulators.
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <map>

#include "demo.h"
#include "util.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace lt {

QString ffmpegProgram() {
    if (qEnvironmentVariableIsSet("LIBRETAP_FFMPEG")) return qEnvironmentVariable("LIBRETAP_FFMPEG");
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("ffmpeg.exe");
#else
    const QString exe = QStringLiteral("ffmpeg");
#endif
    QStringList dirs;
    if (QCoreApplication::instance()) dirs << QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    dirs << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin");
#endif
    for (const QString& d : dirs)
        if (QFileInfo::exists(QDir(d).filePath(exe))) return QDir(d).filePath(exe);
    return QStringLiteral("ffmpeg");
}

namespace {

// The pretend vendor program of demo.h, drawn at 1280×800, 10 frames a second.
class DemoCamera : public Camera {
public:
    QString name() const override { return QStringLiteral("Demo camera (a pretend instrument PC)"); }
    std::vector<CameraMode> modes() const override { return {{1280, 800, 10}, {640, 400, 10}}; }
    CameraMode mode() const override { return mode_; }
    bool start(const CameraMode& m, QString*) override {
        mode_ = m.w > 0 ? m : modes().front();
        clock_.start();
        n_ = 0;
        return true;
    }
    bool grab(CameraFrame* out, QString*) override {
        const double due = n_ / mode_.fps, now = clock_.nsecsElapsed() * 1e-9;
        if (due > now) QThread::usleep(static_cast<unsigned long>((due - now) * 1e6));
        QBuffer buf(&out->jpeg);
        buf.open(QIODevice::WriteOnly);
        out->jpeg.clear();
        demoScreen(mode_.w, mode_.h, wallNow()).save(&buf, "JPG", 85);
        out->t = due;
        out->sequence = n_++;
        return !out->jpeg.isEmpty();
    }
    void stop() override {}

private:
    CameraMode mode_;
    QElapsedTimer clock_;
    int n_ = 0;
};

// Any camera ffmpeg can read, through an ffmpeg process that writes Motion-JPEG to a pipe: DirectShow on Windows,
// AVFoundation on macOS (ids "ffmpeg:dshow:video=<name>", "ffmpeg:avfoundation:<index>"; any other ffmpeg input
// works too, e.g. "ffmpeg:lavfi:testsrc2=size=640x480:rate=30" for tests). A camera that sends Motion-JPEG is
// copied as it is (DirectShow), others are encoded to JPEG by ffmpeg. Frame times are the camera's, as ffmpeg reports
// them for each frame (ffmpeg 6.1 or newer; older ones: when the frame arrives); frames are taken as they come, so
// none are duplicated or dropped on the way.
// The camera's settings (exposure, gamma) cannot be set this way: the camera keeps its own (Windows keeps the last
// ones set, e.g. by the goniometer's vendor software in LibreDrop's case).
class FfmpegCamera : public Camera {
public:
    ~FfmpegCamera() override { stop(); }

    static QString runProbe(const QStringList& args) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(ffmpegProgram(), QStringList{QStringLiteral("-hide_banner")} + args);
        if (!p.waitForStarted(5000)) return {};
        p.closeWriteChannel();
        if (!p.waitForFinished(15000)) p.kill(), p.waitForFinished(2000);
        return QString::fromLocal8Bit(p.readAll());
    }

    static std::vector<CameraInfo> listDevices() {
        std::vector<CameraInfo> out;
#if defined(_WIN32)
        const QString text = runProbe({QStringLiteral("-list_devices"), QStringLiteral("true"), QStringLiteral("-f"),
                                       QStringLiteral("dshow"), QStringLiteral("-i"), QStringLiteral("dummy")});
        // ffmpeg 5+: "[dshow @ ...] "Name" (video)"; older: names listed under "DirectShow video devices"
        const bool tagged = text.contains(QLatin1String("\" (video)"));
        bool inVideo = false;
        static const QRegularExpression q(QStringLiteral("\"([^\"]+)\"(\\s*\\((video|audio|none)[^)]*\\))?"));
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (line.contains(QLatin1String("video devices"))) inVideo = true;
            if (line.contains(QLatin1String("audio devices"))) inVideo = false;
            if (line.contains(QLatin1String("Alternative name"))) continue;
            const auto m = q.match(line);
            if (!m.hasMatch()) continue;
            if (tagged ? m.captured(3) != QLatin1String("video") : !inVideo) continue;
            out.push_back({QStringLiteral("ffmpeg:dshow:video=") + m.captured(1), m.captured(1)});
        }
#elif defined(__APPLE__)
        const QString text = runProbe({QStringLiteral("-f"), QStringLiteral("avfoundation"), QStringLiteral("-list_devices"),
                                       QStringLiteral("true"), QStringLiteral("-i"), QString()});
        bool inVideo = false;
        static const QRegularExpression q(QStringLiteral("\\]\\s*\\[(\\d+)\\]\\s*(.+)$"));
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (line.contains(QLatin1String("video devices"))) inVideo = true;
            if (line.contains(QLatin1String("audio devices"))) inVideo = false;
            const auto m = q.match(line.trimmed());
            if (!inVideo || !m.hasMatch() || m.captured(2).startsWith(QLatin1String("Capture screen"))) continue;
            out.push_back({QStringLiteral("ffmpeg:avfoundation:") + m.captured(1), m.captured(2).trimmed()});
        }
#endif
        return out;
    }

    bool load(const QString& spec, QString* error) {
        const int c = spec.indexOf(QLatin1Char(':'));
        if (c <= 0) {
            if (error) *error = QStringLiteral("not an ffmpeg camera: %1").arg(spec);
            return false;
        }
        format_ = spec.left(c);
        input_ = spec.mid(c + 1);
        name_ = input_.startsWith(QLatin1String("video=")) ? input_.mid(6) : input_;
        if (format_ == QLatin1String("dshow")) {
            static const QRegularExpression re(QStringLiteral(
                "(vcodec|pixel_format)=(\\S+)\\s+min s=(\\d+)x(\\d+) fps=([\\d.]+)\\s+max s=(\\d+)x(\\d+) fps=([\\d.]+)"));
            const QString text = runProbe({QStringLiteral("-list_options"), QStringLiteral("true"), QStringLiteral("-f"),
                                           QStringLiteral("dshow"), QStringLiteral("-i"), input_});
            for (const QString& line : text.split(QLatin1Char('\n'))) {
                const auto m = re.match(line);
                if (!m.hasMatch()) continue;
                Mode md{{m.captured(6).toInt(), m.captured(7).toInt(), m.captured(8).toDouble()},
                        m.captured(1) == QLatin1String("vcodec") && m.captured(2) == QLatin1String("mjpeg")};
                addMode(md);
            }
        } else if (format_ == QLatin1String("avfoundation")) {
            // the supported modes are listed when an unsupported size is asked for
            static const QRegularExpression re(QStringLiteral("(\\d+)x(\\d+)@\\[([\\d.]+) ([\\d.]+)\\]fps"));
            const QString text = runProbe({QStringLiteral("-f"), QStringLiteral("avfoundation"), QStringLiteral("-video_size"),
                                           QStringLiteral("1x1"), QStringLiteral("-i"), input_});
            for (const QString& line : text.split(QLatin1Char('\n'))) {
                const auto m = re.match(line);
                if (m.hasMatch()) addMode({{m.captured(1).toInt(), m.captured(2).toInt(), m.captured(4).toDouble()}, false});
            }
        } else {   // e.g. lavfi: size and rate come with the input
            static const QRegularExpression size(QStringLiteral("size=(\\d+)x(\\d+)")), rate(QStringLiteral("rate=([\\d.]+)"));
            const auto s = size.match(input_), r = rate.match(input_);
            modes_.push_back({{s.hasMatch() ? s.captured(1).toInt() : 320, s.hasMatch() ? s.captured(2).toInt() : 240,
                               r.hasMatch() ? r.captured(1).toDouble() : 25}, false});
            fixed_ = true;
        }
        if (modes_.empty()) {
            if (error)
                *error = QStringLiteral("cannot read the modes of %1 through ffmpeg (%2); is ffmpeg installed?")
                             .arg(name_, ffmpegProgram());
            return false;
        }
        // largest first, as the V4L2 camera lists them
        std::stable_sort(modes_.begin(), modes_.end(), [](const Mode& a, const Mode& b) { return a.m.w * a.m.h > b.m.w * b.m.h; });
        return true;
    }

    QString name() const override { return name_; }
    std::vector<CameraMode> modes() const override {
        std::vector<CameraMode> out;
        for (const Mode& m : modes_) out.push_back(m.m);
        return out;
    }
    CameraMode mode() const override { return mode_.m; }

    bool start(const CameraMode& want, QString* error) override {
        stop();
        mode_ = modes_.front();
        for (const Mode& m : modes_)
            if (m.m.w == want.w && m.m.h == want.h && std::abs(m.m.fps - want.fps) < 0.5) { mode_ = m; break; }
        QStringList a{QStringLiteral("-hide_banner"), QStringLiteral("-nostats"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-fflags"), QStringLiteral("nobuffer")};
        if (format_ == QLatin1String("lavfi")) a << QStringLiteral("-re");
        a << QStringLiteral("-f") << format_;
        if (format_ == QLatin1String("dshow")) {
            a << QStringLiteral("-rtbufsize") << QStringLiteral("256M");
            if (mode_.mjpeg) a << QStringLiteral("-vcodec") << QStringLiteral("mjpeg");
        }
        if (!fixed_)
            a << QStringLiteral("-video_size") << QStringLiteral("%1x%2").arg(mode_.m.w).arg(mode_.m.h)
              << QStringLiteral("-framerate") << QString::number(mode_.m.fps);
        a << QStringLiteral("-i") << input_ << QStringLiteral("-an");
        if (mode_.mjpeg) a << QStringLiteral("-c:v") << QStringLiteral("copy");
        else a << QStringLiteral("-c:v") << QStringLiteral("mjpeg") << QStringLiteral("-q:v") << QStringLiteral("3")
               << QStringLiteral("-pix_fmt") << QStringLiteral("yuvj420p");
        a << QStringLiteral("-fps_mode") << QStringLiteral("passthrough") << QStringLiteral("-flush_packets") << QStringLiteral("1")
          << QStringLiteral("-f") << QStringLiteral("mjpeg");
        stamps_ = ffmpegVersion() >= 601;   // -stats_mux_pre: each frame's number and time, on stderr
        if (stamps_) a << QStringLiteral("-stats_mux_pre") << QStringLiteral("pipe:2") << QStringLiteral("-stats_mux_pre_fmt") << QStringLiteral("{n} {t}");
        a << QStringLiteral("-");
        times_.clear();
        errText_.clear();
        errBuf_.clear();
        proc_ = std::make_unique<QProcess>();
        proc_->start(ffmpegProgram(), a);
        if (!proc_->waitForStarted(10000)) {
            if (error) *error = QStringLiteral("cannot start %1 (install ffmpeg, or set LIBRETAP_FFMPEG)").arg(ffmpegProgram());
            proc_.reset();
            return false;
        }
        buf_.clear();
        scan_ = 0;
        seq_ = 0;
        clock_.start();
        return true;
    }

    bool grab(CameraFrame* out, QString* error) override {
        if (!proc_) {
            if (error) *error = QStringLiteral("camera not started");
            return false;
        }
        QElapsedTimer waited;
        waited.start();
        for (;;) {
            // one JPEG: from SOI (FF D8) to EOI (FF D9); inside the coded data FF is always followed by 00 or a
            // restart marker, so the first FF D9 ends the frame
            const int soi = buf_.indexOf("\xff\xd8", 0);
            if (soi > 0) buf_.remove(0, soi), scan_ = std::max(0, scan_ - soi);
            if (soi >= 0) {
                const int eoi = buf_.indexOf("\xff\xd9", std::max(2, scan_));
                if (eoi >= 0) {
                    double t = clock_.nsecsElapsed() * 1e-9;
                    if (stamps_) {   // its time is written just before the frame; wait briefly if it is late
                        QElapsedTimer w;
                        w.start();
                        while (!times_.count(seq_) && w.elapsed() < 500 && proc_->state() != QProcess::NotRunning) {
                            proc_->waitForReadyRead(20);
                            readErr();
                        }
                        const auto it = times_.find(seq_);
                        if (it != times_.end()) t = it->second;
                        times_.erase(times_.begin(), times_.upper_bound(seq_));
                    }
                    out->jpeg = buf_.left(eoi + 2);
                    buf_.remove(0, eoi + 2);
                    scan_ = 0;
                    out->t = t;
                    out->sequence = seq_++;
                    return true;
                }
                scan_ = std::max(0, int(buf_.size()) - 1);
            }
            if (proc_->state() == QProcess::NotRunning || waited.elapsed() > 5000) {
                readErr();
                const QString msg = errText_.join(QLatin1Char(' ')).trimmed();
                if (error)
                    *error = msg.isEmpty() ? QStringLiteral("no picture from %1").arg(name_) : QStringLiteral("%1: %2").arg(name_, msg);
                return false;
            }
            proc_->waitForReadyRead(200);
            buf_ += proc_->readAllStandardOutput();
            readErr();
            if (buf_.size() > 64 * 1024 * 1024) buf_.clear(), scan_ = 0;   // not JPEG at all
        }
    }

    void stop() override {
        if (!proc_) return;
        proc_->kill();
        proc_->waitForFinished(3000);
        proc_.reset();
    }

private:
    // ffmpeg's version as major*100 + minor (0 if unknown)
    static int ffmpegVersion() {
        static const int v = [] {
            const auto m = QRegularExpression(QStringLiteral("ffmpeg version n?(\\d+)\\.(\\d+)")).match(runProbe({QStringLiteral("-version")}));
            return m.hasMatch() ? m.captured(1).toInt() * 100 + m.captured(2).toInt() : 0;
        }();
        return v;
    }
    // stderr: frame stamps ("n t") and messages (the last few kept for errors)
    void readErr() {
        errBuf_ += proc_->readAllStandardError();
        int nl;
        while ((nl = errBuf_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(errBuf_.left(nl)).trimmed();
            errBuf_.remove(0, nl + 1);
            const int sp = line.indexOf(QLatin1Char(' '));
            bool okN = false, okT = false;
            const int n = sp > 0 ? line.left(sp).toInt(&okN) : 0;
            const double t = sp > 0 ? line.mid(sp + 1).toDouble(&okT) : 0;
            if (okN && okT) times_[n] = t;
            else if (!line.isEmpty()) {
                errText_ << line;
                while (errText_.size() > 3) errText_.removeFirst();
            }
        }
    }
    struct Mode {
        CameraMode m;
        bool mjpeg = false;   // the camera sends Motion-JPEG: copied as it is
    };
    void addMode(const Mode& md) {
        for (Mode& m : modes_)
            if (m.m.w == md.m.w && m.m.h == md.m.h) {   // one entry per size: Motion-JPEG if offered, else the fastest
                if ((md.mjpeg && !m.mjpeg) || (md.mjpeg == m.mjpeg && md.m.fps > m.m.fps)) m = md;
                return;
            }
        modes_.push_back(md);
    }
    QString format_, input_, name_;
    std::vector<Mode> modes_;
    Mode mode_;
    bool fixed_ = false;
    std::unique_ptr<QProcess> proc_;
    QByteArray buf_;
    int scan_ = 0;
    int seq_ = 0;
    QElapsedTimer clock_;
    bool stamps_ = false;
    std::map<int, double> times_;
    QByteArray errBuf_;
    QStringList errText_;
};


#ifdef __linux__
int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && errno == EINTR);
    return r;
}

bool hasMjpeg(int fd) {
    v4l2_fmtdesc d{};
    d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (d.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; ++d.index)
        if (d.pixelformat == V4L2_PIX_FMT_MJPEG) return true;
    return false;
}

class V4l2Camera : public Camera {
public:
    ~V4l2Camera() override {
        stop();
        if (fd_ >= 0) ::close(fd_);
    }
    bool openDevice(const QString& dev, QString* error) {
        dev_ = dev;
        if (!Camera::systemCamerasAllowed()) {   // tests and screenshots: never a real camera
            *error = QStringLiteral("real cameras are switched off (LIBRETAP_NO_SYSTEM_CAMERAS)");
            return false;
        }
        fd_ = ::open(QFile::encodeName(dev).constData(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            *error = QStringLiteral("cannot open %1: %2").arg(dev, QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 || !((cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE)) {
            *error = QStringLiteral("%1 is not a video capture device").arg(dev);
            return false;
        }
        card_ = QString::fromUtf8(reinterpret_cast<const char*>(cap.card)).trimmed();
        if (const int c = card_.indexOf(QStringLiteral(": ")); c > 0 && card_.mid(c + 2).startsWith(card_.left(c)))
            card_ = card_.mid(c + 2);   // "HD USB Camera: HD USB Camera"
        if (!hasMjpeg(fd_)) {
            *error = QStringLiteral("%1 (%2) does not send Motion-JPEG").arg(card_, dev);
            return false;
        }
        return true;
    }
    QString name() const override { return QStringLiteral("%1 (%2)").arg(card_, dev_); }
    std::vector<CameraMode> modes() const override {
        std::vector<CameraMode> out;
        v4l2_frmsizeenum fs{};
        fs.pixel_format = V4L2_PIX_FMT_MJPEG;
        for (fs.index = 0; xioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; ++fs.index) {
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
            CameraMode m{int(fs.discrete.width), int(fs.discrete.height), 0};
            v4l2_frmivalenum fi{};
            fi.pixel_format = V4L2_PIX_FMT_MJPEG;
            fi.width = fs.discrete.width, fi.height = fs.discrete.height;
            for (fi.index = 0; xioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; ++fi.index) {
                if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
                    m.fps = std::max(m.fps, double(fi.stepwise.min.denominator) / std::max(1u, fi.stepwise.min.numerator));
                    break;
                }
                m.fps = std::max(m.fps, double(fi.discrete.denominator) / std::max(1u, fi.discrete.numerator));
            }
            out.push_back(m);
        }
        std::sort(out.begin(), out.end(), [](const CameraMode& a, const CameraMode& b) { return a.w * a.h > b.w * b.h; });
        return out;
    }
    CameraMode mode() const override { return mode_; }
    bool start(const CameraMode& m, QString* error) override {
        stop();
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = m.w, f.fmt.pix.height = m.h;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        f.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &f) < 0) return fail(error, "cannot set the format");
        mode_ = {int(f.fmt.pix.width), int(f.fmt.pix.height), m.fps};
        if (m.fps > 0) {
            v4l2_streamparm p{};
            p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            p.parm.capture.timeperframe.numerator = 1000;
            p.parm.capture.timeperframe.denominator = unsigned(std::lround(m.fps * 1000));
            if (xioctl(fd_, VIDIOC_S_PARM, &p) == 0 && p.parm.capture.timeperframe.numerator)
                mode_.fps = double(p.parm.capture.timeperframe.denominator) / p.parm.capture.timeperframe.numerator;
        }
        v4l2_requestbuffers rb{};
        rb.count = 8;   // room for a stall of a few frames at 120 fps
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rb.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) return fail(error, "cannot get capture buffers");
        for (unsigned i = 0; i < rb.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return fail(error, "cannot query a buffer");
            void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
            if (p == MAP_FAILED) return fail(error, "cannot map a buffer");
            bufs_.push_back({p, b.length});
            if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return fail(error, "cannot queue a buffer");
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return fail(error, "cannot start streaming");
        streaming_ = true;
        return true;
    }
    bool grab(CameraFrame* out, QString* error) override {
        if (!streaming_) return fail(error, "not started");
        pollfd p{fd_, POLLIN, 0};
        int r;
        do r = poll(&p, 1, 2000);
        while (r < 0 && errno == EINTR);
        if (r == 0) {
            if (error) *error = QStringLiteral("no frame from the camera for 2 s (unplugged?)");
            return false;
        }
        if (r < 0) return fail(error, "poll");
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) return grab(out, error);
            return fail(error, "cannot take a frame");
        }
        out->jpeg = QByteArray(static_cast<const char*>(bufs_[b.index].first), int(b.bytesused));
        out->t = b.timestamp.tv_sec + b.timestamp.tv_usec * 1e-6;
        out->sequence = int(b.sequence);
        const bool bad = b.flags & V4L2_BUF_FLAG_ERROR;
        xioctl(fd_, VIDIOC_QBUF, &b);
        if (bad || out->jpeg.size() < 4) return grab(out, error);   // a damaged frame: skip it
        return true;
    }
    void stop() override {
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& b : bufs_) munmap(b.first, b.second);
        bufs_.clear();
        if (fd_ >= 0) {
            v4l2_requestbuffers rb{};
            rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            rb.memory = V4L2_MEMORY_MMAP;
            xioctl(fd_, VIDIOC_REQBUFS, &rb);
        }
    }

private:
    bool fail(QString* error, const char* what) {
        if (error) *error = QStringLiteral("%1: %2 (%3)").arg(dev_, QLatin1String(what), QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    QString dev_, card_;
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<std::pair<void*, std::size_t>> bufs_;
    CameraMode mode_;
};
#endif


}  // namespace

bool Camera::systemCamerasAllowed() { return qEnvironmentVariableIntValue("LIBRETAP_NO_SYSTEM_CAMERAS") == 0; }

std::vector<CameraInfo> Camera::list(bool withDemo) {
    std::vector<CameraInfo> out;
    if (systemCamerasAllowed()) {
#ifdef __linux__
        // names from sysfs: no device is opened (that would wake the camera). Only each camera's first node
        // (index 0) captures pictures; the others carry metadata.
        QDir d(QStringLiteral("/sys/class/video4linux"));
        QStringList devs = d.entryList({QStringLiteral("video*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
        std::sort(devs.begin(), devs.end(), [](const QString& a, const QString& b) { return a.mid(5).toInt() < b.mid(5).toInt(); });
        for (const QString& n : devs) {
            auto readLine = [&](const char* f) {
                QFile file(d.filePath(n + QLatin1Char('/') + QLatin1String(f)));
                return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
            };
            if (const QString idx = readLine("index"); !idx.isEmpty() && idx != QLatin1String("0")) continue;
            QString name = readLine("name");
            if (const int c = name.indexOf(QStringLiteral(": ")); c > 0 && name.mid(c + 2).startsWith(name.left(c)))
                name = name.mid(c + 2);   // "HD USB Camera: HD USB Camera"
            const QString dev = QStringLiteral("/dev/") + n;
            out.push_back({dev, QStringLiteral("%1 (%2)").arg(name.isEmpty() ? QStringLiteral("Camera") : name, dev)});
        }
#else
        for (const CameraInfo& c : FfmpegCamera::listDevices()) out.push_back(c);
#endif
    }
    const QString ff = qEnvironmentVariable("LIBRETAP_FFMPEG_CAMERA");   // testing: any ffmpeg input, e.g. lavfi:testsrc2
    if (!ff.isEmpty()) out.push_back({QStringLiteral("ffmpeg:") + ff, QStringLiteral("ffmpeg: %1").arg(ff)});
    if (withDemo || qEnvironmentVariableIntValue("LIBRETAP_DEMO_CAMERA"))   // screenshots: the pretend PC as a camera
        out.push_back({QStringLiteral("demo"), QStringLiteral("Demo camera (a pretend instrument PC)")});
    return out;
}

std::unique_ptr<Camera> Camera::open(const QString& id, QString* error) {
    if (id == QLatin1String("demo")) return std::make_unique<DemoCamera>();
    if (id.startsWith(QLatin1String("ffmpeg:"))) {
        const QString spec = id.mid(7);
        if (!spec.startsWith(QLatin1String("lavfi:")) && !systemCamerasAllowed()) {
            if (error) *error = QStringLiteral("real cameras are switched off (LIBRETAP_NO_SYSTEM_CAMERAS)");
            return nullptr;
        }
        auto c = std::make_unique<FfmpegCamera>();
        if (!c->load(spec, error)) return nullptr;
        return c;
    }
#ifdef __linux__
    auto c = std::make_unique<V4l2Camera>();
    if (!c->openDevice(id, error)) return nullptr;
    return c;
#else
    if (error) *error = QStringLiteral("no camera %1").arg(id);
    return nullptr;
#endif
}

}  // namespace lt
