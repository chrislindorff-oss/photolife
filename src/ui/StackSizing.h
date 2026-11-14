#pragma once

class QStackedWidget;
class QTabWidget;

namespace pl {

// Sizes a page stack to the page on show. Left alone, a QStackedWidget
// (and so a QTabWidget) can't be smaller than its largest page, shown or
// not -- so the window couldn't be narrower than its widest view needs, in
// every view. Hidden pages' minimum sizes stop counting; each page gets its
// own size policy back when it's shown. Call once its pages are added (any
// added later are handled from their first page change).
void sizeStackToCurrentPage(QStackedWidget *stack);
void sizeStackToCurrentPage(QTabWidget *tabs);

} // namespace pl
