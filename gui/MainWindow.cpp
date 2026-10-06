#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QByteArray>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>
#include <QVariant>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <map>

#include "qtsvfs/Export.h"
#include "qtsvfs/Tree.h"
#include "qtsvfs/codec/Codec.h"

namespace {

QString qFromPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.native());
}

std::filesystem::path pathFromQ(const QString& s) {
    return std::filesystem::path(s.toStdWString());
}

QString human(std::uint64_t v) {
    if (v >= 1ull << 30) {
        return QString::number(double(v) / double(1ull << 30), 'f', 2) + QStringLiteral(" GiB");
    }
    if (v >= 1ull << 20) {
        return QString::number(double(v) / double(1ull << 20), 'f', 2) + QStringLiteral(" MiB");
    }
    if (v >= 1024) {
        return QString::number(double(v) / 1024.0, 'f', 1) + QStringLiteral(" KiB");
    }
    return QString::number(v) + QStringLiteral(" B");
}

QString hex16(std::uint64_t v) {
    return QString::asprintf("%016llX", static_cast<unsigned long long>(v));
}

// 一个目录节点最多铺这么多个子项：包 8 有 34 万个节点，全塞进 QTreeWidget 会把界面钉死。
// 超出的部分留一行提示，完整清单走「导出文件树清单」（那份是全量的）。
constexpr int kMaxChildrenPerNode = 2000;

// 树里每行带的隐藏数据：路径、节点哈希、是否目录、属于哪个包、包节点是否已现算、包显示名。
enum ItemRole {
    kPath = Qt::UserRole,
    kHash,
    kIsDir,
    kPkgKey,
    kFilled,
    kPkgLabel,
};

const QHash<QByteArray, const char*>& sourceLabels() {
    // 来源要能说清成立依据：过了哈希闸门的和只靠「名字在本节点体内」的不是一回事。
    static const QHash<QByteArray, const char*> kMap = {
        {"tree:nodetree", "节点树索引"},
        {"real:catalog", "真名(catalog)"},
        {"object:selfname", "体内自声明"},
        {"object:objname", "体内名字(择优)"},
        {"object:objpath", "体内路径(兜底)"},
        {"object:ingamepath", "模块路径"},
        {"named", "哈希路径"},
        {"obsolete", "废弃·不在当前清单"},
    };
    return kMap;
}

}  // namespace

void LibWorker::setJob(std::vector<std::filesystem::path> roots, std::filesystem::path outDir,
                       qtsvfs::PackageRef target, Job kind, bool fullIndex) {
    roots_ = std::move(roots);
    outDir_ = std::move(outDir);
    target_ = std::move(target);
    kind_ = kind;
    fullIndex_ = fullIndex;
    cancelled_ = false;
}

void LibWorker::run() {
    if (kind_ == kImport) {
        const auto report = [this](const std::string& stage, std::size_t done, std::size_t total) {
            emit progress(QString::fromUtf8(stage.c_str()), int(done), int(total));
        };
        qtsvfs::Library next = qtsvfs::openLibrary(roots_, 0, fullIndex_, fullIndex_, report);
        if (next.pkgs.empty()) {
            emit failed(QString::fromUtf8(
                "这些目录里没找到包（要含与目录同名的 .db 元数据卷）。\n"
                "可用的根：QtsVFSCache\\packages、QtsVFSCache\\MiniApp_<id>、整个 QtsVFSCache。"));
            return;
        }
        *lib_ = std::move(next);
        lib_->opened.clear();
        emit imported();
        return;
    }
    if (kind_ == kHarvest) {
        const qtsvfs::PackageRef ref = target_;
        const std::string key = qtsvfs::pathKey(ref.dir);
        if (!qtsvfs::openPackageEntry(*lib_, ref)) {
            emit failed(QString::fromUtf8("打不开包或读不出 FileNode：%1")
                            .arg(QString::fromStdString(ref.label)));
            return;
        }
        emit harvested(QString::fromStdString(key));
        return;
    }
    if (kind_ == kExport) {
        qtsvfs::ExportOptions opt;
        opt.allowNameless = false;
        opt.threads = 0;
        opt.keyset = lib_->keys.empty() ? nullptr : &lib_->keys;
        opt.dirs = lib_->dirs.empty() ? nullptr : &lib_->dirs;
        opt.cancel = [this] { return cancelled_.load(); };
        const qtsvfs::ExportSummary r = qtsvfs::exportAll(
            lib_->pkgs, outDir_, opt,
            [this](const qtsvfs::ExportSummary& cur, const std::string& name) {
                if (cancelled_.load()) {
                    return;
                }
                emit progress(QString::fromUtf8("导出 ") + QString::fromStdString(name),
                              int(cur.files), int(cur.pending));
            });
        emit exported(int(r.files), quint64(r.bytes), int(r.noName), int(r.noData),
                      QString::fromUtf8(r.error.c_str()));
    }
}

MainWindow::MainWindow() {
    lib_ = std::make_shared<qtsvfs::Library>();

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({QString::fromUtf8("名称"), QString::fromUtf8("大小"),
                            QString::fromUtf8("方法"), QString::fromUtf8("节点哈希"),
                            QString::fromUtf8("来源")});
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c) {
        tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }

    hex_ = new QPlainTextEdit(this);
    hex_->setReadOnly(true);
    hex_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::TypeWriter);
    hex_->setFont(mono);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->addWidget(tree_);
    split->addWidget(hex_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    info_ = new QLabel(QString::fromUtf8("未导入"), this);
    statusBar()->addPermanentWidget(info_);

    QMenu* mFile = menuBar()->addMenu(QString::fromUtf8("文件(&F)"));
    QAction* openOne =
        mFile->addAction(QString::fromUtf8("打开单个包（不建全库索引，快）…(&O)"));
    openOne->setShortcut(QKeySequence::Open);
    connect(openOne, &QAction::triggered, this, &MainWindow::openPackage);
    QAction* importAct =
        mFile->addAction(QString::fromUtf8("导入 packages / MiniApp 根（建全库索引）…(&I)"));
    connect(importAct, &QAction::triggered, this, &MainWindow::importRoot);
    QAction* addRootAct =
        mFile->addAction(QString::fromUtf8("追加导入目录…(&D)"));
    connect(addRootAct, &QAction::triggered, this, &MainWindow::addRoot);
    QAction* expandAllAct =
        mFile->addAction(QString::fromUtf8("展开全部包（现算名单）…(&X)"));
    connect(expandAllAct, &QAction::triggered, this, &MainWindow::expandAll);
    mFile->addSeparator();
    QAction* treeAct = mFile->addAction(QString::fromUtf8("导出文件树清单 (TSV)…(&E)"));
    connect(treeAct, &QAction::triggered, this, &MainWindow::exportTree);
    QAction* allAct = mFile->addAction(QString::fromUtf8("按真名导出全部文件…(&A)"));
    connect(allAct, &QAction::triggered, this, &MainWindow::exportAll);
    QAction* selAct = mFile->addAction(QString::fromUtf8("导出选中文件…(&S)"));
    connect(selAct, &QAction::triggered, this, &MainWindow::exportSelected);
    mFile->addSeparator();
    QAction* quit = mFile->addAction(QString::fromUtf8("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::itemSelected);
    connect(tree_, &QTreeWidget::itemExpanded, this, &MainWindow::expandPackage);

    setWindowTitle(QString::fromUtf8("QtsVFS 管理器"));
    resize(1100, 720);
    statusBar()->showMessage(QString::fromUtf8(
        "名单全部现算，不需要名字表：「导入根」看整棵主缓存与 55 个小程序，「打开单个包」只看一个包"));
}

MainWindow::~MainWindow() = default;

void MainWindow::setStatus(const QString& text) {
    statusBar()->showMessage(text);
}

void MainWindow::startJob(LibWorker::Job kind, std::vector<std::filesystem::path> roots,
                          const std::filesystem::path& outDir, const qtsvfs::PackageRef& target,
                          bool fullIndex, bool modal) {
    if (busy()) {
        return;
    }
    if (modal) {
        progress_.reset(new QProgressDialog(QString::fromUtf8("正在忙…"), QString(), 0, 0, this));
        progress_->setWindowModality(Qt::WindowModal);
        progress_->setMinimumDuration(0);
        progress_->setAutoReset(false);
    }
    thread_ = new QThread(this);
    worker_ = new LibWorker(lib_);
    worker_->moveToThread(thread_);
    worker_->setJob(std::move(roots), outDir, target, kind, fullIndex);
    connect(thread_, &QThread::started, worker_, &LibWorker::run);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, this, [this] {
        thread_->deleteLater();
        thread_ = nullptr;
        worker_ = nullptr;
        finishJob();
    });
    connect(worker_, &LibWorker::progress, this, &MainWindow::onProgress);
    connect(worker_, &LibWorker::imported, this, &MainWindow::onImported);
    connect(worker_, &LibWorker::harvested, this, &MainWindow::onHarvested);
    connect(worker_, &LibWorker::failed, this, &MainWindow::onFailed);
    connect(worker_, &LibWorker::exported, this, &MainWindow::onExported);
    thread_->start();
}

void MainWindow::finishJob() {
    if (progress_) {
        progress_->close();
        progress_.reset();
    }
    setEnabled(true);
}

void MainWindow::onProgress(const QString& stage, int done, int total) {
    if (progress_) {
        progress_->setRange(0, total);
        progress_->setValue(done);
        progress_->setLabelText(QString::fromUtf8("%1：%2 / %3").arg(stage).arg(done).arg(total));
        QApplication::processEvents();
    } else {
        setStatus(QString::fromUtf8("%1：%2 / %3").arg(stage).arg(done).arg(total));
    }
}

void MainWindow::onImported() {
    fillPackages();
    info_->setText(QString::fromUtf8("%1 个包 / %2 个实例，闸门键 %3，目录段索引 %4")
                       .arg(lib_->pkgs.size())
                       .arg(lib_->mounts())
                       .arg(lib_->keys.size())
                       .arg(lib_->dirs.size()));
    if (lib_->pkgs.size() == 1 && tree_->topLevelItemCount() == 1) {
        expandPackage(tree_->topLevelItem(0));
    }
}

void MainWindow::onHarvested(const QString& dirKeyQ) {
    const auto it = lib_->opened.find(dirKeyQ.toStdString());
    if (it == lib_->opened.end()) {
        return;
    }
    QTreeWidgetItem* item = nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cand = tree_->topLevelItem(i);
        if (cand->data(0, kPkgKey).toString() == dirKeyQ) {
            item = cand;
            break;
        }
    }
    if (!item) {
        return;
    }
    fillPackage(item, *it->second);
    const qtsvfs::TreeStats& st = it->second->treeStats;
    setStatus(QString::fromUtf8("包 %1 名单现算完：节点 %2，真名 %3，未定名 %4（其中 %5 个带原厂废弃标记）")
                  .arg(item->data(0, kPkgLabel).toString())
                  .arg(st.files)
                  .arg(st.named)
                  .arg(st.nameless)
                  .arg(st.obsolete));
    if (!expandAllQueue_.empty() && !busy()) {
        const QString nextKey = expandAllQueue_.front();
        expandAllQueue_.pop_front();
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* cand = tree_->topLevelItem(i);
            if (cand->data(0, kPkgKey).toString() == nextKey) {
                expandPackage(cand);
                break;
            }
        }
    }
}

void MainWindow::onFailed(const QString& err) {
    QMessageBox::warning(this, QString::fromUtf8("失败"), err);
    setStatus(QString::fromUtf8("失败：%1").arg(err));
}

void MainWindow::onExported(int files, quint64 bytes, int noName, int noData, const QString& err) {
    QMessageBox::information(
        this, QString::fromUtf8("导出完成"),
        QString::fromUtf8("导出 %1 个文件，%2；未定名跳过 %3，盘上无块 %4%5")
            .arg(files)
            .arg(human(bytes))
            .arg(noName)
            .arg(noData)
            .arg(err.isEmpty() ? QString() : QString::fromUtf8("\n最后一条错误：%1").arg(err)));
    setStatus(QString::fromUtf8("导出完成 %1 个文件").arg(files));
}

void MainWindow::openPackage() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选 packages/<id> 或 MiniApp_<id>/0 目录"),
        pkgDir_.empty() ? QDir::currentPath() : qFromPath(pkgDir_));
    if (dir.isEmpty()) {
        return;
    }
    const std::filesystem::path picked = pathFromQ(dir);
    if (qtsvfs::discoverPackages({picked}).empty()) {
        QMessageBox::warning(this, QString::fromUtf8("打不开包"),
                             QString::fromUtf8("%1 及其下面两层里没有包目录").arg(dir));
        return;
    }
    pkgDir_ = picked;
    lib_ = std::make_shared<qtsvfs::Library>();
    tree_->clear();
    startJob(LibWorker::kImport, {picked}, {}, {}, false, true);
}

void MainWindow::importRoot() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选 packages / MiniApp_<id> / QtsVFSCache 根"),
        pkgDir_.empty() ? QDir::currentPath() : qFromPath(pkgDir_));
    if (dir.isEmpty()) {
        return;
    }
    const std::filesystem::path picked = pathFromQ(dir);
    const std::size_t n = qtsvfs::discoverPackages({picked}).size();
    const auto ans = QMessageBox::question(
        this, QString::fromUtf8("建全库索引"),
        QString::fromUtf8("%1 下展开出 %2 个包。\n建全库节点键与目录段索引要把这个实例的块各解一遍"
                          "（整个 packages 约一分多钟），\n之后体内自声明的名字才落得进真实目录。继续？")
            .arg(dir)
            .arg(n),
        QMessageBox::Yes | QMessageBox::No);
    if (ans != QMessageBox::Yes) {
        return;
    }
    pkgDir_ = picked;
    lib_ = std::make_shared<qtsvfs::Library>();
    tree_->clear();
    startJob(LibWorker::kImport, {picked}, {}, {}, true, true);
}

void MainWindow::expandAll() {
    if (busy() || lib_->pkgs.empty()) {
        return;
    }
    expandAllQueue_.clear();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (!item->data(0, kFilled).toBool()) {
            expandAllQueue_.push_back(item->data(0, kPkgKey).toString());
        }
    }
    if (expandAllQueue_.empty()) {
        return;
    }
    setStatus(QString::fromUtf8("展开全部包：还剩 %1 个待现算").arg(expandAllQueue_.size()));
    const QString firstKey = expandAllQueue_.front();
    expandAllQueue_.pop_front();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree_->topLevelItem(i);
        if (item->data(0, kPkgKey).toString() == firstKey) {
            expandPackage(item);
            break;
        }
    }
}

void MainWindow::addRoot() {
    if (busy()) {
        return;
    }
    std::vector<std::filesystem::path> existingDirs;
    for (const auto& ref : lib_->pkgs) {
        existingDirs.push_back(ref.dir);
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选要追加的 packages / MiniApp 根"),
        pkgDir_.empty() ? QDir::currentPath() : qFromPath(pkgDir_));
    if (dir.isEmpty()) {
        return;
    }
    const std::filesystem::path picked = pathFromQ(dir);
    std::vector<std::filesystem::path> allRoots = existingDirs;
    allRoots.push_back(picked);
    pkgDir_ = picked;
    lib_ = std::make_shared<qtsvfs::Library>();
    tree_->clear();
    startJob(LibWorker::kImport, allRoots, {}, {}, true, true);
}

bool MainWindow::openPath(const std::filesystem::path& dir, std::string& err) {
    if (qtsvfs::discoverPackages({dir}).empty()) {
        err = "既不是包目录（含与目录同名的 .db），下面也没有包: " + dir.string();
        return false;
    }
    pkgDir_ = dir;
    lib_ = std::make_shared<qtsvfs::Library>();
    tree_->clear();
    startJob(LibWorker::kImport, {dir}, {}, {}, false, false);
    return true;
}

// 顶层列出所有包：1748 个包也只是 1748 行，展开时才现算该包名单。
void MainWindow::fillPackages() {
    tree_->clear();
    for (const auto& ref : lib_->pkgs) {
        auto* item = new QTreeWidgetItem;
        item->setText(0, QString::fromStdString(ref.label));
        item->setText(4, QString::fromUtf8("待现算"));
        item->setForeground(4, QBrush(Qt::darkGray));
        item->setData(0, kPkgKey, QString::fromStdString(qtsvfs::pathKey(ref.dir)));
        item->setData(0, kPkgLabel, QString::fromStdString(ref.label));
        item->setData(0, kIsDir, true);
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        tree_->addTopLevelItem(item);
    }
}

void MainWindow::expandPackage(QTreeWidgetItem* item) {
    if (!item || item->data(0, kFilled).toBool() || busy()) {
        return;
    }
    const QString key = item->data(0, kPkgKey).toString();
    if (key.isEmpty()) {
        return;
    }
    const auto done = lib_->opened.find(key.toStdString());
    if (done != lib_->opened.end()) {
        fillPackage(item, *done->second);
        return;
    }
    for (const auto& ref : lib_->pkgs) {
        if (qtsvfs::pathKey(ref.dir) == key.toStdString()) {
            startJob(LibWorker::kHarvest, {}, {}, ref, lib_->fullIndex, false);
            setStatus(QString::fromUtf8("正在现算 %1 的名单…")
                          .arg(item->data(0, kPkgLabel).toString()));
            return;
        }
    }
}

void MainWindow::fillPackage(QTreeWidgetItem* item, const qtsvfs::PackageEntry& entry) {
    item->setData(0, kFilled, true);
    item->setText(4, QString());
    item->takeChildren();
    std::function<QTreeWidgetItem*(const qtsvfs::TreeNode&)> make =
        [&](const qtsvfs::TreeNode& n) {
            auto* sub = new QTreeWidgetItem;
            sub->setText(0, QString::fromStdString(n.name.empty() ? std::string("/") : n.name));
            sub->setData(0, kPkgKey, item->data(0, kPkgKey));
            sub->setData(0, kPkgLabel, item->data(0, kPkgLabel));
            if (n.dir) {
                sub->setText(4, QString::fromUtf8("目录"));
                sub->setData(0, kIsDir, true);
                int shown = 0;
                for (const auto& k : n.kids) {
                    if (shown++ >= kMaxChildrenPerNode) {
                        auto* more = new QTreeWidgetItem;
                        more->setText(0, QString::fromUtf8("…其余 %1 项（用「导出文件树清单」看全部）")
                                            .arg(int(n.kids.size()) - kMaxChildrenPerNode));
                        more->setForeground(0, QBrush(Qt::darkGray));
                        sub->addChild(more);
                        break;
                    }
                    sub->addChild(make(k));
                }
                return sub;
            }
            sub->setText(1, human(n.size));
            sub->setText(2, QString::fromUtf8(qtsvfs::methodName(n.method)));
            sub->setText(3, hex16(n.hash));
            const auto it = sourceLabels().constFind(QByteArray(n.source.c_str()));
            sub->setText(4, it == sourceLabels().constEnd()
                                ? (n.named ? QString::fromUtf8("已定名") : QString::fromUtf8("哈希名"))
                                : QString::fromUtf8(it.value()));
            if (!n.named) {
                sub->setForeground(4, QBrush(Qt::darkGray));
            }
            sub->setData(0, kPath, QString::fromStdString(n.path));
            sub->setData(0, kHash, QVariant::fromValue<qulonglong>(n.hash));
            sub->setData(0, kIsDir, false);
            return sub;
        };
    int shown = 0;
    for (const auto& k : entry.root.kids) {
        if (shown++ >= kMaxChildrenPerNode) {
            auto* more = new QTreeWidgetItem;
            more->setText(0, QString::fromUtf8("…其余 %1 项（用「导出文件树清单」看全部）")
                                .arg(int(entry.root.kids.size()) - kMaxChildrenPerNode));
            more->setForeground(0, QBrush(Qt::darkGray));
            item->addChild(more);
            break;
        }
        item->addChild(make(k));
    }
    const qtsvfs::TreeStats& st = entry.treeStats;
    info_->setText(QString::fromUtf8("包 %1：节点 %2，真名 %3，未定名 %4（含 %5 个废弃），未压 %6")
                       .arg(item->data(0, kPkgLabel).toString())
                       .arg(st.files)
                       .arg(st.named)
                       .arg(st.nameless)
                       .arg(st.obsolete)
                       .arg(human(st.bytes)));
}

void MainWindow::itemSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item) {
        return;
    }
    if (item->data(0, kIsDir).toBool()) {
        hex_->setPlainText(QString::fromUtf8("（目录，无内容可预览）"));
        return;
    }
    const auto entry = lib_->opened.find(item->data(0, kPkgKey).toString().toStdString());
    if (entry == lib_->opened.end()) {
        return;
    }
    const auto hash = item->data(0, kHash).toULongLong();
    const auto it = entry->second->pkg->nodes().find(std::uint64_t(hash));
    if (it == entry->second->pkg->nodes().end()) {
        return;
    }
    std::vector<std::uint8_t> blob;
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = entry->second->pkg->readBlob(it->second, blob, err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        hex_->setPlainText(QString::fromUtf8("解包失败：%1").arg(QString::fromUtf8(err.c_str())));
        return;
    }
    showPreview(blob.data(), std::min<std::size_t>(blob.size(), std::size_t(512)), blob.size());
}

void MainWindow::showPreview(const std::uint8_t* data, std::size_t len, std::uint64_t total) {
    QString out;
    for (std::size_t off = 0; off < len; off += 16) {
        char head[24];
        std::snprintf(head, sizeof(head), "%08zX  ", off);
        out += QString::fromLatin1(head);
        QString ascii;
        for (std::size_t i = 0; i < 16; ++i) {
            if (off + i < len) {
                const unsigned char c = data[off + i];
                char hx[8];
                std::snprintf(hx, sizeof(hx), "%02x ", c);
                out += QString::fromLatin1(hx);
                ascii += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
            } else {
                out += QStringLiteral("   ");
            }
        }
        out += QStringLiteral("|") + ascii + QStringLiteral("|\n");
    }
    out += QStringLiteral("\n预览 %1 / 共 %2\n").arg(human(len), human(total));
    hex_->setPlainText(out);
}

void MainWindow::exportTree() {
    if (lib_->opened.empty()) {
        QMessageBox::information(this, QString::fromUtf8("先展开包"),
                                 QString::fromUtf8("还没现算出任何包的名单。"));
        return;
    }
    const QString file = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("导出文件树清单"), QStringLiteral("tree.tsv"),
        QString::fromUtf8("TSV 清单 (*.tsv *.txt)"));
    if (file.isEmpty()) {
        return;
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QString::fromUtf8("写不出文件"), out.errorString());
        return;
    }
    QTextStream ts(&out);
    ts << "package\tpath\thash\tsize\tmethod\tsource\n";
    std::size_t rows = 0;
    for (const auto& [key, entry] : lib_->opened) {
        std::vector<qtsvfs::TreeRow> list;
        qtsvfs::collectRows(entry->root, list, 0, 0);
        std::string label;
        for (const auto& ref : lib_->pkgs) {
            if (qtsvfs::pathKey(ref.dir) == key) {
                label = ref.label;
                break;
            }
        }
        for (const auto& r : list) {
            ts << QString::fromStdString(label) << '\t' << QString::fromStdString(r.path) << '\t'
               << hex16(r.hash) << '\t' << QString::number(r.size) << '\t'
               << QString::number(int(r.method)) << '\t'
               << (r.source.empty() ? (r.named ? QStringLiteral("named") : QStringLiteral("nameless"))
                                    : QString::fromStdString(r.source))
               << '\n';
            ++rows;
        }
    }
    if (!out.commit()) {
        QMessageBox::warning(this, QString::fromUtf8("写不出文件"), out.errorString());
        return;
    }
    setStatus(QString::fromUtf8("文件树 %1 行已导出：%2").arg(rows).arg(file));
}

void MainWindow::exportAll() {
    if (lib_->pkgs.empty()) {
        QMessageBox::information(this, QString::fromUtf8("先导入"),
                                 QString::fromUtf8("还没有展开任何包。"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("导出到目录"));
    if (dir.isEmpty()) {
        return;
    }
    const auto ans = QMessageBox::question(
        this, QString::fromUtf8("导出范围"),
        QString::fromUtf8("将把 %1 个包里现算出真名的文件解出来（未定名跳过，%2）。继续？")
            .arg(lib_->pkgs.size())
            .arg(lib_->fullIndex ? QString::fromUtf8("名单按全库索引定目录段")
                                 : QString::fromUtf8("没建全库索引：跨包引用定不出名、部分名字会落在根上")),
        QMessageBox::Yes | QMessageBox::No);
    if (ans != QMessageBox::Yes) {
        return;
    }
    startJob(LibWorker::kExport, {}, pathFromQ(dir), {}, lib_->fullIndex, true);
    thread_->setObjectName(QStringLiteral("export"));
}

void MainWindow::exportSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item || item->data(0, kIsDir).toBool()) {
        QMessageBox::information(this, QString::fromUtf8("选中文件"),
                                 QString::fromUtf8("请选中单个文件再导出。"));
        return;
    }
    const auto entry = lib_->opened.find(item->data(0, kPkgKey).toString().toStdString());
    if (entry == lib_->opened.end()) {
        return;
    }
    const QString dst = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出选中文件"),
                                                     item->text(0));
    if (dst.isEmpty()) {
        return;
    }
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = qtsvfs::exportOne(*entry->second->pkg,
                                      std::uint64_t(item->data(0, kHash).toULongLong()),
                                      pathFromQ(dst), err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QString::fromUtf8("导出失败"), QString::fromUtf8(err.c_str()));
    } else {
        setStatus(QString::fromUtf8("已导出 %1").arg(dst));
    }
}
