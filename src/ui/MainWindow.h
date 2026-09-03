#pragma once

#include <QMainWindow>

class QLabel;
class QListView;
class QTreeView;
class QComboBox;
class QAction;

namespace pl {

class Application;

namespace model {
class CaptureListModel;
class TaxonomyTreeModel;
}
namespace scan {
struct ScanProgress;
struct ScanSummary;
}
namespace taxonomy {
class ProjectBuilder;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(Application &app, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void buildCentralWidget();
    void restoreLayout();
    void showAbout();

    void addWatchedFolder();
    void startScan();
    void setScanUiRunning(bool running);
    void onScanProgress(const scan::ScanProgress &progress);
    void onScanFinished(const scan::ScanSummary &summary);
    void updateEmptyState();

    void buildReferenceTreeDock();
    void reloadProjectList();
    void newReferenceTree();

    Application &m_app;

    model::CaptureListModel *m_model = nullptr;
    QListView *m_grid = nullptr;
    QLabel *m_emptyHint = nullptr;
    QLabel *m_statusLabel = nullptr;
    QAction *m_scanAction = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_addFolderAction = nullptr;

    model::TaxonomyTreeModel *m_treeModel = nullptr;
    QTreeView *m_treeView = nullptr;
    QComboBox *m_projectCombo = nullptr;
    taxonomy::ProjectBuilder *m_builder = nullptr;
    QAction *m_newTreeAction = nullptr;
};

} // namespace pl
