#include "analysisviews.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "core/util.h"
#include "theme.h"

namespace lt::gui {

namespace {
QString css() {
    QColor acc = theme::accent();
    acc.setAlphaF(theme::isDark() ? 0.45 : 0.25);
    return QStringLiteral(
               "<style>body { font-size: 10.5pt; } td, th { padding: 5px 8px; vertical-align: top; } "
               "th { text-align: left; color: %1; font-weight: 600; border-bottom: 1px solid %2; } "
               "td { border-bottom: 1px solid %3; } .mono { font-family: monospace; } .dim { color: %1; } "
               ".chg { background: %4; font-weight: 700; } .tag { color: %5; font-weight: 600; } "
               "h3 { margin: 14px 0 4px 0; } </style>")
        .arg(theme::dim().name(), theme::border().name(QColor::HexArgb), theme::isDark() ? QStringLiteral("#3a3a3a") : QStringLiteral("#eeeeee"),
             acc.name(QColor::HexArgb), theme::accent().name());
}

QString esc(const QString& s) { return s.toHtmlEscaped(); }

// bytes as hex with the changed positions highlighted
QString hexMarked(const QByteArray& b, const std::vector<int>& changed, int max = 48) {
    QString out;
    for (int i = 0; i < std::min<int>(int(b.size()), max); ++i) {
        const QString h = QStringLiteral("%1").arg(uchar(b[i]), 2, 16, QLatin1Char('0'));
        out += std::find(changed.begin(), changed.end(), i) != changed.end() ? QStringLiteral("<span class='chg'>%1</span> ").arg(h) : h + QLatin1Char(' ');
    }
    if (b.size() > max) out += QStringLiteral("…");
    return out;
}

QString textMarked(const QByteArray& b, const std::vector<int>& changed, int max = 60) {
    QString out;
    for (int i = 0; i < std::min<int>(int(b.size()), max); ++i) {
        const QString t = esc(textView(b.mid(i, 1)));
        out += std::find(changed.begin(), changed.end(), i) != changed.end() ? QStringLiteral("<span class='chg'>%1</span>").arg(t) : t;
    }
    if (b.size() > max) out += QStringLiteral("…");
    return out;
}
}  // namespace

CompareDialog::CompareDialog(const Project& p, const Timeline& tl, QWidget* parent) : QDialog(parent), p_(p), tl_(tl) {
    setObjectName(QStringLiteral("compare"));
    setWindowTitle(QStringLiteral("Compare two moments"));
    resize(1100, 680);
    msgs_ = messages(p, tl);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 14, 16, 12);
    auto* intro = new QLabel(QStringLiteral("What did the vendor software send differently? For each kind of message, the last one seen after "
                                            "each moment (until the next mark). Changed bytes are highlighted."));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("dimText"));
    v->addWidget(intro);
    auto* row = new QHBoxLayout;
    a_ = new QComboBox;
    b_ = new QComboBox;
    for (QComboBox* c : {a_, b_}) {
        for (const TItem& m : tl.marks) {
            const Event& e = tl.event(p, m);
            c->addItem(QStringLiteral("Mark %1 at %2%3").arg(e.o.value(QStringLiteral("n")).toInt()).arg(clockText(m.t - tl.start, false))
                           .arg(e.str("note").isEmpty() ? QString() : QStringLiteral(": ") + e.str("note")));
        }
        c->addItem(QStringLiteral("The end of the recording"));
        c->setMinimumContentsLength(28);
    }
    all_ = new QCheckBox(QStringLiteral("Also show what stayed the same"));
    row->addWidget(new QLabel(QStringLiteral("A:")));
    row->addWidget(a_, 1);
    row->addSpacing(12);
    row->addWidget(new QLabel(QStringLiteral("B:")));
    row->addWidget(b_, 1);
    row->addSpacing(12);
    row->addWidget(all_);
    v->addLayout(row);
    view_ = new QTextBrowser;
    view_->setFrameShape(QFrame::NoFrame);
    v->addWidget(view_, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    setMoments(0, std::min(1, b_->count() - 1));
    connect(a_, &QComboBox::currentIndexChanged, this, &CompareDialog::update);
    connect(b_, &QComboBox::currentIndexChanged, this, &CompareDialog::update);
    connect(all_, &QCheckBox::toggled, this, &CompareDialog::update);
}

void CompareDialog::setMoments(int a, int b) {
    a_->setCurrentIndex(a);
    b_->setCurrentIndex(b);
    update();
}

void CompareDialog::update() {
    auto window = [&](int k, double* f, double* t) {
        if (k < int(tl_.marks.size())) {
            *f = tl_.marks[size_t(k)].t;
            *t = k + 1 < int(tl_.marks.size()) ? tl_.marks[size_t(k + 1)].t : tl_.end + 1;
        } else {
            *f = tl_.start;
            *t = tl_.end + 1;
        }
    };
    double fa, ta, fb, tb;
    window(a_->currentIndex(), &fa, &ta);
    window(b_->currentIndex(), &fb, &tb);
    const auto rows = diffMoments(msgs_, fa, ta, fb, tb);
    QString h = css() + QStringLiteral("<table width='100%' cellspacing='0'><tr><th>Kind of message</th><th></th><th>A</th><th>B</th></tr>");
    int shown = 0, same = 0;
    for (const DiffRow& r : rows) {
        if (r.status == QLatin1String("same")) {
            ++same;
            if (!all_->isChecked()) continue;
        }
        ++shown;
        const QString status = r.status == QLatin1String("changed") ? QStringLiteral("<span class='tag'>changed</span>")
                               : r.status == QLatin1String("same")  ? QStringLiteral("<span class='dim'>same</span>")
                                                                    : QStringLiteral("<span class='dim'>%1</span>").arg(r.status == QLatin1String("only A") ? QStringLiteral("only in A") : QStringLiteral("only in B"));
        auto cell = [&](const Msg* m) {
            if (!m) return QStringLiteral("<span class='dim'>—</span>");
            if (m->control) return QStringLiteral("%1<br><span class='mono dim'>%2</span>").arg(esc(m->text.section(QStringLiteral(" wLength"), 0, 0)), hexMarked(m->bytes, r.changed));
            if (!looksText(m->bytes)) return QStringLiteral("<span class='dim'>%1 bytes</span><br><span class='mono'>%2</span>").arg(m->bytes.size()).arg(hexMarked(m->bytes, r.changed, 32));
            return QStringLiteral("<span class='mono'>%1</span><br><span class='mono dim'>%2</span>").arg(textMarked(m->bytes, r.changed), hexMarked(m->bytes, r.changed));
        };
        h += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>").arg(esc(r.family), status, cell(r.a), cell(r.b));
    }
    h += QStringLiteral("</table>");
    if (!shown) h += QStringLiteral("<p class='dim'>Nothing differs between these two moments.</p>");
    if (same && !all_->isChecked()) h += QStringLiteral("<p class='dim'>%1 kinds of message are the same in both.</p>").arg(same);
    view_->setHtml(h);
}

void showReport(QWidget* parent, const QString& title, const QString& html, const QString& objectName) {
    auto* d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setObjectName(objectName);
    d->setWindowTitle(title);
    d->resize(1100, 680);
    auto* v = new QVBoxLayout(d);
    v->setContentsMargins(16, 14, 16, 12);
    auto* view = new QTextBrowser;
    view->setFrameShape(QFrame::NoFrame);
    view->setHtml(css() + html);
    v->addWidget(view, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* copy = bb->addButton(QStringLiteral("Copy as text"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, d, [view] { QApplication::clipboard()->setText(view->toPlainText()); });
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    v->addWidget(bb);
    d->show();
}

QString exchangesHtml(const std::vector<ExchangeGroup>& exIn, double t0) {
    std::vector<ExchangeGroup> ex = exIn;   // the most repeated first: the polling, then the rarer commands
    std::stable_sort(ex.begin(), ex.end(), [](const ExchangeGroup& a, const ExchangeGroup& b) { return a.count > b.count; });
    QString h = QStringLiteral("<p class='dim'>Each question the PC sent and what came back. Identical repeats are folded into one line; the most repeated first.</p>"
                               "<table width='100%' cellspacing='0'><tr><th>First</th><th>Link</th><th>Question</th><th>Times</th><th>Answer</th><th>After</th></tr>");
    for (const ExchangeGroup& g : ex) {
        QString answer;
        if (g.answers.empty()) answer = QStringLiteral("<span class='dim'>none</span>");
        else if (g.answers.size() == 1) answer = QStringLiteral("<span class='mono'>%1</span>%2").arg(esc(textView(g.answers[0].first, 60)), g.count > 1 ? QStringLiteral(" <span class='dim'>(always)</span>") : QString());
        else
            answer = QStringLiteral("<span class='tag'>%1 different</span> <span class='dim'>(changed %2×), last</span> <span class='mono'>%3</span>")
                         .arg(g.answers.size()).arg(g.changes).arg(esc(textView(g.answers.back().first, 50)));
        h += QStringLiteral("<tr><td class='dim'>%1</td><td>%2</td><td class='mono'>%3</td><td>%4</td><td>%5</td><td class='dim'>%6 ms</td></tr>")
                 .arg(clockText(g.first - t0, false), esc(g.link), esc(g.questionText.left(80)))
                 .arg(g.count).arg(answer).arg(g.latency * 1000, 0, 'f', 0);
    }
    return h + QStringLiteral("</table>");
}

QString hintsHtml(const std::vector<FamilyHints>& hsIn) {
    QString h = QStringLiteral("<p class='dim'>Clues for each kind of message: what is fixed, what varies (and how it follows the marks), "
                               "length fields and checksum candidates. Candidates are guesses until checked. The most frequent kinds first.</p>");
    std::vector<FamilyHints> hs;
    QStringList once;
    for (const FamilyHints& f : hsIn) {
        if (f.count == 1 && f.checksum.isEmpty() && f.length.isEmpty()) once << f.family;
        else hs.push_back(f);
    }
    std::stable_sort(hs.begin(), hs.end(), [](const FamilyHints& a, const FamilyHints& b) { return a.count > b.count; });
    for (const FamilyHints& f : hs) {
        const QStringList lines = f.lines();
        h += QStringLiteral("<h3>%1</h3><div class='dim'>%2</div>").arg(esc(f.family), esc(lines.first().section(QStringLiteral(": "), 1)));
        QString ul;
        for (int i = 1; i < lines.size(); ++i) ul += QStringLiteral("<li>%1</li>").arg(esc(lines[i].trimmed()));
        if (!ul.isEmpty()) h += QStringLiteral("<ul style='margin-top:2px'>%1</ul>").arg(ul);
    }
    if (!once.isEmpty()) h += QStringLiteral("<h3>Seen once</h3><div class='dim'>%1</div>").arg(esc(once.join(QStringLiteral(" · "))));
    return h;
}

}  // namespace lt::gui
