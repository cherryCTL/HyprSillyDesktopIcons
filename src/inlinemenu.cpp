#include "inlinemenu.h"

#include <QPainter>
#include <QCursor>
#include <QFontMetrics>
#include <QMouseEvent>

// ---------------------------------------------------------------------------
// InlineMenu - child-widget context menu (no QMenu, avoids Wayland grab issues)
// ---------------------------------------------------------------------------

InlineMenu::InlineMenu(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    hide();
}

void InlineMenu::setRoot(InlineMenu *root) { m_root = root; }

void InlineMenu::clear() { m_items.clear(); m_hover = -1; m_activeSubmenu = nullptr; }

void InlineMenu::addAction(const QString &text, std::function<void()> fn) {
    m_items.push_back({text, false, false, false, false, nullptr, std::move(fn)});
}

void InlineMenu::addCheckable(const QString &text, bool checked, std::function<void()> fn) {
    m_items.push_back({text, false, true, checked, false, nullptr, std::move(fn)});
}

void InlineMenu::addSub(const QString &text, InlineMenu *sub) {
    sub->setRoot(m_root ? m_root : this);
    m_items.push_back({text, false, false, false, true, sub, nullptr});
}

void InlineMenu::addSep() { m_items.push_back({"", true}); }

void InlineMenu::popup(const QPoint &pos, const QRect &available) {
    QFontMetrics fm(font());
    int w = kMinW;
    for (auto &it : m_items)
        if (!it.isSep)
            w = qMax(w, fm.horizontalAdvance(it.label) + kPadL + kPadR + 4);
    int h = 8;
    for (auto &it : m_items) h += it.isSep ? kSepH : kItemH;
    m_available = available.isValid() ? available
                : QRect(0, 0, parentWidget()->width(), parentWidget()->height());
    QPoint p = pos;
    // Horizontal: shift left if it would overflow the right edge
    if (p.x() + w > m_available.right())  p.rx() = qMax(m_available.left(), m_available.right() - w);
    // Vertical: open upward when there is more space above than below,
    // or when the menu would overflow downward. This prevents any edge-case
    // narrow band where the menu is clipped by the panel.
    const int spaceBelow = m_available.bottom() - p.y();
    const int spaceAbove = p.y() - m_available.top();
    if (h > spaceBelow && spaceAbove > spaceBelow)
        p.ry() = qMax(m_available.top(), p.y() - h);
    // Final clamp: keep fully inside the available area
    p.ry() = qBound(m_available.top(), p.y(), qMax(m_available.top(), m_available.bottom() - h));
    resize(w, h);
    move(p);
    raise();
    show();
    update();
}

void InlineMenu::closeAll() {
    // Delegate to root - avoids double-close
    InlineMenu *root = m_root ? m_root : this;
    root->_closeTree();
}

void InlineMenu::_closeTree() {
    for (auto &it : m_items)
        if (it.submenu) it.submenu->_hideSelf();
    _hideSelf();
}

void InlineMenu::_hideSelf() {
    m_activeSubmenu = nullptr;
    m_hover = -1;
    hide();
}

bool InlineMenu::hitMenu(const QPoint &scenePos, int depth) const {
    if (depth > 16) return false;  // guard against circular submenu refs
    if (isVisible() && geometry().contains(scenePos)) return true;
    for (auto &it : m_items)
        if (it.submenu && it.submenu->hitMenu(scenePos, depth + 1)) return true;
    return false;
}

int InlineMenu::hitTest(const QPoint &p) const {
    int y = 4;
    for (int i = 0; i < (int)m_items.size(); ++i) {
        int h = m_items[i].isSep ? kSepH : kItemH;
        if (!m_items[i].isSep && p.y() >= y && p.y() < y + h) return i;
        y += h;
    }
    return -1;
}

QRect InlineMenu::itemRect(int idx) const {
    int y = 4;
    for (int i = 0; i < (int)m_items.size(); ++i) {
        int h = m_items[i].isSep ? kSepH : kItemH;
        if (i == idx) return {0, y, width(), h};
        y += h;
    }
    return {};
}

void InlineMenu::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QRect r = rect();

    // Fill background
    p.fillRect(r, QColor(0xC0, 0xC0, 0xC0));

    // Raised border: outer bright/shadow + inner bright/shadow
    p.setPen(QColor(0xFF, 0xFF, 0xFF));          // outer bright (top-left)
    p.drawLine(r.left(),     r.top(),      r.right() - 1, r.top());
    p.drawLine(r.left(),     r.top(),      r.left(),      r.bottom() - 1);
    p.setPen(QColor(0x00, 0x00, 0x00));          // outer shadow (bottom-right)
    p.drawLine(r.right(),    r.top(),      r.right(),     r.bottom());
    p.drawLine(r.left(),     r.bottom(),   r.right(),     r.bottom());
    p.setPen(QColor(0xDF, 0xDF, 0xDF));          // inner bright
    p.drawLine(r.left() + 1, r.top() + 1,    r.right() - 2, r.top() + 1);
    p.drawLine(r.left() + 1, r.top() + 1,    r.left() + 1,  r.bottom() - 2);
    p.setPen(QColor(0x80, 0x80, 0x80));          // inner shadow
    p.drawLine(r.right() - 1, r.top() + 1,    r.right() - 1, r.bottom() - 1);
    p.drawLine(r.left()  + 1, r.bottom() - 1, r.right() - 1, r.bottom() - 1);

    int y = 4;
    for (int i = 0; i < (int)m_items.size(); ++i) {
        const auto &it = m_items[i];
        if (it.isSep) {
            int mid = y + kSepH / 2;
            p.setPen(QColor(0x80, 0x80, 0x80));
            p.drawLine(kPadL - 2, mid,     r.right() - 3, mid);
            p.setPen(Qt::white);
            p.drawLine(kPadL - 2, mid + 1, r.right() - 3, mid + 1);
            y += kSepH;
            continue;
        }
        QRect ir(2, y, r.width() - 4, kItemH);
        if (i == m_hover) {
            p.fillRect(ir, QColor(0x00, 0x00, 0x80));
            p.setPen(Qt::white);
        } else {
            p.setPen(Qt::black);
        }
        // Checkmark column
        if (it.checkable && it.checked)
            p.drawText(QRect(4, y, kPadL - 6, kItemH), Qt::AlignVCenter | Qt::AlignCenter, "\u2713");
        // Label
        p.drawText(QRect(kPadL, y, r.width() - kPadL - kPadR, kItemH), Qt::AlignVCenter, it.label);
        // Submenu arrow
        if (it.isSub)
            p.drawText(QRect(r.width() - kPadR, y, kPadR - 3, kItemH), Qt::AlignVCenter | Qt::AlignRight, "\u25BA");
        y += kItemH;
    }
}

void InlineMenu::mouseMoveEvent(QMouseEvent *ev) {
    int idx = hitTest(ev->pos());
    if (idx == m_hover) return;
    m_hover = idx;
    update();
    InlineMenu *root = m_root ? m_root : this;
    if (idx >= 0 && m_items[idx].isSub && m_items[idx].submenu) {
        QRect r = itemRect(idx);
        m_items[idx].submenu->popup(mapToParent(QPoint(width() - 2, r.top())), m_available);
        root->m_activeSubmenu = m_items[idx].submenu;
    } else if (root->m_activeSubmenu && (idx < 0 || !m_items[idx].isSub)) {
        // Keep submenu open if the mouse is already hovering it
        // (child widget may not receive mouseMoveEvent before parent does)
        if (!root->m_activeSubmenu->underMouse()) {
            root->m_activeSubmenu->_closeTree();
            root->m_activeSubmenu = nullptr;
        }
    }
}

void InlineMenu::leaveEvent(QEvent *ev) {
    QWidget::leaveEvent(ev);

    InlineMenu *root = m_root ? m_root : this;
    if (!root->m_activeSubmenu)
        return;

    QWidget *container = parentWidget();
    if (!container)
        return;

    const QPoint scenePos = container->mapFromGlobal(QCursor::pos());
    if (!root->hitMenu(scenePos)) {
        root->m_activeSubmenu->_closeTree();
        root->m_activeSubmenu = nullptr;
        m_hover = -1;
        update();
    }
}

void InlineMenu::mousePressEvent(QMouseEvent *ev) {
    if (ev->button() == Qt::LeftButton) {
        int idx = hitTest(ev->pos());
        if (idx >= 0 && !m_items[idx].isSep && !m_items[idx].isSub) {
            auto fn = m_items[idx].fn;
            closeAll();
            if (fn) fn();
            return;
        }
    }
    ev->ignore();
}
