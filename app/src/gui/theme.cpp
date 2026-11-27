#include "theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

namespace lt::gui::theme {

namespace {
Mode g_mode = Mode::Auto;
bool g_dark = false;
bool g_connected = false;

bool systemDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown) return cs == Qt::ColorScheme::Dark;
#endif
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QString rgba(const QColor& c) { return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3); }

// libadwaita's neutral colours: window #fafafa / #242424, views white / #1e1e1e, cards white / #303030
QPalette palette(bool dark) {
    QPalette p;
    const QColor window = dark ? QColor("#242424") : QColor("#fafafa");
    const QColor base = dark ? QColor("#1e1e1e") : QColor("#ffffff");
    const QColor text = dark ? QColor("#ffffff") : QColor("#1f1f1f");
    const QColor button = dark ? QColor("#363636") : QColor("#ebebeb");
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, dark ? QColor("#232323") : QColor("#f7f7f7"));
    p.setColor(QPalette::ToolTipBase, dark ? QColor("#383838") : QColor("#ffffff"));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, dark ? QColor("#8f8f8f") : QColor("#8a8a8a"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor("#ff7b63"));
    p.setColor(QPalette::Light, dark ? QColor("#4a4a4a") : QColor("#ffffff"));
    p.setColor(QPalette::Midlight, dark ? QColor("#3a3a3a") : QColor("#f0f0f0"));
    p.setColor(QPalette::Mid, dark ? QColor("#4b4b4b") : QColor("#d6d6d6"));
    p.setColor(QPalette::Dark, dark ? QColor("#151515") : QColor("#bdbdbd"));
    p.setColor(QPalette::Shadow, QColor(0, 0, 0, 90));
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent());
    p.setColor(QPalette::LinkVisited, accent());
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, dark ? QColor("#7a7a7a") : QColor("#a8a8a8"));
    p.setColor(QPalette::Disabled, QPalette::Button, dark ? QColor("#2e2e2e") : QColor("#f0f0f0"));
    return p;
}

QString styleSheet() {
    const bool d = g_dark;
    const QString fg = d ? QStringLiteral("#ffffff") : QStringLiteral("#1f1f1f");
    const QString subtle = d ? QStringLiteral("rgba(255,255,255,0.10)") : QStringLiteral("rgba(0,0,0,0.065)");
    const QString subtleHover = d ? QStringLiteral("rgba(255,255,255,0.15)") : QStringLiteral("rgba(0,0,0,0.10)");
    const QString subtlePressed = d ? QStringLiteral("rgba(255,255,255,0.22)") : QStringLiteral("rgba(0,0,0,0.15)");
    const QString line = rgba(border());
    const QString acc = accent().name();
    const QString rec = record().name();
    QColor accSoft = accent();
    accSoft.setAlphaF(d ? 0.35 : 0.18);
    QColor markSoft = markColor();
    markSoft.setAlphaF(d ? 0.22 : 0.16);
    return QStringLiteral(R"(
QWidget { font-size: 10.5pt; }
QToolTip { max-width: 360px; padding: 6px 8px; border: 1px solid %LINE%; border-radius: 6px; }
QMainWindow::separator { width: 0; height: 0; }
QMenuBar { background: transparent; border: none; padding: 2px 4px; }
QMenuBar::item { padding: 4px 10px; border-radius: 6px; background: transparent; }
QMenuBar::item:selected { background: %SUBTLE%; }
QMenu { background: %CARD%; border: 1px solid %LINE%; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 6px 22px 6px 12px; border-radius: 6px; }
QMenu::item:selected { background: %SUBTLE%; color: %FG%; }
QMenu::separator { height: 1px; background: %LINE%; margin: 5px 8px; }
QStatusBar { color: %DIM%; border: none; }
QStatusBar::item { border: none; }

QFrame#card { background: %CARD%; border: 1px solid %LINE%; border-radius: 12px; }
QFrame#header { background: transparent; border: none; border-bottom: 1px solid %LINE%; }
QLabel#cardTitle { font-weight: 700; color: %ACC%; font-size: 10.5pt; }
QLabel#dot { border-radius: 6px; background: %DOTOFF%; margin-top: 4px; }
QLabel#dot[state="ok"] { background: %OK%; }
QLabel#dot[state="warn"] { background: %WARN%; }
QLabel#hint { border-radius: 8px; padding: 8px 10px; }
QLabel#hint[state="warn"] { background: %WARNSOFT%; border-left: 3px solid %WARN%; }
QLabel#appTitle { font-weight: 700; font-size: 12pt; }
QLabel#subtitle, QLabel#dimText { color: %DIM%; }
QLabel#title { font-size: 13pt; font-weight: 700; }

QPushButton, QToolButton {
  background: %SUBTLE%; color: %FG%; border: none; border-radius: 7px; padding: 7px 14px; font-weight: 600; }
QPushButton:hover, QToolButton:hover { background: %SUBTLEHOVER%; }
QPushButton:pressed, QToolButton:pressed { background: %SUBTLEPRESSED%; }
QPushButton:disabled, QToolButton:disabled { color: %DISABLED%; background: %SUBTLEDIS%; }
QToolButton::menu-indicator { image: none; width: 0; }
QToolButton#flat, QPushButton#flat { background: transparent; }
QToolButton#flat:hover, QPushButton#flat:hover { background: %SUBTLE%; }
QPushButton#link { background: transparent; color: %ACC%; padding: 2px 0; font-weight: 600; text-align: left; }
QPushButton#link:hover { text-decoration: underline; }
QPushButton#suggested { background: %ACCBG%; color: white; }
QPushButton#suggested:hover { background: %ACCHOVER%; }

QPushButton#record { background: %ACCBG%; color: white; border-radius: 14px; font-size: 17pt; font-weight: 800; padding: 10px; }
QPushButton#record:hover { background: %ACCHOVER%; }
QPushButton#record:pressed { background: %ACCPRESSED%; }
QPushButton#record[recording="true"] { background: %REC%; }
QPushButton#record[recording="true"]:hover { background: %RECHOVER%; }
QPushButton#record:disabled { background: %SUBTLEDIS%; color: %DISABLED%; }
QPushButton#mark { background: %MARKSOFT%; color: %MARKFG%; border-radius: 10px; font-size: 12pt; font-weight: 700; padding: 9px; }
QPushButton#mark:hover { background: %MARKHOVER%; }
QPushButton#mark:disabled { background: %SUBTLEDIS%; color: %DISABLED%; }
QPushButton#sync { border-radius: 10px; font-size: 11pt; padding: 9px; }

QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox {
  background: %SUBTLE%; color: %FG%; border: 2px solid transparent; border-radius: 7px; padding: 5px 8px;
  selection-background-color: %ACC%; }
QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border: 2px solid %ACCSOFT%; }
QLineEdit:disabled, QComboBox:disabled { color: %DISABLED%; background: %SUBTLEDIS%; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: %CARD%; border: 1px solid %LINE%; border-radius: 8px; padding: 4px;
  selection-background-color: %SUBTLE%; selection-color: %FG%; outline: none; }
QCheckBox { spacing: 8px; }

QTableView, QTableWidget, QListView {
  background: %CARD%; alternate-background-color: %CARD%; border: none; border-radius: 12px;
  selection-background-color: %ACCSOFT%; selection-color: %FG%; outline: none; gridline-color: transparent; }
QTableView::item { padding: 2px 8px; border-bottom: 1px solid %ROWLINE%; }
QTableView::item:selected { background: %ACCSOFT%; color: %FG%; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section { background: %ACCTINT%; color: %DIM%; border: none; border-bottom: 1px solid %LINE%;
  padding: 8px 8px 6px 8px; font-weight: 700; font-size: 9.5pt; text-align: left; }
QTableCornerButton::section { background: transparent; border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px 4px; }
QScrollBar::handle { background: %SCROLL%; border-radius: 3px; min-height: 30px; min-width: 30px; }
QScrollBar::handle:hover { background: %SCROLLHOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: transparent; }
QProgressBar { background: %SUBTLE%; border: none; border-radius: 4px; height: 8px; text-align: center; }
QProgressBar::chunk { background: %ACC%; border-radius: 4px; }
QDialog { background: %WINDOW%; }
)")
        .replace(QStringLiteral("%LINE%"), line)
        .replace(QStringLiteral("%ROWLINE%"), d ? QStringLiteral("rgba(255,255,255,0.05)") : QStringLiteral("rgba(0,0,0,0.05)"))
        .replace(QStringLiteral("%SUBTLEHOVER%"), subtleHover)
        .replace(QStringLiteral("%SUBTLEPRESSED%"), subtlePressed)
        .replace(QStringLiteral("%SUBTLEDIS%"), d ? QStringLiteral("rgba(255,255,255,0.05)") : QStringLiteral("rgba(0,0,0,0.035)"))
        .replace(QStringLiteral("%SUBTLE%"), subtle)
        .replace(QStringLiteral("%CARD%"), card().name())
        .replace(QStringLiteral("%WINDOW%"), d ? QStringLiteral("#242424") : QStringLiteral("#fafafa"))
        .replace(QStringLiteral("%FG%"), fg)
        .replace(QStringLiteral("%DIM%"), dim().name())
        .replace(QStringLiteral("%DISABLED%"), d ? QStringLiteral("#6f6f6f") : QStringLiteral("#a8a8a8"))
        .replace(QStringLiteral("%ACCSOFT%"), rgba(accSoft))
        .replace(QStringLiteral("%ACCBG%"), (d ? QColor("#4b7f7a") : accent()).name())
        .replace(QStringLiteral("%ACCTINT%"), rgba(QColor(accent().red(), accent().green(), accent().blue(), d ? 30 : 20)))
        .replace(QStringLiteral("%ACCPRESSED%"), (d ? QColor("#3f6c68") : accent().darker(118)).name())
        .replace(QStringLiteral("%WARNSOFT%"), rgba(QColor(warn().red(), warn().green(), warn().blue(), d ? 50 : 45)))
        .replace(QStringLiteral("%WARN%"), warn().name())
        .replace(QStringLiteral("%OK%"), ok().name())
        .replace(QStringLiteral("%DOTOFF%"), d ? QStringLiteral("#5e5e5e") : QStringLiteral("#c4c4c4"))
        .replace(QStringLiteral("%ACCHOVER%"), (d ? QColor("#558b86") : accent().lighter(112)).name())
        .replace(QStringLiteral("%ACC%"), acc)
        .replace(QStringLiteral("%RECHOVER%"), record().lighter(110).name())
        .replace(QStringLiteral("%RECPRESSED%"), record().darker(125).name())
        .replace(QStringLiteral("%REC%"), rec)
        .replace(QStringLiteral("%MARKSOFT%"), rgba(markSoft))
        .replace(QStringLiteral("%MARKHOVER%"), rgba(QColor(markColor().red(), markColor().green(), markColor().blue(), d ? 90 : 64)))
        .replace(QStringLiteral("%MARKFG%"), d ? QStringLiteral("#e3c58f") : QStringLiteral("#7a5a22"))
        .replace(QStringLiteral("%SCROLLHOVER%"), d ? QStringLiteral("rgba(255,255,255,0.35)") : QStringLiteral("rgba(0,0,0,0.35)"))
        .replace(QStringLiteral("%SCROLL%"), d ? QStringLiteral("rgba(255,255,255,0.20)") : QStringLiteral("rgba(0,0,0,0.20)"));
}

void applyNow(QApplication& app) {
    g_dark = g_mode == Mode::Dark || (g_mode == Mode::Auto && systemDark());
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setPalette(palette(g_dark));
    app.setStyleSheet(styleSheet());
    for (QWidget* w : QApplication::allWidgets()) w->update();
}
}  // namespace

void apply(QApplication& app, Mode m) {
    const QString env = qEnvironmentVariable("LIBRETAP_THEME");
    if (env == QLatin1String("dark")) m = Mode::Dark;
    else if (env == QLatin1String("light")) m = Mode::Light;
    g_mode = m;
    applyNow(app);
    if (!g_connected) {
        g_connected = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
            if (g_mode == Mode::Auto) applyNow(app);
        });
#endif
    }
}

bool isDark() { return g_dark; }

// muted, desaturated tones: dusty teal accent, terracotta Record, slate/sage for the two directions, ochre marks
QColor accent() { return g_dark ? QColor("#6fa6a0") : QColor("#4c837e"); }
QColor record() { return g_dark ? QColor("#b8625a") : QColor("#b0544c"); }
QColor pcColor() { return g_dark ? QColor("#8fb0d0") : QColor("#4f74a0"); }
QColor instrumentColor() { return g_dark ? QColor("#93bfa7") : QColor("#5a8a72"); }
QColor markColor() { return g_dark ? QColor("#c9a25a") : QColor("#b98d3e"); }
QColor syncColor() { return g_dark ? QColor("#b49cc4") : QColor("#7d6390"); }
QColor ok() { return QColor("#74a888"); }
QColor warn() { return QColor("#c9a352"); }
QColor card() { return g_dark ? QColor("#303030") : QColor("#ffffff"); }
QColor border() { return g_dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 22); }
QColor dim() { return g_dark ? QColor("#a6a6a6") : QColor("#6e6e6e"); }
QColor pictureBackground() { return g_dark ? QColor("#191919") : QColor("#ececec"); }

}  // namespace lt::gui::theme
