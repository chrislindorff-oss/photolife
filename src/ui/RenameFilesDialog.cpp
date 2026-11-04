#include "ui/RenameFilesDialog.h"

#include "settings/Settings.h"
#include "ui/FileNamingDialog.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pl {

using scan::LibraryRenamer;

namespace {

// "Petroica phoenicea", "Caladenia": named the scientific way.
bool looksScientific(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z][a-z]+( (subsp\\.|var\\.|[a-z][a-z-]*)){0,3}$"));
    return re.match(name).hasMatch();
}

} // namespace

RenameFilesDialog::RenameFilesDialog(const QString &connectionName, const QList<qint64> &captureIds,
                                     Settings &settings, QWidget *parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle(tr("Rename Files on Drive"));
    m_photos = LibraryRenamer::load(connectionName, captureIds);
    m_pattern = settings.libraryRenamePattern();
    if (m_pattern.isEmpty())
        m_pattern = FileNamingDialog::defaultPattern();

    const ThemeColors &colors = themeColors(currentThemeVariant());
    auto *warning = new QLabel(
        tr("<b>This renames the actual files on your drive.</b> A photo's RAW, JPEG and editing sidecars "
           "(.xmp…) are renamed together, and nothing is ever overwritten: a name already taken gets the "
           "next “(2)”. Programs that remember the old names, such as a Lightroom catalogue, "
           "won't follow."),
        this);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(colors.warning.name()));

    // Whether the photos share one species.
    QSet<QString> species;
    for (const auto &p : std::as_const(m_photos))
        species.insert(p.scientificName);
    const bool oneSpecies = species.size() == 1 && !species.begin()->isEmpty();
    const bool anySpecies = !species.isEmpty() && !(species.size() == 1 && species.begin()->isEmpty());

    m_byName = new QRadioButton(tr("Change the name part to:"), this);
    m_byName->setToolTip(tr("Only the name at the start of each file name changes; the place, date and "
                            "number after it are kept."));
    using Form = LibraryRenamer::NameForm;
    m_nameForm = new QComboBox(this);
    m_nameForm->addItem(tr("Common name"), int(Form::Common));
    m_nameForm->addItem(tr("Scientific name"), int(Form::Scientific));
    m_nameForm->addItem(tr("Common name (Scientific name)"), int(Form::CommonScientific));
    m_nameForm->addItem(tr("Scientific name (Common name)"), int(Form::ScientificCommon));
    m_nameForm->addItem(tr("As typed"), int(Form::Typed));
    m_nameForm->setToolTip(tr("Name each photo by its identified species, in this form, or by the name "
                              "typed. A species without a common name goes by its scientific one."));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("Common or scientific name"));
    m_name->setClearButtonEnabled(true);
    if (anySpecies) {
        m_nameForm->setCurrentIndex(0);
        nameFormChosen();
    } else {
        m_nameForm->setCurrentIndex(m_nameForm->count() - 1);
        m_nameForm->setEnabled(false);
        m_nameForm->setToolTip(tr("None of these photos has an identified species, so type the name."));
        if (!m_photos.isEmpty())
            m_name->setText(m_photos.first().nameText);
    }
    m_byParts = new QRadioButton(tr("Build the whole name from parts:"), this);
    m_byParts->setToolTip(tr("As Export Reference Tree names its files: scientific name, date, place, "
                             "sequence number…"));
    m_parts = new QLabel(this);
    m_parts->setWordWrap(true);
    m_chooseParts = new QPushButton(tr("Choose Parts…"), this);
    m_chooseParts->setAutoDefault(false);
    m_byName->setChecked(true);

    m_folder = new QCheckBox(this);
    m_folderName = new QLineEdit(this);
    m_folderName->setClearButtonEnabled(true);
    m_folderNote = new QLabel(this);
    m_folderNote->setWordWrap(true);
    m_folderNote->setObjectName(QStringLiteral("muted"));

    m_preview = new QTreeWidget(this);
    m_preview->setHeaderLabels({tr("Now"), tr("Will be")});
    m_preview->setRootIsDecorated(false);
    m_preview->setUniformRowHeights(true);
    m_preview->setTextElideMode(Qt::ElideMiddle);   // the place, date and number stay in sight
    m_preview->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_summary = new QLabel(this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_ok = buttons->addButton(tr("Rename"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *how = new QGridLayout;
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(m_nameForm);
    nameRow->addWidget(m_name, 1);
    how->addWidget(m_byName, 0, 0);
    how->addLayout(nameRow, 0, 1, 1, 2);
    how->addWidget(m_byParts, 1, 0);
    how->addWidget(m_parts, 1, 1);
    how->addWidget(m_chooseParts, 1, 2);
    how->addWidget(m_folder, 2, 0);
    how->addWidget(m_folderName, 2, 1, 1, 2);
    how->addWidget(m_folderNote, 3, 1, 1, 2);
    how->setColumnStretch(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(warning);
    layout->addSpacing(6);
    layout->addLayout(how);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_summary);
    layout->addWidget(buttons);
    resize(900, 580);

    setUpFolder(connectionName, settings.watchedRoots());

    connect(m_byName, &QRadioButton::toggled, this, &RenameFilesDialog::update);
    connect(m_nameForm, &QComboBox::activated, this, [this, oneSpecies] {
        m_byName->setChecked(true);
        nameFormChosen();
        // The folder too, in the form picked, unless it's been changed by hand.
        const QString name = LibraryRenamer::speciesName(m_photos.first().scientificName,
                                                         m_photos.first().commonName, nameForm());
        if (oneSpecies && !m_folderEdited && m_folderId > 0 && !name.isEmpty())
            m_folderName->setText(LibraryRenamer::cleanName(name));
        update();
    });
    connect(m_name, &QLineEdit::textEdited, this, [this, oneSpecies](const QString &text) {
        m_byName->setChecked(true);
        // A name typed is used as it is.
        m_nameForm->setCurrentIndex(m_nameForm->findData(int(LibraryRenamer::NameForm::Typed)));
        // With no species to name the folder by, it follows the name typed.
        if (!oneSpecies && !m_folderEdited && m_folderId > 0)
            m_folderName->setText(text);
        update();
    });
    connect(m_chooseParts, &QPushButton::clicked, this, &RenameFilesDialog::chooseParts);
    connect(m_folder, &QCheckBox::toggled, this, &RenameFilesDialog::update);
    connect(m_folderName, &QLineEdit::textEdited, this, [this] {
        m_folderEdited = true;
        m_folder->setChecked(true);
        update();
    });
    update();
}

void RenameFilesDialog::setUpFolder(const QString &connectionName, const QStringList &roots)
{
    QSet<int> folders;
    for (const auto &p : std::as_const(m_photos))
        folders.insert(p.folderId);
    const QString path = m_photos.isEmpty() ? QString() : QDir::cleanPath(m_photos.first().folder);
    m_folderNow = QFileInfo(path).fileName();
    QString why;
    if (folders.size() != 1) {
        why = tr("These photos are in different folders, so no folder is renamed.");
    } else {
        for (const QString &root : roots) {
            const QString r = QDir::cleanPath(root);
            if (r == path || r.startsWith(path + QLatin1Char('/')))
                why = tr("“%1” is a library folder set in Settings, so it isn't renamed here.")
                          .arg(m_folderNow);
        }
    }
    if (!why.isEmpty()) {
        m_folder->setText(tr("Also rename their folder"));
        m_folder->setEnabled(false);
        m_folderName->setEnabled(false);
        m_folderNote->setText(why);
        return;
    }
    m_folderId = *folders.begin();
    m_folder->setText(tr("Also rename the folder “%1” to:").arg(m_folderNow));
    m_folder->setToolTip(QDir::toNativeSeparators(path));

    // The species' name, the way the folder is named now.
    const LibraryRenamer::Photo &p = m_photos.first();
    QSet<QString> species;
    for (const auto &photo : std::as_const(m_photos))
        species.insert(photo.scientificName);
    QString name = m_name->text();
    if (species.size() == 1 && !p.scientificName.isEmpty()) {
        const bool scientific = m_folderNow.compare(p.commonName, Qt::CaseInsensitive) != 0
                                && (m_folderNow.compare(p.scientificName, Qt::CaseInsensitive) == 0
                                    || looksScientific(m_folderNow) || p.commonName.isEmpty());
        name = scientific ? p.scientificName : p.commonName;
    }
    m_folderName->setText(LibraryRenamer::cleanName(name));

    QSqlQuery q(QSqlDatabase::database(connectionName, false));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM capture WHERE folder_id = ?"));
    q.addBindValue(m_folderId);
    const int others = q.exec() && q.next() ? q.value(0).toInt() - int(m_photos.size()) : 0;
    QDir dir(path);
    const int subfolders = int(dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
    QStringList notes;
    if (others > 0)
        notes << (others == 1 ? tr("Its 1 other photo stays in it, under the new folder name.")
                              : tr("Its %1 other photos stay in it, under the new folder name.").arg(others));
    if (subfolders > 0)
        notes << (subfolders == 1 ? tr("The folder inside it goes with it.")
                                  : tr("The %1 folders inside it go with it.").arg(subfolders));
    m_folderNote->setText(notes.join(QLatin1Char(' ')));
    m_folderNote->setVisible(!notes.isEmpty());
}

LibraryRenamer::NameForm RenameFilesDialog::nameForm() const
{
    return LibraryRenamer::NameForm(m_nameForm->currentData().toInt());
}

void RenameFilesDialog::nameFormChosen()
{
    const LibraryRenamer::NameForm form = nameForm();
    if (form == LibraryRenamer::NameForm::Typed) {
        m_name->setPlaceholderText(tr("Common or scientific name"));
        if (m_name->text().isEmpty() && !m_photos.isEmpty())
            m_name->setText(m_photos.first().nameText);
        m_name->setFocus();
        return;
    }
    // One species: its name, to see (and change, which makes it typed).
    // Several: each photo's own.
    QSet<QString> names;
    for (const auto &p : std::as_const(m_photos))
        names.insert(LibraryRenamer::speciesName(p.scientificName, p.commonName, form));
    names.remove(QString());
    if (names.size() == 1) {
        m_name->setText(*names.begin());
    } else {
        m_name->clear();
        m_name->setPlaceholderText(tr("Each photo's own species"));
    }
}

int RenameFilesDialog::folderId() const
{
    return m_folder->isChecked() && !folderName().isEmpty() && folderName() != m_folderNow ? m_folderId : 0;
}

QString RenameFilesDialog::folderName() const
{
    return LibraryRenamer::cleanName(m_folderName->text());
}

void RenameFilesDialog::chooseParts()
{
    QList<FileNamingDialog::Sample> samples;
    for (const auto &p : std::as_const(m_photos)) {
        if (samples.size() >= 3)
            break;
        samples.append({p.fields, p.suffix});
    }
    FileNamingDialog dialog(samples, m_pattern, m_settings.exportAuthorName(),
                            tr("Build each photo's new file name. Parts a photo has no value for (e.g. no "
                               "common name or no location) are left out of its name. A photo's RAW and "
                               "JPEG get the same name, and each keeps its own extension."),
                            this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_pattern = dialog.pattern();
    m_settings.setLibraryRenamePattern(m_pattern);
    m_settings.setExportAuthorName(dialog.authorName());
    m_byParts->setChecked(true);
    update();
}

void RenameFilesDialog::update()
{
    m_parts->setText(m_pattern.describe());
    const bool byName = m_byName->isChecked();
    LibraryRenamer::plan(m_photos, byName ? LibraryRenamer::Mode::NamePart : LibraryRenamer::Mode::Pattern,
                         m_name->text(), m_pattern, m_settings.exportAuthorName(), nameForm());
    m_folderName->setEnabled(m_folderId > 0);

    const QColor muted = themeColors(currentThemeVariant()).mutedText;
    m_preview->clear();
    if (folderId() > 0) {
        auto *item = new QTreeWidgetItem(m_preview, {tr("Folder: %1").arg(m_folderNow),
                                                     tr("Folder: %1").arg(folderName())});
        QFont bold = item->font(0);
        bold.setBold(true);
        item->setFont(0, bold);
        item->setFont(1, bold);
    }
    int renamed = 0, online = 0;
    for (const auto &p : std::as_const(m_photos)) {
        const QString suffix = p.suffix.isEmpty() ? QString() : QLatin1Char('.') + p.suffix;
        auto *item = new QTreeWidgetItem(m_preview, {p.oldStem + suffix, p.newStem + suffix});
        item->setToolTip(0, QDir::toNativeSeparators(QDir(p.folder).filePath(p.oldStem + suffix)));
        item->setToolTip(1, p.newStem + suffix);
        if (!p.onDisk) {
            ++online;
            item->setText(1, tr("not renamed: kept online only"));
            item->setForeground(1, muted);
        } else if (p.newStem == p.oldStem) {
            item->setText(1, tr("no change"));
            item->setForeground(1, muted);
        } else {
            ++renamed;
        }
    }

    const bool folder = folderId() > 0;
    QString summary = renamed == 0   ? tr("No photo is renamed")
                      : renamed == 1 ? tr("1 photo is renamed")
                                     : tr("%1 photos are renamed").arg(renamed);
    if (folder)
        summary += tr(", and the folder");
    if (online > 0)
        summary += tr(" · %1 kept online only, left as they are").arg(online);
    m_summary->setText(summary + QLatin1Char('.'));
    m_ok->setEnabled(renamed > 0 || folder);
    m_ok->setText(renamed == 1 ? tr("Rename 1 Photo on Drive")
                  : renamed > 1 ? tr("Rename %1 Photos on Drive").arg(renamed)
                  : folder      ? tr("Rename Folder on Drive")
                                : tr("Rename on Drive"));
}

} // namespace pl
