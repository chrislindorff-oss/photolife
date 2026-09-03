#include <QApplication>

#include "app/Application.h"

int main(int argc, char *argv[])
{
    QApplication qtApp(argc, argv);

    pl::Application app;
    if (!app.initialize())
        return 1;

    app.showMainWindow();
    return QApplication::exec();
}
