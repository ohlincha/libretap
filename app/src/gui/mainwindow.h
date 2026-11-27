#pragma once
// LibreTap's one window. Two pages:
//   Record:   what is plugged in (in sentences), the camera's live view, Record/Stop, Mark (Space), Sync and the
//             traffic as it arrives.
//   Timeline: a recording afterwards: the screen picture on top, the traffic below, a scrub bar that moves both;
//             "Add Tick session…" combines a recording from the instrument PC and matches the clocks.
// The window answers the control socket (LibreTapCLI status/start/mark/sync/stop).
#include <QMainWindow>
#include <QPointer>

#include <memory>

#include "core/camerasource.h"
#include "core/devices.h"
#include "core/protocol.h"
#include "core/project.h"
#include "core/recorder.h"
#include "core/timeline.h"
#include "widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableView;
class QTimer;

namespace lt {
class ControlServer;
}

namespace lt::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void setDemo(bool on);                 // the pretend instrument and camera (no hardware)
    bool startRecording();
    void stopRecording();
    bool openRecording(const QString& path);
    bool addSession(const QString& folder);   // "Add Tick session…" without the dialog
    void showRecordPage();
    void showTimelinePage();
    void setTimelineTime(double t);
    Recorder& recorder() { return rec_; }
    bool loadProtocol(const QString& path);     // a protocol description: the Meaning column and coverage
    void openCompare(int a = 0, int b = 1);     // marks by index; the last entry is the end
    void showExchanges();
    void showHints();
    bool openDemo(const QString& which);        // "cora": the bundled Cora 5000 example with its description
    const Project& project() const { return project_; }
    QString controlProblem() const { return controlProblem_; }

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    QWidget* buildRecordPage();
    QWidget* buildTimelinePage();
    void refreshConnections();
    void updateRecordUi();
    void openCamera(const QString& id);
    void doMark();
    void advancedDialog();
    void clocksDialog();
    void findSpeed();
    void switchHelp();
    void addTickDialog();
    void copyTickDialog();
    void reloadTimeline(double keepTime = -1);
    void showPicture();
    TapConfig tapConfig() const;

    Recorder rec_;
    CameraFeed feed_;
    std::unique_ptr<ControlServer> control_;
    QString controlProblem_;
    bool demo_ = false;
    QString portA_, portB_;                // chosen sides (paths)
    std::vector<Adapter> adapters_;
    QString cameraId_;
    QJsonObject liveNames_;

    QStackedWidget* pages_ = nullptr;
    // record page
    PictureView* preview_ = nullptr;
    QLabel *tapDot_ = nullptr, *tapText_ = nullptr, *camDot_ = nullptr, *camText_ = nullptr, *speedText_ = nullptr;
    QPushButton *swap_ = nullptr, *speed_ = nullptr, *advanced_ = nullptr, *demoButton_ = nullptr;
    QCheckBox* useCamera_ = nullptr;
    QLabel *usbDot_ = nullptr, *usbText_ = nullptr, *ethDot_ = nullptr, *ethText_ = nullptr;
    QCheckBox *useUsb_ = nullptr, *useEth_ = nullptr;
    QComboBox* ifaceBox_ = nullptr;
    QPushButton* switchHelp_ = nullptr;
    QString picoPort_;               // the Pico found, if any
    QComboBox* cameraBox_ = nullptr;
    QPushButton *record_ = nullptr, *mark_ = nullptr, *sync_ = nullptr;
    QLineEdit* note_ = nullptr;
    QLabel* recStatus_ = nullptr;
    QLabel* attention_ = nullptr;      // amber: something needs the operator's attention
    QLabel* headerSub_ = nullptr;
    RowModel* live_ = nullptr;
    QTableView* liveView_ = nullptr;
    QTimer* tick_ = nullptr;
    // timeline page
    Project project_;
    Timeline tl_;
    ScreenBuilder screen_;
    QLabel *tlTitle_ = nullptr, *tlInfo_ = nullptr, *tlTime_ = nullptr;
    QComboBox* pictureBox_ = nullptr;
    PictureView* picture_ = nullptr;
    TimelineBar* bar_ = nullptr;
    RowModel* rows_ = nullptr;
    QTableView* rowView_ = nullptr;
    bool syncingSelection_ = false;
    Protocol proto_;
    bool protoLoaded_ = false;
    void applyProtocol();
};

}  // namespace lt::gui
