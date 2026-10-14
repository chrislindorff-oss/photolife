#include "ui/FileNamingDialog.h"

#include "db/Database.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace pl {

using collection::FileNamePattern;
using collection::NamePart;
using collection::NameToken;

namespace {

constexpr int kTokenRole = Qt::UserRole;
constexpr int kTextRole = Qt::UserRole + 1;

struct Preset
{
    const char *label;
    QList<NameToken> tokens;
};

const QList<Preset> &presets()
{
    static const QList<Preset> list = {
        {QT_TRANSLATE_NOOP("pl::FileNamingDialog", "Scientific name, date, sequence"),
         {NameToken::ScientificName, NameToken::DateTaken, NameToken::Sequence}},
        {QT_TRANSLATE_NOOP("pl::FileNamingDialog", "Scientific name, common name, town, date"),
         {NameToken::ScientificName, NameToken::CommonName, NameToken::Town, NameToken::DateTaken}},
        {QT_TRANSLATE_NOOP("pl::FileNamingDialog", "Date, time, scientific name"),
         {NameToken::DateTaken, NameToken::TimeTaken, NameToken::ScientificName}},
        {QT_TRANSLATE_NOOP("pl::FileNamingDialog", "Scientific name, date, author, sequence"),
         {NameToken::ScientificName, NameToken::DateTaken, NameToken::Author, NameToken::Sequence}},
        {QT_TRANSLATE_NOOP("pl::FileNamingDialog", "Scientific name, original filename"),
         {NameToken::ScientificName, NameToken::OriginalName}},
    };
    return list;
}

QString itemText(NameToken token, const QString &text)
{
    return token == NameToken::Text ? QStringLiteral("“%1”").arg(text)
                                    : FileNamePattern::tokenLabel(token);
}

} // namespace

FileNamingDialog::FileNamingDialog(pl::Database &db, int projectId, const FileNamePattern &pattern,
                                   const QString &authorName, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("File Naming"));
    setModal(true);
    resize(620, 560);

    auto *hint = new QLabel(
        tr("Build the name each exported file gets. Parts a photo has no value for "
           "(e.g. no common name or no location) are left out of its name. A photo's RAW "
           "and JPEG get the same name, and each keeps its own extension."),
        this);
    hint->setWordWrap(true);

    m_presets = new QComboBox(this);
    m_presets->addItem(tr("Start from a preset…"));
    for (const Preset &preset : presets())
        m_presets->addItem(tr(preset.label));
    connect(m_presets, &QComboBox::activated, this, &FileNamingDialog::applyPreset);

    m_available = new QListWidget(this);
    for (NameToken token : FileNamePattern::availableTokens()) {
        auto *item = new QListWidgetItem(token == NameToken::Text ? tr("Custom text…")
                                                                  : FileNamePattern::tokenLabel(token),
                                         m_available);
        item->setData(kTokenRole, int(token));
    }
    m_available->setCurrentRow(0);
    connect(m_available, &QListWidget::itemDoubleClicked, this, &FileNamingDialog::addSelected);

    m_chosen = new QListWidget(this);
    m_chosen->setDragDropMode(QAbstractItemView::InternalMove);
    m_chosen->setDefaultDropAction(Qt::MoveAction);
    m_chosen->setToolTip(tr("Drag to reorder. Double-click custom text to edit it."));
    connect(m_chosen, &QListWidget::itemDoubleClicked, this, &FileNamingDialog::editCustomText);
    connect(m_chosen->model(), &QAbstractItemModel::rowsMoved, this, &FileNamingDialog::updatePreview);
    connect(m_chosen, &QListWidget::currentRowChanged, this, &FileNamingDialog::updateButtons);
    connect(m_available, &QListWidget::currentRowChanged, this, &FileNamingDialog::updateButtons);

    m_add = new QPushButton(tr("&Add ›"), this);
    m_remove = new QPushButton(tr("‹ &Remove"), this);
    m_up = new QPushButton(tr("Move &Up"), this);
    m_down = new QPushButton(tr("Move &Down"), this);
    connect(m_add, &QPushButton::clicked, this, &FileNamingDialog::addSelected);
    connect(m_remove, &QPushButton::clicked, this, &FileNamingDialog::removeSelected);
    connect(m_up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveSelected(1); });
    auto *buttonColumn = new QVBoxLayout;
    buttonColumn->addStretch(1);
    buttonColumn->addWidget(m_add);
    buttonColumn->addWidget(m_remove);
    buttonColumn->addSpacing(12);
    buttonColumn->addWidget(m_up);
    buttonColumn->addWidget(m_down);
    buttonColumn->addStretch(1);

    auto *availableColumn = new QVBoxLayout;
    availableColumn->addWidget(new QLabel(tr("Available parts:"), this));
    availableColumn->addWidget(m_available);
    auto *chosenColumn = new QVBoxLayout;
    chosenColumn->addWidget(new QLabel(tr("File name, in order:"), this));
    chosenColumn->addWidget(m_chosen);
    auto *lists = new QHBoxLayout;
    lists->addLayout(availableColumn, 1);
    lists->addLayout(buttonColumn);
    lists->addLayout(chosenColumn, 1);

    m_separator = new QComboBox(this);
    m_separator->addItem(tr("Underscore  ( _ )"), QStringLiteral("_"));
    m_separator->addItem(tr("Hyphen  ( - )"), QStringLiteral("-"));
    m_separator->addItem(tr("Space"), QStringLiteral(" "));
    m_separator->addItem(tr("Dot  ( . )"), QStringLiteral("."));
    m_separator->addItem(tr("Hyphen with spaces  ( - )"), QStringLiteral(" - "));
    m_separator->addItem(tr("None"), QString());

    m_dateFormat = new QComboBox(this);
    const QDate example(2024, 3, 9);
    for (const QString &format : {QStringLiteral("yyyy-MM-dd"), QStringLiteral("yyyyMMdd"),
                                  QStringLiteral("dd-MM-yyyy"), QStringLiteral("yyyy-MM"),
                                  QStringLiteral("d MMM yyyy")}) {
        m_dateFormat->addItem(example.toString(format), format);
    }

    m_spacesToSeparator = new QCheckBox(
        tr("Replace spaces inside parts with the separator (\"Caladenia carnea\" → "
           "\"Caladenia_carnea\")"),
        this);

    m_author = new QLineEdit(authorName, this);
    m_author->setPlaceholderText(tr("Your name, for the Author part"));

    auto *form = new QFormLayout;
    form->addRow(tr("&Separator:"), m_separator);
    form->addRow(tr("Date &format:"), m_dateFormat);
    form->addRow(QString(), m_spacesToSeparator);
    form->addRow(tr("Au&thor:"), m_author);

    auto *previewBox = new QGroupBox(tr("Preview"), this);
    m_preview = new QLabel(previewBox);
    m_preview->setTextFormat(Qt::RichText);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    m_warning = new QLabel(previewBox);
    m_warning->setWordWrap(true);
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->addWidget(m_preview);
    previewLayout->addWidget(m_warning);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(m_presets);
    layout->addLayout(lists, 1);
    layout->addLayout(form);
    layout->addWidget(previewBox);
    layout->addWidget(buttons);

    loadSamples(db, projectId);
    setPattern(pattern.isEmpty() ? defaultPattern() : pattern);

    connect(m_separator, &QComboBox::currentIndexChanged, this, &FileNamingDialog::updatePreview);
    connect(m_dateFormat, &QComboBox::currentIndexChanged, this, &FileNamingDialog::updatePreview);
    connect(m_spacesToSeparator, &QCheckBox::toggled, this, &FileNamingDialog::updatePreview);
    connect(m_author, &QLineEdit::textChanged, this, &FileNamingDialog::updatePreview);
    updatePreview();
    updateButtons();
}

FileNamePattern FileNamingDialog::defaultPattern()
{
    FileNamePattern pattern;
    pattern.parts = {{NameToken::ScientificName, {}},
                     {NameToken::DateTaken, {}},
                     {NameToken::Sequence, {}}};
    pattern.spacesToSeparator = true;
    return pattern;
}

void FileNamingDialog::loadSamples(pl::Database &db, int projectId)
{
    // A few real photos from this tree, so the preview shows names the user
    // will actually get -- preferring ones with a date and a locality.
    QSqlQuery q(QSqlDatabase::database(db.connectionName(), false));
    q.prepare(QStringLiteral(
        "SELECT r.path, c.captured_on, t.name, t.common_name, g.locality "
        "FROM capture c "
        "JOIN capture_match m ON m.id = ("
        "   SELECT id FROM capture_match WHERE capture_id = c.id "
        "   ORDER BY (decided_by = 'user') DESC, confidence DESC LIMIT 1) "
        "JOIN taxon t ON t.id = m.taxon_id "
        "JOIN rendition r ON r.id = ("
        "   SELECT id FROM rendition WHERE capture_id = c.id ORDER BY (kind = 'raw'), id LIMIT 1) "
        "LEFT JOIN geocode_cache g ON g.lat_round = ROUND(CAST(c.latitude AS NUMERIC), 3) "
        "   AND g.lon_round = ROUND(CAST(c.longitude AS NUMERIC), 3) "
        "WHERE m.status IN ('confirmed', 'auto') "
        "  AND m.taxon_id IN (SELECT taxon_id FROM project_taxon WHERE project_id = ?) "
        "ORDER BY (c.captured_on IS NULL), (g.locality IS NULL), c.id DESC LIMIT 3"));
    q.addBindValue(projectId);
    if (q.exec()) {
        while (q.next()) {
            Sample sample;
            const QFileInfo file(q.value(0).toString());
            sample.fields.originalStem = file.completeBaseName();
            sample.suffix = file.suffix();
            sample.fields.captured = collection::parseCapturedOn(q.value(1).toString());
            sample.fields.scientificName = q.value(2).toString();
            const QString common = q.value(3).toString();
            if (common != sample.fields.scientificName)
                sample.fields.commonName = common;
            const collection::LocalityParts place = collection::splitLocality(q.value(4).toString());
            sample.fields.town = place.town;
            sample.fields.state = place.state;
            sample.fields.country = place.country;
            m_samples << sample;
        }
    }

    if (m_samples.isEmpty()) {
        Sample sample;
        sample.fields.originalStem = QStringLiteral("IMG_4821");
        sample.suffix = QStringLiteral("jpg");
        sample.fields.captured = QDateTime(QDate(2024, 3, 9), QTime(10, 42, 7));
        sample.fields.scientificName = QStringLiteral("Litoria caerulea");
        sample.fields.commonName = QStringLiteral("Australian Green Tree Frog");
        sample.fields.town = QStringLiteral("Cairns");
        sample.fields.state = QStringLiteral("Queensland");
        sample.fields.country = QStringLiteral("Australia");
        m_samples << sample;
    }
}

void FileNamingDialog::setPattern(const FileNamePattern &pattern)
{
    m_chosen->clear();
    for (const NamePart &part : pattern.parts)
        addToken(part.token, part.text);

    int sep = m_separator->findData(pattern.separator);
    if (sep < 0) {
        m_separator->addItem(QStringLiteral("“%1”").arg(pattern.separator), pattern.separator);
        sep = m_separator->count() - 1;
    }
    m_separator->setCurrentIndex(sep);

    int date = m_dateFormat->findData(pattern.dateFormat);
    if (date < 0) {
        m_dateFormat->addItem(QDate(2024, 3, 9).toString(pattern.dateFormat), pattern.dateFormat);
        date = m_dateFormat->count() - 1;
    }
    m_dateFormat->setCurrentIndex(date);
    m_spacesToSeparator->setChecked(pattern.spacesToSeparator);
}

FileNamePattern FileNamingDialog::pattern() const
{
    FileNamePattern pattern;
    for (int row = 0; row < m_chosen->count(); ++row) {
        const QListWidgetItem *item = m_chosen->item(row);
        pattern.parts.append({NameToken(item->data(kTokenRole).toInt()),
                              item->data(kTextRole).toString()});
    }
    pattern.separator = m_separator->currentData().toString();
    pattern.dateFormat = m_dateFormat->currentData().toString();
    pattern.spacesToSeparator = m_spacesToSeparator->isChecked();
    return pattern;
}

QString FileNamingDialog::authorName() const
{
    return m_author->text().trimmed();
}

void FileNamingDialog::applyPreset(int index)
{
    if (index <= 0 || index > presets().size())
        return;
    m_chosen->clear();
    for (NameToken token : presets().at(index - 1).tokens)
        addToken(token, {});
    m_presets->setCurrentIndex(0);
    updatePreview();
    updateButtons();
}

void FileNamingDialog::addSelected()
{
    const QListWidgetItem *item = m_available->currentItem();
    if (!item)
        return;
    const auto token = NameToken(item->data(kTokenRole).toInt());
    QString text;
    if (token == NameToken::Text) {
        bool ok = false;
        text = QInputDialog::getText(this, tr("Custom Text"),
                                     tr("Text to put in every file name:"), QLineEdit::Normal,
                                     QString(), &ok)
                   .trimmed();
        if (!ok || text.isEmpty())
            return;
    }
    addToken(token, text);
    m_chosen->setCurrentRow(m_chosen->count() - 1);
    updatePreview();
}

void FileNamingDialog::addToken(NameToken token, const QString &text)
{
    auto *item = new QListWidgetItem(itemText(token, text), m_chosen);
    item->setData(kTokenRole, int(token));
    item->setData(kTextRole, text);
}

void FileNamingDialog::removeSelected()
{
    delete m_chosen->takeItem(m_chosen->currentRow());
    updatePreview();
    updateButtons();
}

void FileNamingDialog::moveSelected(int delta)
{
    const int row = m_chosen->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_chosen->count())
        return;
    QListWidgetItem *item = m_chosen->takeItem(row);
    m_chosen->insertItem(target, item);
    m_chosen->setCurrentRow(target);
    updatePreview();
}

void FileNamingDialog::editCustomText(QListWidgetItem *item)
{
    if (!item || NameToken(item->data(kTokenRole).toInt()) != NameToken::Text)
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Custom Text"),
                                               tr("Text to put in every file name:"),
                                               QLineEdit::Normal, item->data(kTextRole).toString(),
                                               &ok)
                             .trimmed();
    if (!ok || text.isEmpty())
        return;
    item->setData(kTextRole, text);
    item->setText(itemText(NameToken::Text, text));
    updatePreview();
}

void FileNamingDialog::updatePreview()
{
    const FileNamePattern p = pattern();
    m_ok->setEnabled(!p.isEmpty());
    if (p.isEmpty()) {
        m_preview->setText(tr("<i>Add at least one part to the file name.</i>"));
        m_warning->clear();
        return;
    }

    QStringList lines;
    int sequence = 0;
    for (Sample sample : m_samples) {
        sample.fields.author = authorName();
        if (p.uses(NameToken::Sequence))
            sample.fields.sequence = ++sequence;
        QString name = p.render(sample.fields);
        if (!sample.suffix.isEmpty())
            name += QLatin1Char('.') + sample.suffix;
        const QString original = sample.suffix.isEmpty()
                                     ? sample.fields.originalStem
                                     : sample.fields.originalStem + QLatin1Char('.') + sample.suffix;
        lines << QStringLiteral("%1 &nbsp;→&nbsp; <b>%2</b>")
                     .arg(original.toHtmlEscaped(), name.toHtmlEscaped());
    }
    m_preview->setText(lines.join(QStringLiteral("<br>")));

    QStringList warnings;
    if (!p.uses(NameToken::Sequence) && !p.uses(NameToken::OriginalName)
        && !p.uses(NameToken::TimeTaken)) {
        warnings << tr("Photos that end up with the same name (e.g. the same species on the "
                       "same day) are told apart with \" (2)\", \" (3)\"… Add a sequence "
                       "number or the time taken for tidier names.");
    }
    if (p.uses(NameToken::Author) && authorName().isEmpty())
        warnings << tr("The Author part is empty — type a name above.");
    m_warning->setText(warnings.isEmpty() ? QString()
                                          : QStringLiteral("⚠ ") + warnings.join(QStringLiteral("\n⚠ ")));
}

void FileNamingDialog::updateButtons()
{
    const int row = m_chosen->currentRow();
    m_add->setEnabled(m_available->currentItem() != nullptr);
    m_remove->setEnabled(row >= 0);
    m_up->setEnabled(row > 0);
    m_down->setEnabled(row >= 0 && row < m_chosen->count() - 1);
}

} // namespace pl
