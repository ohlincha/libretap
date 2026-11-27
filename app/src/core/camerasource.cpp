#include "camerasource.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QMutexLocker>
#include <QFileInfo>
#include <QTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>
#include <cmath>

#include "session.h"
#include "util.h"

namespace lt {

namespace {
// A small grey thumbnail for "has the screen changed?" (JPEG decoders scale cheaply by 1/8 while decoding).
QImage thumb(const QByteArray& jpeg, int w = 200, int h = 125) {
    QBuffer b;
    b.setData(jpeg);
    b.open(QIODevice::ReadOnly);
    QImageReader r(&b, "JPG");
    r.setScaledSize(QSize(w, h));
    return r.read().convertToFormat(QImage::Format_Grayscale8);
}

// Pixels that changed clearly (more than camera noise): a new value or a changed word on the screen shows up as a
// few such pixels even in a small thumbnail.
int changedPixels(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull()) return 1 << 30;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* p = a.constScanLine(y);
        const uchar* q = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) n += std::abs(int(p[x]) - int(q[x])) > 14;
    }
    return n;
}
}  // namespace

CameraFeed::CameraFeed(QObject* parent) : QObject(parent) {}

CameraFeed::~CameraFeed() { close(); }

QString CameraFeed::name() const {
    QMutexLocker l(&mutex_);
    return name_;
}

CameraMode CameraFeed::mode() const {
    QMutexLocker l(&mutex_);
    return mode_;
}

bool CameraFeed::open(const QString& id) {
    close();
    id_ = id;
    stop_ = false;
    userChose_ = false;
    thread_ = QThread::create([this] { run(); });
    thread_->start();
    return true;
}

void CameraFeed::close() {
    if (!thread_) return;
    stop_ = true;
    thread_->wait(8000);
    delete thread_;
    thread_ = nullptr;
}

void CameraFeed::attach(SessionWriter* w, double every, CameraSource* source) {
    QMutexLocker l(&mutex_);
    writer_ = w;
    every_ = every;
    source_ = source;
    stills_ = 0;
}

void CameraFeed::detach() {
    QMutexLocker l(&mutex_);
    writer_ = nullptr;
    source_ = nullptr;
}

namespace {
QString settingsGroup(const QString& key) {
    QString k = key;
    k.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.@-]")), QStringLiteral("_"));
    return QStringLiteral("camera/") + k;
}
QImage decode(const QByteArray& jpeg, int maxWidth = 960) {
    QBuffer b;
    b.setData(jpeg);
    b.open(QIODevice::ReadOnly);
    QImageReader r(&b, "JPG");
    const QSize full = r.size();
    if (full.width() > maxWidth) r.setScaledSize(full.scaled(maxWidth, maxWidth, Qt::KeepAspectRatio));
    return r.read();
}
}  // namespace

std::vector<CameraControl> CameraFeed::controls() const {
    QMutexLocker l(&mutex_);
    return controls_;
}

void CameraFeed::setControl(const QString& name, int value) {
    for (const char* k : {"exposure_auto", "exposure", "gain", "focus_auto", "focus"})
        if (name == QLatin1String(k)) userChose_ = true;   // a hand-made choice: no automatic Optimise over it
    QMutexLocker l(&mutex_);
    pending_.push_back({name, value});
}

void CameraFeed::resetControls() {
    QMutexLocker l(&mutex_);
    resetPending_ = true;
    pending_.clear();
}

void CameraFeed::setRegion(const QString& mode, const QRectF& box) {
    QSettings st;
    QMutexLocker l(&mutex_);
    regionMode_ = mode == QLatin1String("whole") || mode == QLatin1String("manual") ? mode : QStringLiteral("auto");
    if (regionMode_ == QLatin1String("manual")) manualBox_ = box.intersected(QRectF(0, 0, 1, 1));
    region_ = regionMode_ == QLatin1String("manual") ? manualBox_ : regionMode_ == QLatin1String("whole") ? QRectF() : region_;
    if (!key_.isEmpty()) {
        st.setValue(settingsGroup(key_) + QStringLiteral("/region_mode"), regionMode_);
        st.setValue(settingsGroup(key_) + QStringLiteral("/region_box"), manualBox_);
    }
}

QString CameraFeed::regionMode() const {
    QMutexLocker l(&mutex_);
    return regionMode_;
}

QRectF CameraFeed::region() const {
    QMutexLocker l(&mutex_);
    return region_;
}

Readability CameraFeed::readability() const {
    QMutexLocker l(&mutex_);
    return read_;
}

void CameraFeed::optimise() {
    QMutexLocker l(&mutex_);
    optimisePending_ = true;
}

QString CameraFeed::lastReport() const {
    QMutexLocker l(&mutex_);
    return report_;
}

bool CameraFeed::saveStill(const QString& path, QString* error) const {
    QByteArray jpeg;
    {
        QMutexLocker l(&mutex_);
        jpeg = lastJpeg_;
    }
    if (jpeg.isEmpty()) {
        if (error) *error = QStringLiteral("no picture yet");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(jpeg) != jpeg.size()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

QJsonObject CameraFeed::describePicture() const {
    QMutexLocker l(&mutex_);
    QJsonArray cs;
    for (const CameraControl& c : controls_) {
        QJsonObject o{{QStringLiteral("name"), c.name}, {QStringLiteral("label"), c.label}, {QStringLiteral("value"), c.value},
                      {QStringLiteral("default"), c.def}, {QStringLiteral("min"), c.min}, {QStringLiteral("max"), c.max},
                      {QStringLiteral("kind"), c.kind == CameraControl::Bool ? QStringLiteral("bool") : c.kind == CameraControl::Menu ? QStringLiteral("menu") : QStringLiteral("int")}};
        if (c.inactive) o.insert(QStringLiteral("inactive"), true);
        if (c.kind == CameraControl::Menu && c.value - c.min < c.menu.size()) o.insert(QStringLiteral("value_text"), c.menu[c.value - c.min]);
        cs.append(o);
    }
    QJsonObject region{{QStringLiteral("mode"), regionMode_}};
    if (!region_.isEmpty()) region.insert(QStringLiteral("box"), QJsonArray{region_.x(), region_.y(), region_.width(), region_.height()});
    return {{QStringLiteral("camera"), name_},
            {QStringLiteral("width"), mode_.w},
            {QStringLiteral("height"), mode_.h},
            {QStringLiteral("controls"), cs},
            {QStringLiteral("region"), region},
            {QStringLiteral("readability"), QJsonObject{{QStringLiteral("verdict"), read_.verdict}, {QStringLiteral("score"), read_.score},
                                                         {QStringLiteral("sharpness"), std::round(read_.sharpness * 10) / 10},
                                                         {QStringLiteral("contrast"), read_.contrast}, {QStringLiteral("median"), read_.median},
                                                         {QStringLiteral("clipped"), std::round(read_.clipped * 1000) / 1000}}},
            {QStringLiteral("optimising"), bool(optimising_)},
            {QStringLiteral("last_optimise"), report_}};
}

void CameraFeed::run() {
    QString err;
    auto cam = Camera::open(id_, &err);
    if (!cam) {
        emit failed(err);
        return;
    }
    const auto modes = cam->modes();
    if (!cam->start(modes.empty() ? CameraMode{} : modes.front(), &err)) {   // full resolution: screens have small text
        emit failed(err);
        return;
    }
    // this camera's saved picture settings and screen region
    QSettings st;
    const QString group = settingsGroup(cam->key());
    const QVariantMap saved = st.value(group + QStringLiteral("/controls")).toMap();
    for (auto it = saved.begin(); it != saved.end(); ++it) cam->setControl(it.key(), it.value().toInt(), nullptr);
    // Optimise by itself, unless the user chose exposure or focus for this camera
    bool autoOptimise = autoAllowed_ && !cam->controls().empty();
    for (const char* k : {"exposure_auto", "exposure", "gain", "focus_auto", "focus"})
        if (saved.contains(QLatin1String(k))) autoOptimise = false;
    {
        QMutexLocker l(&mutex_);
        name_ = cam->name();
        mode_ = cam->mode();
        key_ = cam->key();
        controls_ = cam->controls();
        regionMode_ = st.value(group + QStringLiteral("/region_mode"), QStringLiteral("auto")).toString();
        manualBox_ = st.value(group + QStringLiteral("/region_box")).toRectF();
        region_ = regionMode_ == QLatin1String("manual") ? manualBox_ : QRectF();
    }
    emit opened(cam->name());
    emit controlsChanged();
    double lastPreview = 0, nextStill = 0, lastStored = 0, lastAnalysis = 0;
    const double openedAt = wallNow();
    QImage lastThumb, lastImage;
    int n = 0;
    // one frame, also shown live (Optimise grabs through this too)
    auto grabOne = [&](CameraFrame* f) {
        if (!cam->grab(f, &err)) return false;
        {
            QMutexLocker l(&mutex_);
            lastJpeg_ = f->jpeg;
        }
        const double now = wallNow();
        if (now - lastPreview >= 0.1) {
            lastPreview = now;
            const QImage img = decode(f->jpeg);
            if (!img.isNull()) {
                lastImage = img;
                emit preview(img);
            }
        }
        return true;
    };
    while (!stop_) {
        // settings asked for by the window or an agent
        {
            std::vector<std::pair<QString, int>> todo;
            bool reset;
            {
                QMutexLocker l(&mutex_);
                todo.swap(pending_);
                reset = resetPending_;
                resetPending_ = false;
            }
            if (reset || !todo.empty()) {
                QVariantMap user = st.value(group + QStringLiteral("/controls")).toMap();
                if (reset) {
                    cam->resetControls();
                    user.clear();
                }
                for (const auto& [name, value] : todo) {
                    QString e;
                    if (cam->setControl(name, value, &e)) user.insert(name, value);
                    else emit failed(e);
                }
                if (user.isEmpty()) st.remove(group + QStringLiteral("/controls"));
                else st.setValue(group + QStringLiteral("/controls"), user);
                QMutexLocker l(&mutex_);
                controls_ = cam->controls();
            }
            if (reset || !todo.empty()) emit controlsChanged();
        }
        CameraFrame f;
        if (!grabOne(&f)) {
            emit failed(err);
            break;
        }
        const double now = wallNow();
        // about once a second: where the screen is, and whether its text is readable
        if (now - lastAnalysis >= 1.0 && !lastImage.isNull()) {
            lastAnalysis = now;
            QRectF reg;
            QString modeNow;
            {
                QMutexLocker l(&mutex_);
                modeNow = regionMode_;
                reg = regionMode_ == QLatin1String("manual") ? manualBox_ : QRectF();
            }
            if (modeNow == QLatin1String("auto")) reg = detectScreen(lastImage);
            const Readability r = lt::readability(lastImage, reg);
            {
                QMutexLocker l(&mutex_);
                region_ = reg;
                read_ = r;
            }
            emit readabilityChanged();
            bool run = false;
            {
                QMutexLocker l(&mutex_);
                run = optimisePending_ || (autoOptimise && !userChose_ && now - openedAt > 1.5);
                optimisePending_ = false;
            }
            if (run) {
                autoOptimise = false;
                optimising_ = true;
                emit readabilityChanged();
                PictureIO io;
                io.grab = [&]() -> QImage {
                    CameraFrame g;
                    if (!grabOne(&g)) return {};
                    return decode(g.jpeg);   // this very frame (the live view is refreshed less often)
                };
                io.control = [&](const QString& name) -> std::optional<CameraControl> {
                    for (const CameraControl& c : cam->controls())
                        if (c.name == name) return c;
                    return std::nullopt;
                };
                io.set = [&](const QString& name, int v) { return cam->setControl(name, v, nullptr); };
                io.cancelled = [&] { return bool(stop_); };
                const OptimiseResult res = cam->controls().empty() ? OptimiseResult{false, QStringLiteral("This camera's settings cannot be changed by LibreTap."), r, r}
                                                                   : optimisePicture(io, reg);
                {
                    QMutexLocker l(&mutex_);
                    report_ = res.report;
                    controls_ = cam->controls();
                    read_ = res.after.valid ? res.after : r;
                }
                optimising_ = false;
                emit controlsChanged();
                emit readabilityChanged();
                emit optimised(res.report);
                continue;
            }
        }
        QMutexLocker l(&mutex_);
        if (!writer_) {
            nextStill = 0;
            lastThumb = QImage();
            n = 0;
            continue;
        }
        if (now < nextStill) continue;
        nextStill = (nextStill == 0 ? now : nextStill) + every_;
        if (nextStill < now) nextStill = now + every_;   // the camera was slower than the interval
        const QImage t = thumb(f.jpeg);
        if (!lastThumb.isNull() && changedPixels(t, lastThumb) < 4 && now - lastStored < 5.0) continue;
        lastThumb = t;
        lastStored = now;
        const QString name = QStringLiteral("frames/cam_%1.jpg").arg(++n, 6, 10, QLatin1Char('0'));
        QFile out(writer_->path(name));
        if (!out.open(QIODevice::WriteOnly) || out.write(f.jpeg) != f.jpeg.size()) {
            emit failed(QStringLiteral("cannot write %1 (disk full?)").arg(out.fileName()));
            continue;
        }
        out.close();
        const QJsonObject e = writer_->event(now, QStringLiteral("frame"),
                                             {{QStringLiteral("src"), QStringLiteral("camera")}, {QStringLiteral("file"), name},
                                              {QStringLiteral("full"), true}, {QStringLiteral("w"), mode_.w}, {QStringLiteral("h"), mode_.h}});
        ++stills_;
        if (source_) source_->stillWritten(e);
    }
    cam->stop();
}

QJsonObject cameraCommand(CameraFeed* feed, const QJsonObject& req) {
    auto fail = [](const QString& t) { return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), t}}; };
    if (!feed || !feed->isOpen()) return fail(QStringLiteral("no camera is open"));
    if (req.value(QStringLiteral("reset")).toBool()) feed->resetControls();
    const QJsonObject set = req.value(QStringLiteral("set")).toObject();
    if (!set.isEmpty()) {
        const auto cs = feed->controls();
        for (auto it = set.begin(); it != set.end(); ++it) {
            const auto c = std::find_if(cs.begin(), cs.end(), [&](const CameraControl& k) { return k.name == it.key(); });
            if (c == cs.end()) return fail(QStringLiteral("no setting '%1' on this camera").arg(it.key()));
            feed->setControl(it.key(), it.value().toInt());
        }
    }
    const QJsonValue region = req.value(QStringLiteral("region"));
    if (region.isString()) feed->setRegion(region.toString());
    else if (region.isArray()) {
        const QJsonArray a = region.toArray();
        if (a.size() != 4) return fail(QStringLiteral("region: x,y,w,h as fractions of the picture"));
        feed->setRegion(QStringLiteral("manual"), QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()));
    }
    if (req.value(QStringLiteral("optimise")).toBool()) {
        QEventLoop loop;
        QString report;
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        auto c = QObject::connect(feed, &CameraFeed::optimised, &loop, [&](const QString& r) { report = r; loop.quit(); });
        feed->optimise();
        loop.exec();
        QObject::disconnect(c);
        if (report.isEmpty()) return fail(QStringLiteral("Optimise did not finish within 20 s"));
    } else if (!set.isEmpty() || req.contains(QStringLiteral("reset")) || !region.isUndefined()) {
        QEventLoop loop;   // let the camera thread apply it and measure again
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const QString still = req.value(QStringLiteral("still")).toString();
    if (!still.isEmpty()) {
        QString e;
        if (!feed->saveStill(still, &e)) return fail(e);
    }
    QJsonObject o = feed->describePicture();
    o.insert(QStringLiteral("ok"), true);
    if (!still.isEmpty()) o.insert(QStringLiteral("still"), QFileInfo(still).absoluteFilePath());
    return o;
}

CameraSource::CameraSource(CameraFeed* feed, double every, QObject* parent) : Source(parent), feed_(feed), every_(every) {
    connect(feed_, &CameraFeed::failed, this, &Source::problem);
}

QJsonObject CameraSource::describe() const {
    const CameraMode m = feed_->mode();
    return {{QStringLiteral("id"), id()},
            {QStringLiteral("kind"), kind()},
            {QStringLiteral("label"), QStringLiteral("%1 %2x%3").arg(feed_->name()).arg(m.w).arg(m.h)},
            {QStringLiteral("camera"), feed_->id()},
            {QStringLiteral("width"), m.w},
            {QStringLiteral("height"), m.h},
            {QStringLiteral("every_s"), every_}};
}

bool CameraSource::start(SessionWriter* writer, QString* error) {
    if (!feed_->isOpen()) {
        if (error) *error = QStringLiteral("the camera is not open");
        return false;
    }
    feed_->attach(writer, every_, this);
    return true;
}

void CameraSource::stop() { feed_->detach(); }

QJsonObject CameraSource::stats() const { return {{QStringLiteral("stills"), feed_->stills()}}; }

}  // namespace lt
