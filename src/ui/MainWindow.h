#pragma once

#include <QMainWindow>

namespace pl {

class Application;

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

    Application &m_app;
};

} // namespace pl
