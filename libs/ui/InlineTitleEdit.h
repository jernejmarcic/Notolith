#pragma once

#include <QLineEdit>

namespace noteAppC {

/// The note's title above its text, edited in place. A QLineEdit with
/// nothing but the keys Obsidian gives its title: Esc gives the edit up,
/// Down moves into the note. Enter and losing focus are editingFinished.
class InlineTitleEdit : public QLineEdit
{
    Q_OBJECT
public:
    explicit InlineTitleEdit(QWidget *parent = nullptr);

signals:
    /// Esc: the owner restores the name and takes the focus away.
    void cancelled();
    /// Down: on to the note text.
    void moveDown();

protected:
    void keyPressEvent(QKeyEvent *event) override;
};

} // namespace noteAppC
