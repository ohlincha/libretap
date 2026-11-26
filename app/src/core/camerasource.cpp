#include "camerasource.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QMutexLocker>

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
    {
        QMutexLocker l(&mutex_);
        name_ = cam->name();
        mode_ = cam->mode();
    }
    emit opened(cam->name());
    double lastPreview = 0, nextStill = 0, lastStored = 0;
    QImage lastThumb;
    int n = 0;
    while (!stop_) {
        CameraFrame f;
        if (!cam->grab(&f, &err)) {
            emit failed(err);
            break;
        }
        const double now = wallNow();
        if (now - lastPreview >= 0.1) {
            lastPreview = now;
            QBuffer b;
            b.setData(f.jpeg);
            b.open(QIODevice::ReadOnly);
            QImageReader r(&b, "JPG");
            const QSize full = r.size();
            if (full.width() > 960) r.setScaledSize(full.scaled(960, 960, Qt::KeepAspectRatio));
            const QImage img = r.read();
            if (!img.isNull()) emit preview(img);
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
