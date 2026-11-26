// Camera stills with simulated cameras only (never a real one): the demo camera, and ffmpeg's test picture through
// the ffmpeg backend (the one Windows and macOS use for real cameras) when ffmpeg is installed.
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "core/camerasource.h"
#include "core/session.h"

using namespace lt;

namespace {
// Opens a feed, waits for it, records stills for `seconds`; returns the session.
Session stills(const QString& id, double seconds, int openTimeoutMs, QString* error) {
    CameraFeed feed;
    bool opened = false;
    QString failed;
    QObject::connect(&feed, &CameraFeed::opened, [&] { opened = true; });
    QObject::connect(&feed, &CameraFeed::failed, [&](const QString& e) { failed = e; });
    int previews = 0;
    QObject::connect(&feed, &CameraFeed::preview, [&](const QImage& img) { previews += !img.isNull(); });
    feed.open(id);
    for (int waited = 0; !opened && failed.isEmpty() && waited < openTimeoutMs; waited += 50) QTest::qWait(50);
    Session s;
    if (!opened) {
        *error = failed.isEmpty() ? QStringLiteral("did not open") : failed;
        return s;
    }
    QTemporaryDir dir;
    dir.setAutoRemove(false);
    SessionWriter w;
    w.create(dir.filePath(QStringLiteral("s")), {}, nullptr);
    CameraSource src(&feed, 0.5);
    if (!src.start(&w, error)) return s;
    QTest::qWait(int(seconds * 1000));
    src.stop();
    w.setSources({src.describe()});
    w.finish({{QStringLiteral("stats"), src.stats()}});
    feed.close();
    Session::load(dir.filePath(QStringLiteral("s")), &s, error);
    if (previews == 0) *error = QStringLiteral("no live pictures");
    return s;
}
}  // namespace

class TestCamera : public QObject {
    Q_OBJECT
private slots:
    void listingNeverShowsRealCameras() {
        QVERIFY(!Camera::systemCamerasAllowed());          // ctest sets LIBRETAP_NO_SYSTEM_CAMERAS=1
        for (const CameraInfo& c : Camera::list(true)) QVERIFY(!c.id.startsWith(QLatin1String("/dev/")));
        QString err;
        QVERIFY(!Camera::open(QStringLiteral("/dev/video0"), &err));   // refused without touching the device
        QVERIFY(err.contains(QStringLiteral("switched off")));
    }

    void demoStills() {
        QString err;
        const Session s = stills(QStringLiteral("demo"), 2.6, 10000, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const int frames = s.count(QStringLiteral("frame"));
        QVERIFY2(frames >= 3 && frames <= 7, qPrintable(QString::number(frames)));   // every 0.5 s while the screen changes
        const Event* f = nullptr;
        for (const Event& e : s.events)
            if (e.kind == QLatin1String("frame")) f = &e;
        QVERIFY(f->str("file").startsWith(QStringLiteral("frames/cam_")));
        const QImage img(QDir(s.folder).filePath(f->str("file")));
        QCOMPARE(img.size(), QSize(1280, 800));                 // full resolution
        QCOMPARE(s.source(QStringLiteral("camera")).value(QStringLiteral("width")).toInt(), 1280);
        QDir(s.folder).removeRecursively();
    }

    void ffmpegTestPicture() {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) QSKIP("ffmpeg is not installed");
        QString err;
        // ffmpeg can be slow to start (up to ~20 s on some machines): a long wait for the first picture
        const Session s = stills(QStringLiteral("ffmpeg:lavfi:testsrc2=size=640x360:rate=10"), 2.6, 90000, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QVERIFY(s.count(QStringLiteral("frame")) >= 3);
        for (const Event& e : s.events)
            if (e.kind == QLatin1String("frame")) QCOMPARE(QImage(QDir(s.folder).filePath(e.str("file"))).size(), QSize(640, 360));
        QDir(s.folder).removeRecursively();
    }
};

QTEST_MAIN(TestCamera)
#include "test_camera.moc"
