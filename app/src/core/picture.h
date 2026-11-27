#pragma once
// Is the camera picture good enough to read the instrument PC's screen? Image measures on the screen region of a
// grey picture, finding that region, and the closed loop that sets exposure and focus for it (Optimise picture).
// Thresholds are calibrated with synthetic pictures (tests/test_picture.cpp); calibration on real cameras is pending.
#include <QImage>
#include <QRectF>
#include <QString>

#include <functional>
#include <optional>

#include "camera.h"

namespace lt {

struct Readability {
    bool valid = false;
    double median = 0, low = 0, high = 0;   // grey levels: median, 2nd and 98th percentile (0..255)
    double clipped = 0;                     // fraction of pixels at 250 or above
    double contrast = 0;                    // high − low
    double sharpness = 0;                   // edge strength relative to contrast (scale-normalised)
    int score = 0;                          // 0..100
    QString verdict;                        // "Text readable", "Too blurred", "Too bright (text burnt out)", …
    bool ok() const { return valid && verdict == QLatin1String("Text readable"); }
};

// Measures `region` (fractions of the picture, 0..1; empty = whole picture) of `image` (any format).
Readability readability(const QImage& image, const QRectF& region = {});

// The instrument PC's screen in a camera picture: the largest bright, roughly rectangular area (fractions of the
// picture). Empty when none is found (then the whole picture is used).
QRectF detectScreen(const QImage& image);

// The closed loop behind "Optimise picture", on a camera with controls (V4L2, the simulated camera):
// manual exposure set so the region's brightest text background sits just below clipping (gain raised only when
// the exposure time is at its limit), then focus: autofocus left on where the camera has it, otherwise a sweep of
// manual focus for the sharpest picture. Bounded in steps and time; cancel-safe (stops between frames).
struct PictureIO {
    std::function<QImage()> grab;                                   // the next frame, decoded (any size); null on error
    std::function<std::optional<CameraControl>(const QString&)> control;
    std::function<bool(const QString&, int)> set;
    std::function<bool()> cancelled;
};
struct OptimiseResult {
    bool changed = false;
    QString report;                         // what was done, in a sentence or two
    Readability before, after;
};
OptimiseResult optimisePicture(const PictureIO& io, const QRectF& region, double maxSeconds = 10);

}  // namespace lt
