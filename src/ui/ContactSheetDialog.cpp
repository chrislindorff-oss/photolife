#include "ui/ContactSheetDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace pl {

using collection::ContactSheetLayout;
using collection::ContactSheetOptions;

namespace {

struct SizePreset
{
    const char *label;
    int columns;
};

// Thumbnail-size presets, as thumbnails per row on the page. The last combo
// entry is "Custom" for any other column count.
const QList<SizePreset> &sizePresets()
{
    static const QList<SizePreset> list = {
        {QT_TRANSLATE_NOOP("pl::ContactSheetDialog", "Small"), 6},
        {QT_TRANSLATE_NOOP("pl::ContactSheetDialog", "Medium"), 4},
        {QT_TRANSLATE_NOOP("pl::ContactSheetDialog", "Large"), 3},
    };
    return list;
}

} // namespace

// A miniature of one page: header band, grid of thumbnail boxes and caption
// lines, from the same layout the PDF uses.
class ContactSheetPreview : public QWidget
{
public:
    explicit ContactSheetPreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(170, 220);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }

    void setOptions(const ContactSheetOptions &options)
    {
        m_options = options;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const ContactSheetLayout l = collection::computeContactSheetLayout(m_options);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const double scale = std::min((width() - 8) / l.page.width(),
                                      (height() - 8) / l.page.height());
        const QSizeF pageSize = l.page * scale;
        const QPointF origin((width() - pageSize.width()) / 2,
                             (height() - pageSize.height()) / 2);
        const auto map = [&](double x, double y, double w, double h) {
            return QRectF(origin.x() + x * scale, origin.y() + y * scale, w * scale, h * scale);
        };

        const QColor ink(0x55, 0x55, 0x55);
        const QColor tile(0xc8, 0xd3, 0xdc);
        const QColor line(0xa8, 0xa8, 0xa8);

        p.setPen(QPen(QColor(0x99, 0x99, 0x99), 1));
        p.setBrush(Qt::white);
        p.drawRect(QRectF(origin, pageSize));
        p.setPen(Qt::NoPen);

        const double left = l.margin;
        const double contentW = l.page.width() - 2 * l.margin;
        if (l.headerHeight > 0) {
            p.setBrush(ink);
            if (!m_options.title.isEmpty())
                p.drawRect(map(left, l.margin + 2.5, contentW * 0.45, 3.5));
            if (m_options.showDate || m_options.showSpeciesCount)
                p.drawRect(map(left + contentW * 0.75, l.margin + 4, contentW * 0.25, 1.5));
            if (!m_options.subtitle.isEmpty())
                p.drawRect(map(left, l.margin + 8.5, contentW * 0.3, 1.8));
            p.setBrush(line);
            p.drawRect(map(left, l.margin + l.headerHeight - 3, contentW, 0.4));
        }

        const double lineH = l.captionPt * 25.4 / 72 * 1.3;
        const bool groupRows = !m_options.groupRank.isEmpty() && m_options.groupNewRow;
        // New-row grouping: sketch a heading above the first row and before
        // the last, as two groups would fall.
        constexpr double headingH = 7.0;
        double top = l.margin + l.headerHeight;
        int rows = l.rowsPerPage;
        if (groupRows) {
            const double contentH =
                l.page.height() - top - l.margin - l.footerHeight - 2 * headingH;
            rows = std::max(1, int((contentH + l.gutter) / (l.cellHeight() + l.gutter)));
        }
        for (int r = 0; r < rows; ++r) {
            if (groupRows && (r == 0 || r == rows - 1)) {
                p.setBrush(ink);
                p.drawRect(map(left, top + 2.5, contentW * 0.25, 2.2));
                top += headingH;
            }
            for (int c = 0; c < l.columns; ++c) {
                const double x = left + c * (l.thumb + l.gutter);
                const double y = top + r * (l.cellHeight() + l.gutter);
                if (l.groupStrip > 0) {
                    p.setBrush(line);
                    p.drawRect(map(x, y + l.groupStrip - 0.6, l.thumb, 0.35));
                    if (c == 0) {
                        p.setBrush(ink);
                        p.drawRect(map(x, y + 0.5, l.thumb * 0.6, l.groupStrip - 1.6));
                    }
                }
                p.setBrush(tile);
                p.drawRect(map(x, y + l.groupStrip, l.thumb, l.thumb));
                p.setBrush(line);
                double ty = y + l.groupStrip + l.thumb + 1.2;
                const int lines = int(m_options.captionScientific) + int(m_options.captionCommon)
                                  + int(m_options.credits
                                        == ContactSheetOptions::CreditsUnderPhoto);
                for (int i = 0; i < lines; ++i) {
                    p.drawRect(map(x, ty + lineH * 0.3, l.thumb * (i == 0 ? 0.8 : 0.6),
                                   lineH * 0.4));
                    ty += lineH;
                }
            }
        }

        if (m_options.showPageNumbers) {
            p.setBrush(line);
            p.drawRect(map(l.page.width() / 2 - 6, l.page.height() - l.margin - 2, 12, 1.2));
        }
    }

private:
    ContactSheetOptions m_options;
};

ContactSheetDialog::ContactSheetDialog(const ContactSheetOptions &initial,
                                       const QString &initialPath, int speciesCount,
                                       int withPhotoCount, QWidget *parent)
    : QDialog(parent), m_speciesCount(speciesCount), m_withPhotoCount(withPhotoCount)
{
    setWindowTitle(tr("Export Contact Sheet"));

    // --- Layout ------------------------------------------------------------
    m_pageSize = new QComboBox(this);
    m_pageSize->addItem(tr("A4"), int(QPageSize::A4));
    m_pageSize->addItem(tr("A3"), int(QPageSize::A3));
    m_pageSize->addItem(tr("US Letter"), int(QPageSize::Letter));
    m_pageSize->setCurrentIndex(std::max(0, m_pageSize->findData(int(initial.pageSize))));

    m_orientation = new QComboBox(this);
    m_orientation->addItem(tr("Portrait"), int(QPageLayout::Portrait));
    m_orientation->addItem(tr("Landscape"), int(QPageLayout::Landscape));
    m_orientation->setCurrentIndex(initial.orientation == QPageLayout::Landscape ? 1 : 0);

    m_preset = new QComboBox(this);
    for (const SizePreset &preset : sizePresets())
        m_preset->addItem(tr(preset.label), preset.columns);
    m_preset->addItem(tr("Custom"), 0);

    m_columns = new QSpinBox(this);
    m_columns->setRange(ContactSheetOptions::kMinColumns, ContactSheetOptions::kMaxColumns);
    m_columns->setValue(initial.columns);
    m_columns->setSuffix(tr(" per row"));

    auto *sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_preset, 1);
    sizeRow->addWidget(m_columns);

    m_cropSquare = new QCheckBox(tr("Crop photos to squares"), this);
    m_cropSquare->setChecked(initial.cropSquare);
    m_cropSquare->setToolTip(tr("Off: each photo is shown whole, fitted inside its square."));

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);

    auto *layoutForm = new QFormLayout;
    layoutForm->addRow(tr("Page size:"), m_pageSize);
    layoutForm->addRow(tr("Orientation:"), m_orientation);
    layoutForm->addRow(tr("Thumbnail size:"), sizeRow);
    layoutForm->addRow(QString(), m_cropSquare);
    layoutForm->addRow(QString(), m_summary);
    auto *layoutBox = new QGroupBox(tr("Layout"), this);
    layoutBox->setLayout(layoutForm);

    // --- Headings ----------------------------------------------------------
    m_title = new QLineEdit(initial.title, this);
    m_subtitle = new QLineEdit(initial.subtitle, this);
    m_subtitle->setPlaceholderText(tr("Optional — e.g. a location or field trip"));

    m_groupBy = new QComboBox(this);
    m_groupBy->addItem(tr("None (one alphabetical grid)"), QString());
    m_groupBy->addItem(tr("Order"), QStringLiteral("order"));
    m_groupBy->addItem(tr("Family"), QStringLiteral("family"));
    m_groupBy->addItem(tr("Genus"), QStringLiteral("genus"));
    m_groupBy->setCurrentIndex(std::max(0, m_groupBy->findData(initial.groupRank)));
    m_groupBy->setToolTip(tr("Group the photos under a section heading for each taxon of this "
                             "rank."));
    m_groupNewRow = new QCheckBox(tr("Start each group on a new row"), this);
    m_groupNewRow->setChecked(initial.groupNewRow);
    m_groupNewRow->setToolTip(
        tr("On: each group starts a new row under its own heading.\n"
           "Off: the photos run on as one continuous, full grid, each group marked by a "
           "small label above its photos."));
    m_groupNewRow->setEnabled(!initial.groupRank.isEmpty());
    connect(m_groupBy, &QComboBox::currentIndexChanged, this, [this] {
        m_groupNewRow->setEnabled(!m_groupBy->currentData().toString().isEmpty());
    });

    m_showDate = new QCheckBox(tr("Date"), this);
    m_showDate->setChecked(initial.showDate);
    m_showCount = new QCheckBox(tr("Species count"), this);
    m_showCount->setChecked(initial.showSpeciesCount);
    m_showPageNumbers = new QCheckBox(tr("Page numbers"), this);
    m_showPageNumbers->setChecked(initial.showPageNumbers);
    auto *showRow = new QHBoxLayout;
    showRow->addWidget(m_showDate);
    showRow->addWidget(m_showCount);
    showRow->addWidget(m_showPageNumbers);
    showRow->addStretch(1);

    auto *headingForm = new QFormLayout;
    headingForm->addRow(tr("Title:"), m_title);
    headingForm->addRow(tr("Subtitle:"), m_subtitle);
    headingForm->addRow(tr("Group by:"), m_groupBy);
    headingForm->addRow(QString(), m_groupNewRow);
    headingForm->addRow(tr("Show:"), showRow);
    auto *headingBox = new QGroupBox(tr("Headings"), this);
    headingBox->setLayout(headingForm);

    // --- Captions ----------------------------------------------------------
    m_captionScientific = new QCheckBox(tr("Scientific name"), this);
    m_captionScientific->setChecked(initial.captionScientific);
    m_captionCommon = new QCheckBox(tr("Common name"), this);
    m_captionCommon->setChecked(initial.captionCommon);
    m_includeMissing = new QCheckBox(tr("Include species with no reference photo"), this);
    m_includeMissing->setChecked(initial.includeMissing);
    m_includeMissing->setEnabled(withPhotoCount < speciesCount);

    auto *captionLayout = new QVBoxLayout;
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(m_captionScientific);
    nameRow->addWidget(m_captionCommon);
    nameRow->addStretch(1);
    captionLayout->addLayout(nameRow);
    captionLayout->addWidget(m_includeMissing);
    auto *captionBox = new QGroupBox(tr("Captions"), this);
    captionBox->setLayout(captionLayout);

    // --- Credits -----------------------------------------------------------
    m_creditsPage = new QRadioButton(tr("Numbered list on a credits page at the end"), this);
    m_creditsUnder = new QRadioButton(tr("Small line under each photo"), this);
    m_creditsNone = new QRadioButton(tr("None"), this);
    auto *creditsGroup = new QButtonGroup(this);
    creditsGroup->addButton(m_creditsPage);
    creditsGroup->addButton(m_creditsUnder);
    creditsGroup->addButton(m_creditsNone);
    switch (initial.credits) {
    case ContactSheetOptions::NoCredits: m_creditsNone->setChecked(true); break;
    case ContactSheetOptions::CreditsUnderPhoto: m_creditsUnder->setChecked(true); break;
    case ContactSheetOptions::CreditsPage: m_creditsPage->setChecked(true); break;
    }
    auto *creditsNote = new QLabel(
        tr("<small>Most iNaturalist photos are Creative Commons licensed, which requires "
           "crediting the photographer when you share them.</small>"),
        this);
    creditsNote->setWordWrap(true);
    auto *creditsLayout = new QVBoxLayout;
    creditsLayout->addWidget(m_creditsPage);
    creditsLayout->addWidget(m_creditsUnder);
    creditsLayout->addWidget(m_creditsNone);
    creditsLayout->addWidget(creditsNote);
    auto *creditsBox = new QGroupBox(tr("Photo Credits"), this);
    creditsBox->setLayout(creditsLayout);

    // --- Output ------------------------------------------------------------
    m_path = new QLineEdit(initialPath, this);
    auto *browseButton = new QPushButton(tr("Browse…"), this);
    connect(browseButton, &QPushButton::clicked, this, &ContactSheetDialog::browse);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseButton);
    auto *outputBox = new QGroupBox(tr("Save As"), this);
    outputBox->setLayout(pathRow);

    // --- Preview -----------------------------------------------------------
    m_preview = new ContactSheetPreview(this);
    auto *previewLayout = new QVBoxLayout;
    previewLayout->addWidget(m_preview, 1);
    auto *previewBox = new QGroupBox(tr("Page Preview"), this);
    previewBox->setLayout(previewLayout);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Export PDF"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *settingsColumn = new QVBoxLayout;
    settingsColumn->addWidget(layoutBox);
    settingsColumn->addWidget(headingBox);
    settingsColumn->addWidget(captionBox);
    settingsColumn->addWidget(creditsBox);

    auto *columns = new QHBoxLayout;
    columns->addLayout(settingsColumn, 3);
    columns->addWidget(previewBox, 2);

    auto *root = new QVBoxLayout(this);
    root->addLayout(columns, 1);
    root->addWidget(outputBox);
    root->addWidget(m_buttons);

    // --- Live updates ------------------------------------------------------
    connect(m_preset, &QComboBox::activated, this, &ContactSheetDialog::onPresetChanged);
    connect(m_columns, &QSpinBox::valueChanged, this, [this] {
        syncPresetToColumns();
        updateSummary();
    });
    for (QComboBox *box : {m_pageSize, m_orientation, m_groupBy})
        connect(box, &QComboBox::currentIndexChanged, this, &ContactSheetDialog::updateSummary);
    for (QCheckBox *box : {m_cropSquare, m_groupNewRow, m_showDate, m_showPageNumbers, m_showCount,
                           m_captionScientific, m_captionCommon, m_includeMissing})
        connect(box, &QCheckBox::toggled, this, &ContactSheetDialog::updateSummary);
    for (QRadioButton *button : {m_creditsNone, m_creditsUnder, m_creditsPage})
        connect(button, &QRadioButton::toggled, this, &ContactSheetDialog::updateSummary);
    for (QLineEdit *edit : {m_title, m_subtitle, m_path})
        connect(edit, &QLineEdit::textChanged, this, &ContactSheetDialog::updateSummary);

    syncPresetToColumns();
    updateSummary();
}

ContactSheetOptions ContactSheetDialog::options() const
{
    ContactSheetOptions o;
    o.pageSize = QPageSize::PageSizeId(m_pageSize->currentData().toInt());
    o.orientation = QPageLayout::Orientation(m_orientation->currentData().toInt());
    o.columns = m_columns->value();
    o.cropSquare = m_cropSquare->isChecked();
    o.title = m_title->text().trimmed();
    o.subtitle = m_subtitle->text().trimmed();
    o.showDate = m_showDate->isChecked();
    o.showPageNumbers = m_showPageNumbers->isChecked();
    o.showSpeciesCount = m_showCount->isChecked();
    o.captionScientific = m_captionScientific->isChecked();
    o.captionCommon = m_captionCommon->isChecked();
    o.includeMissing = m_includeMissing->isChecked();
    o.groupRank = m_groupBy->currentData().toString();
    o.groupNewRow = m_groupNewRow->isChecked();
    o.credits = m_creditsNone->isChecked()    ? ContactSheetOptions::NoCredits
                : m_creditsUnder->isChecked() ? ContactSheetOptions::CreditsUnderPhoto
                                              : ContactSheetOptions::CreditsPage;
    return o;
}

QString ContactSheetDialog::outputPath() const
{
    QString path = m_path->text().trimmed();
    if (!path.isEmpty() && QFileInfo(path).suffix().compare(QLatin1String("pdf"),
                                                            Qt::CaseInsensitive) != 0)
        path += QStringLiteral(".pdf");
    return path;
}

void ContactSheetDialog::browse()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Contact Sheet"),
                                                      m_path->text(), tr("PDF files (*.pdf)"));
    if (!path.isEmpty())
        m_path->setText(path);
}

void ContactSheetDialog::onPresetChanged(int index)
{
    const int columns = m_preset->itemData(index).toInt();
    if (columns > 0)
        m_columns->setValue(columns);
}

void ContactSheetDialog::syncPresetToColumns()
{
    const int index = m_preset->findData(m_columns->value());
    m_preset->setCurrentIndex(index >= 0 ? index : m_preset->count() - 1);
}

void ContactSheetDialog::updateSummary()
{
    const ContactSheetOptions o = options();
    const ContactSheetLayout l = collection::computeContactSheetLayout(o);
    const int photos = o.includeMissing ? m_speciesCount : m_withPhotoCount;
    const int pages = std::max(1, int(std::ceil(double(photos) / l.perPage())));

    QString text = tr("≈ %1 mm thumbnails · %2 per page · %3 photos on about %n page(s)",
                      nullptr, pages)
                       .arg(qRound(l.thumb))
                       .arg(l.perPage())
                       .arg(photos);
    if (!o.groupRank.isEmpty() && o.groupNewRow)
        text += tr(", more with group headings");
    if (o.credits == ContactSheetOptions::CreditsPage)
        text += tr(", plus credits");
    m_summary->setText(text);
    m_preview->setOptions(o);

    m_buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(photos > 0 && !m_path->text().trimmed().isEmpty());
}

} // namespace pl
