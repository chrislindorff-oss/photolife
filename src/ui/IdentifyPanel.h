#pragma once

#include "inat/PhotoIdentifier.h"
#include "taxonomy/TaxonomyTypes.h"

#include <QHash>
#include <QWidget>

#include <optional>

class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace pl {

class Application;

// "What is it?" from iNaturalist's computer vision: asks about a photo (or a
// part of it) and lists the suggestions, best first --
//   Flame Robin · Petroica phoenicea — 94% · seen nearby · in your trees
// -- with the coarser taxon they share, when iNaturalist gives one, on top.
// Picking one emits chosen(); nothing is decided here. Without a usable
// token it offers Get Token, and asks again by itself once there is one.
class IdentifyPanel : public QWidget
{
    Q_OBJECT

public:
    IdentifyPanel(Application &app, QWidget *parent = nullptr);

    void identify(const inat::IdentifyRequest &request);
    void clear();
    bool hasSuggestions() const;
    // Moves the keyboard to the list (for keys pressed elsewhere).
    void focusList();
    // Where a suggestion picked names photos (Organise): shows the "Name with
    // common names" choice.
    void setNamesPhotos(bool namesPhotos);
    // The name a photo gets from `taxon`: its common name when that's chosen
    // (Settings::inatIdentifyCommonName()) and it has one, else its scientific name.
    static QString photoName(Application &app, const taxonomy::Taxon &taxon);

signals:
    void chosen(const pl::taxonomy::Taxon &taxon);

private:
    void ask();
    void showResult(const inat::Identification &result);
    void fill();
    void showMessage(const QString &text, bool offerToken = false);
    // iNaturalist's photo of the suggestion at `item`, in a small pop-up at `at`.
    void previewSuggestion(QListWidgetItem *item, const QPoint &at);
    // Right-click: a look at the suggestion, or a coarser taxon above it to
    // use instead (its genus, family... up to its kingdom).
    void suggestionMenu(QListWidgetItem *item, const QPoint &at);
    // The taxa above the suggestions, for that menu: from the catalogue where
    // it has them, else asked of iNaturalist (read-only; nothing is stored).
    void loadHigherRanks();
    void loadAncestors(int generation);
    QString ancestryOf(const taxonomy::Taxon &taxon) const;
    QList<taxonomy::Taxon> higherRanks(const taxonomy::Taxon &taxon) const;   // nearest first

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:

    Application &m_app;
    QLabel *m_status = nullptr;
    QListWidget *m_list = nullptr;
    QCheckBox *m_treesOnly = nullptr;
    QCheckBox *m_commonNames = nullptr;
    QPushButton *m_getToken = nullptr;

    std::optional<inat::IdentifyRequest> m_request;   // the photo being asked about
    std::optional<inat::Identification> m_result;
    bool m_waitingForToken = false;
    QHash<qint64, taxonomy::Taxon> m_higher;   // taxa above the suggestions, by iNat id
    QHash<qint64, QString> m_ancestry;         // for suggestions whose reply didn't carry it
    int m_higherGeneration = 0;                // each result bumps it: older answers are dropped
    int m_higherPending = 0;                   // requests still out
};

} // namespace pl
