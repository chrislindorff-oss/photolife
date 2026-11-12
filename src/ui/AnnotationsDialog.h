#pragma once

#include <QHash>
#include <QString>

class QWidget;

namespace pl {

// Sex, life stage, alive or dead, flowers and fruits -- in iNaturalist's own
// annotation terms -- and, with `text`, a free note: Organise's Notes for This
// Run and the Upload tab's Annotations. `values` is keyed "sex", "lifeStage",
// "alive", "flowers"; an empty value is "not noted". True when OK'd.
bool editAnnotations(QWidget *parent, const QString &title, const QString &intro, QHash<QString, QString> &values,
                     QString *text = nullptr);

} // namespace pl
