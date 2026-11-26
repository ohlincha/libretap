#pragma once
// The analysis views of the timeline: compare two moments side by side, questions and answers, message hints.
// The same analysis as LibreTapCLI exchanges / diff / hints (core/analysis.h), shown for people.
#include <QDialog>

#include "core/analysis.h"

class QCheckBox;
class QComboBox;
class QTextBrowser;

namespace lt::gui {

// Two moments (marks, or the end) side by side: per kind of message the last one seen, changed bytes highlighted.
class CompareDialog : public QDialog {
    Q_OBJECT
public:
    CompareDialog(const Project& p, const Timeline& tl, QWidget* parent = nullptr);
    void setMoments(int a, int b);   // indexes in the lists (marks in order, then "the end")

private:
    void update();
    const Project& p_;
    const Timeline& tl_;
    std::vector<Msg> msgs_;
    QComboBox *a_, *b_;
    QTextBrowser* view_;
    QCheckBox* all_;
};

// A report in a window (exchanges, hints): HTML, with a Copy button.
void showReport(QWidget* parent, const QString& title, const QString& html, const QString& objectName);
QString exchangesHtml(const std::vector<ExchangeGroup>& ex, double t0);
QString hintsHtml(const std::vector<FamilyHints>& h);

}  // namespace lt::gui
