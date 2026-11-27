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
#include "picture.h"
#include "source.h"

#include <QJsonObject>
#include <QRectF>
#include <vector>

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

    // Picture settings (applied in the feed's thread; empty list: the camera has none). A user's changes are saved
    // for this camera and applied again whenever it is opened; resetControls() forgets them.
    std::vector<CameraControl> controls() const;
    void setControl(const QString& name, int value);
    void resetControls();
    // The instrument PC's screen in the picture: "auto" (found in the picture), "whole", or "manual" (a box drawn
    // by the user; fractions 0..1). Saved for this camera.
    void setRegion(const QString& mode, const QRectF& box = {});
    QString regionMode() const;
    QRectF region() const;           // in use now (empty: whole picture)
    Readability readability() const; // of the region, about once a second
    // Sets exposure and focus for the region (Camera has controls). Runs once by itself after the camera opens
    // unless the user saved an exposure, gain or focus setting for it.
    void optimise();
    bool optimising() const { return optimising_; }
    QString lastReport() const;
    bool saveStill(const QString& path, QString* error) const;   // the latest full-resolution frame, as JPEG
    QJsonObject describePicture() const;   // controls, region, readability, last Optimise report (CLI, agents)

signals:
    void preview(const QImage& image);       // about 10 a second, scaled to at most 960 px wide
    void failed(const QString& text);
    void opened(const QString& name);
    void controlsChanged();
    void readabilityChanged();
    void optimised(const QString& report);

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
    // picture (guarded by mutex_)
    QString key_;
    std::vector<CameraControl> controls_;
    std::vector<std::pair<QString, int>> pending_;
    bool resetPending_ = false, optimisePending_ = false;
    std::atomic<bool> optimising_{false};
    QString regionMode_ = QStringLiteral("auto"), report_;
    QRectF manualBox_, region_;
    Readability read_;
    QByteArray lastJpeg_;
};

// The "camera" control request, for the window and `LibreTapCLI record` (and the CLI's own feed):
// {"cmd":"camera"} → settings, region, readability; {"cmd":"camera","set":{"name":value,…}}; {"cmd":"camera",
// "reset":true}; {"cmd":"camera","optimise":true} (waits for it); {"cmd":"camera","region":"auto"|"whole"|[x,y,w,h]};
// {"cmd":"camera","still":"FILE.jpg"}.
QJsonObject cameraCommand(CameraFeed* feed, const QJsonObject& request);

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
