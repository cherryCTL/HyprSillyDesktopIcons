#include "desktopscene.h"
#include "desktopicon.h"
#include "desktopconstants.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>

// ── Drag overlay (local class, defined here so DragOverlay type is known) ─────

class DragOverlay : public QWidget {
public:
    struct Item { QPoint startPos; QPixmap pixmap; };

    DesktopIcon              *leader      = nullptr;
    QPoint                    leaderVPos; // current virtual top-left of leader
    QHash<DesktopIcon*, Item> items;

    explicit DragOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setGeometry(parent->rect());
        raise();
        show();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!leader || items.isEmpty()) return;
        QPainter p(this);
        p.setOpacity(kDragGhostOpacity);
        const QPoint delta = leaderVPos - items.value(leader).startPos;
        for (auto it = items.cbegin(); it != items.cend(); ++it)
            p.drawPixmap(it.value().startPos + delta, it.value().pixmap);
    }
};

// ── Drag feedback cursor ─────────────────────────────────────────────────────

void DesktopScene::updateDragFeedbackCursor(bool canDropOnTarget)
{
    if (!canDropOnTarget) {
        clearDragFeedbackCursor();
        return;
    }

    if (m_dragCursorActive) {
        QApplication::changeOverrideCursor(QCursor(Qt::DragMoveCursor));
    } else {
        QApplication::setOverrideCursor(QCursor(Qt::DragMoveCursor));
        m_dragCursorActive = true;
    }
}

void DesktopScene::clearDragFeedbackCursor()
{
    if (!m_dragCursorActive) return;
    QApplication::restoreOverrideCursor();
    m_dragCursorActive = false;
}

// ── Icon key / folder drop ───────────────────────────────────────────────────

static void hideDraggedIconsImmediately(const QHash<DesktopIcon*, QPoint> &dragStartPositions)
{
    for (auto *icon : dragStartPositions.keys()) {
        if (!icon->isVirtual()) {
            icon->hide();
            icon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        }
    }
}

QString DesktopScene::iconKey(DesktopIcon *icon) const
{
    if (icon->isVirtual()) {
        switch (icon->virtualType()) {
            case DesktopIcon::VirtualType::Trash:    return "__trash__";
            case DesktopIcon::VirtualType::Computer: return "__computer__";
            case DesktopIcon::VirtualType::Home:     return "__home__";
            default:                                 return "__virtual__";
        }
    }
    return icon->fileInfo().fileName();
}

DesktopIcon *DesktopScene::folderDropTargetAt(const QPoint &leaderCenter, DesktopIcon *leader) const
{
    for (auto *icon : m_icons) {
        if (icon == leader || icon->isVirtual() || !icon->fileInfo().isDir())
            continue;
        if (m_dragStartPositions.contains(icon))
            continue;
        if (icon->geometry().contains(leaderCenter))
            return icon;
    }
    return nullptr;
}

bool DesktopScene::moveIconIntoFolder(DesktopIcon *icon, const QFileInfo &targetFolder,
                                      QString *errorMessage)
{
    if (!icon || icon->isVirtual() || !targetFolder.isDir())
        return false;

    const QFileInfo sourceInfo = icon->fileInfo();
    const QString sourcePath = sourceInfo.absoluteFilePath();
    const QString destinationPath = targetFolder.absoluteFilePath() + "/" + sourceInfo.fileName();

    if (sourcePath == destinationPath) {
        if (errorMessage) {
            *errorMessage = QString("'%1' is already in this folder.").arg(sourceInfo.fileName());
        }
        return false;
    }

    if (QFileInfo::exists(destinationPath)) {
        if (errorMessage) {
            *errorMessage = QString("An item named '%1' already exists in '%2'.")
                .arg(sourceInfo.fileName(), targetFolder.fileName());
        }
        return false;
    }

    const bool moved = sourceInfo.isDir()
        ? QDir().rename(sourcePath, destinationPath)
        : QFile::rename(sourcePath, destinationPath);

    if (!moved && errorMessage) {
        *errorMessage = QString("Could not move '%1' to '%2'.")
            .arg(sourceInfo.fileName(), targetFolder.fileName());
    }

    return moved;
}

// ── Drag event handlers ──────────────────────────────────────────────────────

void DesktopScene::onIconDragStarted(DesktopIcon *icon, QPoint /*offset*/)
{
    m_dragStartPositions.clear();
    delete m_dragOverlay;
    auto *overlay   = new DragOverlay(this);
    m_dragOverlay   = overlay;
    overlay->leader = icon;

    for (auto *i : m_icons) {
        if (i->isSelected()) {
            m_dragStartPositions[i] = i->pos();
            overlay->items[i]       = { i->pos(), i->grab() }; // grab before ghost
            i->setGhost(true);
        }
    }
    overlay->leaderVPos = icon->pos();
    clearDragFeedbackCursor();
}

void DesktopScene::onIconDragging(DesktopIcon *leader, QPoint leaderVirtualPos)
{
    if (!m_dragStartPositions.contains(leader) || !m_dragOverlay) return;
    auto *overlay = static_cast<DragOverlay*>(m_dragOverlay);

    // Only redraw the union of old and new bounding rects, not the full overlay
    const QPoint oldDelta = overlay->leaderVPos - overlay->items.value(leader).startPos;
    const QPoint newDelta = leaderVirtualPos    - overlay->items.value(leader).startPos;
    QRect dirty;
    for (auto it = overlay->items.cbegin(); it != overlay->items.cend(); ++it) {
        dirty |= QRect(it.value().startPos + oldDelta, it.value().pixmap.size());
        dirty |= QRect(it.value().startPos + newDelta, it.value().pixmap.size());
    }
    overlay->leaderVPos = leaderVirtualPos;
    overlay->update(dirty.adjusted(-1, -1, 1, 1));

    // Highlight trash icon when ghost is over it
    const QPoint leaderCenter = leaderVirtualPos
                              + QPoint(leader->width() / 2, leader->height() / 2);
    DesktopIcon *targetFolderIcon = folderDropTargetAt(leaderCenter, leader);
    bool canDropOnTarget = false;
    for (auto *icon : m_icons) {
        const bool overTrash = icon->virtualType() == DesktopIcon::VirtualType::Trash
                            && !leader->isVirtual()
                            && icon->geometry().contains(leaderCenter);
        const bool overFolder = icon == targetFolderIcon;
        canDropOnTarget = canDropOnTarget || overTrash || overFolder;
        icon->setDropTarget(overTrash || overFolder);
    }
    updateDragFeedbackCursor(canDropOnTarget);
}

void DesktopScene::onIconMoved(DesktopIcon *leader, QPoint leaderVirtualPos)
{
    clearDragFeedbackCursor();

    // Clear ghost state on all dragged icons
    for (auto *icon : m_dragStartPositions.keys())
        icon->setGhost(false);

    // Destroy overlay
    delete m_dragOverlay;
    m_dragOverlay = nullptr;

    // Clear drag-over highlights on virtual icons
    for (auto *icon : m_icons)
        icon->setDropTarget(false);

    // Fallback: ensure leader is tracked
    if (!m_dragStartPositions.contains(leader))
        m_dragStartPositions[leader] = leader->pos();

    // Delta between where ghost ended up and where icon originally was
    const QPoint delta = leaderVirtualPos - m_dragStartPositions[leader];
    const QPoint leaderCenter = leaderVirtualPos
                              + QPoint(leader->width() / 2, leader->height() / 2);

    // ── Check if dropped onto the Trash icon ─────────────────────────────────
    DesktopIcon *trashIcon = nullptr;
    for (auto *icon : m_icons) {
        if (icon->virtualType() == DesktopIcon::VirtualType::Trash) {
            trashIcon = icon;
            break;
        }
    }
    if (trashIcon && !leader->isVirtual()) {
        if (trashIcon->geometry().contains(leaderCenter)) {
            // Confirm and trash all dragged icons
            QStringList names;
            for (auto *icon : m_dragStartPositions.keys()) {
                if (!icon->isVirtual())
                    names << icon->fileInfo().fileName();
            }
            const auto btn = QMessageBox::question(
                this, "Move to Recycle Bin",
                "Move " + QString::number(names.size()) + " item(s) to the Recycle Bin?",
                QMessageBox::Yes | QMessageBox::No);
            if (btn == QMessageBox::Yes) {
                hideDraggedIconsImmediately(m_dragStartPositions);
                for (auto *icon : m_dragStartPositions.keys()) {
                    if (!icon->isVirtual()) {
                        QProcess::startDetached("gio", {"trash", icon->fileInfo().absoluteFilePath()});
                        m_positions.remove(icon->fileInfo().fileName());
                    }
                }
                queueSavePositions(kSaveDebounceMs);
            }
            // restore ghost state, clear, and return
            for (auto *icon : m_dragStartPositions.keys())
                icon->setGhost(false);
            m_dragStartPositions.clear();
            return;
        }
    }

    // ── Check if dropped onto a real folder ─────────────────────────────────
    DesktopIcon *targetFolderIcon = folderDropTargetAt(leaderCenter, leader);

    if (targetFolderIcon && !leader->isVirtual()) {
        const QFileInfo targetFolder = targetFolderIcon->fileInfo();
        int movedCount = 0;
        QStringList errors;

        for (auto *icon : m_dragStartPositions.keys()) {
            if (icon->isVirtual())
                continue;

            QString errorMessage;
            if (moveIconIntoFolder(icon, targetFolder, &errorMessage)) {
                ++movedCount;
                m_positions.remove(icon->fileInfo().fileName());
            } else if (!errorMessage.isEmpty()) {
                errors << errorMessage;
            }
        }

        if (movedCount > 0)
            hideDraggedIconsImmediately(m_dragStartPositions);

        m_dragStartPositions.clear();
        queueSavePositions(kSaveDebounceMs);
        queueRefresh(10);

        if (!errors.isEmpty()) {
            QMessageBox::warning(this, "Move to Folder", errors.join("\n"));
        }

        return;
    }

    // Step 1 – free all cells belonging to every dragged icon
    for (auto *icon : m_dragStartPositions.keys()) {
        const QString name = iconKey(icon);
        for (auto it = m_occupiedCells.begin(); it != m_occupiedCells.end(); ++it) {
            if (it.value() == name) { m_occupiedCells.erase(it); break; }
        }
    }

    // Step 2 – snap each icon to nearest free cell using VIRTUAL (ghost) position
    auto snapOne = [&](DesktopIcon *icon) {
        const QString name = iconKey(icon);
        const QPoint vpos  = m_dragStartPositions.value(icon, icon->pos()) + delta;
        const auto cell    = nearestFreeCell(posToCell(vpos), "");
        m_occupiedCells[cell] = name;
        const QPoint snapped  = cellToPos(cell.first, cell.second);
        icon->move(snapped);
        m_positions[name] = snapped;
    };
    snapOne(leader);
    for (auto *icon : m_dragStartPositions.keys()) {
        if (icon != leader) snapOne(icon);
    }

    m_dragStartPositions.clear();
    queueSavePositions(20);
    if (m_autoArrange) arrangeAll();
}
