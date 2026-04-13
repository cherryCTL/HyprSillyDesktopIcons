#include "desktopscene.h"
#include "desktopicon.h"
#include "launcherutil.h"
#include "inlinemenu.h"
#include "desktopconstants.h"

#include <cmath>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QProcess>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QMenu>
#include <QContextMenuEvent>
#include <qlogging.h>
#include <QWheelEvent>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include "propertiesdialog.h"
#include "filepropertiesdialog.h"
#include "desktopwindow.h"

#include <functional>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

DesktopScene::DesktopScene(QScreen *screen, QWidget *parent)
    : QWidget(parent)
    , m_screen(screen)
    , m_watcher(new QFileSystemWatcher(this))
{
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setObjectName("DesktopScene");
    setFocusPolicy(Qt::StrongFocus);

    m_desktopPath = QDir::homePath() + "/Desktop";
    QDir().mkpath(m_desktopPath);

    loadPositions();

    // Load persistent UI state
    {
        QSettings cfg(configPath(), QSettings::IniFormat);
        m_autoArrange  = cfg.value("ui/autoArrange",  false).toBool();
        m_showHidden   = cfg.value("ui/showHidden",   false).toBool();
        m_iconSize     = cfg.value("ui/iconSize",     kDefaultIconSize).toInt();
        m_showPreview  = cfg.value("ui/showPreview",  true).toBool();
        m_showTrash    = cfg.value("ui/showTrash",    true).toBool();
        m_showComputer = cfg.value("ui/showComputer", false).toBool();
        m_showHome     = cfg.value("ui/showHome",     false).toBool();
        m_fontFamily   = cfg.value("ui/fontFamily",   "MS Sans Serif").toString();
        m_fontSize     = cfg.value("ui/fontSize",     kDefaultFontSize).toInt();
    }

    // Query Hyprland for the actual reserved (panel) areas of this screen.
    // QScreen::availableGeometry() is unreliable on Wayland/Hyprland.
    // hyprctl monitors -j returns reserved as [left, top, right, bottom].
    {
        QProcess hyprctl;
        hyprctl.start("hyprctl", {"monitors", "-j"});
        if (hyprctl.waitForFinished(kHyprctlMonitorsTimeoutMs)) {
            if (hyprctl.exitCode() != 0) {
                qWarning() << "[IPC] hyprctl monitors -j failed (exit"
                           << hyprctl.exitCode() << "):"
                           << hyprctl.readAllStandardError().trimmed();
            } else {
                const QJsonArray mons =
                    QJsonDocument::fromJson(hyprctl.readAllStandardOutput()).array();
                const QRect full = m_screen ? m_screen->geometry() : QRect();
                for (const QJsonValue &v : mons) {
                    const QJsonObject m = v.toObject();
                    if (m["x"].toInt() == full.x() && m["y"].toInt() == full.y()
                        && m["width"].toInt() == full.width()
                        && m["height"].toInt() == full.height()) {
                        const QJsonArray r = m["reserved"].toArray();
                        // Hyprland reserved: [left, top, right, bottom]
                        m_reservedLeft   = r.size() > 0 ? r[0].toInt() : 0;
                        m_reservedTop    = r.size() > 1 ? r[1].toInt() : 0;
                        m_reservedRight  = r.size() > 2 ? r[2].toInt() : 0;
                        m_reservedBottom = r.size() > 3 ? r[3].toInt() : 0;
                        break;
                    }
                }
            }
        } else {
            qWarning() << "[IPC] hyprctl monitors -j timed out";
        }
    }

    // Defer refresh until after the widget has been given its real size by the
    // layout (width/height are still 0 in the constructor).
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &DesktopScene::refresh);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() { savePositions(); });

    queueRefresh();

    m_watcher->addPath(m_desktopPath);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        queueRefresh(kDirChangeDebounceMs);
        // QFileSystemWatcher silently drops watches if the directory is
        // removed and recreated (e.g. rm -rf ~/Desktop && mkdir ~/Desktop).
        // Re-arm the watch so we don't lose events after a directory recreation.
        if (!m_watcher->directories().contains(m_desktopPath))
            m_watcher->addPath(m_desktopPath);
    });

    // In-surface menus (child widgets, same wl_surface, no popup grab)
    m_ctxArrangeMenu = new InlineMenu(this);
    m_ctxMenu        = new InlineMenu(this);
    m_iconCtxMenu    = new InlineMenu(this);
    m_newMenu        = new InlineMenu(this);
    m_systemMenu     = new InlineMenu(this);

    // Tooltip - top-level Qt::ToolTip window so Hyprland places it above layer-shell surfaces.
    m_tooltipWindow = new QWidget(nullptr,
        Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    m_tooltipWindow->setAttribute(Qt::WA_TranslucentBackground, false);
    m_tooltipWindow->setAttribute(Qt::WA_ShowWithoutActivating);
    m_tooltipWindow->setStyleSheet(
        "background: #FFFFE1;"
        "color: black;"
        "border: 1px solid #000000;");

    m_tooltipLabel = new QLabel(m_tooltipWindow);
    m_tooltipLabel->setStyleSheet(
        "background: transparent;"
        "color: black;"
        "padding: 2px 5px;"
        "font-size: 8pt;");

    auto *tipLayout = new QHBoxLayout(m_tooltipWindow);
    tipLayout->setContentsMargins(0, 0, 0, 0);
    tipLayout->addWidget(m_tooltipLabel);

    m_tooltipTimer = new QTimer(this);
    m_tooltipTimer->setSingleShot(true);
    m_tooltipTimer->setInterval(kTooltipShowDelay);
    connect(m_tooltipTimer, &QTimer::timeout, this, [this]() {
        if (m_tooltipLabel->text().isEmpty()) return;
        // Position kTooltipOffsetY below cursor in global coords
        QPoint global = QCursor::pos() + QPoint(0, kTooltipOffsetY);
        m_tooltipWindow->adjustSize();
        // Keep on screen
        QScreen *scr = m_screen ? m_screen : QGuiApplication::primaryScreen();
        const QRect sg = scr->geometry();
        if (global.x() + m_tooltipWindow->width() > sg.right())
            global.setX(sg.right() - m_tooltipWindow->width() - kTooltipScreenMargin);
        if (global.x() < sg.left()) global.setX(sg.left() + kTooltipScreenMargin);
        if (global.y() + m_tooltipWindow->height() > sg.bottom())
            global.setY(QCursor::pos().y() - m_tooltipWindow->height() - 4);
        m_tooltipWindow->move(global);
        m_tooltipWindow->show();
    });

    // Hyprland IPC, desktopscene_ipc.cpp
    setupHyprlandIPC();

    // Accept drops from external drag (file manager to desktop)
    setAcceptDrops(true);
}

bool DesktopScene::eventFilter(QObject *obj, QEvent *e)
{
    auto *icon = qobject_cast<DesktopIcon *>(obj);
    if (!icon) return false;

    switch (e->type()) {
    case QEvent::Enter:
        m_tooltipLabel->setText(icon->tipText());
        m_tooltipTimer->start();
        break;
    case QEvent::MouseMove: {
        if (!m_tooltipWindow->isVisible()) break;
        // Follow cursor in global coords
        QPoint global = QCursor::pos() + QPoint(0, kTooltipOffsetY);
        m_tooltipWindow->adjustSize();
        QScreen *scr = m_screen ? m_screen : QGuiApplication::primaryScreen();
        const QRect sg = scr->geometry();
        if (global.x() + m_tooltipWindow->width() > sg.right())
            global.setX(sg.right() - m_tooltipWindow->width() - kTooltipScreenMargin);
        if (global.x() < sg.left()) global.setX(sg.left() + kTooltipScreenMargin);
        if (global.y() + m_tooltipWindow->height() > sg.bottom())
            global.setY(QCursor::pos().y() - m_tooltipWindow->height() - 4);
        m_tooltipWindow->move(global);
        break;
    }
    case QEvent::Leave:
    case QEvent::MouseButtonPress:
        m_tooltipTimer->stop();
        m_tooltipWindow->hide();
        break;
    default:
        break;
    }
    return false;
}

DesktopScene::~DesktopScene()
{
    savePositions();
    delete m_tooltipWindow;
    if (m_hypSockFd >= 0) {
        ::close(m_hypSockFd);
        m_hypSockFd = -1;
    }
}

QString DesktopScene::configPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/icon-positions.ini";
}

void DesktopScene::queueRefresh(int delayMs)
{
    if (!m_refreshTimer) return;
    m_refreshTimer->start(qMax(0, delayMs));
}

void DesktopScene::queueSavePositions(int delayMs)
{
    if (!m_saveTimer) return;
    m_saveTimer->start(qMax(0, delayMs));
}

// Drag feedback, desktopscene_dragdrop.cpp

DesktopIcon *DesktopScene::selectedIcon() const
{
    for (auto *icon : m_icons) {
        if (icon->isSelected())
            return icon;
    }
    return nullptr;
}

DesktopIcon *DesktopScene::keyboardNavigationTarget(Qt::Key directionKey, DesktopIcon *fromIcon) const
{
    auto centerOf = [](DesktopIcon *icon) {
        return icon->geometry().center();
    };

    auto sortedIcons = m_icons;
    std::sort(sortedIcons.begin(), sortedIcons.end(), [](DesktopIcon *a, DesktopIcon *b) {
        if (a->y() != b->y()) return a->y() < b->y();
        return a->x() < b->x();
    });

    if (!fromIcon)
        return sortedIcons.isEmpty() ? nullptr : sortedIcons.front();

    const QPoint fromCenter = centerOf(fromIcon);
    DesktopIcon *best = nullptr;
    int bestPrimary = std::numeric_limits<int>::max();
    int bestSecondary = std::numeric_limits<int>::max();

    for (auto *candidate : m_icons) {
        if (candidate == fromIcon)
            continue;

        const QPoint delta = centerOf(candidate) - fromCenter;
        int primary = 0;
        int secondary = 0;
        bool valid = false;

        switch (directionKey) {
            case Qt::Key_Left:
                valid = delta.x() < 0;
                primary = -delta.x();
                secondary = qAbs(delta.y());
                break;
            case Qt::Key_Right:
                valid = delta.x() > 0;
                primary = delta.x();
                secondary = qAbs(delta.y());
                break;
            case Qt::Key_Up:
                valid = delta.y() < 0;
                primary = -delta.y();
                secondary = qAbs(delta.x());
                break;
            case Qt::Key_Down:
                valid = delta.y() > 0;
                primary = delta.y();
                secondary = qAbs(delta.x());
                break;
            default:
                break;
        }

        if (!valid)
            continue;

        if (!best || primary < bestPrimary || (primary == bestPrimary && secondary < bestSecondary)) {
            best = candidate;
            bestPrimary = primary;
            bestSecondary = secondary;
        }
    }

    return best ? best : fromIcon;
}

void DesktopScene::openIconFromKeyboard(DesktopIcon *icon)
{
    if (!icon)
        return;

    if (icon->isVirtual()) {
        QString uri;
        switch (icon->virtualType()) {
            case DesktopIcon::VirtualType::Trash:    uri = "trash:///"; break;
            case DesktopIcon::VirtualType::Computer: uri = "computer:///"; break;
            case DesktopIcon::VirtualType::Home:     uri = QDir::homePath(); break;
            default: break;
        }
        if (!uri.isEmpty())
            LauncherUtil::startDetachedWindowedProgram("gio", {"open", uri});
        return;
    }

    onOpenRequested(icon->fileInfo());
}

// iconKey, folderDropTargetAt, moveIconIntoFolder, desktopscene_dragdrop.cpp

void DesktopScene::loadPositions()
{
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.beginGroup("positions");
    for (const QString &key : cfg.childKeys()) {
        m_positions[key] = cfg.value(key).toPoint();
    }
    cfg.endGroup();
}

void DesktopScene::savePositions()
{
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.beginGroup("positions");
    cfg.remove(""); // clear old
    for (auto it = m_positions.cbegin(); it != m_positions.cend(); ++it) {
        cfg.setValue(it.key(), it.value());
    }
    cfg.endGroup();
}

// ── Grid helpers ─────────────────────────────────────────────────────────────

QRect DesktopScene::workArea() const
{
    if (m_screen) {
        const QRect avail = m_screen->availableGeometry();
        const QRect full  = m_screen->geometry();
        return avail.translated(-full.topLeft()); // scene-local coordinates
    }
    const int w = width()  > 0 ? width()  : 1280;
    const int h = height() > 0 ? height() : 720;
    return QRect(0, 0, w, h);
}

// Returns the scene-local rect within which popups must stay (subtracts panel
// reserved areas queried from Hyprland at startup).
QRect DesktopScene::menuBoundary() const
{
    const int w = width()  > 0 ? width()  : 1280;
    const int h = height() > 0 ? height() : 720;
    return QRect(
        m_reservedLeft,
        m_reservedTop,
        w - m_reservedLeft - m_reservedRight,
        h - m_reservedTop  - m_reservedBottom
    );
}

// ── Grid helpers moved to desktopscene_grid.cpp ──

// ── Core ─────────────────────────────────────────────────────────────────────

void DesktopScene::applyFont(const QString &family, int size)
{
    m_fontFamily = family;
    m_fontSize = size;
    for (auto *icon : m_icons) {
        icon->setLabelFont(family, size);
    }
}

// ── Refresh pipeline ────────────────────────────────────────────────────
// refresh() reads the Desktop folder, adds/removes icons, and syncs
// virtual entries (Trash, Computer, Home). Split into small steps so it's
// easier to follow and debug.

QFileInfoList DesktopScene::scanDesktopFiles() const
{
    QDir dir(m_desktopPath);
    QDir::SortFlags sortFlags;
    switch (m_sortOrder) {
        case SortOrder::ByType: sortFlags = QDir::Type; break;
        case SortOrder::ByDate: sortFlags = QDir::Time; break;
        default:                sortFlags = QDir::Name; break;
    }
    if (m_autoArrange) sortFlags |= QDir::DirsFirst;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (m_showHidden) filters |= QDir::Hidden;
    return dir.entryInfoList(filters, sortFlags);
}

void DesktopScene::removeDeletedIcons(const QSet<QString>& expectedNames)
{
    for (auto it = m_icons.begin(); it != m_icons.end(); ) {
        DesktopIcon *icon = *it;
        // Virtual icons are never removed by refresh
        if (icon->isVirtual()) { ++it; continue; }
        if (!expectedNames.contains(icon->fileInfo().fileName())) {
            m_icons.erase(it);
            icon->deleteLater();
        } else {
            ++it;
        }
    }
}

void DesktopScene::rebuildGridPositions()
{
    // Rebuild occupied cells from the icons that still exist. This prevents
    // stale occupancy from removed/renamed entries from pushing a freshly
    // re-added icon into a neighboring cell during the same refresh cycle.
    m_occupiedCells.clear();
    if (!m_autoArrange) {
        for (auto *icon : m_icons) {
            const QString name = iconKey(icon);
            const auto targetCell = posToCell(m_positions.value(name, icon->pos()));
            const auto finalCell = nearestFreeCell(targetCell, name);
            const QPoint pos = cellToPos(finalCell.first, finalCell.second);
            m_occupiedCells[finalCell] = name;
            m_positions[name] = pos;
            icon->move(pos);
        }
    }
}

void DesktopScene::connectIconSignals(DesktopIcon* icon, bool isVirtual)
{
    connect(icon, &DesktopIcon::pressed,       this, &DesktopScene::onIconPressed);
    connect(icon, &DesktopIcon::released,      this, &DesktopScene::onIconReleased);
    connect(icon, &DesktopIcon::dragStarted,   this, &DesktopScene::onIconDragStarted);
    connect(icon, &DesktopIcon::dragging,      this, &DesktopScene::onIconDragging);
    connect(icon, &DesktopIcon::moved,         this, &DesktopScene::onIconMoved);
    connect(icon, &DesktopIcon::iconContextMenuRequested, this, &DesktopScene::onIconContextMenuRequested);
    if (isVirtual) {
        connect(icon, &DesktopIcon::fileDropped, this, &DesktopScene::onFileDroppedOnTrash);
    } else {
        connect(icon, &DesktopIcon::openRequested,  this, &DesktopScene::onOpenRequested);
        connect(icon, &DesktopIcon::renameRequested,this, &DesktopScene::onRenameRequested);
        connect(icon, &DesktopIcon::deleteRequested,this, &DesktopScene::onDeleteRequested);
    }
    icon->installEventFilter(this);
    icon->setLabelFont(m_fontFamily, m_fontSize);
}

void DesktopScene::addNewIcons(const QFileInfoList& entries)
{
    // Build current name set for collision detection
    QSet<QString> currentNames;
    for (auto *icon : m_icons)
        currentNames.insert(iconKey(icon));

    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        if (currentNames.contains(name))
            continue;  // already present

        auto *icon = new DesktopIcon(fi, m_iconSize, m_showPreview, this);
        QPair<int,int> cell;
        if (!m_autoArrange && m_positions.contains(name)) {
            cell = nearestFreeCell(posToCell(m_positions[name]), name);
        } else {
            cell = nextFreeCell();
        }

        m_occupiedCells[cell] = name;
        const QPoint pos = cellToPos(cell.first, cell.second);
        m_positions[name] = pos;
        icon->move(pos);
        icon->show();

        connectIconSignals(icon, false);
        m_icons.append(icon);
        currentNames.insert(name);
    }
}

void DesktopScene::syncVirtualIcons()
{
    auto hasVirtual = [&](DesktopIcon::VirtualType type) {
        for (auto *icon : m_icons)
            if (icon->isVirtual() && icon->virtualType() == type) return true;
        return false;
    };

    auto ensureVirtual = [&](DesktopIcon::VirtualType type, const QString &key,
                             bool shouldShow) {
        if (shouldShow && !hasVirtual(type)) {
            auto *icon = new DesktopIcon(type, m_iconSize, this);
            QPair<int,int> cell;
            if (m_positions.contains(key))
                cell = nearestFreeCell(posToCell(m_positions[key]), key);
            else
                cell = nextFreeCell();
            m_occupiedCells[cell] = key;
            const QPoint pos = cellToPos(cell.first, cell.second);
            m_positions[key] = pos;
            icon->move(pos);
            icon->show();
            connectIconSignals(icon, true);
            m_icons.append(icon);
        } else if (!shouldShow && hasVirtual(type)) {
            for (auto it = m_icons.begin(); it != m_icons.end(); ++it) {
                if ((*it)->isVirtual() && (*it)->virtualType() == type) {
                    m_positions.remove(key);
                    (*it)->deleteLater();
                    m_icons.erase(it);
                    break;
                }
            }
        }
    };

    ensureVirtual(DesktopIcon::VirtualType::Trash,    "__trash__",    m_showTrash);
    ensureVirtual(DesktopIcon::VirtualType::Computer, "__computer__", m_showComputer);
    ensureVirtual(DesktopIcon::VirtualType::Home,     "__home__",     m_showHome);
}

void DesktopScene::applyAutoArrange()
{
    m_positions.clear();
    m_occupiedCells.clear();
    int col = 0, row = 0;
    for (auto *icon : m_icons) {
        const QString name = iconKey(icon);
        m_occupiedCells[{col, row}] = name;
        const QPoint pos = cellToPos(col, row);
        m_positions[name] = pos;
        icon->move(pos);
        // Advance to next cell
        if (++row >= maxRows()) { row = 0; ++col; }
    }
}

void DesktopScene::updateIconPreview()
{
    for (auto *icon : m_icons) {
        if (!icon->isVirtual())
            icon->updateSettings(m_iconSize, m_showPreview);
    }
}

void DesktopScene::refresh()
{
    // Guard against re-entrant calls (e.g. QFileSystemWatcher firing during
    // a rename operation that's already inside refresh()).
    static bool inRefresh = false;
    if (inRefresh)
        return;
    inRefresh = true;

    // Don't refresh while extraction is running — the file watcher will
    // fire as new files are created, which would conflict with the
    // extraction process.
    if (m_extracting) {
        inRefresh = false;
        return;
    }

    // Close and hide all context menus BEFORE touching the scene.
    // This prevents crashes when a menu is open during a filesystem
    // change (e.g. rename) that triggers this refresh.
    closeAllMenus();
    if (m_ctxMenu)        static_cast<InlineMenu*>(m_ctxMenu)->hide();
    if (m_iconCtxMenu)    static_cast<InlineMenu*>(m_iconCtxMenu)->hide();
    if (m_ctxArrangeMenu) static_cast<InlineMenu*>(m_ctxArrangeMenu)->hide();
    if (m_newMenu)        static_cast<InlineMenu*>(m_newMenu)->hide();
    if (m_systemMenu)     static_cast<InlineMenu*>(m_systemMenu)->hide();

    clearDragFeedbackCursor();
    m_lastTypeSelectChar = QChar();
    m_lastTypeSelectMatch = -1;

    const auto entries = scanDesktopFiles();

    // Build the expected set of file names
    QSet<QString> expectedNames;
    for (const QFileInfo &fi : entries)
        expectedNames.insert(fi.fileName());

    removeDeletedIcons(expectedNames);
    rebuildGridPositions();
    addNewIcons(entries);
    syncVirtualIcons();

    if (m_autoArrange)
        applyAutoArrange();

    updateIconPreview();
    inRefresh = false;
}

// onIconMoved, desktopscene_dragdrop.cpp

void DesktopScene::onOpenRequested(const QFileInfo &fileInfo)
{
    const QString path = fileInfo.absoluteFilePath();
    if (path.isEmpty()) return;

    const QString ext = fileInfo.suffix().toLower();
    const static QSet<QString> textExts = {
        "txt", "md", "log", "cfg", "conf", "ini", "csv", "json", "xml", "yaml", "yml",
        "toml", "sh", "bash", "zsh", "py", "js", "ts", "html", "css", "c", "cpp", "h",
        "hpp", "java", "rb", "go", "rs", "php", "sql"
    };

    if (textExts.contains(ext)) {
        // Helper: tokenize Exec= line respecting shell-like quoting
        auto tokenize = [](const QString &cmd) -> QStringList {
            QStringList tokens;
            QString token;
            bool inSingle = false, inDouble = false;
            for (int i = 0; i < cmd.size(); ++i) {
                const QChar c = cmd[i];
                if (inSingle) {
                    if (c == '\'') inSingle = false;
                    else token += c;
                } else if (inDouble) {
                    if (c == '"') inDouble = false;
                    else token += c;
                } else {
                    if (c == '\'') inSingle = true;
                    else if (c == '"') inDouble = true;
                    else if (c.isSpace()) { if (!token.isEmpty()) { tokens << token; token.clear(); } }
                    else token += c;
                }
            }
            if (!token.isEmpty()) tokens << token;
            return tokens;
        };

        // 1) Try system default handler for text/plain
        QProcess mime;
        mime.start("xdg-mime", {"query", "default", "text/plain"});
        if (mime.waitForFinished(kMimeQueryTimeoutMs)) {
            QString handler = QString::fromLocal8Bit(mime.readAllStandardOutput()).trimmed();
            if (!handler.isEmpty()) {
                const QString lower = handler.toLower();
                const QSet<QString> browserKeywords = {
                    "firefox", "chromium", "chrome", "chromium-browser",
                    "google-chrome", "epiphany", "midori", "netsurf",
                    "falkon", "konqueror", "waterfox", "brave"
                };
                bool isBrowser = false;
                for (const auto &kw : browserKeywords)
                    if (lower.contains(kw)) { isBrowser = true; break; }

                if (!isBrowser) {
                    QStringList appDirs;
                    const QString dataHome = qgetenv("XDG_DATA_HOME").isEmpty()
                        ? QDir::homePath() + "/.local/share"
                        : QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
                    appDirs << dataHome + "/applications";
                    for (const QString &d : QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS")).split(':', Qt::SkipEmptyParts))
                        appDirs << d + "/applications";

                    for (const QString &dir : appDirs) {
                        QFile f(dir + "/" + handler);
                        if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
                        const QString content = QString::fromLocal8Bit(f.readAll());
                        f.close();

                        QString execLine;
                        for (const QString &line : content.split('\n')) {
                            if (line.startsWith("Exec=")) { execLine = line.mid(5).trimmed(); break; }
                        }
                        if (!execLine.isEmpty()) {
                            // Remove desktop field codes (%i, %c, %k, %U, %f, %u, %F)
                            execLine.replace(QRegularExpression("%[icukfUF]"), "");
                            QStringList parts = tokenize(execLine);
                            // Insert the file path as a single argument
                            parts << path;
                            if (!parts.isEmpty()) {
                                LauncherUtil::startDetachedWindowedProgram(parts[0], parts.mid(1));
                                return;
                            }
                        }
                        break;
                    }
                }
            }
        }

        // 2) Fallback: try known editors
        for (const auto &editor : { "gedit", "mousepad", "leafpad", "kate",
                                    "xed", "pluma", "micro", "nvim", "vim",
                                    "nano", "code" }) {
            if (!QStandardPaths::findExecutable(editor).isEmpty()) {
                LauncherUtil::startDetachedWindowedProgram(editor, { path });
                return;
            }
        }
        return;
    }

    // Default: open with xdg-open
    LauncherUtil::startDetachedWindowedProgram("xdg-open", { path });
}

void DesktopScene::onRenameRequested(DesktopIcon *icon)
{
    bool ok = false;
    const QString oldName = icon->fileInfo().fileName();
    const QString oldPath = icon->fileInfo().absoluteFilePath();
    const QString dirPath = icon->fileInfo().absolutePath();
    QString renameSeed = oldName;
    if (icon->fileInfo().suffix().compare("desktop", Qt::CaseInsensitive) == 0) {
        QFile file(oldPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            bool inDesktopEntry = false;
            while (!in.atEnd()) {
                const QString line = in.readLine().trimmed();
                if (line.startsWith('[')) {
                    inDesktopEntry = (line == "[Desktop Entry]");
                    continue;
                }
                if (!inDesktopEntry || line.startsWith('#'))
                    continue;
                if (!line.startsWith("Name="))
                    continue;
                const QString desktopTitle = line.mid(5).trimmed();
                if (!desktopTitle.isEmpty()) {
                    renameSeed = desktopTitle;
                    break;
                }
            }
        }
    }

    // Block the file watcher during the rename dialog. On Wayland, modal
    // dialogs don't block Qt signals, so the watcher fires as the file is
    // renamed — triggering refresh() which deletes the old icon while the
    // context menu may still reference it.
    const bool wasWatching = m_watcher->blockSignals(true);

    const QString newName = QInputDialog::getText(
        this, "Rename", "New name:", QLineEdit::Normal, renameSeed, &ok
    );

    // Restore watcher and manually trigger a refresh to pick up the rename.
    m_watcher->blockSignals(wasWatching);
    queueRefresh(50);

    if (!ok || newName.trimmed().isEmpty()) return;

    const QString trimmedName = newName.trimmed();
    QString targetFileName = trimmedName;
    if (oldPath.toLower().endsWith(".desktop")
        && !trimmedName.endsWith(".desktop", Qt::CaseInsensitive)) {
        targetFileName += ".desktop";
    }
    if (targetFileName == oldName) return;

    const QString newPath = dirPath + "/" + targetFileName;

    if (!QFile::rename(oldPath, newPath)) {
        QMessageBox::warning(nullptr, "Rename",
            "Could not rename \"" + oldName + "\" to \"" + targetFileName + "\".");
    } else {
        if (m_positions.contains(oldName)) {
            m_positions[targetFileName] = m_positions.take(oldName);
            queueSavePositions(kSaveDebounceMs);
        }
        // Manually trigger refresh now that the rename is complete.
        queueRefresh(50);
    }
}

void DesktopScene::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();

    // Ctrl+C / Ctrl+X / Ctrl+V, clipboard
    if (event->modifiers() == Qt::ControlModifier) {
        if (key == Qt::Key_C) {
            copySelectedIcons();
            event->accept();
            return;
        }
        if (key == Qt::Key_X) {
            cutSelectedIcons();
            event->accept();
            return;
        }
        if (key == Qt::Key_V) {
            pasteFromClipboard();
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Delete) {
        const bool permanent = event->modifiers() & Qt::ShiftModifier;
        onDeleteKeys(permanent);
        event->accept();
        return;
    }

    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (DesktopIcon *icon = selectedIcon()) {
            openIconFromKeyboard(icon);
            event->accept();
            return;
        }
    }

    if (event->modifiers() == Qt::NoModifier
        && (key == Qt::Key_Left || key == Qt::Key_Right
            || key == Qt::Key_Up || key == Qt::Key_Down)) {
        DesktopIcon *current = selectedIcon();
        DesktopIcon *target = keyboardNavigationTarget(static_cast<Qt::Key>(key), current);
        if (target) {
            for (auto *icon : m_icons)
                icon->setSelected(icon == target);
            m_lastTypeSelectChar = QChar();
            m_lastTypeSelectMatch = -1;
            event->accept();
            return;
        }
    }

    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool onlyShiftOrNoModifier = (mods == Qt::NoModifier || mods == Qt::ShiftModifier);
    const QString text = event->text();
    if (onlyShiftOrNoModifier && text.size() == 1) {
        const QChar typedChar = text.front().toCaseFolded();
        if (typedChar.isLetter()) {
            QList<DesktopIcon *> matches;
            for (auto *icon : m_icons) {
                if (icon->isVirtual())
                    continue;

                const QString label = icon->tipText().trimmed();
                if (label.isEmpty())
                    continue;
                if (label.front().toCaseFolded() == typedChar)
                    matches.append(icon);
            }

            if (!matches.isEmpty()) {
                int matchIndex = 0;
                if (m_lastTypeSelectChar == typedChar) {
                    matchIndex = (m_lastTypeSelectMatch + 1) % matches.size();
                }

                DesktopIcon *target = matches[matchIndex];
                for (auto *icon : m_icons)
                    icon->setSelected(icon == target);

                m_lastTypeSelectChar = typedChar;
                m_lastTypeSelectMatch = matchIndex;
                event->accept();
                return;
            }

            m_lastTypeSelectChar = QChar();
            m_lastTypeSelectMatch = -1;
        }
    }

    QWidget::keyPressEvent(event);
}

void DesktopScene::onDeleteKeys(bool permanent)
{
    QList<DesktopIcon *> targets;
    for (auto *icon : m_icons)
        if (icon->isSelected() && !icon->isVirtual())
            targets.append(icon);

    if (targets.isEmpty()) return;

    const int count = targets.size();
    QString question;
    if (permanent) {
        question = count == 1
            ? QString("Permanently delete \"%1\"?").arg(targets[0]->fileInfo().fileName())
            : QString("Permanently delete %1 items? This action cannot be undone.").arg(count);
    } else {
        question = count == 1
            ? QString("Move \"%1\" to Trash?").arg(targets[0]->fileInfo().fileName())
            : QString("Move %1 items to Trash?").arg(count);
    }

    const QString title = permanent ? "Permanently Delete" : "Move to Trash";
    const auto btn = QMessageBox::question(this, title, question,
                                           QMessageBox::Yes | QMessageBox::No);
    if (btn != QMessageBox::Yes) return;

    for (auto *icon : targets) {
        const QString path = icon->fileInfo().absoluteFilePath();
        const QString name = icon->fileInfo().fileName();
        if (permanent) {
            if (icon->fileInfo().isDir())
                QDir(path).removeRecursively();
            else
                QFile::remove(path);
        } else {
            QProcess::startDetached("gio", {"trash", path});
        }
        m_positions.remove(name);
    }
    queueSavePositions(kSaveDebounceMs);
}

void DesktopScene::onDeleteRequested(DesktopIcon *icon)
{
    const QString path = icon->fileInfo().absoluteFilePath();
    const auto btn = QMessageBox::question(
        this, "Delete",
        "Delete \"" + icon->fileInfo().fileName() + "\"?",
        QMessageBox::Yes | QMessageBox::No
    );
    if (btn != QMessageBox::Yes) return;

    if (icon->fileInfo().isDir()) {
        QDir(path).removeRecursively();
    } else {
        QFile::remove(path);
    }
    m_positions.remove(icon->fileInfo().fileName());
    queueSavePositions(kSaveDebounceMs);
}

void DesktopScene::onFileDroppedOnTrash(const QString &filePath)
{
    // Move to system trash via gio trash
    const QFileInfo fi(filePath);
    const QString name = fi.fileName();
    const auto btn = QMessageBox::question(
        this, "Move to Recycle Bin",
        "Move \"" + name + "\" to the Recycle Bin?",
        QMessageBox::Yes | QMessageBox::No
    );
    if (btn != QMessageBox::Yes) return;
    QProcess::startDetached("gio", {"trash", filePath});
    m_positions.remove(name);
    queueSavePositions(kSaveDebounceMs);
}

// ── Mouse / rubber-band ───────────────────────────────────────────────────────

void DesktopScene::mousePressEvent(QMouseEvent *event)
{
    // Dismiss in-surface context menu on any click outside it
    if (m_ctxMenu && m_ctxMenu->isVisible())
        static_cast<InlineMenu*>(m_ctxMenu)->closeAll();
    if (m_iconCtxMenu && m_iconCtxMenu->isVisible())
        static_cast<InlineMenu*>(m_iconCtxMenu)->closeAll();

    // Only start rubber-band when clicking on empty background,
    // not on a child icon (icon's mousePressEvent accepts and stops propagation).
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_rubberCtrl = event->modifiers() & Qt::ControlModifier;
    if (!m_rubberCtrl) {
        for (auto *icon : m_icons)
            icon->setSelected(false);
    }
    m_rubberBanding = true;
    m_rubberOrigin  = event->pos();
    m_rubberRect    = QRect();
    QWidget::mousePressEvent(event);
}

void DesktopScene::mouseMoveEvent(QMouseEvent *event)
{
    if (m_ctxMenu && m_ctxMenu->isVisible()) {
        auto *menu = static_cast<InlineMenu*>(m_ctxMenu);
        if (!menu->hitMenu(event->pos()))
            menu->closeAll();
    }
    if (m_iconCtxMenu && m_iconCtxMenu->isVisible()) {
        auto *menu = static_cast<InlineMenu*>(m_iconCtxMenu);
        if (!menu->hitMenu(event->pos()))
            menu->closeAll();
    }

    if (m_rubberBanding && (event->buttons() & Qt::LeftButton)) {
        const QRect oldRect = m_rubberRect;
        m_rubberRect = QRect(m_rubberOrigin, event->pos()).normalized();
        for (auto *icon : m_icons) {
            const bool hit = m_rubberRect.intersects(icon->geometry());
            if (m_rubberCtrl)
                icon->setSelected(icon->isSelected() || hit);
            else
                icon->setSelected(hit);
        }
        // Only repaint the area that changed, not the entire scene
        update((oldRect | m_rubberRect).adjusted(-1, -1, 1, 1));
    }
    QWidget::mouseMoveEvent(event);
}

void DesktopScene::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_rubberBanding) {
        m_rubberBanding = false;
        const QRect dirty = m_rubberRect.adjusted(-1, -1, 1, 1);
        m_rubberRect = QRect();
        update(dirty);
    }
    QWidget::mouseReleaseEvent(event);
}

void DesktopScene::enterEvent(QEnterEvent *event)
{
    m_cursorOnSurface = true;
    QWidget::enterEvent(event);
}

void DesktopScene::leaveEvent(QEvent *event)
{
    // Do NOT cancel rubber-band on leave - in Wayland the compositor delivers
    // move events via the implicit pointer grab (held button) even when the
    // cursor crosses into another surface (e.g. waybar). Cancelling here would
    // break the drag. The rubber band ends correctly in mouseReleaseEvent.
    m_cursorOnSurface = false;
    QWidget::leaveEvent(event);
}

void DesktopScene::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y() > 0 ? 1 : -1;
        applyIconSize(qBound(kIconSizeSliderMin, m_iconSize + delta, kIconSizeSliderMax));
        QSettings cfg(configPath(), QSettings::IniFormat);
        cfg.setValue("ui/iconSize", m_iconSize);
        // No sync() - avoids blocking the UI thread on every scroll tick.
        // Qt will flush to disk on destruction or when convenient.
        event->accept();
    } else {
        QWidget::wheelEvent(event);
    }
}

void DesktopScene::paintEvent(QPaintEvent *)
{
    if (!m_rubberBanding || m_rubberRect.isNull()) return;
    QPainter p(this);
    // Rubber-band: dotted border, faint blue fill
    p.setPen(QPen(Qt::white, 1, Qt::DotLine));
    p.setBrush(QColor(0, 0, 255, 30));
    p.drawRect(m_rubberRect.adjusted(0, 0, -1, -1));
}

void DesktopScene::contextMenuEvent(QContextMenuEvent *event)
{
    auto *menu    = static_cast<InlineMenu*>(m_ctxMenu);
    auto *arrange = static_cast<InlineMenu*>(m_ctxArrangeMenu);
    auto *newMenu = static_cast<InlineMenu*>(m_newMenu);
    auto *systemMenu = static_cast<InlineMenu*>(m_systemMenu);

    // Rebuild arrange submenu
    arrange->clear();
    arrange->addAction("Name",          [this] { onArrangeBy(SortOrder::ByName); });
    arrange->addAction("Type",          [this] { onArrangeBy(SortOrder::ByType); });
    arrange->addAction("Date Modified", [this] { onArrangeBy(SortOrder::ByDate); });

    // Rebuild New submenu
    newMenu->clear();
    newMenu->addAction("Folder",    [this] { onNewFolder(); });
    newMenu->addAction("Document",  [this] { onNewDocument(); });

    // Rebuild System submenu
    systemMenu->clear();
    systemMenu->addAction("Lock", [this] {
        const auto btn = QMessageBox::question(
            this, "Lock", "Lock the current session now?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes)
            QProcess::startDetached("loginctl", {"lock-session"});
    });
    systemMenu->addAction("Log Out", [this] {
        const auto btn = QMessageBox::question(
            this, "Log Out", "End the current desktop session?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes) {
            if (!QProcess::startDetached("hyprctl", {"dispatch", "exit"}))
                qWarning() << "[IPC] failed to dispatch exit";
        }
    });
    systemMenu->addSep();
    systemMenu->addAction("Suspend", [this] {
        const auto btn = QMessageBox::question(
            this, "Suspend", "Suspend the computer now?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes)
            QProcess::startDetached("systemctl", {"suspend"});
    });
    systemMenu->addAction("Restart", [this] {
        const auto btn = QMessageBox::question(
            this, "Restart", "Restart the computer now?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes)
            QProcess::startDetached("systemctl", {"reboot"});
    });
    systemMenu->addAction("Shut Down", [this] {
        const auto btn = QMessageBox::question(
            this, "Shut Down", "Shut down the computer now?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::Yes)
            QProcess::startDetached("systemctl", {"poweroff"});
    });

    // Rebuild main menu
    menu->clear();
    menu->addSub("New", newMenu);

    // Only show Paste when clipboard has files (internal or system)
    bool hasClipboard = !m_clipboardPaths.isEmpty();
    if (!hasClipboard) {
        const auto *mime = QApplication::clipboard()->mimeData();
        if (mime && mime->hasUrls()) {
            for (const QUrl &url : mime->urls()) {
                if (url.isLocalFile()) {
                    hasClipboard = true;
                    break;
                }
            }
        }
    }

    if (hasClipboard) {
        menu->addSep();
        menu->addAction("Paste", [this] { pasteFromClipboard(); });
    }

    menu->addSep();
    menu->addAction("Open in Terminal", [this] { onOpenInTerminal(); });
    menu->addSep();
    menu->addSub("Arrange Icons By", arrange);
    menu->addCheckable("Auto Arrange Icons", m_autoArrange, [this] { onAutoArrangeToggled(); });
    menu->addCheckable("Show Hidden Files",  m_showHidden,  [this] { onShowHiddenToggled(); });
    menu->addSep();
    menu->addAction("Refresh",       [this] { refresh(); });
    menu->addSep();
    menu->addSub("System", systemMenu);
    menu->addSep();
    menu->addAction("Properties...", [this] { onShowProperties(); });

    m_lastContextMenuPos = event->pos();
    menu->popup(event->pos(), menuBoundary());
    event->accept();
}

// ── Icon interaction slots ────────────────────────────────────────────────────

void DesktopScene::onIconPressed(DesktopIcon *icon, bool ctrlHeld)
{
    setFocus();   // grab keyboard so Delete key works after clicking an icon
    if (m_ctxMenu && m_ctxMenu->isVisible())
        static_cast<InlineMenu*>(m_ctxMenu)->closeAll();
    if (m_iconCtxMenu && m_iconCtxMenu->isVisible())
        static_cast<InlineMenu*>(m_iconCtxMenu)->closeAll();

    if (ctrlHeld) {
        // Ctrl+click: toggle this icon's selection
        icon->setSelected(!icon->isSelected());
    } else if (!icon->isSelected()) {
        // Clicking an unselected icon: deselect all, select it
        for (auto *i : m_icons)
            i->setSelected(i == icon);
    }
    // If icon is already selected without Ctrl, keep the multi-selection intact
    // so the user can start a multi-icon drag. onIconReleased handles the
    // deselect-others case when no drag occurs.
}

void DesktopScene::onIconReleased(DesktopIcon *icon, bool wasDrag, bool ctrlHeld)
{
    if (!wasDrag && !ctrlHeld) {
        // Simple click (no drag, no Ctrl): collapse selection to just this icon
        for (auto *i : m_icons)
            i->setSelected(i == icon);
    }
}

// Forward declaration - full definition below.
static QString extractCommandFor(const QString &path);

void DesktopScene::onIconContextMenuRequested(DesktopIcon *icon, QPoint scenePos)
{
    // Safety: the icon might have been deleted by a filesystem refresh
    // (rename) between when the signal was queued and this slot fired.
    // Check BEFORE any pointer access.
    if (!icon || !m_icons.contains(icon))
        return;

    // Close any open desktop context menu first
    if (m_ctxMenu && m_ctxMenu->isVisible())
        static_cast<InlineMenu*>(m_ctxMenu)->closeAll();

    auto *menu = static_cast<InlineMenu*>(m_iconCtxMenu);
    menu->clear();

    if (!icon->isVirtual()) {
        const QFileInfo fi = icon->fileInfo();

        menu->addAction("Open", [this, fi] {
            if (QFileInfo::exists(fi.absoluteFilePath()))
                onOpenRequested(fi);
        });
        menu->addSep();

        // Extract here option for archives
        if (!extractCommandFor(fi.absoluteFilePath()).isEmpty()) {
            menu->addAction("Extract here", [this, fi] {
                if (QFileInfo::exists(fi.absoluteFilePath()))
                    onExtractArchive(fi);
                else
                    QMessageBox::warning(nullptr, "Extract",
                        "The archive file no longer exists.");
            });
            menu->addSep();
        }

        menu->addAction("Copy",  [this] { copySelectedIcons(); });
        menu->addAction("Cut",   [this] { cutSelectedIcons(); });
        menu->addSep();
        menu->addAction("Rename", [this, fi, menu] {
            static_cast<InlineMenu*>(menu)->closeAll();
            static_cast<InlineMenu*>(menu)->hide();

            // Find the icon by path at click time — never capture icon pointer.
            for (auto *i : m_icons) {
                if (!i->isVirtual() && i->fileInfo().absoluteFilePath() == fi.absoluteFilePath()) {
                    emit i->renameRequested(i);
                    break;
                }
            }
        });
        menu->addAction("Delete", [this, fi] {
            for (auto *i : m_icons) {
                if (!i->isVirtual() && i->fileInfo().absoluteFilePath() == fi.absoluteFilePath()) {
                    i->setSelected(true);
                    break;
                }
            }
            onDeleteKeys(false);
        });
        menu->addSep();
        menu->addAction("Properties...", [this, fi] {
            if (QFileInfo::exists(fi.absoluteFilePath())) {
                auto *dlg = new FilePropertiesDialog(fi, nullptr);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->exec();
            }
        });
    } else {
        menu->addAction("Open", [this, icon] {
            QString uri;
            switch (icon->virtualType()) {
                case DesktopIcon::VirtualType::Trash:    uri = "trash:///"; break;
                case DesktopIcon::VirtualType::Computer: uri = "computer:///"; break;
                case DesktopIcon::VirtualType::Home:     uri = QDir::homePath(); break;
                default: break;
            }
            if (!uri.isEmpty())
                LauncherUtil::startDetachedWindowedProgram("gio", {"open", uri});
        });
        if (icon->virtualType() == DesktopIcon::VirtualType::Trash) {
            menu->addSep();
            menu->addAction("Empty Recycle Bin", [this] {
                const auto btn = QMessageBox::question(
                    this, "Empty Recycle Bin",
                    "Are you sure you want to permanently delete all items in the Recycle Bin?",
                    QMessageBox::Yes | QMessageBox::No);
                if (btn != QMessageBox::Yes) return;
                QProcess::startDetached("gio", {"trash", "--empty"});
            });
        }
    }

    menu->popup(scenePos, menuBoundary());
}

// onIconDragStarted, onIconDragging, desktopscene_dragdrop.cpp

// ── Desktop actions ───────────────────────────────────────────────────────────

void DesktopScene::onNewFolder()
{
    // Compute a unique suggested name
    QDir dir(m_desktopPath);
    QString suggested = "New Folder";
    if (dir.exists(suggested)) {
        int n = 1;
        while (dir.exists(QStringLiteral("New Folder (%1)").arg(n)))
            ++n;
        suggested = QStringLiteral("New Folder (%1)").arg(n);
    }

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "New Folder", "Folder name:",
        QLineEdit::Normal, suggested, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QString trimmed = name.trimmed();
    if (dir.exists(trimmed)) {
        QMessageBox::warning(this, "New Folder",
            QStringLiteral("\u201c%1\u201d already exists.").arg(trimmed));
        return;
    }

    // Pre-assign position near the context-menu click (manual layout only)
    if (!m_autoArrange) {
        const auto cell = nearestFreeCell(posToCell(m_lastContextMenuPos), "");
        m_positions[trimmed] = cellToPos(cell.first, cell.second);
    }

    dir.mkdir(trimmed);
    queueSavePositions(kSaveDebounceMs);
    // file watcher triggers refresh()
}

void DesktopScene::onNewDocument()
{
    QDir dir(m_desktopPath);
    QString baseName = "New Document.txt";
    if (dir.exists(baseName)) {
        int n = 1;
        while (dir.exists(QStringLiteral("New Document (%1).txt").arg(n)))
            ++n;
        baseName = QStringLiteral("New Document (%1).txt").arg(n);
    }

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "New Document", "Document name:",
        QLineEdit::Normal, baseName, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    QString trimmed = name.trimmed();
    // Ensure .txt extension
    if (!trimmed.endsWith(".txt", Qt::CaseInsensitive)) {
        trimmed += ".txt";
    }

    if (dir.exists(trimmed)) {
        QMessageBox::warning(this, "New Document",
            QStringLiteral("\u201c%1\u201d already exists.").arg(trimmed));
        return;
    }

    // Pre-assign position near the context-menu click (manual layout only)
    if (!m_autoArrange) {
        const auto cell = nearestFreeCell(posToCell(m_lastContextMenuPos), "");
        m_positions[trimmed] = cellToPos(cell.first, cell.second);
    }

    // Create empty file
    QFile file(dir.filePath(trimmed));
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "New Document",
            QStringLiteral("Failed to create document."));
        return;
    }
    file.close();
    queueSavePositions(kSaveDebounceMs);
    // file watcher triggers refresh()
}

// ── Archive extraction ──────────────────────────────────────────────────────

static QString extractCommandFor(const QString &path)
{
    const QString ext = path.toLower();
    if (ext.endsWith(".zip")) {
        if (!QStandardPaths::findExecutable("unzip").isEmpty())
            return "unzip -o \"%1\"";
    } else if (ext.endsWith(".tar.xz") || ext.endsWith(".tar.zst") || ext.endsWith(".tar.bz2") || ext.endsWith(".tar.gz") || ext.endsWith(".tgz") || ext.endsWith(".tar")) {
        if (!QStandardPaths::findExecutable("tar").isEmpty())
            return "tar xf \"%1\"";
    } else if (ext.endsWith(".xz")) {
        if (!QStandardPaths::findExecutable("xz").isEmpty())
            return "xz -d \"%1\"";
    } else if (ext.endsWith(".7z")) {
        if (!QStandardPaths::findExecutable("7z").isEmpty())
            return "7z x \"%1\" -y";
    } else if (ext.endsWith(".rar")) {
        if (!QStandardPaths::findExecutable("unrar").isEmpty())
            return "unrar x \"%1\"";
    } else if (ext.endsWith(".gz")) {
        if (!QStandardPaths::findExecutable("gzip").isEmpty())
            return "gzip -d \"%1\"";
    } else if (ext.endsWith(".bz2")) {
        if (!QStandardPaths::findExecutable("bzip2").isEmpty())
            return "bzip2 -d \"%1\"";
    }
    return {};
}

void DesktopScene::onExtractArchive(const QFileInfo &fileInfo)
{
    const QString archivePath = fileInfo.absoluteFilePath();
    const QString dir = fileInfo.absolutePath();

    // File might have been deleted or renamed since the menu was opened.
    if (!QFileInfo::exists(archivePath)) {
        QMessageBox::warning(nullptr, "Extract",
            "The archive file no longer exists.");
        return;
    }

    QString cmd = extractCommandFor(archivePath);
    if (cmd.isEmpty()) {
        QMessageBox::warning(nullptr, "Extract",
            "No suitable tool found to extract this archive.\n"
            "Install unzip, tar, 7z, or unrar as needed.");
        return;
    }

    // Cleanup from any previous extraction process.
    if (m_extractProc) {
        disconnect(m_extractProc, nullptr, this, nullptr);
        if (m_extractProc->state() != QProcess::NotRunning) {
            m_extractProc->kill();
            m_extractProc->waitForFinished();
        }
        m_extractProc->deleteLater();
        m_extractProc = nullptr;
    }

    // Mark as extracting — prevents refresh() from running while
    // the file watcher fires during extraction.
    m_extracting = true;

    // Scan top-level files/folders before extraction.
    m_extractExistingFiles.clear();
    QDir beforeDir(dir);
    for (const auto &fi : beforeDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
        m_extractExistingFiles.insert(fi.fileName());

    // Find the archive icon's grid cell.
    m_extractArchiveCell = {-1, -1};
    for (auto *icon : m_icons) {
        if (!icon->isVirtual() && icon->fileInfo().absoluteFilePath() == archivePath) {
            for (auto it = m_occupiedCells.constBegin(); it != m_occupiedCells.constEnd(); ++it) {
                if (it.value() == icon->fileInfo().fileName()) {
                    m_extractArchiveCell = it.key();
                    break;
                }
            }
            break;
        }
    }

    m_extractArchivePath = archivePath;
    m_extractDir = dir;
    m_extractBaseName = fileInfo.baseName();

    cmd = cmd.arg(archivePath);

    m_extractProc = new QProcess(this);
    m_extractProc->setWorkingDirectory(dir);
    connect(m_extractProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DesktopScene::onExtractFinished);
    m_extractProc->start("sh", {"-c", cmd});
}

void DesktopScene::onExtractFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *proc = qobject_cast<QProcess*>(sender());
    if (!proc)
        proc = m_extractProc.data();
    if (!proc)
        return;

    // Ignore stale signals from previous process instances.
    if (proc != m_extractProc) {
        proc->deleteLater();
        return;
    }

    if (exitStatus == QProcess::CrashExit) {
        QMessageBox::warning(nullptr, "Extract", "Extraction process crashed.");
        m_extracting = false;
        proc->deleteLater();
        m_extractProc = nullptr;
        return;
    }

    if (exitCode != 0) {
        const QString errText = QString::fromLocal8Bit(proc->readAllStandardError());
        QMessageBox::warning(nullptr, "Extract",
            QString("Extraction failed:\n%1")
                .arg(errText));
        m_extracting = false;
        proc->deleteLater();
        m_extractProc = nullptr;
        return;
    }

    // Scan top-level files/folders after extraction to find what's new.
    QDir afterDir(m_extractDir);
    QStringList newFiles;
    for (const auto &fi : afterDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!m_extractExistingFiles.contains(fi.fileName()))
            newFiles.append(fi.fileName());
    }

    if (newFiles.isEmpty()) {
        bool likelyEmpty = QFileInfo(m_extractArchivePath).size() < 100;
        showExtractDialog(newFiles, m_extractBaseName, !likelyEmpty, likelyEmpty);
    } else {
        // If exactly one new folder was extracted, rename it to match
        // the archive name (without extension) so it's not confusing.
        if (newFiles.size() == 1) {
            const QString oldName = newFiles.first();
            const QString newName = m_extractBaseName;
            if (oldName != newName && !QFileInfo(m_extractDir + "/" + newName).exists()) {
                const QString oldPath = m_extractDir + "/" + oldName;
                const QString newPath = m_extractDir + "/" + newName;
                if (QFileInfo(oldPath).isDir()) {
                    QDir().rename(oldPath, newPath);
                    newFiles[0] = newName;
                }
            }
        }

        int placed = 0;
        if (m_extractArchiveCell.first >= 0) {
            for (int ring = 1; ring < 10 && placed < newFiles.size(); ++ring) {
                for (int dc = -ring; dc <= ring && placed < newFiles.size(); ++dc) {
                    for (int dr = -ring; dr <= ring && placed < newFiles.size(); ++dr) {
                        if (qAbs(dc) != ring && qAbs(dr) != ring)
                            continue;
                        int c = m_extractArchiveCell.first + dc;
                        int r = m_extractArchiveCell.second + dr;
                        if (c < 0 || c >= maxCols() || r < 0 || r >= maxRows())
                            continue;
                        auto cell = qMakePair(c, r);
                        if (m_occupiedCells.contains(cell))
                            continue;
                        m_occupiedCells[cell] = newFiles[placed];
                        m_positions[newFiles[placed]] = cellToPos(c, r);
                        ++placed;
                    }
                }
            }
        }
        for (int i = placed; i < newFiles.size(); ++i) {
            auto cell = nextFreeCell();
            m_occupiedCells[cell] = newFiles[i];
            m_positions[newFiles[i]] = cellToPos(cell.first, cell.second);
        }
        queueSavePositions(kSaveDebounceMs);
        showExtractDialog(newFiles, m_extractBaseName, false, false);
        queueRefresh(500);
    }

    m_extracting = false;
    proc->deleteLater();
    m_extractProc = nullptr;
}

// ── Extract result dialog ───────────────────────────────────────────────────

void DesktopScene::showExtractDialog(const QStringList &files, const QString &archiveName,
                                      bool alreadyExisted, bool wasEmpty)
{
    auto *dlg = new QDialog(nullptr, Qt::Dialog | Qt::WindowCloseButtonHint);
    dlg->setWindowTitle("Extract");
    dlg->setObjectName("PropertiesDialog");
    dlg->setFixedWidth(360);
    dlg->setSizeGripEnabled(false);

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *banner = new QFrame(dlg);
    banner->setObjectName("PropBanner");
    banner->setFixedHeight(kPropertiesDialogBannerHeight);
    auto *bl = new QHBoxLayout(banner);
    bl->setContentsMargins(10, 6, 10, 6);
    bl->setSpacing(10);

    auto *bIcon = new QLabel(banner);
    bIcon->setPixmap(QIcon::fromTheme("emblem-default",
        QIcon::fromTheme("dialog-ok")).pixmap(32, 32));

    QString bText;
    if (alreadyExisted)
        bText = "All files already on desktop";
    else if (wasEmpty)
        bText = "Archive is empty";
    else
        bText = "Extract complete";
    auto *bLabel = new QLabel(bText, banner);
    bLabel->setObjectName("PropBannerText");

    bl->addWidget(bIcon);
    bl->addWidget(bLabel, 1);
    bl->addStretch();
    root->addWidget(banner);

    auto *content = new QWidget(dlg);
    content->setObjectName("PropContent");
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(kPropertiesDialogContentMargins,
                            kPropertiesDialogContentMargins,
                            kPropertiesDialogContentMargins,
                            kPropertiesDialogContentMargins);
    cl->setSpacing(8);

    auto *info = new QLabel(content);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->setWordWrap(true);

    if (alreadyExisted) {
        info->setText(QStringLiteral("All files from \"%1\" are already on the Desktop.\nNothing new was extracted.").arg(archiveName));
    } else if (wasEmpty) {
        info->setText(QStringLiteral("The archive \"%1\" appears to be empty.").arg(archiveName));
    } else if (files.size() <= 10) {
        info->setText(QStringLiteral("Extracted %1 file%2 from \"%3\":\n\n%4")
            .arg(files.size()).arg(files.size() == 1 ? "" : "s")
            .arg(archiveName).arg(files.join("\n")));
    } else {
        info->setText(QStringLiteral("Extracted %1 files from \"%2\":\n\n%3\n\nand %4 more")
            .arg(files.size()).arg(archiveName)
            .arg(files.mid(0, 10).join("\n"))
            .arg(files.size() - 10));
    }
    cl->addWidget(info);
    cl->addStretch();
    root->addWidget(content);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok, dlg);
    QObject::connect(btns, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    root->addWidget(btns);

    dlg->exec();
    dlg->deleteLater();
}

void DesktopScene::onOpenInTerminal()
{
    const QString desktop = QDir::homePath() + "/Desktop";

    // Try to find the system's default terminal
    const QStringList terminals = {
        "kgx",    // GNOME Console
        "gnome-terminal",
        "xfce4-terminal",
        "konsole",
        "alacritty",
        "kitty",
        "foot",
        "wezterm",
        "xterm",
        "uxterm",
        "terminator",
        "tilix"
    };

    for (const auto &term : terminals) {
        if (QStandardPaths::findExecutable(term).isEmpty()) continue;

        // Most terminals support -e or -- bash -c 'cd <dir> && exec bash'
        // Common patterns:
        //   gnome-terminal: --working-directory=DIR
        //   konsole: --workdir DIR
        //   xfce4-terminal: --working-directory=DIR
        //   alacritty: --working-directory DIR
        //   kitty: --directory DIR
        //   kgx (GNOME Console): --working-directory DIR

        if (term == "gnome-terminal" || term == "xfce4-terminal" || term == "kgx") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--working-directory=" + desktop});
        } else if (term == "konsole") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--workdir", desktop});
        } else if (term == "alacritty") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--working-directory", desktop});
        } else if (term == "kitty") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--directory", desktop});
        } else if (term == "foot") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--cd", desktop});
        } else if (term == "wezterm") {
            LauncherUtil::startDetachedWindowedProgram(term, {"start", "--cwd", desktop});
        } else if (term == "terminator") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--working-directory", desktop});
        } else if (term == "tilix") {
            LauncherUtil::startDetachedWindowedProgram(term, {"--workdir", desktop});
        } else {
            // Generic fallback: open shell that cd's to Desktop
            LauncherUtil::startDetachedWindowedProgram(term,
                                                       {"-e", "bash", "-c",
                                                        "cd '" + desktop + "' && exec bash"});
        }
        return;
    }

    // No terminal found - do nothing
}

void DesktopScene::onArrangeBy(SortOrder order)
{
    m_sortOrder = order;
    m_positions.clear();
    savePositions();
    m_occupiedCells.clear();
    refresh();
}

void DesktopScene::onAutoArrangeToggled()
{
    m_autoArrange = !m_autoArrange;
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.setValue("ui/autoArrange", m_autoArrange);
    if (m_autoArrange) {
        m_positions.clear();
        savePositions();
        m_occupiedCells.clear();
    }
    refresh();
}

void DesktopScene::onShowHiddenToggled()
{
    m_showHidden = !m_showHidden;
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.setValue("ui/showHidden", m_showHidden);
    refresh();
}

// arrangeAll, desktopscene_grid.cpp

void DesktopScene::closeAllMenus()
{
    if (m_ctxMenu)     static_cast<InlineMenu*>(m_ctxMenu)->closeAll();
    if (m_iconCtxMenu) static_cast<InlineMenu*>(m_iconCtxMenu)->closeAll();
}

void DesktopScene::onShowProperties()
{
    // Snapshot state so we can revert on Cancel
    const int  oldSize     = m_iconSize;
    const bool oldPreview  = m_showPreview;
    const bool oldArrange  = m_autoArrange;
    const bool oldHidden   = m_showHidden;
    const auto oldOrder    = m_sortOrder;
    const bool oldTrash    = m_showTrash;
    const bool oldComputer = m_showComputer;
    const bool oldHome     = m_showHome;
    const QString oldFontFamily = m_fontFamily;
    const int oldFontSize = m_fontSize;

    PropertiesDialog::Settings s;
    s.iconSize    = m_iconSize;
    s.showPreview = m_showPreview;
    s.autoArrange = m_autoArrange;
    s.showHidden  = m_showHidden;
    s.sortOrder   = static_cast<int>(m_sortOrder);
    s.showTrash   = m_showTrash;
    s.showComputer= m_showComputer;
    s.showHome    = m_showHome;
    s.fontFamily  = m_fontFamily;
    s.fontSize    = m_fontSize;

    PropertiesDialog dlg(s, nullptr);

    // Live preview: fast resize - remap positions to new grid
    connect(&dlg, &PropertiesDialog::iconSizeChanged, this, [this](int sz) {
        applyIconSize(sz);
    });
    connect(&dlg, &PropertiesDialog::fontChanged, this, [this](const QString &family) {
        applyFont(family, m_fontSize);
    });
    connect(&dlg, &PropertiesDialog::fontSizeChanged, this, [this](int size) {
        applyFont(m_fontFamily, size);
    });
    connect(&dlg, &PropertiesDialog::applicationShortcutRequested, this, [this](const QString &desktopFilePath) {
        QFile sourceFile(desktopFilePath);
        if (!sourceFile.exists()) {
            QMessageBox::warning(this, "Applications",
                "The selected application shortcut could not be found.");
            return;
        }

        const QFileInfo sourceInfo(desktopFilePath);
        const QString baseName = sourceInfo.completeBaseName();
        const QString suffix = sourceInfo.suffix();
        QString targetName = sourceInfo.fileName();
        int copyIndex = 2;
        while (QFileInfo::exists(m_desktopPath + "/" + targetName)) {
            targetName = QString("%1 (%2).%3").arg(baseName).arg(copyIndex++).arg(suffix);
        }

        const QString targetPath = m_desktopPath + "/" + targetName;
        if (!QFile::copy(desktopFilePath, targetPath)) {
            QMessageBox::warning(this, "Applications",
                "Could not create the shortcut on the desktop.");
            return;
        }

        QFile targetFile(targetPath);
        targetFile.setPermissions(targetFile.permissions()
            | QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        queueRefresh(kCreateShortcutRefreshMs);
    });

    const bool accepted = dlg.exec() == QDialog::Accepted;
    if (!accepted) {
        // Revert to previous state
        m_iconSize    = oldSize;
        m_showPreview = oldPreview;
        m_autoArrange = oldArrange;
        m_showHidden  = oldHidden;
        m_sortOrder   = oldOrder;
        m_showTrash   = oldTrash;
        m_showComputer= oldComputer;
        m_showHome    = oldHome;
        m_fontFamily  = oldFontFamily;
        m_fontSize    = oldFontSize;
        applyIconSize(oldSize);
        applyFont(oldFontFamily, oldFontSize);
        return;
    }

    const auto ns = dlg.currentSettings();
    m_iconSize     = ns.iconSize;
    m_showPreview  = ns.showPreview;
    m_autoArrange  = ns.autoArrange;
    m_showHidden   = ns.showHidden;
    m_sortOrder    = static_cast<SortOrder>(ns.sortOrder);
    m_showTrash    = ns.showTrash;
    m_showComputer = ns.showComputer;
    m_showHome     = ns.showHome;
    m_fontFamily   = ns.fontFamily;
    m_fontSize     = ns.fontSize;

    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.setValue("ui/iconSize",     m_iconSize);
    cfg.setValue("ui/showPreview",  m_showPreview);
    cfg.setValue("ui/autoArrange",  m_autoArrange);
    cfg.setValue("ui/showHidden",   m_showHidden);
    cfg.setValue("ui/showTrash",    m_showTrash);
    cfg.setValue("ui/showComputer", m_showComputer);
    cfg.setValue("ui/showHome",     m_showHome);
    cfg.setValue("ui/fontFamily",   m_fontFamily);
    cfg.setValue("ui/fontSize",     m_fontSize);
    cfg.sync();

    savePositions();
    refresh();
}
