#pragma once
// Synthetic sessions for tests: a LibreTap serial-tap session and a Tick session of the same link, written as
// files in the format, with a known clock relation: tap_t = tick_t + offset + drift * (tick_t - tick_t0) (+ the
// few milliseconds between WriteFile and the bytes on the wire).
#include <QColor>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QRandomGenerator>

#include "core/session.h"

namespace synth {

struct Truth {
    double offset, drift, tickT0;
};

// A polling program: "MEAS?\r" every 0.7 s answered with a reading, and now and then a setting ("SET INT n\r").
// Tick sees WriteFile (dir out) slightly before the tap sees the bytes, ReadFile (dir in) 20-60 ms after.
// Tick also records the screen: a full frame first, then changed regions, with a full frame every 10th.
inline Truth write(const QString& tapFolder, const QString& tickFolder, double offset, double drift, double seconds = 600,
                   bool withScreen = true, int settingsEvery = 9) {
    QRandomGenerator rng(42);
    const double tap0 = 1796031300.0;               // the tap's clock at the start
    const double tick0 = tap0 - offset;             // the Tick PC's clock reads this at the same moment
    lt::SessionWriter tap, tick;
    tap.create(tapFolder, {{QStringLiteral("recorder"), QStringLiteral("LibreTap test")}}, nullptr);
    tap.setSources({QJsonObject{{QStringLiteral("id"), QStringLiteral("tap")}, {QStringLiteral("kind"), QStringLiteral("serial-tap")},
                                {QStringLiteral("label"), QStringLiteral("Serial tap 9600 baud 8N1")}, {QStringLiteral("baud"), 9600},
                                {QStringLiteral("names"), QJsonObject{{QStringLiteral("a"), QStringLiteral("PC")}, {QStringLiteral("b"), QStringLiteral("Instrument")}}}}});
    tick.create(tickFolder, {{QStringLiteral("recorder"), QStringLiteral("Tick 0.1.0")}, {QStringLiteral("host"), QStringLiteral("WIN2000-LAB")}}, nullptr);
    QJsonArray tickSources{QJsonObject{{QStringLiteral("id"), QStringLiteral("com1")}, {QStringLiteral("kind"), QStringLiteral("api-serial")},
                                       {QStringLiteral("label"), QStringLiteral("COM1 (VENDOR.EXE)")}}};
    if (withScreen)
        tickSources.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("screen")}, {QStringLiteral("kind"), QStringLiteral("screen")},
                                       {QStringLiteral("label"), QStringLiteral("Screen 320x200")}, {QStringLiteral("width"), 320}, {QStringLiteral("height"), 200}});
    tick.setSources(tickSources);
    // true time τ (seconds since start, tap clock) → tick clock: tick = tick0 + τ' where τ = τ' + drift τ'
    auto tickClock = [&](double tau) { return tick0 + tau / (1 + drift); };
    double tau = 0.3;
    int n = 0, frames = 0;
    QImage screen(320, 200, QImage::Format_RGB32);
    screen.fill(QColor(0, 128, 128));
    while (tau < seconds) {
        ++n;
        QByteArray cmd = settingsEvery && n % settingsEvery == 0 ? "SET INT " + QByteArray::number(rng.bounded(1, 5000)) + "\r" : QByteArray("MEAS?\r");
        QByteArray ans = settingsEvery && n % settingsEvery == 0 ? QByteArray("OK\r\n") : "M " + QByteArray::number(rng.bounded(10000) / 10000.0, 'f', 4) + " V\r\n";
        const double wire = 0.001 + rng.bounded(0.003);          // WriteFile → first byte on the wire
        tick.event(tickClock(tau), QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("com1")}, {QStringLiteral("dir"), QStringLiteral("out")},
                                                             {QStringLiteral("hex"), QString::fromLatin1(cmd.toHex())}, {QStringLiteral("call"), QStringLiteral("WriteFile")}});
        tap.event(tap0 + tau + wire, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("a")},
                                                               {QStringLiteral("hex"), QString::fromLatin1(cmd.toHex())}});
        const double answer = tau + wire + cmd.size() * 10.0 / 9600 + 0.010;
        tap.event(tap0 + answer, QStringLiteral("data"), {{QStringLiteral("src"), QStringLiteral("tap")}, {QStringLiteral("dir"), QStringLiteral("b")},
                                                          {QStringLiteral("hex"), QString::fromLatin1(ans.toHex())}});
        tick.event(tickClock(answer + 0.02 + rng.bounded(0.04)), QStringLiteral("data"),
                   {{QStringLiteral("src"), QStringLiteral("com1")}, {QStringLiteral("dir"), QStringLiteral("in")},
                    {QStringLiteral("hex"), QString::fromLatin1(ans.toHex())}, {QStringLiteral("call"), QStringLiteral("ReadFile")}});
        if (withScreen && n % 3 == 1) {   // the reading on the screen changes: a region (or a full frame)
            const bool full = frames % 10 == 0;
            QImage region(120, 30, QImage::Format_RGB32);
            region.fill(QColor::fromHsv((n * 37) % 360, 200, 220));
            const QString file = QStringLiteral("frames/%1.png").arg(++frames, 6, 10, QLatin1Char('0'));
            if (full) {
                QImage f = screen;
                for (int y = 0; y < 30; ++y)
                    for (int x = 0; x < 120; ++x) f.setPixel(100 + x, 80 + y, region.pixel(x, y));
                f.save(QDir(tickFolder).filePath(file));
                screen = f;
                tick.event(tickClock(answer + 0.1), QStringLiteral("frame"), {{QStringLiteral("src"), QStringLiteral("screen")}, {QStringLiteral("file"), file}, {QStringLiteral("full"), true}});
            } else {
                region.save(QDir(tickFolder).filePath(file));
                tick.event(tickClock(answer + 0.1), QStringLiteral("frame"),
                           {{QStringLiteral("src"), QStringLiteral("screen")}, {QStringLiteral("file"), file}, {QStringLiteral("full"), false},
                            {QStringLiteral("x"), 100}, {QStringLiteral("y"), 80}, {QStringLiteral("w"), 120}, {QStringLiteral("h"), 30}});
            }
        }
        tau += settingsEvery ? 0.7 + rng.bounded(0.05) : 0.5;   // a strict poller: identical writes every 0.5 s
    }
    tap.finish();
    tick.finish();
    return {offset, drift, tick0};
}

}  // namespace synth
