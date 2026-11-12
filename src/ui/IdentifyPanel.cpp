#include "ui/IdentifyPanel.h"

#include "app/Application.h"
#include "net/PhotoCache.h"
#include "db/Database.h"
#include "net/INatClient.h"
#include "settings/Settings.h"
#include "taxonomy/TaxonomyStore.h"
#include "ui/InatTokenGetter.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QScreen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace pl {
namespace {

constexpr int kMaxRows = 8;

QString nameOf(const taxonomy::Taxon &t)
{
    const QString rank = t.rank == QLatin1String("species") || t.rank.isEmpty()
                             ? QString()
                             : QStringLiteral(" (%1)").arg(t.rank);
    return t.commonName.isEmpty() ? t.name + rank
                                  : QStringLiteral("%1 · %2%3").arg(t.commonName, t.name, rank);
}

QString percent(double score)
{
    return score < 1 ? QStringLiteral("<1%") : QStringLiteral("%1%").arg(qRound(score));
}

// A quick look at what a suggestion looks like: iNaturalist's photo of the
// taxon with its names and credit. A pop-up -- a click anywhere, or Esc,
// closes it.
class SuggestionPreview : public QFrame
{
public:
    SuggestionPreview(net::PhotoCache &cache, const taxonomy::Taxon &taxon, QWidget *parent)
        : QFrame(parent, Qt::Popup), m_cache(cache), m_url(taxon.photoUrl)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        const QColor muted = themeColors(currentThemeVariant()).mutedText;
        m_photo = new QLabel(this);
        m_photo->setAlignment(Qt::AlignCenter);
        m_photo->setFixedSize(net::PhotoCache::kMaxEdge, net::PhotoCache::kMaxEdge * 3 / 4);
        auto *names = new QLabel(this);
        names->setWordWrap(true);
        names->setAlignment(Qt::AlignCenter);
        names->setText(taxon.commonName.isEmpty()
                           ? QStringLiteral("<i>%1</i>").arg(taxon.name.toHtmlEscaped())
                           : QStringLiteral("<b>%1</b><br><i>%2</i>")
                                 .arg(taxon.commonName.toHtmlEscaped(), taxon.name.toHtmlEscaped()));
        auto *credit = new QLabel(taxon.photoAttribution, this);
        credit->setWordWrap(true);
        credit->setAlignment(Qt::AlignCenter);
        QFont small = credit->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        credit->setFont(small);
        QPalette pal = credit->palette();
        pal.setColor(QPalette::WindowText, muted);
        credit->setPalette(pal);
        credit->setVisible(!taxon.photoAttribution.isEmpty());
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->addWidget(m_photo);
        layout->addWidget(names);
        layout->addWidget(credit);
        setFixedWidth(net::PhotoCache::kMaxEdge + 16);

        connect(&m_cache, &net::PhotoCache::ready, this, [this](const QString &url) {
            if (url == m_url)
                showPhoto();
        });
        showPhoto(true);
    }

protected:
    void mousePressEvent(QMouseEvent *) override { close(); }

private:
    void showPhoto(bool ask = false)
    {
        if (m_url.isEmpty()) {
            m_photo->setText(tr("iNaturalist has no photo of this taxon."));
            return;
        }
        const QPixmap pm = ask ? m_cache.photo(m_url, net::PhotoCache::Urgency::Now) : m_cache.photo(m_url);
        if (pm.isNull()) {
            m_photo->setText(m_cache.hasFailed(m_url) ? tr("Couldn't download the photo from iNaturalist.")
                                                      : tr("Loading from iNaturalist…"));
            return;
        }
        const qreal dpr = devicePixelRatioF();
        QPixmap shown = pm.scaled(m_photo->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        shown.setDevicePixelRatio(dpr);
        m_photo->setPixmap(shown);
    }

    net::PhotoCache &m_cache;
    QString m_url;
    QLabel *m_photo = nullptr;
};

} // namespace

IdentifyPanel::IdentifyPanel(Application &app, QWidget *parent)
    : QWidget(parent), m_app(app)
{
    setToolTip(tr("The photo is sent to iNaturalist for its computer vision to score; nothing is "
                  "posted."));
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_list = new QListWidget(this);
    m_list->setMinimumHeight(120);
    m_treesOnly = new QCheckBox(tr("Only taxa in my trees"), this);
    m_treesOnly->setChecked(m_app.settings().inatIdentifyTreesOnly());
    m_commonNames = new QCheckBox(tr("Name with common names"), this);
    m_commonNames->setToolTip(tr("Name photos with the suggestion's common name (Flame Robin) rather "
                                 "than its scientific name (Petroica phoenicea). A taxon without a "
                                 "common name gets its scientific name either way."));
    m_commonNames->setChecked(m_app.settings().inatIdentifyCommonName());
    m_commonNames->hide();   // until setNamesPhotos()
    connect(m_commonNames, &QCheckBox::toggled, this,
            [this](bool on) { m_app.settings().setInatIdentifyCommonName(on); });
    m_getToken = new QPushButton(tr("Get Token…"), this);
    m_getToken->setToolTip(tr("Opens iNaturalist's token page in your browser. Sign in there if "
                              "asked, then press Ctrl+A and Ctrl+C."));
    m_getToken->hide();

    connect(m_treesOnly, &QCheckBox::toggled, this, [this](bool on) {
        m_app.settings().setInatIdentifyTreesOnly(on);
        fill();
    });
    connect(m_getToken, &QPushButton::clicked, this, [this] { InatTokenGetter::start(m_app); });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item->flags() & Qt::ItemIsEnabled)
            emit chosen(item->data(Qt::UserRole).value<taxonomy::Taxon>());
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item->flags() & Qt::ItemIsEnabled)
            emit chosen(item->data(Qt::UserRole).value<taxonomy::Taxon>());
    });
    // Right-click (or Space) for a look at what a suggestion is.
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (QListWidgetItem *item = m_list->itemAt(pos))
            suggestionMenu(item, m_list->viewport()->mapToGlobal(pos));
    });
    m_list->installEventFilter(this);
    connect(&m_app, &Application::inatApiTokenChanged, this, [this] {
        if (m_waitingForToken && m_request)
            ask();
    });
    connect(&m_app.photoIdentifier(), &inat::PhotoIdentifier::finished, this,
            [this](const QString &key, const std::optional<QRectF> &crop, const inat::Identification &result) {
                if (m_request && m_request->key == key && m_request->crop == crop)
                    showResult(result);
            });

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_treesOnly);
    bottom->addWidget(m_commonNames);
    bottom->addStretch();
    bottom->addWidget(m_getToken);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_status);
    layout->addWidget(m_list, 1);
    layout->addLayout(bottom);
    clear();
}

void IdentifyPanel::identify(const inat::IdentifyRequest &request)
{
    // Another panel may have changed it meanwhile.
    const QSignalBlocker quiet(m_commonNames);
    m_commonNames->setChecked(m_app.settings().inatIdentifyCommonName());
    m_request = request;
    m_result.reset();
    ask();
}

void IdentifyPanel::clear()
{
    m_request.reset();
    m_result.reset();
    m_waitingForToken = false;
    m_list->clear();
    m_getToken->hide();
    m_status->setText(tr("Press <b>I</b> (or Identify) to ask iNaturalist's computer vision."));
}

void IdentifyPanel::setNamesPhotos(bool namesPhotos)
{
    m_commonNames->setVisible(namesPhotos);
}

QString IdentifyPanel::photoName(Application &app, const taxonomy::Taxon &taxon)
{
    return app.settings().inatIdentifyCommonName() && !taxon.commonName.isEmpty() ? taxon.commonName
                                                                                  : taxon.name;
}

bool IdentifyPanel::hasSuggestions() const
{
    return m_list->count() > 0;
}

void IdentifyPanel::focusList()
{
    m_list->setFocus();
    if (m_list->currentRow() < 0 && m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void IdentifyPanel::ask()
{
    m_list->clear();
    const QString token = m_app.settings().inatApiToken();
    if (!InatTokenGetter::isUsable(token)) {
        m_waitingForToken = true;
        showMessage(token.isEmpty() ? tr("Identifying needs your iNaturalist token. Get one — it "
                                         "lasts a day.")
                                    : tr("Your iNaturalist token has expired (they last a day). Get "
                                         "a fresh one."),
                    true);
        return;
    }
    m_waitingForToken = false;
    m_getToken->hide();
    // As elsewhere, the shared client is given the token when it's needed.
    m_app.inat().setAccessToken(token);
    m_status->setText(m_request->crop ? tr("Asking iNaturalist about the selected part…")
                                      : tr("Asking iNaturalist…"));
    m_app.photoIdentifier().identify(*m_request);
}

void IdentifyPanel::showResult(const inat::Identification &result)
{
    if (!result.ok()) {
        if (result.status == 401) {
            m_waitingForToken = true;
            showMessage(tr("iNaturalist didn't accept the token — it has probably expired. Get a "
                           "fresh one."),
                        true);
        } else if (result.status == 403) {
            showMessage(tr("iNaturalist refused to identify it (it may be limiting requests). Try "
                           "again later."));
        } else if (result.status == 429) {
            showMessage(tr("iNaturalist is busy. Try again in a minute."));
        } else {
            showMessage(tr("Couldn't identify it: %1").arg(result.error));
        }
        return;
    }
    m_result = result;
    fill();
    loadHigherRanks();
}

QString IdentifyPanel::ancestryOf(const taxonomy::Taxon &taxon) const
{
    return taxon.ancestry.isEmpty() ? m_ancestry.value(taxon.inatId) : taxon.ancestry;
}

void IdentifyPanel::loadHigherRanks()
{
    const int generation = ++m_higherGeneration;
    m_higherPending = 0;
    if (!m_result)
        return;
    // A suggestion that came without its chain of ancestors: ask for it first.
    QList<qint64> bare;
    auto consider = [&](const taxonomy::Taxon &t) {
        if (t.inatId > 0 && ancestryOf(t).isEmpty() && t.rank != QLatin1String("kingdom") && !bare.contains(t.inatId))
            bare << t.inatId;
    };
    if (m_result->result.commonAncestor)
        consider(m_result->result.commonAncestor->taxon);
    for (const net::VisionSuggestion &s : m_result->result.suggestions.mid(0, kMaxRows * 2))
        consider(s.taxon);
    if (bare.isEmpty()) {
        loadAncestors(generation);
        return;
    }
    ++m_higherPending;
    QPointer<IdentifyPanel> self(this);
    m_app.inat().fetchTaxa(bare.mid(0, 30), [self, generation](net::Outcome<QList<taxonomy::Taxon>> out) {
        if (!self || generation != self->m_higherGeneration)
            return;
        --self->m_higherPending;
        if (out.ok()) {
            for (const taxonomy::Taxon &t : std::as_const(out.value))
                self->m_ancestry.insert(t.inatId, t.ancestry);
        }
        self->loadAncestors(generation);
    });
}

void IdentifyPanel::loadAncestors(int generation)
{
    if (!m_result)
        return;
    QList<qint64> wanted;
    auto consider = [&](const taxonomy::Taxon &t) {
        for (const QString &part : ancestryOf(t).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            const qint64 id = part.toLongLong();
            if (id > 0 && !m_higher.contains(id) && !wanted.contains(id))
                wanted << id;
        }
    };
    if (m_result->result.commonAncestor)
        consider(m_result->result.commonAncestor->taxon);
    for (const net::VisionSuggestion &s : m_result->result.suggestions.mid(0, kMaxRows * 2))
        consider(s.taxon);
    if (wanted.isEmpty())
        return;
    // Those in the catalogue (anything in a tree) need no asking.
    const taxonomy::TaxonomyStore store(m_app.database().connectionName());
    const QHash<qint64, taxonomy::Taxon> local = store.taxaByInatIds(wanted);
    QList<qint64> toFetch;
    for (qint64 id : std::as_const(wanted)) {
        if (local.contains(id))
            m_higher.insert(id, local.value(id));
        else
            toFetch << id;
    }
    QPointer<IdentifyPanel> self(this);
    for (int from = 0; from < toFetch.size(); from += 30) {
        ++m_higherPending;
        m_app.inat().fetchTaxa(toFetch.mid(from, 30), [self, generation](net::Outcome<QList<taxonomy::Taxon>> out) {
            if (!self || generation != self->m_higherGeneration)
                return;
            --self->m_higherPending;
            if (out.ok()) {
                for (const taxonomy::Taxon &t : std::as_const(out.value))
                    self->m_higher.insert(t.inatId, t);
            }
        });
    }
}

QList<taxonomy::Taxon> IdentifyPanel::higherRanks(const taxonomy::Taxon &taxon) const
{
    QList<taxonomy::Taxon> out;
    const QStringList chain = ancestryOf(taxon).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
        const auto known = m_higher.constFind(it->toLongLong());
        // Not "Life" itself: that names nothing.
        if (known != m_higher.cend() && known->rank != QLatin1String("stateofmatter") && !known->name.isEmpty())
            out << *known;
    }
    return out;
}

void IdentifyPanel::suggestionMenu(QListWidgetItem *item, const QPoint &at)
{
    const QVariant data = item->data(Qt::UserRole);
    if (!data.canConvert<taxonomy::Taxon>())
        return;
    const taxonomy::Taxon taxon = data.value<taxonomy::Taxon>();
    QMenu menu(this);
    menu.addAction(tr("Show a Photo of It"), this, [this, item, at] { previewSuggestion(item, at); });
    menu.addSeparator();
    QAction *heading = menu.addAction(tr("Use a higher rank instead:"));
    heading->setEnabled(false);
    const QList<taxonomy::Taxon> higher = higherRanks(taxon);
    for (const taxonomy::Taxon &t : higher) {
        QString label = nameOf(t);   // "Jelly Babies · Leotia (genus)"
        menu.addAction(label.replace(QLatin1Char('&'), QStringLiteral("&&")), this, [this, t] { emit chosen(t); });
    }
    if (higher.isEmpty() || m_higherPending > 0) {
        QAction *note = menu.addAction(m_higherPending > 0 ? tr("Asking iNaturalist for the ranks above… try again in a moment")
                                                           : tr("The ranks above it couldn't be found"));
        note->setEnabled(false);
    }
    menu.exec(at);
}

void IdentifyPanel::fill()
{
    if (!m_result)
        return;
    m_list->clear();
    const QColor muted = themeColors(currentThemeVariant()).mutedText;
    const bool treesOnly = m_treesOnly->isChecked();
    auto add = [&](const net::VisionSuggestion &s, const QString &label) {
        const bool inTree = m_result->inTrees.contains(s.taxon.inatId);
        if (treesOnly && !inTree)
            return;
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, QVariant::fromValue(s.taxon));
        if (!inTree)
            item->setForeground(muted);
    };
    if (const auto &a = m_result->result.commonAncestor) {
        add(*a, tr("Probably within %1 — %2").arg(nameOf(a->taxon), percent(a->score)));
        if (m_list->count() > 0) {
            QFont italic = m_list->item(0)->font();
            italic.setItalic(true);
            m_list->item(0)->setFont(italic);
        }
    }
    int shown = 0;
    for (const net::VisionSuggestion &s : m_result->result.suggestions) {
        if (shown >= kMaxRows)
            break;
        QStringList notes{percent(s.score)};
        if (s.seenNearby)
            notes << tr("seen nearby");
        if (m_result->inTrees.contains(s.taxon.inatId))
            notes << tr("in your trees");
        const int before = m_list->count();
        add(s, QStringLiteral("%1 — %2").arg(nameOf(s.taxon), notes.join(QStringLiteral(" · "))));
        shown += m_list->count() - before;
    }
    if (m_list->count() == 0) {
        m_status->setText(treesOnly && !m_result->result.suggestions.isEmpty()
                              ? tr("None of iNaturalist's suggestions are in your trees.")
                              : tr("iNaturalist had no suggestions for this photo."));
        return;
    }
    m_status->setText(m_request && m_request->crop
                          ? tr("Suggestions for the selected part — click to use; right-click to see one, or to use its genus, family or higher:")
                          : tr("iNaturalist's suggestions — click to use; right-click to see one, or to use its genus, family or higher:"));
}

bool IdentifyPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space) {
        if (QListWidgetItem *item = m_list->currentItem()) {
            const QRect at = m_list->visualItemRect(item);
            previewSuggestion(item, m_list->viewport()->mapToGlobal(at.bottomLeft() + QPoint(24, 0)));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void IdentifyPanel::previewSuggestion(QListWidgetItem *item, const QPoint &at)
{
    const QVariant data = item->data(Qt::UserRole);
    if (!data.canConvert<taxonomy::Taxon>())
        return;
    auto *preview = new SuggestionPreview(m_app.photoCache(), data.value<taxonomy::Taxon>(), this);
    preview->adjustSize();
    // Beside the pointer, kept on the screen.
    QRect r(at + QPoint(12, 12), preview->size());
    if (const QScreen *screen = QGuiApplication::screenAt(at)) {
        const QRect avail = screen->availableGeometry();
        if (r.right() > avail.right())
            r.moveRight(at.x() - 12);
        if (r.bottom() > avail.bottom())
            r.moveBottom(avail.bottom());
        if (r.left() < avail.left())
            r.moveLeft(avail.left());
        if (r.top() < avail.top())
            r.moveTop(avail.top());
    }
    preview->move(r.topLeft());
    preview->show();
}

void IdentifyPanel::showMessage(const QString &text, bool offerToken)
{
    m_list->clear();
    m_status->setText(text);
    m_getToken->setVisible(offerToken);
}

} // namespace pl
