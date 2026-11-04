#pragma once

#include "collection/FileNamePattern.h"

#include <QDialog>
#include <QList>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace pl {
class Database;
}

namespace pl {

// Builds the naming protocol for an exported collection's files: pick parts
// (scientific name, date taken, town, author, custom text, ...) from a list,
// put them in order, and choose how they're joined. A live preview renders
// the pattern against a few real photos from the reference tree.
class FileNamingDialog : public QDialog
{
    Q_OBJECT

public:
    // A photo the preview shows its name for.
    struct Sample
    {
        collection::NameFields fields;
        QString suffix;
    };

    FileNamingDialog(pl::Database &db, int projectId, const collection::FileNamePattern &pattern,
                     const QString &authorName, QWidget *parent = nullptr);
    // For naming other photos than an export's (renaming library files):
    // `samples` are previewed, `hint` says what the names are for.
    FileNamingDialog(const QList<Sample> &samples, const collection::FileNamePattern &pattern,
                     const QString &authorName, const QString &hint, QWidget *parent = nullptr);

    collection::FileNamePattern pattern() const;
    QString authorName() const;

    // A sensible starting pattern when the user has never built one.
    static collection::FileNamePattern defaultPattern();

private:
    static QList<Sample> loadSamples(pl::Database &db, int projectId);
    void setPattern(const collection::FileNamePattern &pattern);
    void applyPreset(int index);
    void addSelected();
    void addToken(collection::NameToken token, const QString &text);
    void removeSelected();
    void moveSelected(int delta);
    void editCustomText(QListWidgetItem *item);
    void updatePreview();
    void updateButtons();

    QList<Sample> m_samples;

    QComboBox *m_presets;
    QListWidget *m_available;
    QListWidget *m_chosen;
    QPushButton *m_add;
    QPushButton *m_remove;
    QPushButton *m_up;
    QPushButton *m_down;
    QComboBox *m_separator;
    QComboBox *m_dateFormat;
    QCheckBox *m_spacesToSeparator;
    QLineEdit *m_author;
    QLabel *m_preview;
    QLabel *m_warning;
    QPushButton *m_ok;
};

} // namespace pl
