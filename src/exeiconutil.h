#pragma once

#include <QFileInfo>
#include <QPixmap>

// ── Windows executable icon extraction ──────────────────────────────────────
// Extracts the embedded icon from a .exe file using wrestool+icotool (icoutils).
// Caches the result as PNG so we don't re-extract every time.
// Returns a null QPixmap if the file isn't .exe, icoutils isn't installed,
// or extraction fails.
namespace ExeIconUtil {

QPixmap loadEmbeddedIcon(const QFileInfo &fileInfo);

}
