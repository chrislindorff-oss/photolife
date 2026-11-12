#include "ui/AnnotationsDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace pl {

bool editAnnotations(QWidget *parent, const QString &title, const QString &intro, QHash<QString, QString> &values,
                     QString *text)
{
    auto tr = [](const char *s) { return QCoreApplication::translate("pl::AnnotationsDialog", s); };
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    struct Field
    {
        const char *key;
        const char *label;
        QStringList choices;
    };
    const QList<Field> fields = {
        {"sex", "Sex:", {QStringLiteral("Female"), QStringLiteral("Male"), QStringLiteral("Cannot Be Determined")}},
        {"lifeStage", "Life stage:",
         {QStringLiteral("Adult"), QStringLiteral("Juvenile"), QStringLiteral("Subimago"), QStringLiteral("Teneral"),
          QStringLiteral("Nymph"), QStringLiteral("Pupa"), QStringLiteral("Larva"), QStringLiteral("Egg")}},
        {"alive", "Alive or dead:", {QStringLiteral("Alive"), QStringLiteral("Dead"), QStringLiteral("Cannot Be Determined")}},
        {"flowers", "Flowers and fruits:",
         {QStringLiteral("Flowers"), QStringLiteral("Flower Buds"), QStringLiteral("Fruits or Seeds"),
          QStringLiteral("No Flowers or Fruits")}},
    };
    auto *form = new QFormLayout;
    QList<QComboBox *> boxes;
    for (const Field &f : fields) {
        auto *box = new QComboBox(&dialog);
        box->addItem(QStringLiteral("—"), QString());
        for (const QString &c : f.choices)
            box->addItem(tr(c.toUtf8().constData()), c);
        box->setCurrentIndex(std::max(0, box->findData(values.value(QLatin1String(f.key)))));
        form->addRow(tr(f.label), box);
        boxes << box;
    }
    QPlainTextEdit *edit = nullptr;
    if (text) {
        edit = new QPlainTextEdit(*text, &dialog);
        edit->setPlaceholderText(tr("Anything else: behaviour, habitat, what it was on…"));
        edit->setTabChangesFocus(true);
        edit->setMaximumHeight(90);
        form->addRow(tr("Notes:"), edit);
    }
    auto *label = new QLabel(intro, &dialog);
    label->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(460, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return false;
    for (int i = 0; i < fields.size(); ++i) {
        const QString v = boxes.at(i)->currentData().toString();
        if (v.isEmpty())
            values.remove(QLatin1String(fields.at(i).key));
        else
            values.insert(QLatin1String(fields.at(i).key), v);
    }
    if (text)
        *text = edit->toPlainText().trimmed();
    return true;
}

} // namespace pl
