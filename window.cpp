#include "window.h"
#include "conversion.h"
#include "dsktowoz2.h"
#include "crc32.h"
#include <QtGlobal>
#include <QDebug>
#include <QApplication>
#include <QShortcut>
#include <QLineEdit>
#include <QGridLayout>
#include <QFileDialog>
#include <QProgressDialog>
#include <QDirIterator>
#include <QHeaderView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QSize>
#include <QString>
#include <QAbstractItemView>

enum {
    absoluteFileNameRole = Qt::UserRole + 1
};

static QComboBox *createComboBox(const QString &text) {
    auto *const comboBox = new QComboBox();
    comboBox->setEditable(true);
    comboBox->addItem(text);
    comboBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return comboBox;
}

Window::Window(QWidget *parent)
    : QWidget(parent) {
    connect(new QShortcut(QKeySequence::Quit, this), &QShortcut::activated, QCoreApplication::instance(), &QApplication::quit);

    setWindowTitle(tr("DskToWoz2"));





    auto *const mainLayout = new QGridLayout(this);

    mainLayout->addWidget(new QLabel(tr("File name patterns:")), 0, 0);
    this->fileComboBox = createComboBox(tr("*.dsk; *.do; *.d16; *.d13"));
    this->fileComboBox->setFocusPolicy(Qt::StrongFocus);
    mainLayout->addWidget(this->fileComboBox, 0, 1, 1, 2);

    mainLayout->addWidget(new QLabel(tr("Directory:")), 2, 0);    
    this->directoryComboBox = createComboBox(QDir::toNativeSeparators(QDir::currentPath()));
    this->directoryComboBox->setFocusPolicy(Qt::StrongFocus);
    mainLayout->addWidget(this->directoryComboBox, 2, 1);
    auto *browseButton = new QPushButton(QApplication::style()->standardIcon(QStyle::SP_DirIcon), tr("\u2190  Browse"), this);
    browseButton->setFocusPolicy(Qt::StrongFocus);
    connect(browseButton, &QAbstractButton::clicked, this, &Window::browse);
    mainLayout->addWidget(browseButton, 2, 2);

    auto *const findButton = new QPushButton(tr("\u2193  Find  \u2193"), this);
    findButton->setFocusPolicy(Qt::StrongFocus);
    findButton->setFocus();
    connect(findButton, &QAbstractButton::clicked, this, &Window::find);
    mainLayout->addWidget(findButton, 3, 0);

    createFilesTable();
    mainLayout->addWidget(this->filesTable, 4, 0, 1, 3);

    this->filesFoundLabel = new QLabel("(Search not yet performed.)");
    mainLayout->addWidget(this->filesFoundLabel, 5, 0, 1, 3);

    this->convertButton = new QPushButton(QApplication::style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Convert"), this);
    this->convertButton->setFocusPolicy(Qt::StrongFocus);
    this->convertButton->setEnabled(false);
    connect(this->convertButton, &QAbstractButton::clicked, this, &Window::convert);
    mainLayout->addWidget(this->convertButton, 6, 0);

    setLayout(mainLayout);



    setWindowState(Qt::WindowMaximized);
    show();
}

Window::~Window() {
    delete this->filesTable;
    delete this->model;
}

void Window::browse() {
    QString directory = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Root directory"), QDir::currentPath()));
    if (!directory.isEmpty()) {
        if (directoryComboBox->findText(directory) == -1) {
            directoryComboBox->addItem(directory);
        }
        directoryComboBox->setCurrentIndex(directoryComboBox->findText(directory));
    }
}

static QIcon iconOkNg(const bool ok) {
    return QApplication::style()->standardIcon(
        ok
        ? QStyle::SP_DialogApplyButton
        : QStyle::SP_DialogCancelButton);
}

static void updateComboBox(QComboBox *comboBox) {
    if (comboBox->findText(comboBox->currentText()) == -1) {
        comboBox->addItem(comboBox->currentText());
    }
}

static QStandardItem *colFile(const Conversion &cvt) {
    return new QStandardItem(cvt.dsk().fileName());
}

static QStandardItem *colDir(const Conversion &cvt) {
    const QString rel = cvt.root().dir().relativeFilePath(cvt.dsk().filePath());
    return new QStandardItem(QFileInfo(rel).path()+QDir::separator());
}

static QStandardItem *colFileWoz(const Conversion &cvt) {
    if (cvt.dos().isEmpty()) {
        return new QStandardItem("");
    }
    const bool ok = !cvt.woz().exists();
    return new QStandardItem(
        iconOkNg(ok),
        cvt.woz().fileName()+(ok ? "" : " (exists)"));
}

static QStandardItem *colVtoc(const Conversion &cvt) {
    const QString dos = cvt.dos();
    return new QStandardItem(
        iconOkNg(!dos.isEmpty()),
        dos.isEmpty() ? "(no VTOC)" : dos);
}

static QStandardItem *colSize(const Conversion &cvt) {
    const qint64 size = cvt.dsk().size();
    const bool ok = (size == 0x23000u || size == 0x1C700u);
    return new QStandardItem(
        iconOkNg(ok),
        QString("$%1").arg(size, 1, 16).toUpper());
}

static QList<QStandardItem*> buildRow(const Conversion &cvt) {
    QList<QStandardItem*> items;

    items.append(colFile(cvt));
    items.append(colSize(cvt));
    items.append(colVtoc(cvt));
    items.append(colFileWoz(cvt));
    items.append(colDir(cvt));

    return items;
}

void Window::find() {
    QString path = QDir::toNativeSeparators(QDir::cleanPath(this->directoryComboBox->currentText()));
    if (!path.endsWith(QDir::separator())) {
        path = path+QDir::separator();
    }

    this->dirRoot = QFileInfo(path);

    updateComboBox(this->fileComboBox);
    updateComboBox(this->directoryComboBox);

    QProgressDialog progressDialog(tr("Locating disk images..."), tr("&Cancel"), 0, 0, this, nullptr);
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setWindowTitle(tr("Find Files"));
    progressDialog.show();

    delete this->model;
    this->model = new QStandardItemModel();
    QStringList header;
    header << tr("Source DSK file") << tr("Size") << tr("VTOC") << tr("Destination WOZ file") << tr("Directory");
    this->model->setHorizontalHeaderLabels(header);
    this->filesTable->setModel(this->model);

    this->convertButton->setEnabled(false);

    QStringList filter = this->fileComboBox->currentText().split(";");
    filter.replaceInStrings(QRegularExpression("^ *(.*) *$"), "\\1");
    QDirIterator it(path, filter, QDir::AllEntries | QDir::Readable | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    std::int32_t c = 0;
    while (it.hasNext() && !progressDialog.wasCanceled()) {
        const QString filePath = it.next();
        ++c;

        const Conversion cvt(QFileInfo(filePath), QFileInfo(this->dirRoot));

        this->model->appendRow(buildRow(cvt));
        this->filesTable->scrollToBottom();
        this->filesFoundLabel->setText(tr("%n file(s) found. (Search in progress.)", "", c));

        if (cvt.ok()) {
            this->cvts << cvt;
        }

        QCoreApplication::processEvents();
    }
    const int cols = this->model->columnCount();
    for (int i = 1; i < cols; ++i) {
        this->filesTable->resizeColumnToContents(i);
    }

    if (progressDialog.wasCanceled()) {
        this->filesFoundLabel->setText(tr("%n file(s) found. (Search was cancelled.)", "", c));
    } else {
        this->filesFoundLabel->setText(tr("%n file(s) found. (Search complete.)", "", c));
    }

    this->convertButton->setEnabled(this->cvts.size());
    if (this->cvts.size()) {
        this->convertButton->setFocus();
    }
}

void Window::createFilesTable() {
    delete this->filesTable;
    this->filesTable = new QTableView(this);
    this->filesTable->setFocusPolicy(Qt::NoFocus);
    this->filesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    this->filesTable->horizontalHeader()->setStretchLastSection(true);
    this->filesTable->setAlternatingRowColors(true);
    this->filesTable->setShowGrid(true);
    this->filesTable->setGridStyle(Qt::DotLine);
    this->filesTable->setFont(QFont("Helvetica", 8));
    this->filesTable->setIconSize(QSize(8, 8));
    QHeaderView *const verticalHeader = this->filesTable->verticalHeader();
    verticalHeader->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader->setMinimumSectionSize(1);
    verticalHeader->setDefaultSectionSize(14);
    this->filesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->filesTable->setSelectionMode(QAbstractItemView::NoSelection);
}

static void dskToWoz2cpp(const QByteArray &rbDsk, const QFileInfo dsk, const std::uint32_t crc, const bool sector16, const QFileInfo woz) {
    dskToWoz2(
        reinterpret_cast<const uint8_t *>(rbDsk.constData()),
        qPrintable(dsk.fileName()),
        crc,
        sector16 ? 1 : 0,
        qPrintable(woz.filePath()));
}

void Window::convert() {
    for (QLinkedList<const Conversion>::const_iterator i = this->cvts.constBegin(); i != this->cvts.constEnd(); ++i) {
        const Conversion &cvt(*i);
        qDebug() << cvt.dsk().filePath();
        QFile src(cvt.dsk().filePath());
        src.open(QIODevice::ReadOnly);
        QByteArray rbSrc = src.readAll();
        qDebug() << "    bytes:" << QString("$%1").arg(rbSrc.size(), 1, 16).toUpper();
        std::uint32_t crc = crc32(reinterpret_cast<const uint8_t *>(rbSrc.constData()), size_t(rbSrc.size()));
        qDebug() << "    crc:" << QString("$%1").arg(crc, 1, 16).toUpper();
        qDebug() << "    dos:" << cvt.dos();
        qDebug() << "    sectors per track:" << (cvt.is13() ? "13" : "16");
        dskToWoz2cpp(rbSrc, cvt.dsk(), crc, !cvt.is13(), cvt.woz());
    }
}
