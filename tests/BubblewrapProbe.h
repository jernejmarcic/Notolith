// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

// Whether a test that runs a sandboxed tool can run at all. A builder that denies unprivileged user namespaces
// (a container, a hardened kernel) has bwrap on PATH and cannot use it, so "bwrap is missing" is not the same
// question as "the sandbox works here". The answer is found once, with the flags the sandbox itself uses.
#include <QProcess>
#include <QStandardPaths>
#include <QString>

inline bool bubblewrapUsable()
{
    // Looked up on every call, as the tests that edit PATH did before; only the run is remembered.
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (bwrap.isEmpty())
        return false;
    static const bool works = [&bwrap] {
        QProcess probe;
        probe.start(bwrap, {QStringLiteral("--unshare-all"), QStringLiteral("--unshare-user"), QStringLiteral("--disable-userns"),
                            QStringLiteral("--cap-drop"), QStringLiteral("ALL"), QStringLiteral("--ro-bind"), QStringLiteral("/"),
                            QStringLiteral("/"), QStringLiteral("true")});
        return probe.waitForFinished(10000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    }();
    return works;
}
