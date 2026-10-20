#include "MainWindow.h"
#include <QApplication>
#include <QTimer>
#include <QTabWidget>
#include <QDir>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("OpenXR FOV Manager");
    app.setApplicationVersion(FOV_VERSION);
    app.setOrganizationName("OpenXRFovExtension");
    MainWindow w;
    w.show();
    auto args = app.arguments();
    int smoke = args.indexOf("--smoke-test");
    if (smoke >= 0 && smoke + 1 < args.size()) {
        auto path = args[smoke + 1];
        QTimer::singleShot(100, &app, [&app, &w, path]() {
            QDir().mkpath(path);
            auto tabs = w.findChild<QTabWidget *>();
            bool ok = tabs != nullptr;
            if (tabs)
                for (int i = 0; i < tabs->count(); i++) {
                    tabs->setCurrentIndex(i);
                    app.processEvents();
                    ok = w.grab().save(path + QString("/page-%1.png").arg(i)) && ok;
                }
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
