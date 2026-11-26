#pragma once
// What is plugged in, in plain words: the USB-serial adapters of the serial tap and the cameras.
// LIBRETAP_FAKE_ADAPTERS="path=label;path=label" adds ports that are not USB adapters (pseudo-terminals in tests
// and screenshots).
#include <QJsonObject>
#include <QString>

#include <vector>

#include "camera.h"

namespace lt {

struct Adapter {
    QString path;                  // what to open: /dev/ttyUSB0, COM3
    QString port;                  // short name: ttyUSB0, COM3
    QString chip;                  // FTDI, Prolific PL2303, … (from the USB vendor id)
    QString description, manufacturer, serial;
    int vid = 0, pid = 0;
    bool usb = false;
    QString label() const;         // "ttyUSB0 (FTDI FT232R USB UART)"
    QJsonObject toJson() const;
};

// The Pico USB sniffers (usb-sniffer-lite: Raspberry Pi vendor id 2e8a, or "sniffer" in its name);
// LIBRETAP_FAKE_PICO=path adds one (a pseudo-terminal in tests).
std::vector<Adapter> listPicos();

// USB-serial adapters (and fake ones); with all, also built-in and other serial ports.
std::vector<Adapter> listAdapters(bool all = false);

struct TapGuess {
    bool ready = false;
    QString a, b;                  // paths: PC side, instrument side
    QString sentence;
};
// Picks the PC side and the instrument side among the adapters: the last choice when both are still there
// (preferA/preferB), else the first two in a stable order.
TapGuess guessTap(const std::vector<Adapter>& adapters, const QString& preferA = {}, const QString& preferB = {});

QJsonObject connectionsJson();     // adapters, cameras, the tap guess, sentences (`LibreTapCLI ports`)

}  // namespace lt
