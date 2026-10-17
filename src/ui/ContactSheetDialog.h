#pragma once

#include "collection/ContactSheetWriter.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace pl {

class ContactSheetPreview;

// Settings for exporting the Reference Photos tab as a PDF contact sheet:
// page and grid (thumbnail size), headings, captions, photo credits and the
// output file. A summary line and a miniature page preview follow the
// choices live, both from computeContactSheetLayout().
class ContactSheetDialog : public QDialog
{
    Q_OBJECT

public:
    // `initial` supplies every choice, including the default title;
    // `speciesCount`/`withPhotoCount` size the page estimate.
    ContactSheetDialog(const collection::ContactSheetOptions &initial, const QString &initialPath,
                       int speciesCount, int withPhotoCount, QWidget *parent = nullptr);

    collection::ContactSheetOptions options() const;
    QString outputPath() const;

private:
    void browse();
    void onPresetChanged(int index);
    void syncPresetToColumns();
    void updateSummary();

    int m_speciesCount = 0;
    int m_withPhotoCount = 0;

    QComboBox *m_pageSize = nullptr;
    QComboBox *m_orientation = nullptr;
    QComboBox *m_preset = nullptr;
    QSpinBox *m_columns = nullptr;
    QCheckBox *m_cropSquare = nullptr;
    QLabel *m_summary = nullptr;
    ContactSheetPreview *m_preview = nullptr;

    QLineEdit *m_title = nullptr;
    QLineEdit *m_subtitle = nullptr;
    QComboBox *m_groupBy = nullptr;
    QCheckBox *m_groupNewRow = nullptr;
    QCheckBox *m_showDate = nullptr;
    QCheckBox *m_showPageNumbers = nullptr;
    QCheckBox *m_showCount = nullptr;

    QCheckBox *m_captionScientific = nullptr;
    QCheckBox *m_captionCommon = nullptr;
    QCheckBox *m_includeMissing = nullptr;

    QRadioButton *m_creditsNone = nullptr;
    QRadioButton *m_creditsUnder = nullptr;
    QRadioButton *m_creditsPage = nullptr;

    QLineEdit *m_path = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pl
