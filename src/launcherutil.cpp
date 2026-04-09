#include "launcherutil.h"
#include "desktopconstants.h"

#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

namespace LauncherUtil {
namespace {

constexpr int kWindowedLaunchSize = 720;

QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace('\'', "'\"'\"'");
    return '\'' + escaped + '\'';
}

QString joinCommand(const QString &program, const QStringList &arguments)
{
    QStringList parts;
    parts << shellQuote(program);
    for (const QString &argument : arguments)
        parts << shellQuote(argument);
    return parts.join(' ');
}

bool canUseHyprlandFloatDispatch()
{
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return false;

    static const bool hasHyprctl = !QStandardPaths::findExecutable("hyprctl").isEmpty();
    return hasHyprctl;
}

bool startWithHyprlandFloatRule(const QString &command)
{
    const QString rules = QString("[float; size %1 %1; center 1] ")
                              .arg(kWindowedLaunchSize);
    bool ok = QProcess::startDetached("hyprctl",
                                   {"dispatch", "exec", rules + command});
    if (!ok)
        qWarning() << "[IPC] hyprctl dispatch exec failed for:" << command.left(80);
    return ok;
}

} // namespace

bool startDetachedWindowedCommand(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty())
        return false;

    if (canUseHyprlandFloatDispatch() && startWithHyprlandFloatRule(trimmed))
        return true;

    return QProcess::startDetached("sh", {"-lc", trimmed});
}

bool startDetachedWindowedProgram(const QString &program, const QStringList &arguments)
{
    if (program.trimmed().isEmpty())
        return false;

    if (canUseHyprlandFloatDispatch()) {
        const QString command = joinCommand(program, arguments);
        if (startWithHyprlandFloatRule(command))
            return true;
    }

    return QProcess::startDetached(program, arguments);
}

} // namespace LauncherUtil
