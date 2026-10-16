#pragma once

#include <QDateTime>
#include <QString>

namespace noteAppC::MomentFormat {

/// Renders `when` through a moment.js-style format string - the tokens
/// Obsidian's daily notes and templates plugins both use, and the ones users
/// actually write in practice: YYYY, YY, MMMM, MMM, MM, M, DD, Do, D, dddd,
/// ddd, E, W, WW, gggg, HH, H, hh, h, mm, m, ss, s, A, a, and `[literal text]`. Anything else in
/// the pattern (dashes, slashes, spaces) passes through unchanged, the same
/// as moment.js.
QString format(const QDateTime &when, const QString &pattern);

} // namespace noteAppC::MomentFormat
