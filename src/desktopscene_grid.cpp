#include "desktopscene.h"
#include "desktopicon.h"
#include "desktopconstants.h"

#include <QDir>

// ── Grid helpers ──────────────────────────────────────────────────────────────

int DesktopScene::maxCols() const
{
    return qMax(1, (workArea().width() - kGridMarginX) / cellW());
}

int DesktopScene::maxRows() const
{
    return qMax(1, (workArea().height() - kGridMarginY) / cellH());
}

QPoint DesktopScene::cellToPos(int col, int row) const
{
    const QRect wa = workArea();
    const int xStart = qMax(kGridMarginX,
        (wa.width() + kGridMarginX - maxCols() * cellW()) / 2);
    return { wa.left() + xStart + col * cellW(),
             wa.top()  + kGridMarginY + row * cellH() };
}

QPair<int,int> DesktopScene::posToCell(QPoint pos) const
{
    const QRect wa = workArea();
    const int xStart = qMax(kGridMarginX,
        (wa.width() + kGridMarginX - maxCols() * cellW()) / 2);
    int col = qRound(qreal(pos.x() - wa.left() - xStart) / cellW());
    int row = qRound(qreal(pos.y() - wa.top()  - kGridMarginY) / cellH());
    col = qBound(0, col, maxCols() - 1);
    row = qBound(0, row, maxRows() - 1);
    return {col, row};
}

// First free cell in column-first order (classic Windows desktop layout)
QPair<int,int> DesktopScene::nextFreeCell() const
{
    for (int col = 0; col < maxCols(); ++col) {
        for (int row = 0; row < maxRows(); ++row) {
            const auto cell = qMakePair(col, row);
            if (!m_occupiedCells.contains(cell))
                return cell;
        }
    }
    return {0, 0}; // fallback: all cells full
}

// Nearest free cell to target (BFS outward in rings), skipping the cell
// already owned by skipName so the dragged icon doesn't block itself.
QPair<int,int> DesktopScene::nearestFreeCell(QPair<int,int> target,
                                              const QString &skipName) const
{
    const int cols = maxCols();
    const int rows = maxRows();

    for (int radius = 0; radius <= cols + rows; ++radius) {
        for (int dc = -radius; dc <= radius; ++dc) {
            for (int dr = -radius; dr <= radius; ++dr) {
                // Only visit the perimeter of each ring
                if (qAbs(dc) != radius && qAbs(dr) != radius)
                    continue;
                const int col = qBound(0, target.first  + dc, cols - 1);
                const int row = qBound(0, target.second + dr, rows - 1);
                const auto cell = qMakePair(col, row);
                const QString owner = m_occupiedCells.value(cell);
                if (owner.isEmpty() || owner == skipName)
                    return cell;
            }
        }
    }
    return target; // fallback
}

// ── Icon sizing ───────────────────────────────────────────────────────────────

// Resize icons and remap their order to the new grid (no absolute cell carry-over).
void DesktopScene::applyIconSize(int newSize)
{
    if (newSize == m_iconSize) return;

    // Snapshot each icon's current pixel position AND cell BEFORE changing
    // m_iconSize.
    struct IconState {
        QString  name;
        DesktopIcon *icon;
        QPair<int,int> cell; // current grid cell (based on m_positions)
    };
    QList<IconState> states;
    states.reserve(m_icons.size());
    for (auto *icon : m_icons) {
        const QString name = this->iconKey(icon);
        states.append({name, icon,
                        posToCell(m_positions.value(name, icon->pos()))});
    }

    // Switch to new size (cellW/cellH/maxRows/maxCols now reflect newSize)
    const int oldMaxCols = maxCols();
    m_iconSize = newSize;
    m_occupiedCells.clear();

    const int newMaxCols = maxCols();
    const int newMaxRows = maxRows();

    // Determine the new cell for each icon.
    //   - If the icon's old cell still fits: keep it there (no move).
    //   - If the old cell is out of bounds: project the icon's CURRENT widget
    //     position onto the new grid.  This handles both cases:
    //       a) Grid shrank, icon is pushed to a new cell.
    //       b) Grid expanded, icon was previously pushed; re-projecting from
    //          widget pos lets it drift back toward its original column.
    //
    // IMPORTANT: we do NOT update m_positions here.  m_positions stores the
    // user-intended position and is only modified by explicit drag actions.
    // During resize, the widget may move but m_positions stays anchored.
    for (const auto &st : states) {
        QPair<int,int> targetCell;
        if (st.cell.first < newMaxCols && st.cell.second < newMaxRows) {
            targetCell = st.cell;  // still fits - no change
        } else {
            // Out of bounds in new grid - re-project from widget position.
            targetCell = posToCell(st.icon->pos());
        }

        // Claim the cell (resolve collisions via nearestFreeCell).
        QPair<int,int> finalCell = targetCell;
        if (m_occupiedCells.contains(finalCell))
            finalCell = nearestFreeCell(finalCell, st.name);

        st.icon->updateSize(newSize);
        m_occupiedCells[finalCell] = st.name;
        // NOTE: m_positions is NOT updated here - it preserves the user's
        // intended placement and is only changed by drag/move actions.
        const QPoint pos = cellToPos(finalCell.first, finalCell.second);
        st.icon->move(pos);
    }
}

// ── Arrange all ───────────────────────────────────────────────────────────────

void DesktopScene::arrangeAll()
{
    m_occupiedCells.clear();
    for (auto *icon : m_icons) {
        const QString name = icon->fileInfo().fileName();
        const auto cell = nextFreeCell();
        m_occupiedCells[cell] = name;
        const QPoint pos = cellToPos(cell.first, cell.second);
        m_positions[name] = pos;
        icon->move(pos);
    }
    savePositions();
}
