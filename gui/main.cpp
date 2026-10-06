#include <QApplication>
#include <QString>

#include <filesystem>
#include <string>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QtsVFS Manager"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));
    MainWindow window;
    window.show();

    // 支持 qtsvfs-gui [包目录]，名字表从包内自动构建。
    std::filesystem::path pkgDir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (!a.empty() && a[0] != '-') {
            pkgDir = std::filesystem::path(QString::fromLocal8Bit(a.c_str()).toStdWString());
        }
    }
    std::string err;
    if (!pkgDir.empty() && !window.openPath(pkgDir, err)) {
        qWarning("打开包失败: %s", err.c_str());
    }
    return app.exec();
}
