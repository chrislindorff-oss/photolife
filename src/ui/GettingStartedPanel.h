#pragma once

#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace pl {

// What the Getting Started guide needs to know to show where the user is up
// to. Filled in by MainWindow from the catalogue and settings.
struct GuideState
{
    int photos = 0;            // photos in the library
    int folders = 0;           // library folders added
    int trees = 0;             // reference trees
    QStringList treeNames;
    int matched = 0;           // photos matched to a taxon
    int toReview = 0;          // waiting in Review Unmatched
    bool exploredCoverage = false;
    QString inatUser;          // iNaturalist username, if given
    int inatPhotos = 0;        // photos downloaded from iNaturalist
    bool organised = false;    // a download folder has been organised
    bool server = false;       // a shared (server) catalogue
    bool editable = true;      // ...that this computer may change now
};

// The Getting Started guide: a checklist of PhotoLife's workflow beside the
// main window -- add photos, build a reference tree, match, review, explore,
// and, when ready, iNaturalist downloads and organising new photos. Each step
// has a picture, a few plain sentences and a button that opens the real
// action (actionRequested()); steps tick themselves off from setState(). The
// next step to do is expanded, done ones fold to a line saying what was
// achieved, and any can be clicked open. No app logic here.
class GettingStartedPanel : public QWidget
{
    Q_OBJECT

public:
    enum Step { Library, Tree, Match, Review, Explore, Inat, Organise, StepCount };
    static constexpr int kBasics = 5;   // Library .. Explore; the rest are extras

    explicit GettingStartedPanel(QWidget *parent = nullptr);

    void setState(const GuideState &state);
    // Wide enough for its pictures to be read.
    QSize sizeHint() const override { return {420, 700}; }
    void setShowAtStartup(bool on);

    // Whether the guide is still worth opening at start-up: no library yet,
    // or no reference tree.
    static bool basicsMissing(const GuideState &state);

    // For tests (and the window): where things stand.
    bool isDone(Step step) const;
    int nextStep() const { return m_next; }   // -1 when everything is done
    bool isExpanded(Step step) const;
    int basicsDone() const;
    // The help-page section each step's "More about this" opens.
    static QString helpAnchor(Step step);

signals:
    // `which`: 0 the main button, 1 the second one (e.g. Import a Tree File).
    void actionRequested(int step, int which);
    void helpRequested(const QString &anchor);
    void showAtStartupChanged(bool on);
    void hideRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Card
    {
        QWidget *frame = nullptr;
        QPushButton *header = nullptr;   // badge + title; click to open/close
        QLabel *summary = nullptr;       // what's been done (or what's waiting)
        QWidget *body = nullptr;
        QLabel *image = nullptr;
        QPixmap picture;
        QLabel *text = nullptr;
        QLabel *note = nullptr;          // why the button is off, when it is
        QPushButton *primary = nullptr;
        QPushButton *secondary = nullptr;
        bool done = false;
        bool expanded = false;
        bool userToggled = false;        // opened/closed by hand: left as it is
    };

    void buildCard(Step step, QVBoxLayout *into);
    void updateCards();
    void setExpanded(Step step, bool on);
    void fitImages();
    void makeEnlargeable(QLabel *label);
    void showLarge(const QPixmap &picture, const QString &title);

    GuideState m_state;
    QList<Card> m_cards;
    int m_next = Library;
    QLabel *m_progress = nullptr;
    QLabel *m_flow = nullptr;
    QPixmap m_flowPicture;
    QLabel *m_serverNote = nullptr;
    QLabel *m_extrasHeading = nullptr;
    QCheckBox *m_showAtStartup = nullptr;
};

} // namespace pl
