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

    // 支持 qtsvfs-gui [包目录] [--names=名字表.tsv]，方便脚本直接拉到界面上看
    std::filesystem::path pkgDir, names;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--names=", 0) == 0) {
            names = std::filesystem::path(QString::fromLocal8Bit(a.c_str() + 8).toStdWString());
        } else if (!a.empty() && a[0] != '-') {
            pkgDir = std::filesystem::path(QString::fromLocal8Bit(a.c_str()).toStdWString());
        }
    }
    std::string err;
    if (!names.empty() && !window.loadNamesFile(names, err)) {
        qWarning("名字表加载失败: %s", err.c_str());
    }
    if (!pkgDir.empty() && !window.openPath(pkgDir, err)) {
        qWarning("打开包失败: %s", err.c_str());
    }
    return app.exec();
}
