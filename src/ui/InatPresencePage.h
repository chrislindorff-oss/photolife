#pragma once

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pl {

class Application;
class InatAccountBar;
namespace inat {
class PresenceCheck;
}

// Already on iNaturalist?: finds library photos that are already on the
// user's iNaturalist account -- posted from the app or website, or before
// PhotoLife -- by comparing them with the details of their observations
// (inat::PresenceCheck). High matches are marked on iNaturalist; the rest
// are listed here, by confidence and observation, for a yes or no. Only
// reads from iNaturalist.
class InatPresencePage : public QWidget
{
    Q_OBJECT

public:
    InatPresencePage(Application &app, InatAccountBar &account, QWidget *parent = nullptr);

    // Decisions need an editable catalogue; this says whether it is (and may
    // offer to start editing).
    void setEditGuard(std::function<bool()> canEdit) { m_canEdit = std::move(canEdit); }
    void reload();

signals:
    // Photos marked on iNaturalist or not: the To upload lists change.
    void changed();

private:
    void check();
    void finished();
    void fill();
    QList<int> selectedCaptures() const;
    QList<QTreeWidgetItem *> selectedPhotoRows() const;
    void yes();
    void no();
    void openSelected();
    void undoLastCheck();
    void updateButtons();
    bool canEdit();

    Application &m_app;
    InatAccountBar &m_account;
    inat::PresenceCheck *m_check = nullptr;
    QPushButton *m_checkButton = nullptr;
    QCheckBox *m_rereadAll = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_toCheck = nullptr;
    QToolButton *m_autoMarked = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_yes = nullptr;
    QPushButton *m_no = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_undo = nullptr;
    std::function<bool()> m_canEdit;
};

} // namespace pl
