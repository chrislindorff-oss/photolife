#pragma once

#include "collection/ExportEngine.h"

#include <QDialog>
#include <QList>
#include <QString>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListView;
class QRadioButton;

namespace pl {
class Database;
}
namespace pl::thumb {
class ThumbnailCache;
}
namespace pl::model {
class CaptureListModel;
}

namespace pl {

// Collects what ExportService needs to run: whether to create a new
// collection or update an existing one, a destination folder and the
// export-time choices (which match statuses count, whether to scaffold
// folders for taxa with no photos yet, whether to match the reference tree
// dock's current view filters, whether to include common names in folder
// names). A live preview grid shows exactly which photos are currently in
// scope, updating as the choices above change.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    ExportDialog(pl::Database &db, pl::thumb::ThumbnailCache &thumbs, int projectId,
                const QString &treeName, const QList<qint64> &visibleTaxonIds,
                bool treePhotographedOnly, const QString &initialDestFolder,
                const QString &initialCollectionFolder, QWidget *parent = nullptr);

    // New mode: the parent folder a "<tree name>" collection is created in.
    // Update mode: the existing collection's own folder.
    QString destRoot() const;
    bool updatesExistingCollection() const;
    collection::ExportOptions options() const;

private:
    void browseForFolder();
    void updateOkState();
    void updatePreviewScope();
    void updatePreviewCount();
    void updateScaffoldState();
    void onModeChanged();
    void updateFolderCheck();

    int m_projectId = 0;
    QList<qint64> m_visibleTaxonIds;
    bool m_treePhotographedOnly = false;
    QString m_projectName;       // project.name -- the combo text may carry " (customized)"
    QString m_treeFolderName;    // m_projectName as ExportEngine names the collection folder
    QString m_newDestText;       // per-mode folder text, swapped on a mode switch
    QString m_collectionText;
    bool m_showingUpdate = false;   // which mode's text m_destFolder currently holds

    // Update mode's verdict on the chosen folder.
    enum class FolderCheck { Ok, NeedsConfirm, Blocked };
    FolderCheck m_folderCheck = FolderCheck::Blocked;

    QRadioButton *m_modeNew;
    QRadioButton *m_modeUpdate;
    QLabel *m_destLabel;
    QLabel *m_folderStatus;
    QPushButton *m_useSubfolder;
    QCheckBox *m_confirmFolder;

    QLineEdit *m_destFolder;
    QCheckBox *m_includeAuto;
    QRadioButton *m_previewAll;
    QRadioButton *m_previewAutoOnly;
    QCheckBox *m_scaffoldEmpty;
    QCheckBox *m_matchVisibleTree;
    QCheckBox *m_includeCommonName;
    QDialogButtonBox *m_buttons;

    model::CaptureListModel *m_previewModel;
    QListView *m_previewGrid;
    QLabel *m_previewCount;
};

} // namespace pl
