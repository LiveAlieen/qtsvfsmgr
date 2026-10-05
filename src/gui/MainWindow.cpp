#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QMenuBar>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>

#include <cinttypes>
#include <cstdio>
#include <functional>

#include "qtsvfs/Export.h"
#include "qtsvfs/Package.h"
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

// 名字表默认搜索位置：exe 同级，以及仓库里的 out/
std::filesystem::path findDefaultNames(const QString& appDir) {
    const std::filesystem::path base = pathFromQ(appDir);
    std::error_code ec;
    for (const auto& cand : {base / L"names.merged.tsv", base / L"../../out/names.merged.tsv",
                             base / L"../../../out/names.merged.tsv"}) {
        if (std::filesystem::exists(cand, ec)) {
            return std::filesystem::weakly_canonical(cand, ec);
        }
    }
    return {};
}

}  // namespace

// Package 与指向它的节点指针数组同生命周期，包在树就在。
struct PkgHolder {
    qtsvfs::Package pkg;
    std::vector<const qtsvfs::FileNode*> nodes;
    qtsvfs::TreeNode root;
    qtsvfs::TreeStats stats;
};

MainWindow::~MainWindow() = default;

void ExportWorker::run() {
    qtsvfs::Package pkg;
    std::string err;
    if (!pkg.open(pkgDir_, err) || !pkg.loadNodes(err)) {
        emit finished(0, 0, 0, QString::fromUtf8(err.c_str()));
        return;
    }
    const qtsvfs::ExportResult r = qtsvfs::exportPackage(
        pkg, names_, outDir_, limit_, [this] { return cancelled_.load(); },
        [this](const qtsvfs::ExportResult& cur) {
            emit progress(int(cur.files), quint64(cur.bytes));
        });
    emit finished(int(r.files), quint64(r.bytes), int(r.failed),
                  QString::fromUtf8(r.error.c_str()));
}

MainWindow::MainWindow() {
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

    info_ = new QLabel(QString::fromUtf8("未打开包"), this);
    statusBar()->addPermanentWidget(info_);

    QMenu* mFile = menuBar()->addMenu(QString::fromUtf8("文件(&F)"));
    struct Item {
        QString text;
        QKeySequence key;
        void (MainWindow::*slot)();
    };
    const Item items[] = {
        {QString::fromUtf8("打开包目录…(&O)"), QKeySequence::Open, &MainWindow::openPackage},
        {QString::fromUtf8("加载名字表…(&N)"), QKeySequence(), &MainWindow::loadNames},
        {QString::fromUtf8("导出文件树清单 (TSV)…(&E)"), QKeySequence(), &MainWindow::exportTree},
        {QString::fromUtf8("按真名导出全部文件…(&A)"), QKeySequence(), &MainWindow::exportAll},
        {QString::fromUtf8("导出选中文件…(&S)"), QKeySequence(), &MainWindow::exportSelected},
    };
    for (const Item& it : items) {
        QAction* act = mFile->addAction(it.text);
        if (!it.key.isEmpty()) {
            act->setShortcut(it.key);
        }
        connect(act, &QAction::triggered, this, it.slot);
    }
    mFile->addSeparator();
    QAction* quit = mFile->addAction(QString::fromUtf8("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::itemSelected);

    setWindowTitle(QString::fromUtf8("QtsVFS 管理器"));
    resize(1100, 720);

    const std::filesystem::path names = findDefaultNames(QApplication::applicationDirPath());
    if (!names.empty()) {
        std::string err;
        if (loadNamesFile(names, err)) {
            statusBar()->showMessage(QString::fromUtf8("已自动加载名字表：%1").arg(qFromPath(names)));
        }
    } else {
        statusBar()->showMessage(
            QString::fromUtf8("没找到 names.merged.tsv，树里会是哈希名；用「文件 → 加载名字表」补"));
    }
}

void MainWindow::setStatus(const QString& text) {
    statusBar()->showMessage(text);
}

bool MainWindow::confirmWithNames() {
    if (!names_.empty()) {
        return true;
    }
    const auto r = QMessageBox::question(
        this, QString::fromUtf8("还没有名字表"),
        QString::fromUtf8("没加载名字表时树里全是哈希名。现在加载吗？"), QMessageBox::Yes | QMessageBox::No);
    if (r == QMessageBox::Yes) {
        loadNames();
    }
    return !names_.empty();
}

void MainWindow::loadNames() {
    const QString file = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("选择名字表"), QDir::currentPath(),
        QString::fromUtf8("TSV 名单 (*.tsv *.txt);;所有文件 (*)"));
    if (file.isEmpty()) {
        return;
    }
    std::string err;
    if (!loadNamesFile(pathFromQ(file), err)) {
        QMessageBox::warning(this, QString::fromUtf8("加载失败"), QString::fromUtf8(err.c_str()));
    }
}

bool MainWindow::loadNamesFile(const std::filesystem::path& file, std::string& err) {
    qtsvfs::NameTable table;
    qtsvfs::SourceTable sources;
    if (!qtsvfs::loadNameTable(file, table, err, &sources)) {
        return false;
    }
    qtsvfs::makeUnique(table);
    names_ = std::move(table);
    sources_ = std::move(sources);
    namesPath_ = file;
    info_->setText(QString::fromUtf8("名字表 %1 条").arg(names_.size()));
    setStatus(QString::fromUtf8("名字表已加载 %1 条").arg(names_.size()));
    return true;
}

void MainWindow::openPackage() {
    confirmWithNames();
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选择 packages/<id> 目录"),
        pkgDir_.empty() ? QDir::currentPath() : qFromPath(pkgDir_));
    if (dir.isEmpty()) {
        return;
    }
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = openPath(pathFromQ(dir), err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QString::fromUtf8("打不开包"), QString::fromUtf8(err.c_str()));
    }
}

bool MainWindow::openPath(const std::filesystem::path& dir, std::string& err) {
    auto holder = std::make_unique<PkgHolder>();
    if (!holder->pkg.open(dir, err) || !holder->pkg.loadNodes(err)) {
        return false;
    }
    for (const auto& [h, n] : holder->pkg.nodes()) {
        (void)h;
        holder->nodes.push_back(&n);
    }
    qtsvfs::buildTree(holder->nodes, names_, holder->root, holder->stats, &sources_);
    qtsvfs::sortTree(holder->root);
    pkgDir_ = dir;
    pkg_ = std::move(holder);
    setWindowTitle(QString::fromUtf8("QtsVFS 管理器 — %1").arg(qFromPath(pkgDir_.filename())));
    fillTree();
    info_->setText(QString::fromUtf8("节点 %1，真名 %2，未定名 %3（其中 %4 个带原厂废弃标记），未压 %5")
                       .arg(pkg_->nodes.size())
                       .arg(pkg_->stats.named)
                       .arg(pkg_->stats.nameless)
                       .arg(pkg_->stats.obsolete)
                       .arg(human(pkg_->stats.bytes)));
    setStatus(QString::fromUtf8("已打开 %1").arg(qFromPath(pkgDir_)));
}

void MainWindow::fillTree() {
    tree_->clear();
    if (!pkg_) {
        return;
    }
    std::function<QTreeWidgetItem*(const qtsvfs::TreeNode&)> make =
        [&](const qtsvfs::TreeNode& n) {
            auto* item = new QTreeWidgetItem;
            item->setText(0, QString::fromStdString(n.name.empty() ? std::string("/") : n.name));
            item->setText(3, n.dir ? QString() : hex16(n.hash));
            if (n.dir) {
                item->setText(4, QString::fromUtf8("目录"));
            } else {
                item->setText(1, human(n.size));
                item->setText(2, QString::fromUtf8(qtsvfs::methodName(n.method)));
                // 来源要能说清成立依据：过了哈希闸门的和只靠「名字在本节点体内」的不是一回事
                static const QHash<QByteArray, const char*> kSourceText = {
                    {"tree:nodetree", "节点树索引"},
                    {"real:catalog", "真名(catalog)"},
                    {"object:selfname", "体内自声明"},
                    {"object:objname", "体内名字(择优)"},
                    {"object:objpath", "体内路径(兜底)"},
                    {"object:ingamepath", "模块路径"},
                    {"named", "哈希路径"},
                    {"obsolete", "废弃·不在当前清单"},
                };
                const auto it = kSourceText.constFind(QByteArray(n.source.c_str()));
                item->setText(4, it == kSourceText.constEnd()
                                     ? (n.named ? QString::fromUtf8("已定名") : QString::fromUtf8("哈希名"))
                                     : QString::fromUtf8(it.value()));
                if (!n.named) {
                    item->setForeground(4, QBrush(Qt::darkGray));
                }
            }
            item->setData(0, Qt::UserRole, QString::fromStdString(n.path));
            item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(n.hash));
            item->setData(0, Qt::UserRole + 2, n.dir);
            for (const auto& k : n.kids) {
                item->addChild(make(k));
            }
            return item;
        };
    for (const auto& k : pkg_->root.kids) {
        tree_->addTopLevelItem(make(k));
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        tree_->topLevelItem(i)->setExpanded(false);
    }
}

void MainWindow::itemSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item || !pkg_) {
        return;
    }
    if (item->data(0, Qt::UserRole + 2).toBool()) {
        hex_->setPlainText(QString::fromUtf8("（目录，无内容可预览）"));
        return;
    }
    const auto hash = item->data(0, Qt::UserRole + 1).toULongLong();
    const auto it = pkg_->pkg.nodes().find(std::uint64_t(hash));
    if (it == pkg_->pkg.nodes().end()) {
        return;
    }
    std::vector<std::uint8_t> blob;
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = pkg_->pkg.readBlob(it->second, blob, err);
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
    if (!pkg_) {
        QMessageBox::information(this, QString::fromUtf8("先打开包"), QString::fromUtf8("还没有打开包。"));
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
    ts << "path\thash\tsize\tmethod\tsource\n";
    std::vector<qtsvfs::TreeRow> rows;
    qtsvfs::collectRows(pkg_->root, rows, 0, 0);
    for (const auto& r : rows) {
        ts << QString::fromStdString(r.path) << '\t' << hex16(r.hash) << '\t'
           << QString::number(r.size) << '\t' << QString::number(int(r.method)) << '\t'
           << (r.source.empty() ? (r.named ? QStringLiteral("named") : QStringLiteral("nameless"))
                                : QString::fromStdString(r.source)) << '\n';
    }
    if (!out.commit()) {
        QMessageBox::warning(this, QString::fromUtf8("写不出文件"), out.errorString());
        return;
    }
    setStatus(QString::fromUtf8("文件树 %1 行已导出：%2").arg(rows.size()).arg(file));
}

void MainWindow::exportAll() {
    if (!pkg_) {
        QMessageBox::information(this, QString::fromUtf8("先打开包"), QString::fromUtf8("还没有打开包。"));
        return;
    }
    if (names_.empty()) {
        QMessageBox::information(this, QString::fromUtf8("没有名字表"),
                                 QString::fromUtf8("按真名导出需要先加载名字表。"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("导出到目录"));
    if (dir.isEmpty()) {
        return;
    }
    auto* thread = new QThread(this);
    worker_ = new ExportWorker(pkgDir_, pathFromQ(dir), names_, 0);
    worker_->moveToThread(thread);

    QProgressDialog dlg(QString::fromUtf8("正在解包导出…"), QString::fromUtf8("取消"), 0, 0, this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoReset(false);
    dlg.setAutoClose(false);

    connect(worker_, &ExportWorker::progress, &dlg,
            [this, &dlg](int done, quint64 bytes) {
                dlg.setValue(done);
                dlg.setLabelText(QString::fromUtf8("已导出 %1 个文件，%2").arg(done).arg(human(bytes)));
                if (dlg.wasCanceled()) {
                    worker_->cancel();
                }
            });
    connect(worker_, &ExportWorker::finished, &dlg,
            [this, &dlg, thread](int files, quint64 bytes, int failed, const QString& err) {
                dlg.setValue(0);
                dlg.close();
                worker_ = nullptr;
                thread->quit();
                QMessageBox::information(
                    this, QString::fromUtf8("导出完成"),
                    QString::fromUtf8("导出 %1 个文件，%2，失败 %3%4").arg(files).arg(human(bytes)).arg(failed).arg(err));
                setStatus(QString::fromUtf8("导出完成 %1 个文件").arg(files));
            });
    connect(thread, &QThread::started, worker_, &ExportWorker::run);
    connect(thread, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    dlg.exec();
    if (dlg.wasCanceled()) {
        setStatus(QString::fromUtf8("导出已请求取消，等待收尾…"));
    }
}

void MainWindow::exportSelected() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (!item || !pkg_) {
        return;
    }
    if (item->data(0, Qt::UserRole + 2).toBool()) {
        QMessageBox::information(this, QString::fromUtf8("选中的是目录"),
                                 QString::fromUtf8("请选中单个文件再导出。"));
        return;
    }
    const auto hash = item->data(0, Qt::UserRole + 1).toULongLong();
    const QString dst = QFileDialog::getSaveFileName(this, QString::fromUtf8("导出选中文件"),
                                                     item->text(0));
    if (dst.isEmpty()) {
        return;
    }
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = qtsvfs::exportOne(pkg_->pkg, std::uint64_t(hash), pathFromQ(dst), err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QString::fromUtf8("导出失败"), QString::fromUtf8(err.c_str()));
    } else {
        setStatus(QString::fromUtf8("已导出 %1").arg(dst));
    }
}
