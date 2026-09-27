#include "desktopwindow.h"

#include <QApplication>
#include <QFile>
#include <QScreen>
#include <QHash>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("HyprSillyDesktopIcons");
    app.setOrganizationName("HyprSillyDesktopIcons");
    app.setQuitOnLastWindowClosed(false);

    // Load retro stylesheet
    QFile qssFile(":/styles/styles/retro.qss");
    if (qssFile.open(QFile::ReadOnly)) {
        app.setStyleSheet(QString::fromUtf8(qssFile.readAll()));
        qssFile.close();
    }

    // Create one desktop window per screen
    QHash<QScreen *, DesktopWindow *> windows;
    for (QScreen *screen : app.screens()) {
        windows.insert(screen, new DesktopWindow(screen));
    }

    // Handle screens added at runtime
    QObject::connect(&app, &QApplication::screenAdded, [&](QScreen *screen) {
        windows.insert(screen, new DesktopWindow(screen));
    });

    // Handle screen removal - hot-unplug cleanup
    QObject::connect(&app, &QApplication::screenRemoved, [&](QScreen *screen) {
        if (auto *w = windows.take(screen)) {
            w->close();
            w->deleteLater();
        }
    });

    return app.exec();
}
