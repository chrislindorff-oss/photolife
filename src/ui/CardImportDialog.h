#pragma once

#include "organise/CardImporter.h"

#include <QDialog>
#include <QList>
#include <QStringList>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;

namespace pl {

class Application;

// Import from Card: choose the card (one with a DCIM folder is found by
// itself) and where the photos go; it says what's on the card -- how many,
// over which days, and how many came off before -- then copies the new ones
// into a folder per day, each verified, and the card is left as it was.
// importedFolders() then names the folders written to, oldest first.
class CardImportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CardImportDialog(Application &app, QWidget *parent = nullptr);
    ~CardImportDialog() override;

    QStringList importedFolders() const { return m_folders; }

    void reject() override;   // stops an import in progress first

private:
    void chooseSource();
    void chooseDestination();
    void survey();            // what's on the card, on a worker
    void showSurvey();
    void startImport();
    void updateButtons();

    Application &m_app;
    QComboBox *m_source = nullptr;
    QLineEdit *m_destination = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_summary = nullptr;
    QCheckBox *m_again = nullptr;
    QProgressBar *m_progress = nullptr;
    QDialogButtonBox *m_buttons = nullptr;

    QList<organise::CardImporter::Source> m_files;
    int m_surveyGeneration = 0;
    bool m_surveying = false;
    bool m_importing = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QStringList m_folders;
};

} // namespace pl
