#pragma once

#include <QMainWindow>
#include <QObject>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "qtsvfs/Names.h"

class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QLabel;

struct PkgHolder;

// 后台解包导出，不阻塞界面：进度与完成信号回到主线程刷新进度条。
class ExportWorker : public QObject {
    Q_OBJECT
public:
    ExportWorker(std::filesystem::path pkgDir, std::filesystem::path outDir, qtsvfs::NameTable names,
                 std::uint64_t limit)
        : pkgDir_(std::move(pkgDir)), outDir_(std::move(outDir)), names_(std::move(names)),
          limit_(limit) {}

    void cancel() { cancelled_ = true; }

public slots:
    void run();

signals:
    void progress(int done, quint64 bytes);
    void finished(int files, quint64 bytes, int failed, const QString& err);

private:
    std::filesystem::path pkgDir_, outDir_;
    qtsvfs::NameTable names_;
    std::uint64_t limit_;
    std::atomic<bool> cancelled_{false};
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // 命令行直接打开时用这两个；菜单里的「打开包目录/加载名字表」也走同一条路。
    bool loadNamesFile(const std::filesystem::path& file, std::string& err);
    bool openPath(const std::filesystem::path& dir, std::string& err);

private slots:
    void openPackage();
    void loadNames();
    void exportTree();
    void exportAll();
    void exportSelected();
    void itemSelected();

private:
    void setStatus(const QString& text);
    void fillTree();
    void showPreview(const std::uint8_t* data, std::size_t len, std::uint64_t total);
    bool confirmWithNames();

    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* hex_ = nullptr;
    QLabel* info_ = nullptr;

    std::filesystem::path pkgDir_;
    std::filesystem::path namesPath_;
    qtsvfs::NameTable names_;
    qtsvfs::SourceTable sources_;
    std::unique_ptr<PkgHolder> pkg_;
    ExportWorker* worker_ = nullptr;
};
