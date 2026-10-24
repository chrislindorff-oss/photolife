#include "ui/GettingStartedPanel.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMouseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

namespace pl {

namespace {

struct StepText
{
    const char *title;
    const char *text;
    const char *image;
    const char *primary;
    const char *secondary;   // may be null
};

// Plain words for someone who has never used PhotoLife.
const StepText kSteps[GettingStartedPanel::StepCount] = {
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Add your photos"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "Tell PhotoLife where your nature photos are. It reads them where they are — "
                       "nothing is moved or changed — and recognises species from your folder and file "
                       "names, so photos already sorted into folders like <i>Flame Robin</i> or "
                       "<i>Diuris pardina</i> work best. JPEG and RAW are both fine."),
     ":/guide/library.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Add a Folder…"), nullptr},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Build a reference tree"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "A reference tree is the list of species to aim for. Pick a group and a region — "
                       "say <i>Orchids</i> in <i>Victoria</i> — and PhotoLife builds it from every species "
                       "recorded there on iNaturalist. You can build as many as you like, or import one "
                       "someone has shared."),
     ":/guide/tree.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "New Reference Tree…"),
     QT_TRANSLATE_NOOP("GettingStartedPanel", "Import a Tree File…")},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Match your photos"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "PhotoLife ties each photo to a species by its folder and file names — scientific "
                       "or common names both work. It only takes a moment, and it's safe to run again "
                       "whenever you add photos."),
     ":/guide/match.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Match Library"), nullptr},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Review the uncertain ones"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "Some names can't be matched for certain. Review Unmatched shows each one beside "
                       "an iNaturalist reference photo: confirm the right taxon, settle for the genus, or "
                       "skip it for now. PhotoLife remembers the names you confirm."),
     ":/guide/review.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Open Review Unmatched"), nullptr},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Explore your coverage"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "Pick a tree on the left to see which species you've photographed and which are "
                       "still to find — with counts on every genus and family, a map of where you took "
                       "them, and iNaturalist reference photos of the ones you're missing."),
     ":/guide/explore.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Show My Trees"), nullptr},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "Bring in your iNaturalist observations"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "Uploaded photos to iNaturalist? Enter your iNaturalist username and pick a tree: "
                       "PhotoLife lists your observations of its species that your library doesn't have "
                       "yet, ready to download into it."),
     ":/guide/inat.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Open iNaturalist Download"), nullptr},
    {QT_TRANSLATE_NOOP("GettingStartedPanel", "After a field trip: organise new photos"),
     QT_TRANSLATE_NOOP("GettingStartedPanel",
                       "Back with a folder of <i>DSC_1234</i> files? Open it here, click the first photo "
                       "of each species and name it, mark the rejects, then let PhotoLife sort them into "
                       "a folder per species, ready for your library. It can all be undone."),
     ":/guide/organise.png", QT_TRANSLATE_NOOP("GettingStartedPanel", "Open a Download Folder…"), nullptr},
};

} // namespace

GettingStartedPanel::GettingStartedPanel(QWidget *parent) : QWidget(parent)
{
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("Welcome to PhotoLife"), content);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.35);
    tf.setBold(true);
    title->setFont(tf);
    auto *intro = new QLabel(tr("PhotoLife hangs your nature photos on the tree of life, so you can "
                                "see which species you've photographed and which are still to find. "
                                "These steps set it up — each button opens the real thing, and the "
                                "steps tick themselves off as you go."),
                             content);
    intro->setWordWrap(true);
    m_flowPicture = QPixmap(QStringLiteral(":/guide/flow.png"));
    m_flow = new QLabel(content);
    m_flow->setAlignment(Qt::AlignCenter);
    m_flow->setProperty("guideTitle", tr("How PhotoLife works"));
    makeEnlargeable(m_flow);
    m_progress = new QLabel(content);
    QFont pf = m_progress->font();
    pf.setBold(true);
    m_progress->setFont(pf);
    m_serverNote = new QLabel(content);
    m_serverNote->setWordWrap(true);
    m_serverNote->hide();

    layout->addWidget(title);
    layout->addWidget(intro);
    layout->addWidget(m_flow);
    layout->addWidget(m_progress);
    layout->addWidget(m_serverNote);

    for (int s = 0; s < StepCount; ++s) {
        if (s == Inat) {
            m_extrasHeading = new QLabel(tr("When you're ready"), content);
            QFont hf = m_extrasHeading->font();
            hf.setBold(true);
            m_extrasHeading->setFont(hf);
            layout->addSpacing(6);
            layout->addWidget(m_extrasHeading);
        }
        buildCard(Step(s), layout);
    }

    layout->addSpacing(6);
    m_showAtStartup = new QCheckBox(tr("Show at start-up until set up"), content);
    m_showAtStartup->setToolTip(tr("Opens this guide when PhotoLife starts, until you have a photo "
                                   "library and a reference tree."));
    m_showAtStartup->setChecked(true);
    connect(m_showAtStartup, &QCheckBox::toggled, this, &GettingStartedPanel::showAtStartupChanged);
    auto *hide = new QPushButton(tr("Hide Guide"), content);
    hide->setToolTip(tr("Open it again any time from File or Help ▸ Getting Started"));
    connect(hide, &QPushButton::clicked, this, &GettingStartedPanel::hideRequested);
    auto *footer = new QHBoxLayout;
    footer->addWidget(m_showAtStartup);
    footer->addStretch(1);
    footer->addWidget(hide);
    layout->addLayout(footer);
    auto *reopen = new QLabel(tr("You can open this guide again from File or Help ▸ Getting Started."),
                              content);
    reopen->setWordWrap(true);
    layout->addWidget(reopen);
    layout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    setMinimumWidth(300);

    updateCards();
}

void GettingStartedPanel::buildCard(Step step, QVBoxLayout *into)
{
    const StepText &t = kSteps[step];
    Card c;
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("guideCard"));   // styled by name: not its child labels
    frame->setFrameShape(QFrame::StyledPanel);
    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(6);

    c.header = new QPushButton(frame);
    c.header->setFlat(true);
    c.header->setCursor(Qt::PointingHandCursor);
    c.header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QFont hf = c.header->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() * 1.08);
    c.header->setFont(hf);
    connect(c.header, &QPushButton::clicked, this, [this, step] {
        m_cards[step].userToggled = true;
        setExpanded(step, !m_cards[step].expanded);
    });
    c.summary = new QLabel(frame);
    c.summary->setWordWrap(true);

    c.body = new QWidget(frame);
    auto *b = new QVBoxLayout(c.body);
    b->setContentsMargins(0, 0, 0, 0);
    b->setSpacing(6);
    c.picture = QPixmap(QLatin1String(t.image));
    c.image = new QLabel(c.body);
    c.image->setAlignment(Qt::AlignCenter);
    c.image->setProperty("guideStep", int(step));
    c.image->setProperty("guideTitle", tr(t.title));
    makeEnlargeable(c.image);
    c.text = new QLabel(tr(t.text), c.body);
    c.text->setWordWrap(true);
    c.text->setTextFormat(Qt::RichText);
    c.note = new QLabel(c.body);
    c.note->setWordWrap(true);
    c.primary = new QPushButton(tr(t.primary), c.body);
    connect(c.primary, &QPushButton::clicked, this, [this, step] { emit actionRequested(step, 0); });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(c.primary);
    if (t.secondary) {
        c.secondary = new QPushButton(tr(t.secondary), c.body);
        connect(c.secondary, &QPushButton::clicked, this, [this, step] { emit actionRequested(step, 1); });
        buttons->addWidget(c.secondary);
    }
    buttons->addStretch(1);
    auto *help = new QLabel(QStringLiteral("<a href=\"#\">%1</a>").arg(tr("More about this in the help…")),
                            c.body);
    help->setAlignment(Qt::AlignRight);
    connect(help, &QLabel::linkActivated, this, [this, step] { emit helpRequested(helpAnchor(step)); });
    b->addWidget(c.image);
    b->addWidget(c.text);
    b->addWidget(c.note);
    b->addLayout(buttons);
    b->addWidget(help);

    v->addWidget(c.header);
    v->addWidget(c.summary);
    v->addWidget(c.body);
    c.frame = frame;
    into->addWidget(frame);
    m_cards.append(c);
}

QString GettingStartedPanel::helpAnchor(Step step)
{
    switch (step) {
    case Library:  return QStringLiteral("library");
    case Tree:     return QStringLiteral("trees");
    case Match:    return QStringLiteral("match");
    case Review:   return QStringLiteral("review");
    case Explore:  return QStringLiteral("browse");
    case Inat:     return QStringLiteral("inat");
    case Organise: return QStringLiteral("organise");
    case StepCount: break;
    }
    return {};
}

bool GettingStartedPanel::basicsMissing(const GuideState &s)
{
    return s.folders == 0 || s.photos == 0 || s.trees == 0;
}

void GettingStartedPanel::setState(const GuideState &state)
{
    m_state = state;
    updateCards();
}

void GettingStartedPanel::setShowAtStartup(bool on)
{
    const QSignalBlocker block(m_showAtStartup);
    m_showAtStartup->setChecked(on);
}

bool GettingStartedPanel::isDone(Step step) const
{
    return m_cards.value(step).done;
}

bool GettingStartedPanel::isExpanded(Step step) const
{
    return m_cards.value(step).expanded;
}

int GettingStartedPanel::basicsDone() const
{
    int n = 0;
    for (int s = 0; s < kBasics; ++s)
        n += m_cards.at(s).done ? 1 : 0;
    return n;
}

void GettingStartedPanel::updateCards()
{
    const GuideState &s = m_state;
    const ThemeColors &t = themeColors(currentThemeVariant());
    const QLocale loc;

    // What's done, and what each says about it.
    struct Status
    {
        bool done;
        QString summary;
        QString blockedNote;   // non-empty: the main button is off, and why
    };
    QString trees = s.treeNames.mid(0, 3).join(QStringLiteral(", "));
    if (s.treeNames.size() > 3)
        trees += tr(" and %1 more").arg(s.treeNames.size() - 3);
    const Status status[StepCount] = {
        {s.folders > 0 && s.photos > 0,
         s.folders > 0 ? tr("%1 photos in %2 folder(s)").arg(loc.toString(s.photos)).arg(s.folders)
                       : QString(),
         {}},
        {s.trees > 0, s.trees > 0 ? tr("%1 tree(s): %2").arg(s.trees).arg(trees) : QString(), {}},
        {s.matched > 0,
         s.matched > 0 ? tr("%1 photos matched").arg(loc.toString(s.matched)) : QString(),
         s.photos == 0 ? tr("Add your photos first.")
         : s.trees == 0 ? tr("Build a reference tree first, so there's something to match against.")
                        : QString()},
        {s.matched > 0 && s.toReview == 0,
         s.toReview > 0 ? tr("%1 waiting for you").arg(loc.toString(s.toReview))
         : s.matched > 0 ? tr("Nothing waiting — all done") : QString(),
         s.matched == 0 && s.toReview == 0 ? tr("Match your photos first.") : QString()},
        {s.exploredCoverage, s.exploredCoverage ? tr("Visited") : QString(),
         s.trees == 0 ? tr("Build a reference tree first.") : QString()},
        {s.inatPhotos > 0,
         s.inatPhotos > 0 ? tr("%1 photos downloaded from iNaturalist").arg(loc.toString(s.inatPhotos))
         : !s.inatUser.isEmpty() ? tr("Signed in as %1").arg(s.inatUser) : QString(),
         {}},
        {s.organised, s.organised ? tr("You've organised a download folder") : QString(), {}},
    };

    // The next thing to do: the first basic not done yet, then the extras.
    m_next = -1;
    for (int i = 0; i < StepCount; ++i) {
        if (!status[i].done) {
            m_next = i;
            break;
        }
    }

    for (int i = 0; i < StepCount; ++i) {
        Card &c = m_cards[i];
        c.done = status[i].done;
        const bool next = i == m_next && i < kBasics;
        const QString badge = c.done ? QStringLiteral("✓") : next ? QStringLiteral("➜") : QStringLiteral("○");
        const QString number = i < kBasics ? QStringLiteral("%1. ").arg(i + 1) : QString();
        c.header->setText(badge + QStringLiteral("  ") + number + tr(kSteps[i].title));
        c.header->setToolTip(c.expanded ? tr("Click to fold away") : tr("Click to open"));
        c.header->setStyleSheet(QStringLiteral("QPushButton { text-align: left; padding: 3px 2px; "
                                               "border: none; color: %1; }")
                                    .arg((c.done ? t.statusAuto : next ? t.primaryAccent : t.text).name()));
        c.summary->setText(status[i].summary);
        c.summary->setVisible(!status[i].summary.isEmpty());
        QPalette sp = c.summary->palette();
        sp.setColor(QPalette::WindowText, t.mutedText);
        c.summary->setPalette(sp);
        c.frame->setStyleSheet(next ? QStringLiteral("#guideCard { border: 2px solid %1; border-radius: %2px; }")
                                          .arg(t.primaryAccent.name())
                                          .arg(t.radius)
                                    : QString());
        c.primary->setEnabled(status[i].blockedNote.isEmpty());
        if (c.secondary)
            c.secondary->setEnabled(status[i].blockedNote.isEmpty());
        c.note->setText(status[i].blockedNote);
        c.note->setVisible(!status[i].blockedNote.isEmpty());
        QPalette np = c.note->palette();
        np.setColor(QPalette::WindowText, t.warning);
        c.note->setPalette(np);
        // The next basic step is open; done ones fold away. Left alone once
        // opened or closed by hand.
        if (!c.userToggled)
            setExpanded(Step(i), next);
    }
    // A shared catalogue: nothing can be added until editing starts.
    if (s.server && !s.editable) {
        m_serverNote->setText(tr("<b>This is a shared catalogue.</b> To add photos or trees, first "
                                 "choose <b>Server ▸ Start Editing</b> (the buttons below ask too)."));
        m_serverNote->show();
    } else {
        m_serverNote->hide();
    }
    m_progress->setText(basicsDone() == kBasics
                            ? tr("✓ All set up — the extras below are there when you want them.")
                            : tr("%1 of %2 basics done").arg(basicsDone()).arg(kBasics));
    fitImages();
}

void GettingStartedPanel::setExpanded(Step step, bool on)
{
    Card &c = m_cards[step];
    c.expanded = on;
    c.body->setVisible(on);
    if (on)
        fitImages();
}

// The pictures are small in the panel: a click shows one full size.
void GettingStartedPanel::makeEnlargeable(QLabel *label)
{
    label->setCursor(Qt::PointingHandCursor);
    label->setToolTip(tr("Click to see it larger"));
    label->installEventFilter(this);
}

bool GettingStartedPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        auto *label = qobject_cast<QLabel *>(watched);
        if (label == m_flow) {
            showLarge(m_flowPicture, label->property("guideTitle").toString());
            return true;
        }
        const QVariant step = label ? label->property("guideStep") : QVariant();
        if (step.isValid()) {
            showLarge(m_cards.at(step.toInt()).picture, label->property("guideTitle").toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GettingStartedPanel::showLarge(const QPixmap &picture, const QString &title)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    auto *image = new QLabel(&dialog);
    QPixmap shown = picture;
    shown.setDevicePixelRatio(2.0);   // drawn at twice its size: shown at its own
    image->setPixmap(shown);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(image);
    layout->addWidget(buttons);
    dialog.exec();
}

void GettingStartedPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitImages();
}

// Each picture as wide as the panel allows (never wider than drawn).
void GettingStartedPanel::fitImages()
{
    const qreal dpr = devicePixelRatioF();
    auto fit = [dpr](QLabel *label, const QPixmap &picture, int width) {
        if (picture.isNull() || width <= 0)
            return;
        const int w = std::min(int(width * dpr), picture.width());
        QPixmap scaled = picture.scaledToWidth(w, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        label->setPixmap(scaled);
    };
    const int width = std::max(120, this->width() - 40);
    fit(m_flow, m_flowPicture, width);
    for (Card &c : m_cards) {
        if (c.expanded)
            fit(c.image, c.picture, width - 20);
    }
}

} // namespace pl
