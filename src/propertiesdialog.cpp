#include "propertiesdialog.h"
#include "desktopconstants.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace {
struct DesktopApplicationEntry {
    QString name;
    QString icon;
    QString desktopFilePath;
};

DesktopApplicationEntry readDesktopApplication(const QString &filePath)
{
    DesktopApplicationEntry entry;
    entry.desktopFilePath = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return entry;

    QTextStream in(&file);
    bool inDesktopEntry = false;
    bool hidden = false;
    bool noDisplay = false;
    QString type = "Application";

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith('[')) {
            inDesktopEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry || !line.contains('='))
            continue;

        const int split = line.indexOf('=');
        const QString key = line.left(split).trimmed();
        const QString value = line.mid(split + 1).trimmed();

        if (key == "Name" && entry.name.isEmpty()) {
            entry.name = value;
        } else if (key == "Icon" && entry.icon.isEmpty()) {
            entry.icon = value;
        } else if (key == "NoDisplay") {
            noDisplay = (value.compare("true", Qt::CaseInsensitive) == 0);
        } else if (key == "Hidden") {
            hidden = (value.compare("true", Qt::CaseInsensitive) == 0);
        } else if (key == "Type") {
            type = value;
        }
    }

    if (hidden || noDisplay || type.compare("Application", Qt::CaseInsensitive) != 0)
        return {};

    if (entry.name.isEmpty())
        entry.name = QFileInfo(filePath).completeBaseName();

    return entry;
}
}

PropertiesDialog::PropertiesDialog(const Settings &s, QWidget *parent)
    : QDialog(parent, Qt::Dialog)
{
    setWindowTitle("Desktop Properties");
    setObjectName("PropertiesDialog");
    setFixedWidth(kPropertiesDialogWidth);
    setSizeGripEnabled(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *banner = new QFrame(this);
    banner->setObjectName("PropBanner");
    banner->setFixedHeight(kPropertiesDialogBannerHeight);
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(10, 6, 10, 6);
    bannerLayout->setSpacing(10);

    auto *bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(QIcon::fromTheme("preferences-desktop",
        QIcon::fromTheme("preferences-system")).pixmap(32, 32));

    auto *bannerText = new QLabel("Desktop Properties", banner);
    bannerText->setObjectName("PropBannerText");

    bannerLayout->addWidget(bannerIcon);
    bannerLayout->addWidget(bannerText);
    bannerLayout->addStretch();
    root->addWidget(banner);

    auto *content = new QWidget(this);
    content->setObjectName("PropContent");
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(kPropertiesDialogContentMargins, kPropertiesDialogContentMargins, kPropertiesDialogContentMargins, kPropertiesDialogContentMargins);
    contentLayout->setSpacing(8);

    auto *tabs = new QTabWidget(content);

    auto *generalTab = new QWidget(tabs);
    auto *generalLayout = new QVBoxLayout(generalTab);
    generalLayout->setContentsMargins(8, 8, 8, 8);
    generalLayout->setSpacing(8);

    auto *iconGroup = new QGroupBox("Icons", generalTab);
    auto *iconLayout = new QGridLayout(iconGroup);
    iconLayout->setColumnStretch(1, 1);
    iconLayout->setVerticalSpacing(8);

    iconLayout->addWidget(new QLabel("Icon size:"), 0, 0);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(6);
    m_sizeSlider = new QSlider(Qt::Horizontal, iconGroup);
    m_sizeSlider->setRange(kIconSizeSliderMin, kIconSizeSliderMax);
    m_sizeSlider->setSingleStep(kIconSizeSliderSingleStep);
    m_sizeSlider->setPageStep(kIconSizeSliderPageStep);
    m_sizeSlider->setValue(s.iconSize);
    m_sizeSlider->setTickPosition(QSlider::TicksBelow);
    m_sizeSlider->setTickInterval(kIconSizeSliderTickInterval);

    m_sizeLabel = new QLabel(QString::number(s.iconSize) + " px", iconGroup);
    m_sizeLabel->setFixedWidth(38);
    m_sizeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) {
        m_sizeLabel->setText(QString::number(v) + " px");
        emit iconSizeChanged(v);
    });

    sizeRow->addWidget(m_sizeSlider);
    sizeRow->addWidget(m_sizeLabel);
    iconLayout->addLayout(sizeRow, 0, 1);

    iconLayout->addWidget(new QLabel("Font:"), 1, 0);
    auto *fontRow = new QHBoxLayout;
    fontRow->setSpacing(6);
    m_fontFamilyCombo = new QComboBox(iconGroup);
    m_fontSizeSpin = new QSpinBox(iconGroup);
    m_fontSizeSpin->setRange(kFontSizeSpinMin, kFontSizeSpinMax);
    m_fontSizeSpin->setValue(s.fontSize);
    m_fontSizeSpin->setSuffix(" pt");
    m_fontSizeSpin->setFixedWidth(65);

    populateFontList();
    int fontIdx = m_fontFamilyCombo->findText(s.fontFamily, Qt::MatchFixedString);
    if (fontIdx >= 0)
        m_fontFamilyCombo->setCurrentIndex(fontIdx);

    connect(m_fontFamilyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        emit fontChanged(m_fontFamilyCombo->currentText());
    });
    connect(m_fontSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        emit fontSizeChanged(v);
    });

    fontRow->addWidget(m_fontFamilyCombo, 1);
    fontRow->addWidget(m_fontSizeSpin);
    iconLayout->addLayout(fontRow, 1, 1);

    m_previewCheck = new QCheckBox("Show image previews", iconGroup);
    m_previewCheck->setChecked(s.showPreview);
    iconLayout->addWidget(m_previewCheck, 2, 0, 1, 2);
    generalLayout->addWidget(iconGroup);

    auto *fileGroup = new QGroupBox("File Display", generalTab);
    auto *fileLayout = new QVBoxLayout(fileGroup);
    fileLayout->setSpacing(6);

    m_showHiddenCheck = new QCheckBox("Show hidden files", fileGroup);
    m_showHiddenCheck->setChecked(s.showHidden);
    fileLayout->addWidget(m_showHiddenCheck);
    generalLayout->addWidget(fileGroup);

    auto *desktopGroup = new QGroupBox("Desktop Icons", generalTab);
    auto *desktopLayout = new QVBoxLayout(desktopGroup);
    desktopLayout->setSpacing(6);

    m_showTrashCheck = new QCheckBox("Recycle Bin", desktopGroup);
    m_showTrashCheck->setChecked(s.showTrash);
    desktopLayout->addWidget(m_showTrashCheck);

    m_showComputerCheck = new QCheckBox("My Computer", desktopGroup);
    m_showComputerCheck->setChecked(s.showComputer);
    desktopLayout->addWidget(m_showComputerCheck);

    m_showHomeCheck = new QCheckBox("My Documents", desktopGroup);
    m_showHomeCheck->setChecked(s.showHome);
    desktopLayout->addWidget(m_showHomeCheck);
    generalLayout->addWidget(desktopGroup);

    auto *arrGroup = new QGroupBox("Arrangement", generalTab);
    auto *arrLayout = new QGridLayout(arrGroup);
    arrLayout->setColumnStretch(1, 1);
    arrLayout->setVerticalSpacing(8);

    m_autoArrangeCheck = new QCheckBox("Auto arrange icons", arrGroup);
    m_autoArrangeCheck->setChecked(s.autoArrange);
    arrLayout->addWidget(m_autoArrangeCheck, 0, 0, 1, 2);

    arrLayout->addWidget(new QLabel("Sort icons by:"), 1, 0);
    m_sortCombo = new QComboBox(arrGroup);
    m_sortCombo->addItems({"Name", "Type", "Date Modified"});
    m_sortCombo->setCurrentIndex(s.sortOrder);
    arrLayout->addWidget(m_sortCombo, 1, 1);
    generalLayout->addWidget(arrGroup);
    generalLayout->addStretch();

    auto *applicationsTab = new QWidget(tabs);
    auto *applicationsLayout = new QVBoxLayout(applicationsTab);
    applicationsLayout->setContentsMargins(8, 8, 8, 8);
    applicationsLayout->setSpacing(8);

    auto *appGroup = new QGroupBox("Installed Applications", applicationsTab);
    auto *appLayout = new QVBoxLayout(appGroup);
    appLayout->setSpacing(6);

    auto *description = new QLabel(
        "Select an installed application to create a shortcut on the desktop.", appGroup);
    description->setWordWrap(true);
    appLayout->addWidget(description);

    m_appSearchEdit = new QLineEdit(appGroup);
    m_appSearchEdit->setPlaceholderText("Search applications");
    appLayout->addWidget(m_appSearchEdit);

    m_appList = new QListWidget(appGroup);
    m_appList->setAlternatingRowColors(false);
    m_appList->setUniformItemSizes(true);
    appLayout->addWidget(m_appList, 1);

    m_createShortcutButton = new QPushButton("Create Shortcut", appGroup);
    m_createShortcutButton->setEnabled(false);
    appLayout->addWidget(m_createShortcutButton, 0, Qt::AlignRight);

    connect(m_appList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_createShortcutButton->setEnabled(m_appList->currentItem() != nullptr);
    });
    connect(m_appSearchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        const QString needle = text.trimmed();
        for (int i = 0; i < m_appList->count(); ++i) {
            auto *item = m_appList->item(i);
            const bool matches = needle.isEmpty()
                || item->text().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matches);
        }
        if (m_appList->currentItem() && m_appList->currentItem()->isHidden())
            m_appList->setCurrentItem(nullptr);
    });
    connect(m_appList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item) return;
        emit applicationShortcutRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_createShortcutButton, &QPushButton::clicked, this, [this]() {
        if (!m_appList->currentItem()) return;
        emit applicationShortcutRequested(m_appList->currentItem()->data(Qt::UserRole).toString());
    });

    applicationsLayout->addWidget(appGroup);
    applicationsLayout->addStretch();

    tabs->addTab(generalTab, "General");
    tabs->addTab(applicationsTab, "Applications");
    contentLayout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, content);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &PropertiesDialog::applied);
    contentLayout->addWidget(buttons);

    root->addWidget(content);

    populateApplicationsList();
}

void PropertiesDialog::populateApplicationsList()
{
    m_appList->clear();

    QStringList appDirs;
    const QString dataHome = qEnvironmentVariableIsEmpty("XDG_DATA_HOME")
        ? QDir::homePath() + "/.local/share"
        : qEnvironmentVariable("XDG_DATA_HOME");
    appDirs << dataHome + "/applications";

    const QStringList dataDirs = qEnvironmentVariableIsEmpty("XDG_DATA_DIRS")
        ? QStringList{"/usr/local/share", "/usr/share"}
        : qEnvironmentVariable("XDG_DATA_DIRS").split(':', Qt::SkipEmptyParts);
    for (const QString &dir : dataDirs)
        appDirs << dir + "/applications";

    QSet<QString> seenDesktopFiles;
    QList<DesktopApplicationEntry> entries;

    for (const QString &dirPath : appDirs) {
        QDir dir(dirPath);
        if (!dir.exists())
            continue;

        const QFileInfoList files = dir.entryInfoList({"*.desktop"}, QDir::Files, QDir::Name);
        for (const QFileInfo &fileInfo : files) {
            if (seenDesktopFiles.contains(fileInfo.fileName()))
                continue;

            const DesktopApplicationEntry entry = readDesktopApplication(fileInfo.absoluteFilePath());
            if (entry.name.isEmpty())
                continue;

            seenDesktopFiles.insert(fileInfo.fileName());
            entries.append(entry);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const DesktopApplicationEntry &a,
                                                 const DesktopApplicationEntry &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });

    for (const DesktopApplicationEntry &entry : entries) {
        auto *item = new QListWidgetItem(entry.name, m_appList);
        item->setData(Qt::UserRole, entry.desktopFilePath);

        if (!entry.icon.isEmpty()) {
            QIcon icon = QIcon::fromTheme(entry.icon);
            if (icon.isNull()) {
                QFileInfo iconFile(entry.icon);
                if (iconFile.exists())
                    icon = QIcon(iconFile.absoluteFilePath());
            }
            if (!icon.isNull())
                item->setIcon(icon);
        }
    }
}

void PropertiesDialog::populateFontList()
{
    m_fontFamilyCombo->clear();
    QFontDatabase db;
    QStringList fonts = db.families();
    fonts.sort(Qt::CaseInsensitive);
    for (const QString &family : fonts)
        m_fontFamilyCombo->addItem(family);
}

PropertiesDialog::Settings PropertiesDialog::currentSettings() const
{
    Settings s;
    s.iconSize     = m_sizeSlider->value();
    s.showPreview  = m_previewCheck->isChecked();
    s.autoArrange  = m_autoArrangeCheck->isChecked();
    s.showHidden   = m_showHiddenCheck->isChecked();
    s.sortOrder    = m_sortCombo->currentIndex();
    s.showTrash    = m_showTrashCheck->isChecked();
    s.showComputer = m_showComputerCheck->isChecked();
    s.showHome     = m_showHomeCheck->isChecked();
    s.fontFamily   = m_fontFamilyCombo->currentText();
    s.fontSize     = m_fontSizeSpin->value();
    return s;
}
