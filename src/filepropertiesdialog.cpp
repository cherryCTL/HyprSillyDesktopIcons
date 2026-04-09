#include "filepropertiesdialog.h"
#include "desktopconstants.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

static QString humanSize(qint64 bytes)
{
    const QLocale locale = QLocale::system();
    if (bytes < 1024LL)
        return QString("%1 bytes").arg(locale.toString(bytes));
    if (bytes < 1024LL * 1024)
        return QString("%1 KB (%2 bytes)")
            .arg(locale.toString(bytes / 1024.0, 'f', 1))
            .arg(locale.toString(bytes));
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MB (%2 bytes)")
            .arg(locale.toString(bytes / (1024.0 * 1024), 'f', 1))
            .arg(locale.toString(bytes));
    return QString("%1 GB (%2 bytes)")
        .arg(locale.toString(bytes / (1024.0 * 1024 * 1024), 'f', 2))
        .arg(locale.toString(bytes));
}

FilePropertiesDialog::FilePropertiesDialog(const QFileInfo &info, QWidget *parent)
    : QDialog(parent, Qt::Dialog)
    , m_info(info)
    , m_isDesktopEntry(info.suffix().compare("desktop", Qt::CaseInsensitive) == 0)
{
    setWindowTitle("Properties");
    setObjectName("PropertiesDialog");
    setFixedWidth(kPropertiesDialogWidth);
    setSizeGripEnabled(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *banner = new QFrame(this);
    banner->setObjectName("PropBanner");
    banner->setFixedHeight(kPropertiesDialogBannerHeight);
    {
        auto *bl = new QHBoxLayout(banner);
        bl->setContentsMargins(10, 6, 10, 6);
        bl->setSpacing(10);

        QFileIconProvider prov;
        auto *iconLbl = new QLabel(banner);
        iconLbl->setPixmap(prov.icon(info).pixmap(32, 32));

        auto *nameLbl = new QLabel(info.fileName(), banner);
        nameLbl->setObjectName("PropBannerText");
        nameLbl->setWordWrap(true);

        bl->addWidget(iconLbl);
        bl->addWidget(nameLbl, 1);
    }
    root->addWidget(banner);

    auto *content = new QWidget(this);
    content->setObjectName("PropContent");
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(kPropertiesDialogContentMargins, 6, kPropertiesDialogContentMargins, 6);
    contentLayout->setSpacing(6);

    QMimeDatabase mimeDb;
    QString typeStr;
    if (info.isDir()) {
        typeStr = "Folder";
    } else {
        const auto mime = mimeDb.mimeTypeForFile(info);
        typeStr = mime.comment().isEmpty() ? mime.name() : mime.comment();
    }

    auto *infoGroup = new QGroupBox("General", content);
    auto *form = new QFormLayout(infoGroup);
    form->setContentsMargins(kPropertiesDialogContentMargins, 6, kPropertiesDialogContentMargins, 6);
    form->setSpacing(3);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto addRow = [&](const QString &label, const QString &value) {
        auto *v = new QLabel(value, infoGroup);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setWordWrap(true);
        form->addRow(label, v);
    };

    addRow("Type:",     typeStr);
    addRow("Location:", info.absolutePath());
    if (!info.isDir())
        addRow("Size:", humanSize(info.size()));
    addRow("Created:",  QLocale::system().toString(info.birthTime(),     QLocale::ShortFormat));
    addRow("Modified:", QLocale::system().toString(info.lastModified(),  QLocale::ShortFormat));
    contentLayout->addWidget(infoGroup);

    if (m_isDesktopEntry) {
        auto *launcherGroup = new QGroupBox("Desktop Entry", content);
        auto *launcherForm = new QFormLayout(launcherGroup);
        launcherForm->setContentsMargins(8, 6, 8, 6);
        launcherForm->setSpacing(4);
        launcherForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_nameEdit = new QLineEdit(launcherGroup);
        m_execEdit = new QLineEdit(launcherGroup);
        m_iconEdit = new QLineEdit(launcherGroup);

        launcherForm->addRow("Name:", m_nameEdit);
        launcherForm->addRow("Exec:", m_execEdit);
        launcherForm->addRow("Icon:", m_iconEdit);
        contentLayout->addWidget(launcherGroup);

        loadDesktopEntryFields();
    }

    auto *permGroup = new QGroupBox("Permissions", content);
    {
        auto *pg = new QGridLayout(permGroup);
        pg->setContentsMargins(8, 6, 8, 6);
        pg->setSpacing(0);
        pg->setColumnMinimumWidth(0, 52);

        const char *headers[] = { "Read", "Write", "Exec" };
        for (int c = 0; c < 3; ++c) {
            auto *h = new QLabel(headers[c], permGroup);
            h->setAlignment(Qt::AlignCenter);
            pg->addWidget(h, 0, c + 1);
        }

        struct PermRow { const char *label; QFile::Permission r, w, x; };
        const PermRow prows[] = {
            { "Owner",  QFile::ReadOwner,  QFile::WriteOwner,  QFile::ExeOwner  },
            { "Group",  QFile::ReadGroup,  QFile::WriteGroup,  QFile::ExeGroup  },
            { "Others", QFile::ReadOther,  QFile::WriteOther,  QFile::ExeOther  },
        };

        const QFile::Permissions perms = QFile::permissions(info.absoluteFilePath());
        for (int i = 0; i < 3; ++i) {
            pg->addWidget(new QLabel(prows[i].label, permGroup), i + 1, 0);
            const QFile::Permission cols[] = { prows[i].r, prows[i].w, prows[i].x };
            for (int j = 0; j < 3; ++j) {
                const bool on = bool(perms & cols[j]);
                auto *lbl = new QLabel(on ? "✓" : "–", permGroup);
                lbl->setAlignment(Qt::AlignCenter);
                lbl->setStyleSheet(on
                    ? "color: #00c800; font-weight: bold;"
                    : "color: #808080;");
                pg->addWidget(lbl, i + 1, j + 1);
            }
        }
    }
    contentLayout->addWidget(permGroup);

    QDialogButtonBox::StandardButtons buttons = QDialogButtonBox::Ok;
    if (m_isDesktopEntry)
        buttons = QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel;

    auto *btnBox = new QDialogButtonBox(buttons, content);
    if (m_isDesktopEntry) {
        connect(btnBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() {
            saveDesktopEntryFields();
        });
        connect(btnBox, &QDialogButtonBox::accepted, this, [this]() {
            if (saveDesktopEntryFields())
                accept();
        });
        connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    } else {
        connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    }
    contentLayout->addWidget(btnBox);

    root->addWidget(content);
}

void FilePropertiesDialog::loadDesktopEntryFields()
{
    if (!m_isDesktopEntry || !m_nameEdit || !m_execEdit || !m_iconEdit)
        return;

    QFile file(m_info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream in(&file);
    bool inDesktopEntry = false;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith('[')) {
            inDesktopEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry || line.startsWith('#') || !line.contains('='))
            continue;

        const int split = line.indexOf('=');
        const QString key = line.left(split).trimmed();
        const QString value = line.mid(split + 1).trimmed();

        if (key == "Name") m_nameEdit->setText(value);
        else if (key == "Exec") m_execEdit->setText(value);
        else if (key == "Icon") m_iconEdit->setText(value);
    }
}

bool FilePropertiesDialog::saveDesktopEntryFields()
{
    if (!m_isDesktopEntry || !m_nameEdit || !m_execEdit || !m_iconEdit)
        return true;

    if (m_nameEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "Desktop Entry", "Name cannot be empty.");
        return false;
    }

    QFile file(m_info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Desktop Entry", "Could not open the launcher for editing.");
        return false;
    }
    QStringList lines;
    QTextStream in(&file);
    while (!in.atEnd())
        lines << in.readLine();
    file.close();

    bool inDesktopEntry = false;
    bool foundName = false;
    bool foundExec = false;
    bool foundIcon = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.startsWith('[')) {
            if (inDesktopEntry)
                break;
            inDesktopEntry = (trimmed == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry || trimmed.startsWith('#') || !trimmed.contains('='))
            continue;

        const int split = trimmed.indexOf('=');
        const QString key = trimmed.left(split).trimmed();
        if (key == "Name") {
            lines[i] = "Name=" + m_nameEdit->text().trimmed();
            foundName = true;
        } else if (key == "Exec") {
            lines[i] = "Exec=" + m_execEdit->text().trimmed();
            foundExec = true;
        } else if (key == "Icon") {
            lines[i] = "Icon=" + m_iconEdit->text().trimmed();
            foundIcon = true;
        }
    }

    int desktopEntryEnd = -1;
    inDesktopEntry = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed == "[Desktop Entry]") {
            inDesktopEntry = true;
            desktopEntryEnd = i + 1;
            continue;
        }
        if (inDesktopEntry && trimmed.startsWith('[')) {
            desktopEntryEnd = i;
            break;
        }
        if (inDesktopEntry)
            desktopEntryEnd = i + 1;
    }

    if (desktopEntryEnd < 0) {
        lines.prepend("[Desktop Entry]");
        desktopEntryEnd = 1;
    }
    if (!foundName) lines.insert(desktopEntryEnd++, "Name=" + m_nameEdit->text().trimmed());
    if (!foundExec) lines.insert(desktopEntryEnd++, "Exec=" + m_execEdit->text().trimmed());
    if (!foundIcon) lines.insert(desktopEntryEnd++, "Icon=" + m_iconEdit->text().trimmed());

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Desktop Entry", "Could not save the launcher changes.");
        return false;
    }

    QTextStream out(&file);
    for (const QString &line : lines)
        out << line << '\n';
    file.close();

    return true;
}
