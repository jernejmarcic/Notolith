// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#include "FormatColours.h"

#include "Theme.h"

namespace noteAppC {

void applyUserColours(Theme &theme, const std::function<QString(const QString &key)> &text)
{
    QHash<QString, QString> pins;
    for (const FormatRow &row : formatRows()) {
        for (const FormatColour &colour : row.colours) {
            const QString value = text(formatColourKey(colour, theme.isDark()));
            if (Theme::parseColor(value).isValid())
                pins.insert(QLatin1String(colour.token), value);
        }
    }
    if (!pins.isEmpty())
        theme.setTokens(pins);
}

} // namespace noteAppC
