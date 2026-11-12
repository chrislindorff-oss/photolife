#include "ui/CardImportDialog.h"

#include "app/Application.h"
#include "settings/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMessageBox>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pl {

using organise::CardImporter;

CardImportDialog::CardImportDialog(Application &app, QWidget *parent) : QDialog(parent), m_app(app)
{
    setWindowTitle(tr("Import from Card"));

    m_source = new QComboBox(this);
    m_source->setEditable(false);
    for (const QString &card : CardImporter::cardsMounted())
        m_source->addItem(QDir::toNativeSeparators(card), card);
    auto *chooseSourceButton = new QPushButton(tr("Choose…"), this);
    chooseSourceButton->setAutoDefault(false);
    connect(chooseSourceButton, &QPushButton::clicked, this, &CardImportDialog::chooseSource);
    connect(m_source, &QComboBox::currentIndexChanged, this, &CardImportDialog::survey);
    auto *sourceRow = new QHBoxLayout;
    sourceRow->addWidget(m_source, 1);
    sourceRow->addWidget(chooseSourceButton);

    QString dest = m_app.settings().cardImportFolder();
    if (dest.isEmpty()) {
        // Beside the folders sorted before, else in Pictures.
        const QString last = m_app.settings().organiseFolder();
        dest = !last.isEmpty() && QFileInfo(last).dir().exists()
                   ? QFileInfo(last).absolutePath()
                   : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    }
    m_destination = new QLineEdit(QDir::toNativeSeparators(dest), this);
    auto *chooseDestButton = new QPushButton(tr("Choose…"), this);
    chooseDestButton->setAutoDefault(false);
    connect(chooseDestButton, &QPushButton::clicked, this, &CardImportDialog::chooseDestination);
    connect(m_destination, &QLineEdit::editingFinished, this, &CardImportDialog::survey);
    auto *destRow = new QHBoxLayout;
    destRow->addWidget(m_destination, 1);
    destRow->addWidget(chooseDestButton);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("optional, e.g. Anakie"));
    m_name->setToolTip(tr("Added after the date in each folder's name: “2026-11-13 Anakie”."));
    connect(m_name, &QLineEdit::textChanged, this, &CardImportDialog::showSurvey);

    m_summary = new QLabel(tr("Choose the card, or a folder on it."), this);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    m_again = new QCheckBox(this);
    m_again->hide();
    connect(m_again, &QCheckBox::toggled, this, &CardImportDialog::updateButtons);
    m_progress = new QProgressBar(this);
    m_progress->hide();

    auto *note = new QLabel(tr("Each photo is copied, read back and compared with the original before it counts; "
                               "the copy keeps the photo's date. Nothing on the card is changed or deleted — "
                               "format it in the camera once you're happy."),
                            this);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("muted"));

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CardImportDialog::startImport);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &CardImportDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("From:"), sourceRow);
    form->addRow(tr("Into:"), destRow);
    form->addRow(tr("Folder name:"), m_name);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_summary);
    layout->addWidget(m_again);
    layout->addWidget(m_progress);
    layout->addStretch(1);
    layout->addWidget(note);
    layout->addWidget(m_buttons);
    resize(640, 360);
    updateButtons();
    if (m_source->count() > 0)
        survey();
    else
        m_summary->setText(tr("No camera card found (a drive with a DCIM folder). Put one in and choose it, "
                              "or choose any folder of photos."));
}

CardImportDialog::~CardImportDialog()
{
    if (m_cancel)
        *m_cancel = true;
}

void CardImportDialog::chooseSource()
{
    const QString start = m_source->currentData().toString();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Import Photos From"),
                                                          start.isEmpty() ? QDir::rootPath() : start);
    if (dir.isEmpty())
        return;
    int at = m_source->findData(dir);
    if (at < 0) {
        m_source->addItem(QDir::toNativeSeparators(dir), dir);
        at = m_source->count() - 1;
    }
    if (at == m_source->currentIndex())
        survey();
    else
        m_source->setCurrentIndex(at);
}

void CardImportDialog::chooseDestination()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Import Photos Into"), m_destination->text());
    if (dir.isEmpty())
        return;
    m_destination->setText(QDir::toNativeSeparators(dir));
    survey();
}

void CardImportDialog::survey()
{
    const QString source = m_source->currentData().toString();
    const QString dest = QDir::fromNativeSeparators(m_destination->text().trimmed());
    m_files.clear();
    const int generation = ++m_surveyGeneration;
    if (source.isEmpty() || m_importing) {
        updateButtons();
        return;
    }
    m_surveying = true;
    m_summary->setText(tr("Looking at what's on %1…").arg(QDir::toNativeSeparators(source)));
    m_again->hide();
    updateButtons();
    auto *watcher = new QFutureWatcher<QList<CardImporter::Source>>(this);
    connect(watcher, &QFutureWatcher<QList<CardImporter::Source>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_surveyGeneration)
            return;
        m_surveying = false;
        m_files = watcher->result();
        showSurvey();
    });
    watcher->setFuture(QtConcurrent::run([source, dest] { return CardImporter::survey(source, dest); }));
}

void CardImportDialog::showSurvey()
{
    if (m_surveying || m_importing)
        return;
    if (m_source->currentData().toString().isEmpty()) {
        updateButtons();
        return;
    }
    if (m_files.isEmpty()) {
        m_summary->setText(tr("There are no photos there."));
        m_again->hide();
        updateButtons();
        return;
    }
    QMap<QDate, QPair<int, int>> days;   // day -> (new, before)
    int before = 0;
    for (const CardImporter::Source &s : std::as_const(m_files)) {
        auto &d = days[s.taken];
        ++(s.alreadyImported ? d.second : d.first);
        before += s.alreadyImported ? 1 : 0;
    }
    const QLocale locale;
    QStringList lines;
    for (auto it = days.cbegin(); it != days.cend(); ++it) {
        const QString folder = CardImporter::folderName(it.key(), m_name->text());
        QString line = tr("<b>%1</b> — %n new", nullptr, it.value().first).arg(folder.toHtmlEscaped());
        if (it.value().second > 0)
            line += tr(", %n imported before", nullptr, it.value().second);
        lines << line;
    }
    QString text = tr("%n file(s) on the card, taken on %1 day(s):", nullptr, int(m_files.size())).arg(days.size());
    const int shown = 8;
    text += QStringLiteral("<br>") + lines.mid(0, shown).join(QStringLiteral("<br>"));
    if (lines.size() > shown)
        text += QStringLiteral("<br>") + tr("…and %n more day(s)", nullptr, int(lines.size() - shown));
    m_summary->setText(text);
    m_again->setText(tr("Also copy the %n file(s) imported before", nullptr, before));
    m_again->setVisible(before > 0);
    updateButtons();
}

void CardImportDialog::updateButtons()
{
    int toCopy = 0;
    for (const CardImporter::Source &s : std::as_const(m_files))
        toCopy += !s.alreadyImported || m_again->isChecked() ? 1 : 0;
    QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok);
    ok->setText(toCopy > 0 ? tr("Import %n File(s)", nullptr, toCopy) : tr("Import"));
    ok->setEnabled(!m_surveying && !m_importing && toCopy > 0
                   && !m_destination->text().trimmed().isEmpty());
}

void CardImportDialog::startImport()
{
    const QString dest = QDir::fromNativeSeparators(m_destination->text().trimmed());
    if (dest.isEmpty() || m_files.isEmpty())
        return;
    // A copy onto the card itself would be a write to it.
    const QString source = QDir(m_source->currentData().toString()).absolutePath();
    if (QDir(dest).absolutePath().startsWith(source + QLatin1Char('/')) || QDir(dest).absolutePath() == source) {
        QMessageBox::warning(this, windowTitle(), tr("Choose a folder on this computer, not on the card."));
        return;
    }
    m_app.settings().setCardImportFolder(dest);
    m_importing = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_progress->setRange(0, 0);
    m_progress->setFormat(tr("%v of %m copied"));
    m_progress->show();
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Stop"));
    updateButtons();
    const QList<CardImporter::Source> files = m_files;
    const QString name = m_name->text();
    const bool again = m_again->isChecked();
    auto cancel = m_cancel;
    auto *watcher = new QFutureWatcher<CardImporter::Result>(this);
    connect(watcher, &QFutureWatcher<CardImporter::Result>::progressRangeChanged, m_progress, &QProgressBar::setRange);
    connect(watcher, &QFutureWatcher<CardImporter::Result>::progressValueChanged, m_progress, &QProgressBar::setValue);
    connect(watcher, &QFutureWatcher<CardImporter::Result>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const CardImporter::Result r = watcher->result();
        m_importing = false;
        m_progress->hide();
        m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Close"));
        m_folders = r.folders;
        QString text = tr("Copied %n file(s)", nullptr, r.copied);
        if (r.skipped > 0)
            text += tr(", skipped %n already imported", nullptr, r.skipped);
        text += QLatin1Char('.');
        if (r.cancelled)
            text += QLatin1Char(' ') + tr("Stopped before the end: importing again carries on where it stopped.");
        if (!r.failures.isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("%n file(s) couldn't be copied, and are still only on the card:\n\n%1",
                                    nullptr, int(r.failures.size()))
                                     .arg(r.failures.mid(0, 12).join(QLatin1Char('\n'))));
        }
        if (r.copied > 0 && !r.cancelled && r.failures.isEmpty()) {
            QDialog::accept();
            return;
        }
        m_summary->setText(text);
        if (r.copied > 0) {
            // Something came off: Organise can still open it.
            m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Open in Organise"));
            m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            disconnect(m_buttons, &QDialogButtonBox::accepted, this, nullptr);
            connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        }
    });
    watcher->setFuture(QtConcurrent::run([files, dest, name, again, cancel](QPromise<CardImporter::Result> &promise) {
        promise.setProgressRange(0, 0);
        promise.addResult(CardImporter::import(files, dest, name, again,
                                               [&promise](int done, int total) {
                                                   promise.setProgressRange(0, total);
                                                   promise.setProgressValue(done);
                                               },
                                               cancel.get()));
    }));
}

void CardImportDialog::reject()
{
    if (m_importing) {
        if (QMessageBox::question(this, windowTitle(),
                                  tr("Stop the import? What has been copied so far stays, and importing again "
                                     "carries on from there."))
            == QMessageBox::Yes && m_cancel)
            *m_cancel = true;
        return;
    }
    QDialog::reject();
}

} // namespace pl
