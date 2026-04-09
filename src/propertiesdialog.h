#pragma once

#include <QDialog>
#include <QFont>

class QSlider;
class QLabel;
class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;
class QLineEdit;
class QSpinBox;

class PropertiesDialog : public QDialog
{
    Q_OBJECT
public:
    struct Settings {
        int  iconSize    = 40;
        bool showPreview = true;
        bool autoArrange = false;
        bool showHidden  = false;
        int  sortOrder   = 0;   // 0=Name, 1=Type, 2=Date
        bool showTrash   = true;
        bool showComputer= false;
        bool showHome    = false;
        QString fontFamily = "MS Sans Serif";
        int  fontSize    = 8;
    };

    explicit PropertiesDialog(const Settings &s, QWidget *parent = nullptr);
    Settings currentSettings() const;

signals:
    void iconSizeChanged(int size);
    void fontChanged(const QString &family);
    void fontSizeChanged(int size);
    void applied();
    void applicationShortcutRequested(const QString &desktopFilePath);

private:
    void populateApplicationsList();
    void populateFontList();

    QSlider   *m_sizeSlider;
    QLabel    *m_sizeLabel;
    QComboBox *m_fontFamilyCombo;
    QSpinBox  *m_fontSizeSpin;
    QCheckBox *m_previewCheck;
    QCheckBox *m_autoArrangeCheck;
    QCheckBox *m_showHiddenCheck;
    QCheckBox *m_showTrashCheck;
    QCheckBox *m_showComputerCheck;
    QCheckBox *m_showHomeCheck;
    QComboBox *m_sortCombo;
    QLineEdit *m_appSearchEdit;
    QListWidget *m_appList;
    QPushButton *m_createShortcutButton;
};
