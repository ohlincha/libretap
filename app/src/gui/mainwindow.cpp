#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QAction>
#include <QFrame>
#include <QMenu>
#include <QToolButton>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

#include "core/control.h"
#include "core/match.h"
#include "core/util.h"
#include "splash.h"
#include "theme.h"

namespace lt::gui {

namespace {
QSettings settings() { return QSettings(QStringLiteral("LibreTap"), QStringLiteral("LibreTap")); }

QLabel* dot() {
    auto* l = new QLabel;
    l->setFixedSize(12, 16);
    l->setObjectName(QStringLiteral("dot"));
    return l;
}

// Wrapped labels in a grid do not claim their height by themselves.
void fitHeight(QLabel* l) {
    const int w = l->width() > 80 ? l->width() : 440;
    l->setMinimumHeight(l->heightForWidth(w));
}

void setDot(QLabel* l, const char* state) {
    l->setProperty("state", QString::fromLatin1(state));
    l->style()->unpolish(l);
    l->style()->polish(l);
}

// A card: a rounded surface with a small accent title (instead of a sunken group box).
QFrame* card(const QString& title, QBoxLayout** body) {
    auto* f = new QFrame;
    f->setObjectName(QStringLiteral("card"));
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(16, 12, 16, 14);
    v->setSpacing(8);
    if (!title.isEmpty()) {
        auto* t = new QLabel(title);
        t->setObjectName(QStringLiteral("cardTitle"));
        v->addWidget(t);
    }
    *body = v;
    return f;
}

QTableView* table(RowModel* m) {
    auto* v = new QTableView;
    v->setModel(m);
    v->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->setSelectionMode(QAbstractItemView::SingleSelection);
    v->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->verticalHeader()->hide();
    v->verticalHeader()->setDefaultSectionSize(v->fontMetrics().height() + 12);
    v->setShowGrid(false);
    v->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    v->horizontalHeader()->setHighlightSections(false);
    v->setFrameShape(QFrame::NoFrame);
    v->setWordWrap(false);
    v->setAlternatingRowColors(false);
    v->horizontalHeader()->setStretchLastSection(true);
    v->setColumnWidth(0, 96);
    v->setColumnWidth(1, 190);
    v->setColumnWidth(2, 380);
    return v;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LibreTap"));
    setWindowIcon(appIcon());
    resize(1280, 820);
    auto s = settings();
    portA_ = s.value(QStringLiteral("portA")).toString();
    portB_ = s.value(QStringLiteral("portB")).toString();

    pages_ = new QStackedWidget;
    pages_->addWidget(buildRecordPage());
    pages_->addWidget(buildTimelinePage());
    auto* central = new QWidget;
    auto* cv = new QVBoxLayout(central);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    // a header bar (GNOME style): the app's name and what is open, Open… and the main menu
    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("header"));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 8, 10, 8);
    hl->setSpacing(10);
    auto* logo = new QLabel;
    logo->setPixmap(appIcon().pixmap(28, 28));
    auto* names = new QVBoxLayout;
    names->setSpacing(0);
    auto* appTitle = new QLabel(QStringLiteral("LibreTap"));
    appTitle->setObjectName(QStringLiteral("appTitle"));
    headerSub_ = new QLabel(QStringLiteral("New recording"));
    headerSub_->setObjectName(QStringLiteral("subtitle"));
    names->addWidget(appTitle);
    names->addWidget(headerSub_);
    auto* openBtn = new QPushButton(QStringLiteral("Open…"));
    openBtn->setToolTip(QStringLiteral("Open a recording (Ctrl+O)"));
    auto* newBtn = new QPushButton(QStringLiteral("New recording"));
    newBtn->setObjectName(QStringLiteral("flat"));
    auto* menuBtn = new QToolButton;
    menuBtn->setText(QStringLiteral("☰"));
    menuBtn->setObjectName(QStringLiteral("flat"));
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    menuBtn->setToolTip(QStringLiteral("Main menu"));
    hl->addWidget(logo);
    hl->addLayout(names);
    hl->addStretch(1);
    hl->addWidget(newBtn);
    hl->addWidget(openBtn);
    hl->addWidget(menuBtn);
    cv->addWidget(header);
    cv->addWidget(pages_, 1);
    setCentralWidget(central);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::showRecordPage);
    auto* openAct = new QAction(QStringLiteral("&Open recording…"), this);
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Open a recording (or a session folder)"), workspaceDir());
        if (!d.isEmpty()) openRecording(d);
    });
    connect(openBtn, &QPushButton::clicked, openAct, &QAction::trigger);
    addAction(openAct);
    auto* mainMenu = new QMenu(this);
    menuBtn->setMenu(mainMenu);
#ifdef Q_OS_MACOS
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
#else
    menuBar()->hide();
    QMenu* file = mainMenu;
    QMenu* help = mainMenu;
#endif
    file->addAction(QStringLiteral("&New recording"), QKeySequence::New, this, &MainWindow::showRecordPage);
    file->addAction(openAct);
    file->addAction(QStringLiteral("Add &Tick session…"), this, &MainWindow::addTickDialog);
    file->addSeparator();
    help->addAction(QStringLiteral("Try the &demo (no hardware)"), this, [this] {
        setDemo(true);
        showRecordPage();
    });
    help->addAction(QStringLiteral("&About LibreTap"), this, [this] {
        QMessageBox::about(this, QStringLiteral("About LibreTap"),
                           QStringLiteral("<h3>LibreTap %1</h3><p><i>Your instrument outlived its computer.</i></p>"
                                          "<p>LibreTap records what old vendor software says to its instrument, through passive "
                                          "listen-only taps, together with pictures of the old PC's screen. Afterwards the timeline "
                                          "shows both side by side, so the protocol can be understood and the instrument freed from "
                                          "its old computer.</p><p>Tick, a small companion program for the old PC, records its screen "
                                          "and the vendor program's port calls; add its folder with File ▸ Add Tick session.</p>"
                                          "<p>Agents and scripts: <tt>LibreTapCLI status | start | mark | sync | stop</tt> control this "
                                          "window.</p><p>MIT licence.</p>").arg(QStringLiteral(LIBRETAP_VERSION)));
    });
    file->addSeparator();
    auto* quit = file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    addAction(quit);

    // the control socket: agents drive this window
    rec_.startHandler = [this](const QJsonObject&) {
        if (!startRecording()) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), recStatus_->text()}};
        QJsonObject o = rec_.status();
        o.insert(QStringLiteral("ok"), true);
        return o;
    };
    control_ = std::make_unique<ControlServer>([this](const QJsonObject& r) { return rec_.handleCommand(r); });
    if (!control_->listen(&controlProblem_)) statusBar()->showMessage(controlProblem_);
    else statusBar()->showMessage(QStringLiteral("Agents and scripts can control this window: LibreTapCLI status, mark, sync, stop"));

    connect(&rec_, &Recorder::recorded, this, [this](const QJsonObject& e) {
        const QString k = e.value(QStringLiteral("kind")).toString();
        if (k == QLatin1String("frame") || k == QLatin1String("config")) return;
        live_->append(rowFromEvent(e, liveNames_));
        liveView_->scrollToBottom();
    });
    connect(&rec_, &Recorder::stopped, this, [this](const QString& folder) {
        updateRecordUi();
        openRecording(folder);
    });
    connect(&rec_, &Recorder::started, this, [this] { updateRecordUi(); });
    connect(&rec_, &Recorder::problem, this, [this](const QString& t) { statusBar()->showMessage(t, 10000); });
    connect(&feed_, &CameraFeed::preview, this, [this](const QImage& img) { preview_->setImage(img); });
    connect(&feed_, &CameraFeed::failed, this, [this](const QString& t) {
        preview_->setImage({});
        preview_->setPlaceholder(QStringLiteral("The camera stopped: %1").arg(t));
        camText_->setText(QStringLiteral("Camera problem: %1").arg(t));
        setDot(camDot_, "warn");
    });
    connect(&feed_, &CameraFeed::opened, this, [this](const QString& name) {
        const CameraMode m = feed_.mode();
        camText_->setText(QStringLiteral("<b>Camera ready.</b> Pictures of %1 × %2 pixels.").arg(m.w).arg(m.h));
        (void)name;
        camText_->setToolTip(QStringLiteral("Point the camera at the instrument PC's screen."));
        setDot(camDot_, "ok");
    });

    tick_ = new QTimer(this);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (rec_.isRecording()) updateRecordUi();
        else refreshConnections();
    });
    tick_->start(1000);
    refreshConnections();
    updateRecordUi();
}

MainWindow::~MainWindow() {
    rec_.stop();
    feed_.close();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (rec_.isRecording() &&
        QMessageBox::question(this, QStringLiteral("Stop recording?"), QStringLiteral("A recording is running. Stop it and quit?")) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    rec_.stop();
    feed_.close();
    e->accept();
}

QWidget* MainWindow::buildRecordPage() {
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 14, 16, 8);
    outer->setSpacing(14);
    auto* top = new QHBoxLayout;
    top->setSpacing(14);
    outer->addLayout(top, 1);

    preview_ = new PictureView;
    preview_->setPlaceholder(QStringLiteral("No camera.\nThe traffic is recorded all the same."));
    top->addWidget(preview_, 3);

    // the side column scrolls when everything is plugged in and the window is small
    auto* sideScroll = new QScrollArea;
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setWidgetResizable(true);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; } QWidget#qt_scrollarea_viewport { background: transparent; }"));
    auto* side = new QVBoxLayout;
    side->setSpacing(12);
    top->addLayout(side, 2);
    QBoxLayout* connBody = nullptr;
    auto* conn = card(QStringLiteral("Connections"), &connBody);
    conn->setMinimumWidth(500);   // the sentences fit in two lines
    auto* cl = new QGridLayout;
    cl->setHorizontalSpacing(10);
    cl->setVerticalSpacing(8);
    connBody->addLayout(cl);
    cl->setColumnStretch(1, 1);
    tapDot_ = dot();
    tapText_ = new QLabel;
    tapText_->setWordWrap(true);
    cl->addWidget(tapDot_, 0, 0, Qt::AlignTop);
    cl->addWidget(tapText_, 0, 1);
    tapText_->setMinimumHeight(tapText_->fontMetrics().lineSpacing() * 2 + 6);   // wrapped labels in a grid lose height
    auto* tapButtons = new QHBoxLayout;
    tapButtons->setSpacing(6);
    swap_ = new QPushButton(QStringLiteral("Swap sides"));
    swap_->setToolTip(QStringLiteral("The PC side is the adapter on the tap lead that carries what the PC sends (DB-9 pin 3).\n"
                                     "Not sure? Record a few seconds: the PC usually speaks first."));
    connect(swap_, &QPushButton::clicked, this, [this] {
        std::swap(portA_, portB_);
        auto s = settings();
        s.setValue(QStringLiteral("portA"), portA_);
        s.setValue(QStringLiteral("portB"), portB_);
        refreshConnections();
    });
    speed_ = new QPushButton(QStringLiteral("Find speed"));
    speed_->setToolTip(QStringLiteral("Listens at each common speed while the vendor software talks to the instrument"));
    connect(speed_, &QPushButton::clicked, this, &MainWindow::findSpeed);
    advanced_ = new QPushButton(QStringLiteral("Advanced…"));
    connect(advanced_, &QPushButton::clicked, this, &MainWindow::advancedDialog);
    tapButtons->addWidget(swap_);
    tapButtons->addWidget(speed_);
    tapButtons->addWidget(advanced_);
    tapButtons->addStretch();
    cl->addLayout(tapButtons, 1, 1);
    speedText_ = new QLabel;
    speedText_->setObjectName(QStringLiteral("dimText"));
    tapButtons->addWidget(speedText_);
    camDot_ = dot();
    camText_ = new QLabel;
    camText_->setWordWrap(true);
    camText_->setMinimumHeight(camText_->fontMetrics().lineSpacing() + 2);
    cl->addWidget(camDot_, 3, 0, Qt::AlignTop);
    cl->addWidget(camText_, 3, 1);
    auto* camRow = new QHBoxLayout;
    useCamera_ = new QCheckBox(QStringLiteral("Use the camera"));
    useCamera_->setChecked(settings().value(QStringLiteral("useCamera"), true).toBool());
    cameraBox_ = new QComboBox;
    cameraBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    camRow->addWidget(useCamera_);
    camRow->addWidget(cameraBox_, 1);
    cl->addLayout(camRow, 4, 1);
    connect(useCamera_, &QCheckBox::toggled, this, [this](bool on) {
        settings().setValue(QStringLiteral("useCamera"), on);
        openCamera(on ? cameraBox_->currentData().toString() : QString());
    });
    connect(cameraBox_, &QComboBox::activated, this, [this] {
        if (useCamera_->isChecked()) openCamera(cameraBox_->currentData().toString());
    });
    demoButton_ = new QPushButton(QStringLiteral("No hardware here? Try the demo"));
    demoButton_->setFlat(true);
    demoButton_->setObjectName(QStringLiteral("link"));
    connect(demoButton_, &QPushButton::clicked, this, [this] { setDemo(!demo_); });
    // the USB sniffer (shown when a Pico is plugged in)
    usbDot_ = dot();
    usbText_ = new QLabel;
    usbText_->setWordWrap(true);
    usbText_->setMinimumHeight(usbText_->fontMetrics().lineSpacing() * 2 + 6);
    useUsb_ = new QCheckBox(QStringLiteral("Use the USB sniffer"));
    useUsb_->setChecked(settings().value(QStringLiteral("useUsb"), true).toBool());
    connect(useUsb_, &QCheckBox::toggled, this, [this](bool on) {
        settings().setValue(QStringLiteral("useUsb"), on);
        updateRecordUi();
    });
    cl->addWidget(usbDot_, 5, 0, Qt::AlignTop);
    cl->addWidget(usbText_, 5, 1);
    cl->addWidget(useUsb_, 6, 1);
    // the network: off unless asked for
    ethDot_ = dot();
    ethText_ = new QLabel;
    ethText_->setWordWrap(true);
    auto* ethRow = new QHBoxLayout;
    ethRow->setSpacing(8);
    useEth_ = new QCheckBox(QStringLiteral("Record the network on"));
    useEth_->setChecked(settings().value(QStringLiteral("useEthernet"), false).toBool());
    ifaceBox_ = new QComboBox;
    ifaceBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ifaceBox_->setMinimumContentsLength(14);
    switchHelp_ = new QPushButton(QStringLiteral("Switch setup?"));
    switchHelp_->setObjectName(QStringLiteral("link"));
    switchHelp_->setToolTip(QStringLiteral("How to make the switch copy the instrument's traffic to this computer"));
    connect(switchHelp_, &QPushButton::clicked, this, &MainWindow::switchHelp);
    connect(useEth_, &QCheckBox::toggled, this, [this](bool on) {
        settings().setValue(QStringLiteral("useEthernet"), on);
        refreshConnections();
    });
    connect(ifaceBox_, &QComboBox::activated, this, [this] { settings().setValue(QStringLiteral("ethIface"), ifaceBox_->currentData().toString()); });
    ethRow->addWidget(useEth_);
    ethRow->addWidget(ifaceBox_, 1);
    ethRow->addWidget(switchHelp_);
    cl->addWidget(ethDot_, 7, 0, Qt::AlignTop);
    cl->addLayout(ethRow, 7, 1);
    cl->addWidget(ethText_, 8, 1);
    cl->addWidget(demoButton_, 9, 1, Qt::AlignLeft);
    sideScroll->setWidget(conn);
    sideScroll->setMinimumWidth(520);
    side->addWidget(sideScroll, 1);   // only the connections scroll; Record and Mark stay in view

    record_ = new QPushButton(QStringLiteral("●  Record"));
    record_->setObjectName(QStringLiteral("record"));
    record_->setMinimumHeight(58);
    connect(record_, &QPushButton::clicked, this, [this] {
        if (rec_.isRecording()) stopRecording();
        else startRecording();
    });
    side->addWidget(record_);
    recStatus_ = new QLabel;
    recStatus_->setWordWrap(true);
    recStatus_->setObjectName(QStringLiteral("dimText"));
    side->addWidget(recStatus_);
    attention_ = new QLabel;
    attention_->setObjectName(QStringLiteral("hint"));
    attention_->setProperty("state", QStringLiteral("warn"));
    attention_->setWordWrap(true);
    attention_->hide();
    side->addWidget(attention_);

    QBoxLayout* ml = nullptr;
    auto* markBox = card(QStringLiteral("While recording"), &ml);
    auto* hint = new QLabel(QStringLiteral("Change one setting at a time. Just before each change, press <b>Mark</b> (space bar)."));
    hint->setObjectName(QStringLiteral("dimText"));
    hint->setWordWrap(true);
    ml->addWidget(hint);
    note_ = new QLineEdit;
    note_->setPlaceholderText(QStringLiteral("What you are about to change (optional), Enter marks"));
    connect(note_, &QLineEdit::returnPressed, this, &MainWindow::doMark);
    ml->addWidget(note_);
    auto* mb = new QHBoxLayout;
    mark_ = new QPushButton(QStringLiteral("Mark  (Space)"));
    mark_->setObjectName(QStringLiteral("mark"));
    mark_->setMinimumHeight(40);
    connect(mark_, &QPushButton::clicked, this, &MainWindow::doMark);
    sync_ = new QPushButton(QStringLiteral("Sync"));
    sync_->setObjectName(QStringLiteral("sync"));
    sync_->setMinimumHeight(40);
    sync_->setToolTip(QStringLiteral("Press at the same moment as Sync in Tick on the instrument PC: it lines up the two clocks"));
    connect(sync_, &QPushButton::clicked, this, [this] { rec_.sync(); });
    mb->addWidget(mark_, 2);
    mb->addWidget(sync_, 1);
    ml->addLayout(mb);
    side->addWidget(markBox);
    auto* sc = new QShortcut(QKeySequence(Qt::Key_Space), page);
    connect(sc, &QShortcut::activated, this, &MainWindow::doMark);

    QBoxLayout* lb = nullptr;
    auto* liveCard = card(QStringLiteral("Traffic as it arrives"), &lb);
    lb->setContentsMargins(4, 12, 4, 4);
    live_ = new RowModel(this);
    liveView_ = table(live_);
    lb->addWidget(liveView_);
    liveCard->setMinimumHeight(170);
    outer->addWidget(liveCard, 0);
    return page;
}

QWidget* MainWindow::buildTimelinePage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(16, 12, 16, 8);
    v->setSpacing(10);
    auto* head = new QHBoxLayout;

    tlTitle_ = new QLabel;
    tlTitle_->setObjectName(QStringLiteral("title"));
    auto* addTick = new QPushButton(QStringLiteral("Add Tick session…"));
    addTick->setObjectName(QStringLiteral("addTick"));
    addTick->setToolTip(QStringLiteral("Combine a recording that Tick made on the instrument PC at the same time"));
    connect(addTick, &QPushButton::clicked, this, &MainWindow::addTickDialog);
    auto* clocks = new QPushButton(QStringLiteral("Clocks…"));
    connect(clocks, &QPushButton::clicked, this, &MainWindow::clocksDialog);
    auto* folder = new QPushButton(QStringLiteral("Show folder"));
    connect(folder, &QPushButton::clicked, this, [this] {
        const QString f = project_.isSaved() ? project_.folder : (project_.sessions.empty() ? QString() : project_.sessions[0].session.folder);
        if (!f.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(f));
    });
    head->addWidget(tlTitle_, 1);
    head->addWidget(addTick);
    head->addWidget(clocks);
    head->addWidget(folder);
    v->addLayout(head);
    tlInfo_ = new QLabel;
    tlInfo_->setWordWrap(true);
    v->addWidget(tlInfo_);

    auto* split = new QSplitter(Qt::Vertical);
    auto* picBox = new QWidget;
    auto* pl = new QVBoxLayout(picBox);
    pl->setContentsMargins(0, 0, 0, 0);
    pictureBox_ = new QComboBox;
    connect(pictureBox_, &QComboBox::currentIndexChanged, this, [this] {
        screen_.reset();
        showPicture();
    });
    picture_ = new PictureView;
    pl->addWidget(pictureBox_, 0, Qt::AlignLeft);
    pl->addWidget(picture_, 1);
    split->addWidget(picBox);

    auto* lower = new QWidget;
    auto* ll = new QVBoxLayout(lower);
    ll->setContentsMargins(0, 0, 0, 0);
    auto* barRow = new QHBoxLayout;
    auto* prev = new QPushButton(QStringLiteral("◀"));
    prev->setToolTip(QStringLiteral("Previous message (Left / Up)"));
    auto* next = new QPushButton(QStringLiteral("▶"));
    next->setToolTip(QStringLiteral("Next message (Right / Down)"));
    prev->setFixedWidth(36);
    next->setFixedWidth(36);
    bar_ = new TimelineBar;
    tlTime_ = new QLabel;
    tlTime_->setMinimumWidth(150);
    barRow->addWidget(prev);
    barRow->addWidget(bar_, 1);
    barRow->addWidget(next);
    barRow->addWidget(tlTime_);
    ll->addLayout(barRow);
    rows_ = new RowModel(this);
    rowView_ = table(rows_);
    QBoxLayout* rb = nullptr;
    auto* rowsCard = card(QString(), &rb);
    rb->setContentsMargins(4, 4, 4, 4);
    rb->addWidget(rowView_);
    ll->addWidget(rowsCard, 1);
    split->addWidget(lower);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    connect(bar_, &TimelineBar::timeChanged, this, &MainWindow::setTimelineTime);
    auto step = [this](int d) {
        if (tl_.rows.empty()) return;
        int r = rowView_->currentIndex().isValid() ? rowView_->currentIndex().row() : tl_.rowAt(bar_->time());
        r = std::clamp(r + d, 0, int(tl_.rows.size()) - 1);
        setTimelineTime(tl_.rows[size_t(r)].t);
    };
    connect(prev, &QPushButton::clicked, this, [step] { step(-1); });
    connect(next, &QPushButton::clicked, this, [step] { step(+1); });
    connect(rowView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& i) {
        if (syncingSelection_ || !i.isValid()) return;
        syncingSelection_ = true;
        bar_->setTime(tl_.rows[size_t(i.row())].t);
        showPicture();
        syncingSelection_ = false;
    });
    return page;
}

void MainWindow::setDemo(bool on) {
    demo_ = on;
    demoButton_->setText(on ? QStringLiteral("Leave the demo (use the real connections)") : QStringLiteral("No hardware here? Try the demo"));
    refreshConnections();
    if (on) {
        useCamera_->setChecked(true);
        openCamera(QStringLiteral("demo"));
    } else {
        openCamera(useCamera_->isChecked() ? cameraBox_->currentData().toString() : QString());
    }
}

TapConfig MainWindow::tapConfig() const {
    auto s = settings();
    TapConfig c;
    c.portA = portA_;
    c.portB = portB_;
    c.nameA = s.value(QStringLiteral("nameA"), QStringLiteral("PC")).toString();
    c.nameB = s.value(QStringLiteral("nameB"), QStringLiteral("Instrument")).toString();
    c.line.baud = s.value(QStringLiteral("baud"), 9600).toInt();
    LineFormat::parse(s.value(QStringLiteral("format"), QStringLiteral("8N1")).toString(), &c.line);
    c.gapMs = s.value(QStringLiteral("gapMs"), 20.0).toDouble();
    return c;
}

void MainWindow::refreshConnections() {
    if (rec_.isRecording()) return;
    adapters_ = listAdapters();
    const TapConfig c = tapConfig();
    if (demo_) {
        tapText_->setText(QStringLiteral("<b>Demo:</b> a pretend instrument on a pretend serial tap. Press Record to try LibreTap."));
        setDot(tapDot_, "ok");
        speedText_->setText(QString());
    } else {
        const TapGuess g = guessTap(adapters_, portA_, portB_);
        if (g.ready) portA_ = g.a, portB_ = g.b;
        tapText_->setText(g.ready ? QStringLiteral("<b>Serial tap ready.</b> ") + g.sentence.mid(18) : g.sentence);
        setDot(tapDot_, g.ready ? "ok" : "warn");
        speedText_->setText(g.ready ? QStringLiteral("%1 baud, %2").arg(c.line.baud).arg(c.line.text()) : QString());
        speedText_->setToolTip(QStringLiteral("Not sure of the speed? Press Find speed while the vendor software talks to the instrument."));
    }
    // USB sniffer
    const auto picos = listPicos();
    picoPort_ = picos.empty() ? QString() : picos.front().path;
    const bool usbShown = !picos.empty() && !demo_;
    usbDot_->setVisible(usbShown);
    usbText_->setVisible(usbShown);
    useUsb_->setVisible(usbShown);
    if (usbShown) {
        usbText_->setText(QStringLiteral("<b>USB sniffer ready</b> (Pico on %1). It captures in short pieces: press Record, do one action, press Stop.")
                              .arg(picos.front().port));
        setDot(usbDot_, "ok");
    }
    // network
    {
        const auto ifs = listInterfaces();
        QStringList ids{QString()};
        for (const NetInterface& n : ifs) ids << n.name;
        QStringList have;
        for (int i = 0; i < ifaceBox_->count(); ++i) have << ifaceBox_->itemData(i).toString();
        if (ids != have) {
            const QString cur = ifaceBox_->count() ? ifaceBox_->currentData().toString() : settings().value(QStringLiteral("ethIface")).toString();
            ifaceBox_->clear();
            ifaceBox_->addItem(QStringLiteral("automatic"), QString());
            ifaceBox_->setItemData(0, QStringLiteral("The wired port that sees the most traffic when recording starts"), Qt::ToolTipRole);
            for (const NetInterface& n : ifs) ifaceBox_->addItem(n.label, n.name);
            const int k = ifaceBox_->findData(cur);
            ifaceBox_->setCurrentIndex(k >= 0 ? k : 0);
        }
        const bool on = useEth_->isChecked() && !demo_;
        ifaceBox_->setEnabled(on);
        ethText_->setVisible(on);
        useEth_->setVisible(!demo_);
        ifaceBox_->setVisible(!demo_);
        switchHelp_->setVisible(!demo_);
        ethDot_->setVisible(!demo_);
        if (on) {
            const CaptureAccess acc = captureAccess();
            ethText_->setText(acc.ok ? QStringLiteral("The switch must copy (mirror) the instrument's and the PC's ports to this computer's port.")
                                     : acc.sentence);
            setDot(ethDot_, acc.ok ? "ok" : "warn");
        } else {
            setDot(ethDot_, "off");
        }
    }
    swap_->setVisible(!demo_);
    speed_->setVisible(!demo_);
    swap_->setEnabled(!portA_.isEmpty());
    speed_->setEnabled(!portA_.isEmpty());
    // cameras: the list changes when one is plugged in
    const auto cams = Camera::list(demo_);
    QStringList ids;
    for (const CameraInfo& ci : cams) ids << ci.id;
    QStringList have;
    for (int i = 0; i < cameraBox_->count(); ++i) have << cameraBox_->itemData(i).toString();
    if (ids != have) {
        const QString cur = cameraBox_->currentData().toString();
        cameraBox_->clear();
        for (const CameraInfo& ci : cams) cameraBox_->addItem(ci.name, ci.id);
        const int k = cameraBox_->findData(cur);
        if (k >= 0) cameraBox_->setCurrentIndex(k);
        if (!feed_.isOpen() && useCamera_->isChecked() && !cams.empty()) openCamera(cameraBox_->currentData().toString());
    }
    cameraBox_->setVisible(!cams.empty());
    useCamera_->setVisible(!cams.empty());
    if (cams.empty()) {
        camText_->setText(QStringLiteral("No camera found. Fine when Tick records the PC's screen; otherwise plug in a webcam aimed at it."));
        setDot(camDot_, "off");
        preview_->setPlaceholder(QStringLiteral("No camera.\nThe traffic is recorded all the same."));
    } else if (!useCamera_->isChecked()) {
        camText_->setText(QStringLiteral("Camera off: no pictures of the screen will be kept."));
        setDot(camDot_, "off");
    }
    for (QLabel* l : {tapText_, camText_, usbText_, ethText_}) fitHeight(l);
    updateRecordUi();
}

void MainWindow::openCamera(const QString& id) {
    if (id == cameraId_ && feed_.isOpen()) return;
    feed_.close();
    preview_->setImage({});
    cameraId_ = id;
    if (id.isEmpty()) {
        preview_->setPlaceholder(QStringLiteral("Camera off.\nThe traffic is recorded all the same."));
        return;
    }
    preview_->setPlaceholder(QStringLiteral("Starting the camera…"));
    camText_->setText(QStringLiteral("Starting the camera…"));
    feed_.open(id);
}

void MainWindow::updateRecordUi() {
    const bool on = rec_.isRecording();
    record_->setText(on ? QStringLiteral("■  Stop") : QStringLiteral("●  Record"));
    record_->setProperty("recording", on);
    record_->style()->unpolish(record_);
    record_->style()->polish(record_);
    const bool ready = demo_ || !guessTap(adapters_, portA_, portB_).a.isEmpty() || (feed_.isOpen() && useCamera_->isChecked()) ||
                       (!picoPort_.isEmpty() && useUsb_->isChecked()) || useEth_->isChecked();
    record_->setEnabled(on || ready);
    mark_->setEnabled(on);
    sync_->setEnabled(on);
    note_->setEnabled(on);
    swap_->setEnabled(!on && !portA_.isEmpty());
    speed_->setEnabled(!on && !portA_.isEmpty());
    advanced_->setEnabled(!on);
    demoButton_->setEnabled(!on);
    useCamera_->setEnabled(!on);
    useUsb_->setEnabled(!on);
    useEth_->setEnabled(!on);
    ifaceBox_->setEnabled(!on && useEth_->isChecked());
    cameraBox_->setEnabled(!on);
    attention_->setVisible(false);
    if (on) {
        const QJsonObject st = rec_.status();
        const QJsonObject s0 = st.value(QStringLiteral("stats")).toObject();
        if (s0.contains(QStringLiteral("usb_events"))) {
            attention_->setText(QStringLiteral("USB: do one action, then press Stop. The sniffer's memory holds well under a minute."));
            attention_->setVisible(true);
        }
        if (!demo_ && rec_.elapsed() > 8 && s0.contains(QStringLiteral("bytes_a"))) {
            const bool noA = s0.value(QStringLiteral("bytes_a")).toInt() == 0, noB = s0.value(QStringLiteral("bytes_b")).toInt() == 0;
            if (noA || noB) {
                attention_->setText(noA && noB ? QStringLiteral("Nothing on the link yet. Is the vendor software talking to the instrument, and the tap plugged in between?")
                                               : QStringLiteral("No data from the %1 side yet. Check that lead of the tap, or the speed (Find speed).")
                                                     .arg(noA ? QStringLiteral("PC") : QStringLiteral("instrument")));
                attention_->setVisible(true);
            }
        }
        const QJsonObject s = st.value(QStringLiteral("stats")).toObject();
        recStatus_->setText(QStringLiteral("<b>Recording %1</b><br>%2 bytes from the PC, %3 from the instrument · %4 marks · %5 pictures")
                                .arg(clockText(rec_.elapsed(), false))
                                .arg(s.value(QStringLiteral("bytes_a")).toInt() + s.value(QStringLiteral("eth_bytes_a")).toInt())
                                .arg(s.value(QStringLiteral("bytes_b")).toInt() + s.value(QStringLiteral("eth_bytes_b")).toInt())
                                .arg(st.value(QStringLiteral("marks")).toInt())
                                .arg(s.value(QStringLiteral("stills")).toInt()));
    } else if (!ready) {
        recStatus_->setText(QStringLiteral("Connect the serial tap (or a camera) to record."));
    } else {
        QString where = QDir::toNativeSeparators(workspaceDir());
        if (where.startsWith(QDir::homePath())) where = QStringLiteral("~") + where.mid(QDir::homePath().size());
        recStatus_->setText(recStatus_->fontMetrics().elidedText(QStringLiteral("Saved in %1").arg(where), Qt::ElideMiddle, 460));
        recStatus_->setToolTip(QDir::toNativeSeparators(workspaceDir()));
    }
}

bool MainWindow::startRecording() {
    if (rec_.isRecording()) return true;
    RecordOptions o;
    o.demo = demo_;
    o.tapConfig = tapConfig();
    o.tap = !demo_ && !o.tapConfig.portA.isEmpty() && !o.tapConfig.portB.isEmpty() && !guessTap(adapters_, portA_, portB_).a.isEmpty();
    o.usb = !demo_ && !picoPort_.isEmpty() && useUsb_->isChecked();
    o.pico.port = picoPort_;
    o.pico.lowSpeed = settings().value(QStringLiteral("usbLowSpeed"), false).toBool();
    o.ethernet = !demo_ && useEth_->isChecked();
    o.eth.iface = ifaceBox_->currentData().toString();
    o.eth.instrumentIp = settings().value(QStringLiteral("instrumentIp")).toString();
    o.stillEvery = settings().value(QStringLiteral("stillEvery"), 0.5).toDouble();
    rec_.setCamera(useCamera_->isChecked() && feed_.isOpen() ? &feed_ : nullptr);
    liveNames_ = QJsonObject{{QStringLiteral("a"), o.tapConfig.nameA}, {QStringLiteral("b"), o.tapConfig.nameB}};
    live_->clear();
    QString err;
    if (!rec_.start(o, &err)) {
        recStatus_->setText(QStringLiteral("<b>Could not start:</b> %1").arg(err.toHtmlEscaped()));
        return false;
    }
    live_->setT0(wallNow());
    note_->setFocus();
    note_->clearFocus();
    updateRecordUi();
    return true;
}

void MainWindow::stopRecording() { rec_.stop(); }

void MainWindow::doMark() {
    if (!rec_.isRecording()) return;
    rec_.mark(note_->text().trimmed());
    note_->clear();
    note_->clearFocus();
}

void MainWindow::findSpeed() {
    if (portA_.isEmpty()) return;
    QProgressDialog pd(QStringLiteral("Listening… keep the vendor software talking to the instrument."), QStringLiteral("Cancel"), 0,
                       int(commonBauds().size()), this);
    pd.setWindowTitle(QStringLiteral("Find speed"));
    pd.setMinimumDuration(0);
    int k = 0;
    const BaudGuess g = guessBaud({portA_, portB_}, 1.5, [&](int b) {
        pd.setValue(k++);
        pd.setLabelText(QStringLiteral("Listening at %1 baud… keep the vendor software talking to the instrument.").arg(b));
    });
    pd.close();
    if (g.baud) settings().setValue(QStringLiteral("baud"), g.baud);
    refreshConnections();
    QMessageBox::information(this, QStringLiteral("Find speed"), g.sentence);
}

void MainWindow::switchHelp() {
    QMessageBox::information(this, QStringLiteral("Setting up the switch"),
        QStringLiteral("<p>A normal switch only sends this computer its own traffic. A switch with <b>port mirroring</b> "
                       "(e.g. TP-Link TL-SG105E) can copy the instrument's and the PC's traffic to it.</p>"
                       "<ol><li>Connect this computer alone to the switch. Its address is 192.168.0.1: give this computer "
                       "192.168.0.10 / 255.255.255.0 on that port and open <tt>http://192.168.0.1</tt> (user admin, password admin; "
                       "set a new one).</li>"
                       "<li>Monitoring ▸ Port Mirror: enable; <b>mirroring port</b> = this computer's port (say 5); "
                       "<b>mirrored ports</b> = the instrument's and the PC's ports (say 1 and 2), each with Ingress and Egress. "
                       "Apply, then System ▸ Save Config.</li>"
                       "<li>Plug the instrument into port 1 and its PC into port 2. They keep their own addresses; the switch is "
                       "invisible to them. This computer only listens.</li></ol>"
                       "<p>Menu names are from TP-Link's documentation; check them on your switch.</p>"));
}

void MainWindow::advancedDialog() {
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("Advanced settings"));
    auto* f = new QFormLayout(&d);
    auto s = settings();
    auto portBox = [&](const QString& cur) {
        auto* b = new QComboBox;
        b->setEditable(true);
        for (const Adapter& a : listAdapters(true)) b->addItem(a.label(), a.path);
        const int k = b->findData(cur);
        if (k >= 0) b->setCurrentIndex(k);
        else b->setEditText(cur);
        return b;
    };
    auto* pa = portBox(portA_);
    auto* pb = portBox(portB_);
    f->addRow(QStringLiteral("PC side (PC's TX, pin 3):"), pa);
    f->addRow(QStringLiteral("Instrument side (pin 2):"), pb);
    auto* baud = new QComboBox;
    baud->setEditable(true);
    for (int b : commonBauds()) baud->addItem(QString::number(b));
    baud->setCurrentText(QString::number(s.value(QStringLiteral("baud"), 9600).toInt()));
    f->addRow(QStringLiteral("Speed (baud):"), baud);
    auto* fmt = new QComboBox;
    fmt->addItems({QStringLiteral("8N1"), QStringLiteral("7E1"), QStringLiteral("7O1"), QStringLiteral("8E1"), QStringLiteral("8O1"),
                   QStringLiteral("8N2"), QStringLiteral("7E2"), QStringLiteral("7N2")});
    fmt->setEditable(true);
    fmt->setCurrentText(s.value(QStringLiteral("format"), QStringLiteral("8N1")).toString());
    f->addRow(QStringLiteral("Frame (bits, parity, stop):"), fmt);
    auto* gap = new QDoubleSpinBox;
    gap->setRange(1, 2000);
    gap->setSuffix(QStringLiteral(" ms"));
    gap->setValue(s.value(QStringLiteral("gapMs"), 20.0).toDouble());
    gap->setToolTip(QStringLiteral("Silence that ends a message in the list"));
    f->addRow(QStringLiteral("Message gap:"), gap);
    auto* na = new QLineEdit(s.value(QStringLiteral("nameA"), QStringLiteral("PC")).toString());
    auto* nb = new QLineEdit(s.value(QStringLiteral("nameB"), QStringLiteral("Instrument")).toString());
    f->addRow(QStringLiteral("Name of the PC side:"), na);
    f->addRow(QStringLiteral("Name of the instrument side:"), nb);
    auto* every = new QDoubleSpinBox;
    every->setRange(0.1, 60);
    every->setSuffix(QStringLiteral(" s"));
    every->setValue(s.value(QStringLiteral("stillEvery"), 0.5).toDouble());
    f->addRow(QStringLiteral("Camera picture every:"), every);
    auto* ip = new QLineEdit(s.value(QStringLiteral("instrumentIp")).toString());
    ip->setPlaceholderText(QStringLiteral("all traffic"));
    ip->setToolTip(QStringLiteral("Only record network traffic to and from this address"));
    f->addRow(QStringLiteral("Network: instrument's address:"), ip);
    auto* usbLow = new QCheckBox(QStringLiteral("Low-speed USB device (1.5 Mbit/s: keyboards, some old devices)"));
    usbLow->setChecked(s.value(QStringLiteral("usbLowSpeed"), false).toBool());
    f->addRow(QStringLiteral("USB sniffer:"), usbLow);
    auto* ws = new QLineEdit(workspaceDir());
    auto* wsRow = new QHBoxLayout;
    auto* browse = new QPushButton(QStringLiteral("…"));
    connect(browse, &QPushButton::clicked, &d, [&] {
        const QString dir = QFileDialog::getExistingDirectory(&d, QStringLiteral("Save recordings in"), ws->text());
        if (!dir.isEmpty()) ws->setText(dir);
    });
    wsRow->addWidget(ws, 1);
    wsRow->addWidget(browse);
    f->addRow(QStringLiteral("Save recordings in:"), wsRow);
    QString hint;
    for (const QString& p : {portA_, portB_})
        if (const QString h = latencyHint(p); !h.isEmpty()) hint += h + QLatin1Char('\n');
    if (!hint.isEmpty()) {
        auto* l = new QLabel(hint.trimmed());
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        f->addRow(l);
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    f->addRow(bb);
    if (d.exec() != QDialog::Accepted) return;
    auto pathOf = [](QComboBox* b) {
        const int k = b->findText(b->currentText());
        return k >= 0 ? b->itemData(k).toString() : b->currentText().trimmed();
    };
    portA_ = pathOf(pa);
    portB_ = pathOf(pb);
    s.setValue(QStringLiteral("portA"), portA_);
    s.setValue(QStringLiteral("portB"), portB_);
    s.setValue(QStringLiteral("baud"), baud->currentText().toInt());
    LineFormat lf;
    if (LineFormat::parse(fmt->currentText(), &lf)) s.setValue(QStringLiteral("format"), lf.text());
    s.setValue(QStringLiteral("gapMs"), gap->value());
    s.setValue(QStringLiteral("nameA"), na->text().trimmed().isEmpty() ? QStringLiteral("PC") : na->text().trimmed());
    s.setValue(QStringLiteral("nameB"), nb->text().trimmed().isEmpty() ? QStringLiteral("Instrument") : nb->text().trimmed());
    s.setValue(QStringLiteral("stillEvery"), every->value());
    s.setValue(QStringLiteral("instrumentIp"), ip->text().trimmed());
    s.setValue(QStringLiteral("usbLowSpeed"), usbLow->isChecked());
    if (ws->text() != workspaceDir()) s.setValue(QStringLiteral("workspace"), ws->text());
    refreshConnections();
}

// ---- timeline ----

void MainWindow::showRecordPage() {
    pages_->setCurrentIndex(0);
    headerSub_->setText(QStringLiteral("New recording"));
    refreshConnections();
}

void MainWindow::showTimelinePage() { pages_->setCurrentIndex(1); }

bool MainWindow::openRecording(const QString& path) {
    Project p;
    QString err;
    if (!Project::load(path, &p, &err)) {
        QMessageBox::warning(this, QStringLiteral("Open recording"), err);
        return false;
    }
    project_ = std::move(p);
    reloadTimeline();
    showTimelinePage();
    return true;
}

void MainWindow::reloadTimeline(double keep) {
    screen_.reset();
    tl_.build(project_);
    tlTitle_->setText(QStringLiteral("%1 · %2").arg(project_.name.toHtmlEscaped(), clockText(tl_.end - tl_.start, false)));
    headerSub_->setText(QStringLiteral("Recording %1").arg(project_.name));
    QStringList info;
    for (const ProjectSession& s : project_.sessions) {
        QString clock;
        if (s.how == QLatin1String("reference")) clock = QStringLiteral("reference clock");
        else if (s.how == QLatin1String("none")) clock = QStringLiteral("<span style='color:#c0392b'>clock not matched</span> (Clocks…)");
        else clock = QStringLiteral("clock matched by %1 (%2 s)").arg(s.how).arg(s.offset, 0, 'f', 3);
        info << QStringLiteral("%1: %2 messages, %3 marks, %4 pictures, %5")
                    .arg(s.session.recorder().toHtmlEscaped())
                    .arg(s.session.count(QStringLiteral("data")))
                    .arg(s.session.count(QStringLiteral("mark")))
                    .arg(s.session.count(QStringLiteral("frame")))
                    .arg(clock);
    }
    tlInfo_->setText(info.join(QStringLiteral("<br>")));
    std::vector<Row> rows;
    std::vector<std::pair<double, int>> traffic;
    rows.reserve(tl_.rows.size());
    for (const TItem& it : tl_.rows) {
        const RowText r = describe(project_, it);
        rows.push_back({it.t, tl_.event(project_, it).kind, r.who, r.text, r.hex, r.dir});
        if (tl_.event(project_, it).kind == QLatin1String("data")) traffic.emplace_back(it.t, r.dir);
    }
    rows_->setRows(std::move(rows), tl_.start);
    bar_->setRange(tl_.start, tl_.end);
    bar_->setTraffic(std::move(traffic));
    std::vector<TimelineBar::Flag> marks, syncs;
    for (const TItem& m : tl_.marks) {
        const Event& e = tl_.event(project_, m);
        const int n = e.o.value(QStringLiteral("n")).toInt();
        marks.push_back({m.t, QString::number(n), QStringLiteral("Mark %1 at %2: %3").arg(n).arg(clockText(m.t - tl_.start), e.str("note"))});
    }
    for (const TItem& s : tl_.syncs)
        syncs.push_back({s.t, {}, QStringLiteral("Sync %1 (%2)").arg(tl_.event(project_, s).o.value(QStringLiteral("n")).toInt()).arg(project_.sessions[s.s].session.recorder())});
    bar_->setMarks(std::move(marks));
    bar_->setSyncs(std::move(syncs));
    pictureBox_->blockSignals(true);
    pictureBox_->clear();
    for (const Picture& p : tl_.pictures) pictureBox_->addItem(QStringLiteral("%1 (%2 pictures)").arg(p.label).arg(p.frames.size()));
    pictureBox_->setVisible(tl_.pictures.size() > 1);
    pictureBox_->blockSignals(false);
    picture_->setPlaceholder(tl_.pictures.empty() ? QStringLiteral("No pictures in this recording.\nAdd a Tick session to see the instrument PC's screen.")
                                                  : QStringLiteral("No picture yet at this time."));
    // start at the first mark, else the first traffic, so there is something to see
    double t = keep;
    if (t < 0) t = !tl_.marks.empty() ? tl_.marks.front().t : !tl_.rows.empty() ? tl_.rows.front().t : tl_.start;
    setTimelineTime(t);
}

void MainWindow::setTimelineTime(double t) {
    bar_->setTime(t);
    if (!syncingSelection_) {
        syncingSelection_ = true;
        const int r = tl_.rowAt(t + 1e-9);
        if (r >= 0) {
            rowView_->selectRow(r);
            rowView_->scrollTo(rows_->index(r, 0), QAbstractItemView::PositionAtCenter);
        } else {
            rowView_->clearSelection();
        }
        syncingSelection_ = false;
    }
    showPicture();
}

void MainWindow::showPicture() {
    const double t = bar_->time();
    tlTime_->setText(QStringLiteral("%1 / %2").arg(clockText(t - tl_.start), clockText(tl_.end - tl_.start, false)));
    const int k = pictureBox_->currentIndex();
    if (k < 0 || k >= int(tl_.pictures.size())) {
        picture_->setImage({});
        picture_->setCaption({});
        return;
    }
    double ft = 0;
    picture_->setImage(screen_.at(project_, tl_.pictures[size_t(k)], t, &ft));
    picture_->setCaption(picture_->image().isNull() ? QString()
                                                    : QStringLiteral("%1 · picture from %2 (%3 s before)")
                                                          .arg(tl_.pictures[size_t(k)].label, clockText(ft - tl_.start))
                                                          .arg(t - ft, 0, 'f', 1));
}

bool MainWindow::addSession(const QString& folder) {
    QString err;
    if (!project_.isSaved()) {
        if (project_.sessions.empty()) return false;
        // a session opened on its own: make a recording folder next to it
        const QString base = QFileInfo(project_.sessions[0].session.folder).absolutePath();
        Project np;
        if (!Project::create(QDir(base).filePath(project_.name + QStringLiteral("-combined")), &np, &err) ||
            !np.addSession(project_.sessions[0].session.folder, &err) || !np.save(&err)) {
            QMessageBox::warning(this, QStringLiteral("Add session"), err);
            return false;
        }
        project_ = std::move(np);
    }
    int idx = -1;
    if (!project_.addSession(folder, &err, &idx)) {
        QMessageBox::warning(this, QStringLiteral("Add session"), err);
        return false;
    }
    const MatchResult m = idx > 0 ? matchInProject(project_, 0, idx) : MatchResult{};
    project_.save(&err);
    reloadTimeline();
    if (idx > 0) statusBar()->showMessage(m.message, 20000);
    return true;
}

void MainWindow::addTickDialog() {
    if (project_.sessions.empty()) {
        QMessageBox::information(this, QStringLiteral("Add Tick session"), QStringLiteral("Open or record a recording first; the Tick session is added to it."));
        return;
    }
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Tick's session folder (from the USB stick)"), QDir::homePath());
    if (!d.isEmpty()) addSession(d);
}

void MainWindow::clocksDialog() {
    if (project_.sessions.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Clocks"),
                                 QStringLiteral("This recording has one clock only. Add a Tick session to combine two recordings."));
        return;
    }
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("Clocks"));
    d.resize(720, 300);
    auto* v = new QVBoxLayout(&d);
    auto* l = new QLabel(QStringLiteral("Each recorder has its own clock. LibreTap lines them up by the traffic both saw, or by Sync presses. "
                                        "You can also shift a session by hand (seconds added to its times)."));
    l->setWordWrap(true);
    v->addWidget(l);
    auto* t = new QTableWidget(int(project_.sessions.size()), 4);
    t->setHorizontalHeaderLabels({QStringLiteral("Session"), QStringLiteral("Matched by"), QStringLiteral("Shift (s)"), QStringLiteral("Drift (ppm)")});
    t->horizontalHeader()->setStretchLastSection(true);
    t->setColumnWidth(0, 260);
    std::vector<QDoubleSpinBox*> spins;
    for (int i = 0; i < int(project_.sessions.size()); ++i) {
        const ProjectSession& s = project_.sessions[size_t(i)];
        t->setItem(i, 0, new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(s.session.name(), s.session.recorder())));
        t->setItem(i, 1, new QTableWidgetItem(s.how + (s.match.contains(QStringLiteral("matches")) ? QStringLiteral(", %1 matches").arg(s.match.value(QStringLiteral("matches")).toInt()) : QString())));
        auto* sp = new QDoubleSpinBox;
        sp->setRange(-1e9, 1e9);
        sp->setDecimals(3);
        sp->setValue(s.offset);
        sp->setEnabled(i > 0);
        t->setCellWidget(i, 2, sp);
        spins.push_back(sp);
        t->setItem(i, 3, new QTableWidgetItem(QString::number(s.drift * 1e6, 'f', 1)));
    }
    v->addWidget(t);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* autoBtn = bb->addButton(QStringLiteral("Match automatically"), QDialogButtonBox::ActionRole);
    connect(autoBtn, &QPushButton::clicked, &d, [&] {
        for (auto& s : project_.sessions)
            if (s.how == QLatin1String("manual")) s.how = QStringLiteral("none");
        matchAll(project_);
        QStringList msg;
        for (int i = 1; i < int(project_.sessions.size()); ++i) {
            spins[size_t(i)]->setValue(project_.sessions[size_t(i)].offset);
            t->item(i, 1)->setText(project_.sessions[size_t(i)].how);
            t->item(i, 3)->setText(QString::number(project_.sessions[size_t(i)].drift * 1e6, 'f', 1));
            msg << project_.sessions[size_t(i)].match.value(QStringLiteral("message")).toString();
        }
        l->setText(msg.join(QLatin1Char('\n')));
    });
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) {
        Project reload;
        if (project_.isSaved() && Project::load(project_.folder, &reload, nullptr)) project_ = std::move(reload);
        reloadTimeline(bar_->time());
        return;
    }
    for (int i = 1; i < int(project_.sessions.size()); ++i) {
        ProjectSession& s = project_.sessions[size_t(i)];
        if (std::abs(spins[size_t(i)]->value() - s.offset) > 5e-4) {
            s.offset = spins[size_t(i)]->value();
            s.how = QStringLiteral("manual");
        }
    }
    QString err;
    if (project_.isSaved()) project_.save(&err);
    reloadTimeline(bar_->time());
}

}  // namespace lt::gui
