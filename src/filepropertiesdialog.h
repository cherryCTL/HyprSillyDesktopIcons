#pragma once

#include <QDialog>
#include <QFileInfo>

class QLineEdit;

class FilePropertiesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FilePropertiesDialog(const QFileInfo &info, QWidget *parent = nullptr);

private:
    void loadDesktopEntryFields();
    bool saveDesktopEntryFields();

    QFileInfo m_info;
    bool m_isDesktopEntry = false;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_execEdit = nullptr;
    QLineEdit *m_iconEdit = nullptr;
};
