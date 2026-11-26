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
//   LIBRETAP_SHOT_SIZE=WxH                 window size
//   LIBRETAP_SHOT_PROTOCOL=file.json       load a protocol description
//   LIBRETAP_SHOT_VIEW=compare,A,B | exchanges | hints   open that analysis window (screenshot it)
#include <QApplication>
#include <QDialog>
#include <QTimer>

#include "core/util.h"
#include "mainwindow.h"
#include "splash.h"
#include "theme.h"

using namespace lt::gui;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("LibreTap"));
    QApplication::setApplicationName(QStringLiteral("LibreTap"));
    QApplication::setApplicationVersion(QStringLiteral(LIBRETAP_VERSION));
    QApplication::setWindowIcon(appIcon());
    theme::apply(app);
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
        if (const QString sz = qEnvironmentVariable("LIBRETAP_SHOT_SIZE"); !sz.isEmpty())   // testing: WxH
            w.resize(sz.section(QLatin1Char('x'), 0, 0).toInt(), sz.section(QLatin1Char('x'), 1, 1).toInt());
        w.show();
        splash.finish(&w);
        const int demo = args.indexOf(QStringLiteral("--demo"));
        const QString demoName = demo > 0 && demo + 1 < args.size() && !args[demo + 1].startsWith(QLatin1String("--")) ? args[demo + 1] : QString();
        if (!demoName.isEmpty()) w.openDemo(demoName);        // LibreTap --demo cora: an example recording
        else if (demo > 0) w.setDemo(true);                  // LibreTap --demo: the pretend instrument
        for (int i = 1; i < args.size(); ++i)
            if (!args[i].startsWith(QLatin1String("--")) && args[i] != demoName) w.openRecording(args[i]);
        if (const QString pr = qEnvironmentVariable("LIBRETAP_SHOT_PROTOCOL"); !pr.isEmpty()) w.loadProtocol(pr);
        if (const QString v = qEnvironmentVariable("LIBRETAP_SHOT_VIEW"); !v.isEmpty()) {   // testing: compare, exchanges, hints
            if (v.startsWith(QLatin1String("compare"))) w.openCompare(v.section(QLatin1Char(','), 1, 1).toInt(), v.section(QLatin1Char(','), 2, 2).toInt());
            else if (v == QLatin1String("exchanges")) w.showExchanges();
            else if (v == QLatin1String("hints")) w.showHints();
        }
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
                QWidget* target = &w;
                for (QWidget* t : QApplication::topLevelWidgets())   // an analysis window on top of it
                    if (qobject_cast<QDialog*>(t) && t->isVisible()) target = t;
                target->grab().save(shot.section(QLatin1Char(','), 0, 0));
                w.recorder().stop();
                QApplication::quit();
            });
        });
    }
    return app.exec();
}
