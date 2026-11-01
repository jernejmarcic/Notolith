// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QAbstractButton>

namespace noteAppC {

class Theme;

/// A painted on/off switch for the settings page, since QCheckBox carries
/// Fusion's own box-and-tick look that nothing here asked for. Checkable,
/// so it drops in anywhere a QCheckBox would.
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    void setTheme(const Theme &theme);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_onColor;
    QColor m_offColor;
    QColor m_knobColor;
};

} // namespace noteAppC
