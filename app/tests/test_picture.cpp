// Picture quality for reading a screen through a camera: the readability measure on synthetic pictures (sharp,
// blurred, burnt out, dark, flat), finding the screen in the picture, and Optimise picture against the simulated
// camera (sim: autofocus; sim:manualfocus: focus sweep), also through CameraFeed and the "camera" control request.
// Never a real camera (LIBRETAP_NO_SYSTEM_CAMERAS=1).
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include "core/camera.h"
#include "core/camerasource.h"
#include "core/picture.h"

using namespace lt;

namespace {
// A camera's view of a screen with text: dim room, white screen, dark strokes 2–4 px wide.
QImage screenPicture(double light = 1.0, double blur = 0, double contrast = 1.0, double room = 0.08) {
    QImage img(960, 540, QImage::Format_RGB32);
    auto g = [&](double l) { const int v = std::clamp(int(std::lround(128 + (255 * l * light - 128) * contrast)), 0, 255); return QColor(v, v, v); };
    img.fill(g(room));
    QPainter p(&img);
    const QRect screen(165, 90, 630, 390);
    p.fillRect(screen, g(1.0));
    quint32 r = 7;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return (r >> 8) & 0xffff; };
    for (int line = 0; line < 13; ++line)
        for (int x = 0; x < 570;) {
            const int w = 2 + int(rnd() % 3), h = rnd() % 5 ? 8 : 11;
            if (rnd() % 9 == 0) { x += 10; continue; }
            p.fillRect(QRect(195 + x, 115 + line * 27 + (11 - h), w, h), g(0.04));
            x += w + 2 + int(rnd() % 3);
        }
    p.end();
    if (blur > 0) {
        const QSize s(int(960 / (1 + blur)), int(540 / (1 + blur)));
        img = img.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).scaled(960, 540, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return img;
}
const QRectF kScreen(165.0 / 960, 90.0 / 540, 630.0 / 960, 390.0 / 540);

QImage decode(const QByteArray& jpeg) {
    QBuffer b;
    b.setData(jpeg);
    b.open(QIODevice::ReadOnly);
    return QImageReader(&b, "JPG").read();
}
}  // namespace

class TestPicture : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qputenv("LIBRETAP_NO_SYSTEM_CAMERAS", "1");
        QCoreApplication::setOrganizationName(QStringLiteral("LibreTapTest"));
        QCoreApplication::setApplicationName(QStringLiteral("test_picture"));
        QSettings().clear();
    }

    void verdicts_data() {
        QTest::addColumn<double>("light");
        QTest::addColumn<double>("blur");
        QTest::addColumn<double>("contrast");
        QTest::addColumn<QString>("verdict");
        QTest::newRow("sharp") << 0.92 << 0.0 << 1.0 << "Text readable";
        QTest::newRow("slightly soft") << 0.92 << 0.4 << 1.0 << "Text readable";
        QTest::newRow("blurred") << 0.92 << 2.5 << 1.0 << "Too blurred";
        QTest::newRow("very blurred") << 0.92 << 5.0 << 1.0 << "Too blurred";
        QTest::newRow("burnt out") << 3.0 << 0.0 << 1.0 << "Too bright (text burnt out)";
        QTest::newRow("dark") << 0.18 << 0.0 << 1.0 << "Too dark";
        QTest::newRow("flat") << 0.9 << 0.0 << 0.25 << "Low contrast";
    }
    void verdicts() {
        QFETCH(double, light);
        QFETCH(double, blur);
        QFETCH(double, contrast);
        QFETCH(QString, verdict);
        const Readability r = readability(screenPicture(light, blur, contrast), kScreen);
        QVERIFY2(r.verdict == verdict, qPrintable(QStringLiteral("%1 (sharpness %2, contrast %3, clipped %4, high %5)")
                                                      .arg(r.verdict).arg(r.sharpness).arg(r.contrast).arg(r.clipped).arg(r.high)));
        QCOMPARE(r.ok(), verdict == QLatin1String("Text readable"));
        QVERIFY(r.ok() ? r.score >= 50 : r.score < 50);
    }

    void sharpnessDoesNotDependOnResolution() {
        const QImage big = screenPicture(0.92), small = big.scaled(640, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const double a = readability(big, kScreen).sharpness, b = readability(small, kScreen).sharpness;
        QVERIFY2(std::abs(a - b) < 0.5 * std::max(a, b), qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
    }

    void findsTheScreen() {
        const QRectF r = detectScreen(screenPicture(0.92));
        QVERIFY(!r.isEmpty());
        QVERIFY2(std::abs(r.x() - kScreen.x()) < 0.03 && std::abs(r.width() - kScreen.width()) < 0.04 && std::abs(r.y() - kScreen.y()) < 0.04 &&
                     std::abs(r.height() - kScreen.height()) < 0.05,
                 qPrintable(QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())));
        // a uniform picture: no screen
        QImage flat(320, 180, QImage::Format_RGB32);
        flat.fill(QColor(120, 120, 120));
        QVERIFY(detectScreen(flat).isEmpty());
        QVERIFY(detectScreen(QImage()).isEmpty());
    }

    void simulatedCameraControls() {
        QString err;
        auto cam = Camera::open(QStringLiteral("sim"), &err);
        QVERIFY2(cam, qPrintable(err));
        QVERIFY(cam->start({}, &err));
        const auto cs = cam->controls();
        QVERIFY(std::any_of(cs.begin(), cs.end(), [](const CameraControl& c) { return c.name == QLatin1String("exposure_auto"); }));
        QVERIFY(!cam->setControl(QStringLiteral("nonsense"), 1, &err));
        // automatic exposure burns the screen out, as a webcam in a dim room does
        CameraFrame f;
        QVERIFY(cam->grab(&f, &err));
        QCOMPARE(readability(decode(f.jpeg), detectScreen(decode(f.jpeg))).verdict, QStringLiteral("Too bright (text burnt out)"));
    }

    void optimiseFixesExposure() {
        for (const QString id : {QStringLiteral("sim"), QStringLiteral("sim:manualfocus")}) {
            QString err;
            auto cam = Camera::open(id, &err);
            QVERIFY(cam && cam->start({}, &err));
            PictureIO io;
            io.grab = [&]() -> QImage {
                CameraFrame f;
                return cam->grab(&f, &err) ? decode(f.jpeg) : QImage();
            };
            io.control = [&](const QString& n) -> std::optional<CameraControl> {
                for (const CameraControl& c : cam->controls())
                    if (c.name == n) return c;
                return std::nullopt;
            };
            io.set = [&](const QString& n, int v) { return cam->setControl(n, v, nullptr); };
            CameraFrame f;
            QVERIFY(cam->grab(&f, &err));
            const QRectF region = detectScreen(decode(f.jpeg));
            QVERIFY(!region.isEmpty());
            const OptimiseResult res = optimisePicture(io, region, 30);
            QVERIFY2(!res.before.ok(), qPrintable(res.before.verdict));
            QVERIFY2(res.after.ok(), qPrintable(res.report + QStringLiteral(" sharpness %1 clipped %2 high %3").arg(res.after.sharpness).arg(res.after.clipped).arg(res.after.high)));
            QVERIFY(res.report.contains(QStringLiteral("manual")));
            if (id == QLatin1String("sim:manualfocus")) QVERIFY2(res.report.contains(QStringLiteral("focus set to")), qPrintable(res.report));
            else QVERIFY(res.report.contains(QStringLiteral("autofocus on")));
        }
    }

    void cancelStopsOptimise() {
        QString err;
        auto cam = Camera::open(QStringLiteral("sim:manualfocus"), &err);
        QVERIFY(cam && cam->start({}, &err));
        int frames = 0;
        PictureIO io;
        io.grab = [&]() -> QImage {
            ++frames;
            CameraFrame f;
            return cam->grab(&f, &err) ? decode(f.jpeg) : QImage();
        };
        io.control = [&](const QString& n) -> std::optional<CameraControl> {
            for (const CameraControl& c : cam->controls())
                if (c.name == n) return c;
            return std::nullopt;
        };
        io.set = [&](const QString& n, int v) { return cam->setControl(n, v, nullptr); };
        io.cancelled = [&] { return frames > 3; };
        QElapsedTimer t;
        t.start();
        optimisePicture(io, {}, 30);
        QVERIFY(t.elapsed() < 3000);
    }

    void feedOptimisesAndRemembers() {
        CameraFeed feed;
        QSignalSpy opt(&feed, &CameraFeed::optimised);
        feed.open(QStringLiteral("sim"));
        QVERIFY(opt.wait(30000));   // by itself, once, after opening
        QTRY_VERIFY_WITH_TIMEOUT(feed.readability().ok(), 5000);
        QVERIFY(!feed.region().isEmpty());
        // a user's change is applied and saved for this camera
        feed.setControl(QStringLiteral("brightness"), 140);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const CameraControl& c : feed.controls())
                if (c.name == QLatin1String("brightness")) return c.value == 140;
            return false;
        }(), 3000);
        QCOMPARE(QSettings().value(QStringLiteral("camera/Simulated_camera/controls")).toMap().value(QStringLiteral("brightness")).toInt(), 140);
        // the "camera" control request
        QTemporaryDir dir;
        const QString still = dir.filePath(QStringLiteral("s.jpg"));
        QJsonObject a = cameraCommand(&feed, {{QStringLiteral("cmd"), QStringLiteral("camera")}, {QStringLiteral("still"), still}});
        QVERIFY2(a.value(QStringLiteral("ok")).toBool(), qPrintable(a.value(QStringLiteral("error")).toString()));
        QVERIFY(QFileInfo(still).size() > 1000);
        QVERIFY(!a.value(QStringLiteral("controls")).toArray().isEmpty());
        a = cameraCommand(&feed, {{QStringLiteral("cmd"), QStringLiteral("camera")}, {QStringLiteral("set"), QJsonObject{{QStringLiteral("bogus"), 1}}}});
        QVERIFY(!a.value(QStringLiteral("ok")).toBool());
        a = cameraCommand(&feed, {{QStringLiteral("cmd"), QStringLiteral("camera")}, {QStringLiteral("region"), QJsonArray{0.1, 0.1, 0.5, 0.5}}});
        QCOMPARE(a.value(QStringLiteral("region")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("manual"));
        a = cameraCommand(&feed, {{QStringLiteral("cmd"), QStringLiteral("camera")}, {QStringLiteral("region"), QStringLiteral("auto")}, {QStringLiteral("reset"), true}});
        QVERIFY(a.value(QStringLiteral("ok")).toBool());
        QVERIFY(QSettings().value(QStringLiteral("camera/Simulated_camera/controls")).toMap().isEmpty());
        a = cameraCommand(&feed, {{QStringLiteral("cmd"), QStringLiteral("camera")}, {QStringLiteral("optimise"), true}});
        QVERIFY2(a.value(QStringLiteral("last_optimise")).toString().contains(QStringLiteral("Text readable")), qPrintable(a.value(QStringLiteral("last_optimise")).toString()));
        feed.close();
        // an ffmpeg/demo camera has no controls: Optimise says so
        CameraFeed demo;
        QSignalSpy o2(&demo, &CameraFeed::optimised);
        demo.open(QStringLiteral("demo"));
        QTRY_VERIFY_WITH_TIMEOUT(demo.readability().valid, 5000);
        demo.optimise();
        QVERIFY(o2.wait(5000));
        QVERIFY(o2.first().first().toString().contains(QStringLiteral("cannot be changed")));
        demo.close();
    }
};

QTEST_MAIN(TestPicture)
#include "test_picture.moc"
