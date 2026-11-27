// The window, offscreen: a demo recording with a mark, the timeline afterwards, control through the socket while
// the window records, and a Tick session added to the recording.
#include <QApplication>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>
#include "core/camerasource.h"
#include <QLabel>
#include <QComboBox>
#include <QSignalSpy>
#include <QDialog>
#include <QSlider>

#include <atomic>
#include <thread>

#include "core/control.h"
#include "gui/mainwindow.h"
#include "gui/splash.h"
#include "synth.h"

using namespace lt;

namespace {
// A control client in its own thread, as an agent's LibreTapCLI would be (the window answers from its event loop).
bool ask(const QJsonObject& req, QJsonObject* ans, QString* err) {
    std::atomic<bool> done{false};
    bool ok = false;
    std::thread t([&] {
        ok = controlRequest(req, ans, err);
        done = true;
    });
    while (!done) QTest::qWait(10);
    t.join();
    return ok;
}
}  // namespace

class TestGui : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qputenv("LIBRETAP_HOME", ws_.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", ws_.filePath(QStringLiteral("config")).toLocal8Bit());
    }

    // the simulated camera with picture controls: readability under the live view, Optimise by itself, the Picture
    // panel reaching the camera
    void cameraPicture() {
        qputenv("LIBRETAP_SIM_CAMERA", "1");
        gui::MainWindow w;
        w.resize(1400, 900);
        w.show();
        auto* combo = w.findChild<QComboBox*>();
        (void)combo;
        QTRY_VERIFY_WITH_TIMEOUT(w.camera().isOpen(), 5000);
        QSignalSpy opt(&w.camera(), &CameraFeed::optimised);
        QVERIFY(opt.wait(30000));
        QLabel* read = w.findChild<QLabel*>(QStringLiteral("readability"));
        QVERIFY(read && read->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(read->text().contains(QStringLiteral("Text readable")), 5000);
        // the Picture panel: a slider reaches the camera
        QPushButton* pic = nullptr;
        for (auto* b : w.findChildren<QPushButton*>())
            if (b->text() == QStringLiteral("Picture…")) pic = b;
        QVERIFY(pic);
        pic->click();
        QDialog* d = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((d = w.findChild<QDialog*>()) && d->isVisible(), 3000);
        auto* bright = d->findChild<QSlider*>(QStringLiteral("brightness"));
        QVERIFY(bright);
        const int before = bright->value();
        bright->triggerAction(QAbstractSlider::SliderPageStepAdd);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const CameraControl& c : w.camera().controls())
                if (c.name == QLatin1String("brightness")) return c.value > before;
            return false;
        }(), 3000);
        QVERIFY(d->findChild<QComboBox*>(QStringLiteral("regionMode")));
        if (!qEnvironmentVariableIsEmpty("LIBRETAP_TEST_SHOT_CAMERA")) {
            QTest::qWait(1200);
            w.grab().save(qEnvironmentVariable("LIBRETAP_TEST_SHOT_CAMERA"));
            d->grab().save(qEnvironmentVariable("LIBRETAP_TEST_SHOT_CAMERA").replace(QStringLiteral(".png"), QStringLiteral("-panel.png")));
        }
        d->close();
        w.close();
        qunsetenv("LIBRETAP_SIM_CAMERA");
    }

    void splashAndIcon() {
        const QImage dark = gui::drawSplash(1.0, true), light = gui::drawSplash(1.0, false);
        QCOMPARE(dark.size(), QSize(gui::kSplashW, gui::kSplashH));
        QVERIFY(dark != light);
        QVERIFY(!gui::drawIcon(64).isNull());
    }

    void recordAndReview() {
        gui::MainWindow w;
        w.show();
        QVERIFY(w.controlProblem().isEmpty());
        w.setDemo(true);
        auto* record = w.findChild<QPushButton*>(QStringLiteral("record"));
        QVERIFY(record && record->isEnabled());
        QTest::qWait(1500);                                   // the demo camera starts
        record->click();
        QVERIFY(w.recorder().isRecording());
        QTest::qWait(1200);
        // an agent marks while the operator works
        QJsonObject ans;
        QString err;
        QVERIFY2(ask({{QStringLiteral("cmd"), QStringLiteral("mark")}, {QStringLiteral("note"), QStringLiteral("gain x4")}}, &ans, &err), qPrintable(err));
        QCOMPARE(ans.value(QStringLiteral("mark")).toInt(), 1);
        QTest::keyClick(&w, Qt::Key_Space);                   // the operator marks too
        QTest::qWait(1200);
        QVERIFY(ask({{QStringLiteral("cmd"), QStringLiteral("stop")}}, &ans, &err));
        QVERIFY(!w.recorder().isRecording());
        // the timeline is shown with the recording
        QCOMPARE(w.project().sessions.size(), size_t(1));
        const Session& s = w.project().sessions[0].session;
        QCOMPARE(s.count(QStringLiteral("mark")), 2);
        QVERIFY(s.count(QStringLiteral("data")) >= 8);
        QVERIFY(s.count(QStringLiteral("frame")) >= 1);
        QTableView* rows = nullptr;
        for (auto* t : w.findChildren<QTableView*>())
            if (t->isVisible()) rows = t;
        QVERIFY(rows && rows->model()->rowCount() > 8);
        // a message selected: the screen before it and after it side by side, changes outlined
        {
            int found = 0;
            for (int r = rows->model()->rowCount() - 1; r >= 0 && !found; --r) {
                rows->setCurrentIndex(rows->model()->index(r, 0));
                for (auto* v : w.findChildren<gui::PictureView*>())
                    if (v->isVisible() && v->caption().contains(QStringLiteral("s after")) && !v->highlights().empty()) found = r;
            }
            QVERIFY2(found > 0, "no message showed a changed picture after it");
            if (!qEnvironmentVariableIsEmpty("LIBRETAP_TEST_SHOT")) w.grab().save(qEnvironmentVariable("LIBRETAP_TEST_SHOT"));
        }
        // start again from the socket ("start" uses the window's connections)
        QVERIFY(ask({{QStringLiteral("cmd"), QStringLiteral("start")}}, &ans, &err));
        QVERIFY2(ans.value(QStringLiteral("ok")).toBool(), qPrintable(ans.value(QStringLiteral("error")).toString()));
        QTest::qWait(600);
        w.stopRecording();
        w.close();
    }

    void addTick() {
        QTemporaryDir dir;
        synth::write(dir.filePath(QStringLiteral("rec/tap")), dir.filePath(QStringLiteral("tick")), 100, 0, 60, true);
        QFile pj(dir.filePath(QStringLiteral("rec/project.json")));
        QVERIFY(pj.open(QIODevice::WriteOnly));
        pj.write(R"({"format":"libretap-project","version":1,"sessions":[{"path":"tap","how":"reference"}]})");
        pj.close();
        gui::MainWindow w;
        w.show();
        QVERIFY(w.openRecording(dir.filePath(QStringLiteral("rec"))));
        QVERIFY(w.addSession(dir.filePath(QStringLiteral("tick"))));
        QCOMPARE(w.project().sessions.size(), size_t(2));
        QCOMPARE(w.project().sessions[1].how, QStringLiteral("traffic"));
        w.setTimelineTime(w.project().start() + 30);
        auto* view = w.findChild<gui::PictureView*>();
        QVERIFY(view);
        bool shown = false;
        for (auto* v : w.findChildren<gui::PictureView*>()) shown |= v->isVisible() && !v->image().isNull();
        QVERIFY(shown);                                       // the Tick screen at that time
        w.close();
    }

private:
    QTemporaryDir ws_;
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
