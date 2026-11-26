#pragma once
// Cameras, adapted from LibreDrop's src/core/camera.* (same author). LibreTap points a webcam at the instrument
// PC's screen and keeps a full-resolution still every half second; the live view shows the same frames.
//
// Frames are kept as the camera's own JPEG data: UVC webcams send Motion-JPEG at their larger sizes, so a still is
// stored without re-encoding; each frame carries the driver's capture time.
//
// Backends: V4L2 (Linux), ffmpeg (Windows: DirectShow, macOS: AVFoundation; any ffmpeg input as
// "ffmpeg:<format>:<input>", e.g. "ffmpeg:lavfi:testsrc2=size=640x480:rate=10" for tests), and "demo" (the pretend
// vendor program of demo.h, drawn; needs a QGuiApplication).
//
// Listing cameras never opens a device (Linux: names from /sys/class/video4linux), so merely starting LibreTap does
// not wake a camera. LIBRETAP_NO_SYSTEM_CAMERAS=1 hides and refuses all real cameras (tests, screenshots);
// LIBRETAP_FFMPEG_CAMERA=<format>:<input> adds an ffmpeg input to the list.
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace lt {

struct CameraInfo {
    QString id;                    // /dev/videoN, ffmpeg:<format>:<input>, demo
    QString name;
};

struct CameraMode {
    int w = 0, h = 0;
    double fps = 0;                // the fastest the camera offers at this size
};

struct CameraFrame {
    QByteArray jpeg;
    double t = 0;                  // seconds, capture time on the camera's clock (monotonic or stream time)
    int sequence = 0;              // the driver's frame counter (gaps: frames the camera dropped)
};

class Camera {
public:
    virtual ~Camera() = default;
    // Real cameras (not opened to list them), then the demo camera when withDemo.
    static std::vector<CameraInfo> list(bool withDemo = false);
    static std::unique_ptr<Camera> open(const QString& id, QString* error);
    static bool systemCamerasAllowed();

    virtual QString name() const = 0;
    virtual std::vector<CameraMode> modes() const = 0;   // largest first
    virtual bool start(const CameraMode& m, QString* error) = 0;
    // The next frame; waits for it (up to a few seconds). False on error or timeout (error says which).
    virtual bool grab(CameraFrame* out, QString* error) = 0;
    virtual void stop() = 0;
    virtual CameraMode mode() const = 0;   // as started (the size and rate the camera accepted)
};

QString ffmpegProgram();   // $LIBRETAP_FFMPEG, next to the program (Windows/macOS bundles), Homebrew, or PATH

}  // namespace lt
