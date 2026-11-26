#pragma once
// Try LibreTap without hardware: a pretend instrument on a pretend serial tap, and a pretend camera that films the
// pretend vendor program (camera id "demo"). The program polls a reading twice a second and now and then changes
// a setting, as an operator would; the camera picture shows the same setting, so screen and traffic agree on the
// timeline. Everything recorded is marked as a demo in session.json.
#include <QImage>
#include <QTimer>

#include "serialtap.h"

namespace lt {

class DemoTapSource : public SerialTapSource {
    Q_OBJECT
public:
    explicit DemoTapSource(QObject* parent = nullptr);
    QJsonObject describe() const override;

protected:
    bool openLines(QString* error) override;
    void closeLines() override;
    QString label() const override { return QStringLiteral("Demo serial tap 9600 baud 8N1"); }

private:
    void step();
    QTimer timer_;
    int n_ = 0;
};

// The pretend vendor program's window as the camera would see it, at time t (wall clock).
QImage demoScreen(int width, int height, double t);

}  // namespace lt
