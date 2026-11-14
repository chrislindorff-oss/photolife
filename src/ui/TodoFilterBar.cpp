#include "ui/TodoFilterBar.h"

#include "model/CaptureListModel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>

namespace pl {

using model::CaptureListModel;

TodoFilterBar::TodoFilterBar(bool withNotInTree, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *label = new QLabel(tr("To do:"), this);
    label->setToolTip(tr("Show only the photos that still need something. Tick several to see the "
                         "photos needing any of them."));
    layout->addWidget(label);
    layout->addWidget(addChip(tr("Unnamed"),
                              tr("No species yet: never matched, or waiting in Review Unmatched."),
                              CaptureListModel::TodoUnnamed));
    layout->addWidget(addChip(tr("To upload"),
                              tr("Named and with a location, but not on iNaturalist yet: ready to upload."),
                              CaptureListModel::TodoToUpload));
    layout->addWidget(addChip(tr("No location"),
                              tr("No coordinates, so they can't be mapped or uploaded yet. Set Location "
                                 "on Map, Paste Location or a GPX track gives them one."),
                              CaptureListModel::TodoNoLocation));
    if (withNotInTree)
        layout->addWidget(addChip(tr("Not in a tree"),
                                  tr("Named, but the species isn't in any of your reference trees."),
                                  CaptureListModel::TodoNotInTree));
    m_clear = new QToolButton(this);
    m_clear->setText(tr("Clear"));
    m_clear->setAutoRaise(true);
    m_clear->setVisible(false);
    connect(m_clear, &QToolButton::clicked, this, &TodoFilterBar::clear);
    layout->addWidget(m_clear);
    layout->addStretch(1);
}

QToolButton *TodoFilterBar::addChip(const QString &text, const QString &tip, int flag)
{
    auto *chip = new QToolButton(this);
    chip->setText(text);
    chip->setToolTip(tip);
    chip->setCheckable(true);
    chip->setProperty("chip", true);   // styled as a chip (theme.qss)
    chip->setProperty("todoFlag", flag);
    connect(chip, &QToolButton::toggled, this, [this] {
        m_clear->setVisible(flags() != 0);
        emit flagsChanged(flags());
    });
    m_chips << chip;
    return chip;
}

int TodoFilterBar::flags() const
{
    int f = 0;
    for (const QToolButton *chip : m_chips)
        f |= chip->isChecked() ? chip->property("todoFlag").toInt() : 0;
    return f;
}

void TodoFilterBar::clear()
{
    if (flags() == 0)
        return;
    for (QToolButton *chip : std::as_const(m_chips)) {
        const QSignalBlocker quiet(chip);
        chip->setChecked(false);
    }
    m_clear->setVisible(false);
    emit flagsChanged(0);
}

} // namespace pl
