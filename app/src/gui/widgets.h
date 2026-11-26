#pragma once
// The GUI's building blocks: the traffic table's model, the picture view and the timeline bar.
#include <QAbstractTableModel>
#include <QImage>
#include <QJsonObject>
#include <QWidget>

#include <vector>

#include "core/timeline.h"

namespace lt::gui {

struct Row {
    double t = 0;                  // project time (or wall time while recording)
    QString kind, who, text, hex;
    int dir = -1;
};

// Time | Who | Text | Hex. Marks and syncs are rows of their own, tinted.
class RowModel : public QAbstractTableModel {
    Q_OBJECT
public:
    using QAbstractTableModel::QAbstractTableModel;
    void setRows(std::vector<Row> rows, double t0);
    void append(const Row& r);     // live: keeps the last 20000
    void clear();
    const Row& row(int i) const { return rows_[size_t(i)]; }
    int rowCount(const QModelIndex& = {}) const override { return int(rows_.size()); }
    int columnCount(const QModelIndex& = {}) const override { return 4; }
    QVariant data(const QModelIndex& i, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    void setT0(double t0) { t0_ = t0; }

private:
    std::vector<Row> rows_;
    double t0_ = 0;
};

Row rowFromEvent(const QJsonObject& e, const QJsonObject& sources);   // a live event

class PictureView : public QWidget {
    Q_OBJECT
public:
    explicit PictureView(QWidget* parent = nullptr);
    void setImage(const QImage& img) { img_ = img; update(); }
    void setPlaceholder(const QString& t) { placeholder_ = t; update(); }
    void setCaption(const QString& t) { caption_ = t; update(); }
    const QImage& image() const { return img_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage img_;
    QString placeholder_, caption_;
};

// The scrub bar: traffic as ticks (PC's above the line, the instrument's below), pictures as dots, marks as
// numbered flags, syncs as diamonds; click or drag to move the time.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    explicit TimelineBar(QWidget* parent = nullptr);
    void setRange(double start, double end);
    void setTraffic(std::vector<std::pair<double, int>> t) { traffic_ = std::move(t); update(); }
    void setFrames(std::vector<double> f) { frames_ = std::move(f); update(); }
    struct Flag { double t; QString label, tip; };
    void setMarks(std::vector<Flag> m) { marks_ = std::move(m); update(); }
    void setSyncs(std::vector<Flag> s) { syncs_ = std::move(s); update(); }
    void setTime(double t);
    double time() const { return t_; }
    QSize sizeHint() const override { return {600, 64}; }
    QSize minimumSizeHint() const override { return {200, 64}; }

signals:
    void timeChanged(double t);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    bool event(QEvent* e) override;

private:
    double xOf(double t) const;
    double tOf(double x) const;
    double start_ = 0, end_ = 1, t_ = 0;
    std::vector<std::pair<double, int>> traffic_;
    std::vector<double> frames_;
    std::vector<Flag> marks_, syncs_;
};

}  // namespace lt::gui
