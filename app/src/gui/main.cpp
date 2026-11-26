// LibreTap: graphical program.   LibreTap [RECORDING-FOLDER | --demo]
//
// Testing and screenshots (never a real camera: set LIBRETAP_NO_SYSTEM_CAMERAS=1):
//   LIBRETAP_SPLASH_PNG=file.png[,light]   save the splash artwork and quit
//   LIBRETAP_ICON_PNG=file.png[,size]      save the icon and quit
//   LIBRETAP_SHOT=file.png[,seconds]       save a screenshot of the window after that time (default 3) and quit
//   LIBRETAP_SHOT_RECORD=a,b               press Record at a seconds and Stop at b seconds (b optional)
//   LIBRETAP_SHOT_MARKS="1.5:note;2.5:note" marks at those seconds
//   LIBRETAP_SHOT_ADD=folder               add that session (e.g. a Tick session) to the opened recording
//   LIBRETAP_SHOT_TIME=seconds             move the timeline to that time after the start
#include <QApplication>
#include <QPalette>
#include <QTimer>

#include "core/util.h"
#include "mainwindow.h"
#include "splash.h"

using namespace lt::gui;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("LibreTap"));
    QApplication::setApplicationName(QStringLiteral("LibreTap"));
    QApplication::setApplicationVersion(QStringLiteral(LIBRETAP_VERSION));
    QApplication::setWindowIcon(appIcon());
    {
        // accents only (teal from the splash, red for recording, amber for marks): light and dark themes both work
        QPalette pal = app.palette();
        pal.setColor(QPalette::Highlight, QColor(0x1f8f87));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        app.setPalette(pal);
        app.setStyleSheet(QStringLiteral(
            "QGroupBox { font-weight: 600; }"
            "QGroupBox::title { color: #1f8f87; }"
            "QLabel#dot { border-radius: 7px; background: #9aa3ad; margin-top: 2px; }"
            "QLabel#dot[state=\"ok\"] { background: #2eaa5c; }"
            "QLabel#dot[state=\"warn\"] { background: #e0a800; }"
            "QPushButton#record { background: #fbeaea; color: #b3261e; border: 2px solid #d9a3a0; border-radius: 8px;"
            "  font-weight: 700; font-size: 18pt; }"
            "QPushButton#record:hover { background: #f6dcdc; }"
            "QPushButton#record[recording=\"true\"] { background: #c62828; color: white; border-color: #a31f1f; }"
            "QPushButton#record:disabled { color: rgba(179,38,30,110); border-color: rgba(217,163,160,110); }"
            "QPushButton#mark { background: #fdf0dc; color: #8a4b00; border: 2px solid #e8b264; border-radius: 6px; font-weight: 700; font-size: 12pt; }"
            "QPushButton#mark:disabled { color: rgba(138,75,0,100); border-color: rgba(232,178,100,100); }"
            "QPushButton#link { color: #1f8f87; text-decoration: underline; border: none; padding: 2px; }"
            "QLabel#title { font-size: 13pt; }"
            "QHeaderView::section { background: rgba(31,143,135,24); border: none;"
            "  border-right: 1px solid rgba(31,143,135,40); border-bottom: 1px solid rgba(31,143,135,110); padding: 3px 6px; }"));
    }
    if (qEnvironmentVariableIsSet("LIBRETAP_SPLASH_PNG")) {
        const QString v = qEnvironmentVariable("LIBRETAP_SPLASH_PNG");
        return drawSplash(2.0, v.section(QLatin1Char(','), 1) != QLatin1String("light")).save(v.section(QLatin1Char(','), 0, 0)) ? 0 : 1;
    }
    if (qEnvironmentVariableIsSet("LIBRETAP_ICON_PNG")) {
        const QString v = qEnvironmentVariable("LIBRETAP_ICON_PNG");
        const int size = v.section(QLatin1Char(','), 1).isEmpty() ? 256 : v.section(QLatin1Char(','), 1).toInt();
        return drawIcon(size).save(v.section(QLatin1Char(','), 0, 0)) ? 0 : 1;
    }
    Splash splash;
    splash.show();
    app.processEvents();
    MainWindow w;
    const QStringList args = app.arguments();
    QTimer::singleShot(1200, &w, [&] {
        w.show();
        splash.finish(&w);
        if (args.contains(QStringLiteral("--demo"))) w.setDemo(true);
        for (const QString& a : args.mid(1))
            if (!a.startsWith(QLatin1String("--"))) w.openRecording(a);
        if (const QString add = qEnvironmentVariable("LIBRETAP_SHOT_ADD"); !add.isEmpty()) w.addSession(add);
    });
    const QString shot = qEnvironmentVariable("LIBRETAP_SHOT");
    if (!shot.isEmpty()) {
        const int ms = shot.section(QLatin1Char(','), 1).isEmpty() ? 3000 : int(1000 * shot.section(QLatin1Char(','), 1).toDouble());
        const QStringList rec = qEnvironmentVariable("LIBRETAP_SHOT_RECORD").split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (!rec.isEmpty()) QTimer::singleShot(int(rec[0].toDouble() * 1000), &w, [&w] { w.startRecording(); });
        if (rec.size() > 1) QTimer::singleShot(int(rec[1].toDouble() * 1000), &w, [&w] { w.stopRecording(); });
        for (const QString& m : qEnvironmentVariable("LIBRETAP_SHOT_MARKS").split(QLatin1Char(';'), Qt::SkipEmptyParts))
            QTimer::singleShot(int(m.section(QLatin1Char(':'), 0, 0).toDouble() * 1000), &w,
                               [&w, m] { w.recorder().mark(m.section(QLatin1Char(':'), 1)); });
        QTimer::singleShot(ms, &w, [&w, shot] {
            if (qEnvironmentVariableIsSet("LIBRETAP_SHOT_TIME"))
                w.setTimelineTime(w.project().start() + qEnvironmentVariable("LIBRETAP_SHOT_TIME").toDouble());
            QTimer::singleShot(300, &w, [&w, shot] {
                w.grab().save(shot.section(QLatin1Char(','), 0, 0));
                w.recorder().stop();
                QApplication::quit();
            });
        });
    }
    return app.exec();
}
