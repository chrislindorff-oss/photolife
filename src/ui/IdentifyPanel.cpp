#include "ui/IdentifyPanel.h"

#include "app/Application.h"
#include "settings/Settings.h"
#include "ui/InatTokenGetter.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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
                          ? tr("iNaturalist's suggestions for the selected part — click one to use it:")
                          : tr("iNaturalist's suggestions — click one to use it:"));
}

void IdentifyPanel::showMessage(const QString &text, bool offerToken)
{
    m_list->clear();
    m_status->setText(text);
    m_getToken->setVisible(offerToken);
}

} // namespace pl
