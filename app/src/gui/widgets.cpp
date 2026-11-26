#include "widgets.h"

#include <QFontDatabase>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>

#include "core/util.h"
#include "theme.h"

namespace lt::gui {

namespace {
QColor kPcC() { return theme::pcColor(); }
QColor kInstrC() { return theme::instrumentColor(); }
QColor kMarkC() { return theme::markColor(); }
QColor kSyncC() { return theme::syncColor(); }
}
#define kPc kPcC()
#define kInstr kInstrC()
#define kMark kMarkC()
#define kSync kSyncC()

void RowModel::setRows(std::vector<Row> rows, double t0) {
    beginResetModel();
    rows_ = std::move(rows);
    t0_ = t0;
    endResetModel();
}

void RowModel::append(const Row& r) {
    if (rows_.size() >= 20000) {
        beginRemoveRows({}, 0, 999);
        rows_.erase(rows_.begin(), rows_.begin() + 1000);
        endRemoveRows();
    }
    beginInsertRows({}, int(rows_.size()), int(rows_.size()));
    rows_.push_back(r);
    endInsertRows();
}

void RowModel::clear() { setRows({}, t0_); }

QVariant RowModel::data(const QModelIndex& i, int role) const {
    const Row& r = rows_[size_t(i.row())];
    const bool flag = r.kind == QLatin1String("mark") || r.kind == QLatin1String("sync");
    const int c = col(i.column());
    if (role == Qt::DisplayRole) {
        switch (c) {
        case 0: return clockText(r.t - t0_);
        case 1: return r.who;
        case 2: return r.text;
        case 3: return r.meaning.isEmpty() && r.decodable ? QStringLiteral("not explained yet") : r.meaning;
        case 4: return r.hex;
        }
    }
    if (role == Qt::ToolTipRole && c >= 2) return c == 2 ? r.text : c == 3 ? r.meaning : r.hex;
    if (role == Qt::ForegroundRole && c == 3 && r.meaning.isEmpty()) return theme::warn();
    if (role == Qt::FontRole && c == 3 && r.meaning.isEmpty()) {
        QFont f;
        f.setItalic(true);
        return f;
    }
    if (role == Qt::FontRole && (c == 2 || c == 4 || c == 0) && !flag) return QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (role == Qt::FontRole && flag) {
        QFont f;
        f.setBold(true);
        return f;
    }
    if (role == Qt::ForegroundRole && c == 1 && r.dir >= 0) return r.dir == 0 ? kPc : kInstr;
    if (role == Qt::ForegroundRole && c == 0) return theme::dim();
    if (role == Qt::BackgroundRole && flag) {
        QColor c = r.kind == QLatin1String("mark") ? kMark : kSync;
        c.setAlpha(theme::isDark() ? 50 : 40);
        return c;
    }
    if (role == Qt::BackgroundRole && r.kind == QLatin1String("error")) return QColor(220, 60, 60, 40);
    return {};
}

QVariant RowModel::headerData(int s, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* const h[] = {"Time", "Who", "What was sent (text)", "Meaning", "Bytes (hex)"};
    return QString::fromLatin1(h[col(s)]);
}

Row rowFromEvent(const QJsonObject& e, const QJsonObject& names) {
    Row r;
    r.t = e.value(QStringLiteral("t")).toDouble();
    r.kind = e.value(QStringLiteral("kind")).toString();
    if (r.kind == QLatin1String("data")) {
        const QByteArray b = QByteArray::fromHex(e.value(QStringLiteral("hex")).toString().toLatin1());
        r.text = textView(b);
        r.hex = hexView(b);
        const QString a = names.value(QStringLiteral("a")).toString(QStringLiteral("PC"));
        const QString bn = names.value(QStringLiteral("b")).toString(QStringLiteral("Instrument"));
        r.dir = e.value(QStringLiteral("dir")).toString() == QLatin1String("a") ? 0 : 1;
        r.who = r.dir == 0 ? a + QStringLiteral(" → ") + bn : bn + QStringLiteral(" → ") + a;
    } else if (r.kind == QLatin1String("mark")) {
        r.who = QStringLiteral("Mark %1").arg(e.value(QStringLiteral("n")).toInt());
        r.text = e.value(QStringLiteral("note")).toString();
    } else if (r.kind == QLatin1String("sync")) {
        r.who = QStringLiteral("Sync %1").arg(e.value(QStringLiteral("n")).toInt());
    } else if (r.kind == QLatin1String("error")) {
        r.who = QStringLiteral("Problem");
        r.text = e.value(QStringLiteral("text")).toString();
    } else {
        r.who = QStringLiteral("Note");
        r.text = e.value(QStringLiteral("text")).toString();
    }
    return r;
}

PictureView::PictureView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PictureView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::border(), 1));
    p.setBrush(theme::pictureBackground());
    p.drawRoundedRect(r, 12, 12);
    const int capH = caption_.isEmpty() ? 0 : fontMetrics().height() + 8;
    if (img_.isNull()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.15);
        p.setFont(f);
        p.drawText(r.adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
    } else {
        const QRectF area = r.adjusted(10, 10, -10, -10 - capH);
        QSizeF s = img_.size();
        s.scale(area.size(), Qt::KeepAspectRatio);
        const QRectF target(area.center().x() - s.width() / 2, area.center().y() - s.height() / 2, s.width(), s.height());
        p.drawImage(target, img_);
    }
    if (capH) {
        p.setPen(theme::dim());
        p.drawText(r.adjusted(10, 0, -10, -6), Qt::AlignBottom | Qt::AlignHCenter, caption_);
    }
}

TimelineBar::TimelineBar(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(64);
}

void TimelineBar::setRange(double s, double e) {
    start_ = s;
    end_ = std::max(e, s + 1e-3);
    update();
}

void TimelineBar::setTime(double t) {
    t_ = std::clamp(t, start_, end_);
    update();
}

double TimelineBar::xOf(double t) const { return 12 + (t - start_) / (end_ - start_) * (width() - 24); }
double TimelineBar::tOf(double x) const { return std::clamp(start_ + (x - 12) / std::max(1, width() - 24) * (end_ - start_), start_, end_); }

void TimelineBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double mid = 36;
    p.setPen(Qt::NoPen);
    p.setBrush(theme::card());
    p.setPen(QPen(theme::border(), 1));
    p.drawRoundedRect(QRectF(4.5, 18.5, width() - 9, 35), 8, 8);
    p.setPen(Qt::NoPen);
    // traffic: a tick per chunk, PC's up, instrument's down
    for (const auto& [t, d] : traffic_) {
        const double x = xOf(t);
        p.setPen(QPen(d == 0 ? kPc : kInstr, 1));
        if (d == 0) p.drawLine(QPointF(x, mid - 1), QPointF(x, mid - 12));
        else p.drawLine(QPointF(x, mid + 1), QPointF(x, mid + 12));
    }
    p.setPen(QPen(theme::border(), 1));
    p.drawLine(QPointF(12, mid), QPointF(width() - 12, mid));
    // pictures
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::PlaceholderText));
    for (double t : frames_) p.drawRect(QRectF(xOf(t) - 0.5, 50, 1.2, 3));
    // marks: flags with numbers above the bar
    QFont f = font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    for (const Flag& m : marks_) {
        const double x = xOf(m.t);
        p.setPen(QPen(kMark, 2));
        p.drawLine(QPointF(x, 18), QPointF(x, 54));
        p.setPen(Qt::NoPen);
        p.setBrush(kMark);
        const QRectF box(x, 2, std::max(14.0, p.fontMetrics().horizontalAdvance(m.label) + 6.0), 14);
        p.drawRoundedRect(box, 3, 3);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, m.label);
    }
    for (const Flag& s : syncs_) {
        const double x = xOf(s.t);
        QPainterPath d;
        d.moveTo(x, 4);
        d.lineTo(x + 6, 10);
        d.lineTo(x, 16);
        d.lineTo(x - 6, 10);
        d.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(kSync);
        p.drawPath(d);
        p.setPen(QPen(kSync, 1, Qt::DashLine));
        p.drawLine(QPointF(x, 16), QPointF(x, 54));
    }
    // the cursor
    const double x = xOf(t_);
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawLine(QPointF(x, 14), QPointF(x, 60));
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Highlight));
    p.drawEllipse(QPointF(x, 58), 5, 5);
}

void TimelineBar::mousePressEvent(QMouseEvent* e) {
    setTime(tOf(e->position().x()));
    emit timeChanged(t_);
}

void TimelineBar::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) {
        setTime(tOf(e->position().x()));
        emit timeChanged(t_);
    }
}

bool TimelineBar::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* h = static_cast<QHelpEvent*>(e);
        QString tip;
        for (const Flag& m : marks_)
            if (std::abs(xOf(m.t) - h->pos().x()) < 8) tip = m.tip;
        for (const Flag& s : syncs_)
            if (std::abs(xOf(s.t) - h->pos().x()) < 8) tip = s.tip;
        if (tip.isEmpty()) tip = clockText(tOf(h->pos().x()) - start_);
        QToolTip::showText(h->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(e);
}

}  // namespace lt::gui
