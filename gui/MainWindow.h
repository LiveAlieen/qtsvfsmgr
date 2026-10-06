#pragma once

#include <QMainWindow>
#include <QObject>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "qtsvfs/Harvest.h"

class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QLabel;
class QThread;
class QProgressDialog;

// 后台干活：导入（展开根 + 建全库索引）、逐包现算名单、全量导出。
// 共享状态全在 qtsvfs::Library 里（CLI 用同一个类型、同一套定名规则）。
// 一次只跑一个任务，界面忙的时候不接新活。
class LibWorker : public QObject {
    Q_OBJECT
public:
    explicit LibWorker(std::shared_ptr<qtsvfs::Library> lib) : lib_(std::move(lib)) {}

    void cancel() { cancelled_ = true; }

    // 线程启动前由界面线程调用：QThread::start 提供 happens-before。
    enum Job { kImport = 1, kHarvest = 2, kExport = 3 };
    void setJob(std::vector<std::filesystem::path> roots, std::filesystem::path outDir,
                qtsvfs::PackageRef target, Job kind, bool fullIndex,
                std::filesystem::path cachePath = {});

public slots:
    void run();

signals:
    void progress(const QString& stage, int done, int total);
    void imported();
    void harvested(const QString& dirKey);
    void failed(const QString& err);
    void exported(int files, quint64 bytes, int noName, int noData, const QString& err);

private:
    std::shared_ptr<qtsvfs::Library> lib_;
    std::vector<std::filesystem::path> roots_;
    std::filesystem::path outDir_;
    qtsvfs::PackageRef target_;
    Job kind_ = kImport;
    bool fullIndex_ = true;
    std::filesystem::path cachePath_;
    std::atomic<bool> cancelled_{false};
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // 命令行直接带包目录时用；菜单里的「打开包目录」也走同一条路。
    bool openPath(const std::filesystem::path& dir, std::string& err);

private slots:
    void openPackage();
    void addRoot();
    void importRoot();
    void expandAll();
    void expandPackage(QTreeWidgetItem* item);
    void exportTree();
    void exportAll();
    void exportSelected();
    void itemSelected();
    void onProgress(const QString& stage, int done, int total);
    void onImported();
    void onHarvested(const QString& dirKey);
    void onFailed(const QString& err);
    void onExported(int files, quint64 bytes, int noName, int noData, const QString& err);

private:
    void setStatus(const QString& text);
    void fillPackages();
    void fillPackage(QTreeWidgetItem* item, const qtsvfs::PackageEntry& entry);
    void showPreview(const std::uint8_t* data, std::size_t len, std::uint64_t total);
    void startJob(LibWorker::Job kind, std::vector<std::filesystem::path> roots,
                  const std::filesystem::path& outDir, const qtsvfs::PackageRef& target,
                  bool fullIndex, bool modal);
    void finishJob();
    bool busy() const { return thread_ != nullptr; }

    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* hex_ = nullptr;
    QLabel* info_ = nullptr;

    std::shared_ptr<qtsvfs::Library> lib_;
    std::filesystem::path pkgDir_;
    LibWorker* worker_ = nullptr;
    QThread* thread_ = nullptr;
    std::unique_ptr<QProgressDialog> progress_;
    std::deque<QString> expandAllQueue_;
};
