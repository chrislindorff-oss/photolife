#include "ui/ReviewPane.h"

#include "db/Database.h"
#include "model/ReviewQueueModel.h"
#include "thumb/ThumbnailCache.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace pl {
namespace {
using model::ReviewQueueModel;
constexpr int kBigThumb = 320;
} // namespace

ReviewPane::ReviewPane(pl::Database &db, pl::thumb::ThumbnailCache &thumbs, QWidget *parent)
    : QWidget(parent)
    , m_db(db)
    , m_thumbs(thumbs)
    , m_queue(new ReviewQueueModel(db, thumbs, this))
    , m_reviewer(db.connectionName())
    , m_finder(db.connectionName())
{
    m_queueView = new QListView(this);
    m_queueView->setModel(m_queue);
    m_queueView->setIconSize(QSize(64, 64));
    m_queueView->setUniformItemSizes(true);
    m_queueView->setMinimumWidth(280);
    connect(m_queueView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { showCurrent(); });

    m_thumb = new QLabel(this);
    m_thumb->setMinimumSize(kBigThumb, kBigThumb);
    m_thumb->setAlignment(Qt::AlignCenter);
    m_thumb->setFrameShape(QFrame::StyledPanel);

    m_info = new QLabel(this);
    m_info->setTextFormat(Qt::RichText);
    m_info->setWordWrap(true);

    m_guess = new QLabel(this);
    m_guess->setWordWrap(true);

    m_candidates = new QListWidget(this);
    connect(m_candidates, &QListWidget::itemDoubleClicked, this, [this] { confirm(); });

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search for a taxon…"));
    connect(m_search, &QLineEdit::textEdited, this, &ReviewPane::runSearch);

    auto *confirmBtn = new QPushButton(tr("&Confirm"), this);
    auto *rejectBtn = new QPushButton(tr("&Reject"), this);
    auto *genusBtn = new QPushButton(tr("&Genus only"), this);
    auto *notTaxonBtn = new QPushButton(tr("&Not a taxon"), this);
    auto *skipBtn = new QPushButton(tr("&Skip"), this);
    confirmBtn->setDefault(true);
    connect(confirmBtn, &QPushButton::clicked, this, &ReviewPane::confirm);
    connect(rejectBtn, &QPushButton::clicked, this, &ReviewPane::reject);
    connect(genusBtn, &QPushButton::clicked, this, &ReviewPane::genusOnly);
    connect(notTaxonBtn, &QPushButton::clicked, this, &ReviewPane::notATaxon);
    connect(skipBtn, &QPushButton::clicked, this, &ReviewPane::skip);

    auto *actions = new QHBoxLayout;
    actions->addWidget(confirmBtn);
    actions->addWidget(rejectBtn);
    actions->addWidget(genusBtn);
    actions->addWidget(notTaxonBtn);
    actions->addWidget(skipBtn);
    actions->addStretch(1);

    m_bulkLabel = new QLabel(this);
    m_bulkButton = new QPushButton(tr("Apply to folder"), this);
    connect(m_bulkButton, &QPushButton::clicked, this, &ReviewPane::applyToFolder);
    auto *bulk = new QHBoxLayout;
    bulk->addWidget(m_bulkLabel, 1);
    bulk->addWidget(m_bulkButton);

    auto *detail = new QWidget(this);
    auto *detailLayout = new QVBoxLayout(detail);
    auto *top = new QHBoxLayout;
    top->addWidget(m_thumb);
    top->addWidget(m_info, 1);
    detailLayout->addLayout(top);
    detailLayout->addWidget(m_guess);
    detailLayout->addWidget(new QLabel(tr("Choose the taxon:"), this));
    detailLayout->addWidget(m_candidates, 1);
    detailLayout->addWidget(m_search);
    detailLayout->addLayout(actions);
    detailLayout->addLayout(bulk);

    m_empty = new QLabel(tr("The review queue is empty. Run Match Library, or everything is decided."),
                         this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);

    auto *rightStack = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightStack);
    rightLayout->addWidget(m_empty);
    rightLayout->addWidget(detail, 1);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_queueView);
    split->addWidget(rightStack);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(split);

    reload();
}

void ReviewPane::reload()
{
    m_queue->reload();
    if (m_queue->rowCount() > 0)
        m_queueView->setCurrentIndex(m_queue->index(0));
    showCurrent();
}

int ReviewPane::queueCount() const
{
    return m_queue->queueCount();
}

qint64 ReviewPane::currentCaptureId() const
{
    const QModelIndex idx = m_queueView->currentIndex();
    return idx.isValid() ? idx.data(ReviewQueueModel::CaptureIdRole).toLongLong() : 0;
}

int ReviewPane::currentFolderId() const
{
    const QModelIndex idx = m_queueView->currentIndex();
    return idx.isValid() ? idx.data(ReviewQueueModel::FolderIdRole).toInt() : 0;
}

qint64 ReviewPane::chosenTaxonInatId() const
{
    const QListWidgetItem *item = m_candidates->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void ReviewPane::showCurrent()
{
    const QModelIndex idx = m_queueView->currentIndex();
    const bool have = idx.isValid();
    m_empty->setVisible(!have && m_queue->rowCount() == 0);

    if (!have) {
        m_info->clear();
        m_guess->clear();
        m_candidates->clear();
        m_thumb->clear();
        m_bulkLabel->clear();
        m_bulkButton->setEnabled(false);
        return;
    }

    const QString folder = idx.data(ReviewQueueModel::FolderPathRole).toString();
    const QString nameText = idx.data(ReviewQueueModel::NameTextRole).toString();
    const QString baseName = idx.data(ReviewQueueModel::BaseNameRole).toString();
    m_info->setText(QStringLiteral("<b>%1</b><br><span style='color:gray'>%2</span>")
                        .arg((nameText.isEmpty() ? baseName : nameText).toHtmlEscaped(),
                             folder.toHtmlEscaped()));

    const QString guessName = idx.data(ReviewQueueModel::GuessNameRole).toString();
    const double conf = idx.data(ReviewQueueModel::ConfidenceRole).toDouble();
    const QString note = idx.data(ReviewQueueModel::NoteRole).toString();
    if (!guessName.isEmpty())
        m_guess->setText(tr("Engine's guess: %1  (%2%)").arg(guessName).arg(int(conf * 100)));
    else
        m_guess->setText(note.isEmpty() ? tr("No guess.") : note);

    const QString path = idx.data(ReviewQueueModel::PreviewPathRole).toString();
    const QPixmap pm = path.isEmpty()
                           ? QPixmap()
                           : m_thumbs.thumbnail({}, path, thumb::ThumbnailCache::kPreviewPx);
    m_thumb->setPixmap(pm.isNull() ? QPixmap()
                                   : pm.scaled(m_thumb->size(), Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    if (pm.isNull())
        m_thumb->setText(tr("(preview loading)"));

    const auto guessInat = idx.data(ReviewQueueModel::GuessInatIdRole).toLongLong();
    populateCandidates(m_finder.forCapture(currentCaptureId()), guessInat);

    const int inFolder = m_queue->pendingInFolder(currentFolderId());
    m_bulkLabel->setText(inFolder > 1
                             ? tr("%1 more pending in this folder").arg(inFolder - 1)
                             : QString());
    m_bulkButton->setEnabled(inFolder > 1);
    m_search->clear();
}

void ReviewPane::populateCandidates(const QList<pl::match::TaxonCandidate> &candidates,
                                    qint64 preselectInatId)
{
    m_candidates->clear();
    for (const auto &c : candidates) {
        QString label = c.name;
        if (!c.commonName.isEmpty() && c.commonName != c.name)
            label += QStringLiteral("  ·  %1").arg(c.commonName);
        label += QStringLiteral("   [%1").arg(c.rank);
        if (!c.matchedVia.isEmpty())
            label += QStringLiteral(", %1").arg(c.matchedVia);
        if (c.score > 0.0)
            label += QStringLiteral(" %1%").arg(int(c.score * 100));
        label += QLatin1Char(']');

        auto *item = new QListWidgetItem(label, m_candidates);
        item->setData(Qt::UserRole, c.inatId);
        if (c.inatId == preselectInatId)
            m_candidates->setCurrentItem(item);
    }
    if (!m_candidates->currentItem() && m_candidates->count() > 0)
        m_candidates->setCurrentRow(0);
}

void ReviewPane::runSearch(const QString &text)
{
    if (text.trimmed().size() < 3) {
        showCurrent();
        return;
    }
    populateCandidates(m_finder.search(text), 0);
}

void ReviewPane::afterDecision(qint64 captureId)
{
    const QModelIndex idx = m_queueView->currentIndex();
    const int nextRow = idx.isValid() ? idx.row() : 0;
    m_queue->dropCapture(captureId);
    if (m_queue->rowCount() > 0) {
        const int row = qMin(nextRow, m_queue->rowCount() - 1);
        m_queueView->setCurrentIndex(m_queue->index(row));
    }
    showCurrent();
    emit queueChanged();
}

void ReviewPane::confirm()
{
    const qint64 cid = currentCaptureId();
    if (cid <= 0)
        return;
    if (m_reviewer.confirm(cid, chosenTaxonInatId()))
        afterDecision(cid);
}

void ReviewPane::reject()
{
    const qint64 cid = currentCaptureId();
    if (cid > 0 && m_reviewer.reject(cid))
        afterDecision(cid);
}

void ReviewPane::genusOnly()
{
    const qint64 cid = currentCaptureId();
    if (cid <= 0)
        return;
    const qint64 genus = m_finder.folderGenusInatId(cid);
    if (genus > 0 && m_reviewer.setGenusOnly(cid, genus))
        afterDecision(cid);
}

void ReviewPane::notATaxon()
{
    const qint64 cid = currentCaptureId();
    if (cid > 0 && m_reviewer.markNotATaxon(cid, true))
        afterDecision(cid);
}

void ReviewPane::skip()
{
    if (m_queue->rowCount() < 2)
        return;
    const int row = m_queueView->currentIndex().row();
    m_queueView->setCurrentIndex(m_queue->index((row + 1) % m_queue->rowCount()));
}

void ReviewPane::applyToFolder()
{
    const int folderId = currentFolderId();
    const qint64 taxon = chosenTaxonInatId();
    if (folderId <= 0 || taxon <= 0)
        return;

    const int changed = m_reviewer.applyTaxonToFolder(folderId, taxon, false, true);
    if (changed <= 0)
        return;

    m_queue->reload();
    if (m_queue->rowCount() > 0)
        m_queueView->setCurrentIndex(m_queue->index(0));
    showCurrent();
    emit queueChanged();
}

} // namespace pl
