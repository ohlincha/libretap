#pragma once
// The camera at the instrument PC's screen: a feed that runs while LibreTap is open (live view) and, while a
// session is recorded, keeps a full-resolution still every half second (frames/cam_NNNNNN.jpg, a "frame" event
// each). A still that looks the same as the last one kept is skipped (a quiet screen costs no disk), but at least
// one is kept every 5 s. Frame times are this computer's wall clock when the frame arrived (a frame or two late:
// screens change slowly, traffic is matched to them by eye or by key frames).
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QThread>

#include <atomic>

#include "camera.h"
#include "source.h"

namespace lt {

class CameraSource;

class CameraFeed : public QObject {
    Q_OBJECT
public:
    explicit CameraFeed(QObject* parent = nullptr);
    ~CameraFeed() override;
    bool open(const QString& id);  // starts the feed thread; failures arrive as failed()
    void close();
    bool isOpen() const { return thread_ != nullptr; }
    QString id() const { return id_; }
    QString name() const;
    CameraMode mode() const;
    // While attached, stills go into the session (called by CameraSource).
    void attach(SessionWriter* w, double everySeconds, CameraSource* source);
    void detach();
    int stills() const { return stills_; }

signals:
    void preview(const QImage& image);       // about 10 a second, scaled to at most 960 px wide
    void failed(const QString& text);
    void opened(const QString& name);

private:
    void run();
    QString id_;
    QThread* thread_ = nullptr;
    std::atomic<bool> stop_{false};
    mutable QMutex mutex_;
    QString name_;
    CameraMode mode_;
    SessionWriter* writer_ = nullptr;
    CameraSource* source_ = nullptr;
    double every_ = 0.5;
    std::atomic<int> stills_{0};
};

class CameraSource : public Source {
    Q_OBJECT
public:
    explicit CameraSource(CameraFeed* feed, double everySeconds = 0.5, QObject* parent = nullptr);
    QString id() const override { return QStringLiteral("camera"); }
    QString kind() const override { return QStringLiteral("camera"); }
    QJsonObject describe() const override;
    bool start(SessionWriter* writer, QString* error) override;
    void stop() override;
    QJsonObject stats() const override;
    void stillWritten(const QJsonObject& e) { emit recorded(e); }   // from the feed's thread

private:
    CameraFeed* feed_;
    double every_;
};

}  // namespace lt
