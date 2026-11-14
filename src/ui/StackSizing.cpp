#include "ui/StackSizing.h"

#include <QSizePolicy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVariant>

namespace pl {
namespace {

constexpr const char *kOwnPolicy = "pl_ownSizePolicy";

void applyToPages(QStackedWidget *stack)
{
    for (int i = 0; i < stack->count(); ++i) {
        QWidget *page = stack->widget(i);
        if (!page->property(kOwnPolicy).isValid())
            page->setProperty(kOwnPolicy, QVariant::fromValue(page->sizePolicy()));
        const QSizePolicy own = page->property(kOwnPolicy).value<QSizePolicy>();
        page->setSizePolicy(page == stack->currentWidget() ? own
                                                           : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
    }
    stack->updateGeometry();
}

} // namespace

void sizeStackToCurrentPage(QStackedWidget *stack)
{
    if (!stack)
        return;
    applyToPages(stack);
    QObject::connect(stack, &QStackedWidget::currentChanged, stack, [stack] { applyToPages(stack); });
}

void sizeStackToCurrentPage(QTabWidget *tabs)
{
    if (!tabs)
        return;
    // The tabs' own stack (Qt names it so).
    sizeStackToCurrentPage(tabs->findChild<QStackedWidget *>(QStringLiteral("qt_tabwidget_stackedwidget")));
}

} // namespace pl
