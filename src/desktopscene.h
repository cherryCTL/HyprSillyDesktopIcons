#pragma once

#include <QWidget>
#include <QFileInfo>
#include <QHash>
#include <QPoint>
#include <QFileSystemWatcher>
#include <QScreen>
#include <QRect>
#include <QByteArray>
#include <QContextMenuEvent>
#include <QSocketNotifier>
#include <QLabel>
#include <QTimer>
#include <QKeyEvent>
#include "desktopconstants.h"

class DesktopIcon;

class DesktopScene : public QWidget
{
    Q_OBJECT
public:
    enum class SortOrder { ByName, ByType, ByDate };

    explicit DesktopScene(QScreen *screen, QWidget *parent = nullptr);
    ~DesktopScene() override;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private slots:
    void refresh();
    void onIconPressed(DesktopIcon *icon, bool ctrlHeld);
    void onIconReleased(DesktopIcon *icon, bool wasDrag, bool ctrlHeld);
    void onIconDragStarted(DesktopIcon *icon, QPoint offset);
    void onIconDragging(DesktopIcon *icon, QPoint scenePos);
    void onIconMoved(DesktopIcon *icon, QPoint scenePos);
    void onOpenRequested(const QFileInfo &fileInfo);
    void onRenameRequested(DesktopIcon *icon);
    void onDeleteRequested(DesktopIcon *icon);
    void onDeleteKeys(bool permanent);
    void onFileDroppedOnTrash(const QString &filePath);
    void onNewFolder();
    void onNewDocument();
    void onOpenInTerminal();
    void onArrangeBy(SortOrder order);
    void onAutoArrangeToggled();
    void onShowHiddenToggled();
    void onShowProperties();
    void onIconContextMenuRequested(DesktopIcon *icon, QPoint scenePos);
    void closeAllMenus();

private:
    QString iconKey(DesktopIcon *icon) const;
    DesktopIcon *folderDropTargetAt(const QPoint &leaderCenter, DesktopIcon *leader) const;
    bool moveIconIntoFolder(DesktopIcon *icon, const QFileInfo &targetFolder, QString *errorMessage);
    void queueRefresh(int delayMs = 0);
    void queueSavePositions(int delayMs = 0);
    void updateDragFeedbackCursor(bool canDropOnTarget);
    void clearDragFeedbackCursor();
    DesktopIcon *selectedIcon() const;
    DesktopIcon *keyboardNavigationTarget(Qt::Key directionKey, DesktopIcon *fromIcon) const;
    void openIconFromKeyboard(DesktopIcon *icon);
    void loadPositions();
    void savePositions();
    QString configPath() const;
    void arrangeAll();
    void applyIconSize(int newSize); // resize + remap positions to new grid
    void applyFont(const QString &family, int size); // update font on all icons
    void setupHyprlandIPC(); // socket2 connection + event parsing (extracted to desktopscene_ipc.cpp)
    QRect workArea() const;    // screen work area in scene-local coordinates
    QRect menuBoundary() const; // popup boundary respecting panel reserved areas

    // Grid helpers
    QPoint cellToPos(int col, int row) const;
    QPair<int,int> posToCell(QPoint pos) const;
    QPair<int,int> nextFreeCell() const;
    QPair<int,int> nearestFreeCell(QPair<int,int> target, const QString &skipName) const;
    int maxCols() const;
    int maxRows() const;

    // Refresh pipeline helpers
    QFileInfoList scanDesktopFiles() const;
    void removeDeletedIcons(const QSet<QString>& expectedNames);
    void rebuildGridPositions();
    void addNewIcons(const QFileInfoList& entries);
    void connectIconSignals(DesktopIcon* icon, bool isVirtual);
    void syncVirtualIcons();
    void applyAutoArrange();
    void updateIconPreview();

    QList<DesktopIcon *> m_icons;
    QHash<QString, QPoint>          m_positions;          // filename -> snapped pixel pos
    QHash<QPair<int,int>, QString>  m_occupiedCells;      // grid cell -> filename
    QHash<DesktopIcon*, QPoint>     m_dragStartPositions; // for multi-icon drag
    QFileSystemWatcher *m_watcher;
    QScreen            *m_screen    = nullptr;
    QWidget            *m_dragOverlay = nullptr; // ghost overlay raised above icons
    QString             m_desktopPath;
    SortOrder           m_sortOrder   = SortOrder::ByName;
    QPoint              m_lastContextMenuPos;
    QTimer             *m_refreshTimer = nullptr;
    QTimer             *m_saveTimer = nullptr;
    bool                m_autoArrange = false;
    bool                m_showHidden  = false;
    bool                m_showTrash   = true;
    bool                m_showComputer = false;
    bool                m_showHome    = false;
    int                 m_iconSize    = 40;
    bool                m_showPreview = true;
    QString             m_fontFamily  = "MS Sans Serif";
    int                 m_fontSize    = 8;

    QWidget            *m_ctxMenu        = nullptr;
    QWidget            *m_ctxArrangeMenu = nullptr;
    QWidget            *m_iconCtxMenu    = nullptr;
    QWidget            *m_newMenu        = nullptr;
    QWidget            *m_systemMenu     = nullptr;

    // Hyprland IPC listener
    QSocketNotifier    *m_hypNotifier  = nullptr;
    int                 m_hypSockFd    = -1;
    QString             m_currentWorkspace;
    // Panel reserved areas (queried from Hyprland, [left,top,right,bottom])
    int m_reservedLeft   = 0;
    int m_reservedTop    = 0;
    int m_reservedRight  = 0;
    int m_reservedBottom = 0;
    QByteArray          m_hypBuf;

    // Rubber-band selection
    bool   m_rubberBanding    = false;
    bool   m_rubberCtrl       = false;
    bool   m_cursorOnSurface  = true;  // false while cursor is outside our wl_surface
    bool   m_dragCursorActive = false;
    QChar  m_lastTypeSelectChar;
    int    m_lastTypeSelectMatch = -1;
    QPoint m_rubberOrigin;
    QRect  m_rubberRect;

    // In-scene tooltip - top-level Qt::ToolTip window so it appears above layer-shell surfaces
    QWidget *m_tooltipWindow = nullptr;
    QLabel  *m_tooltipLabel  = nullptr;
    QTimer  *m_tooltipTimer  = nullptr;

    int iconW() const { return m_iconSize + kIconExtraWidth; }
    int iconH() const { return m_iconSize + kIconExtraHeight; }
    int cellW() const { return iconW() + kGridMarginX; }
    int cellH() const { return iconH() + kGridMarginY; }
};
