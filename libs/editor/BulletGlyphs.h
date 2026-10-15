#pragma once

#include <QChar>
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QPainter;

namespace noteAppC {

/// What a list bullet looks like in live preview. The file keeps its `-`, `*`
/// or `+` (CommonMark has no other bullet markers; `=` is not one); only the
/// drawing changes, and `AsTyped` draws the character itself.
enum class BulletGlyph { Dot, Ring, Square, Diamond, Dash, Arrow, Star, Check, AsTyped };

/// The glyph each bullet marker is drawn as: "-=dash,*=dot,+=diamond" in the
/// `editor.bulletGlyphs` setting. Anything unreadable falls back to the
/// default for that marker, so a hand-edited settings file cannot break it.
struct BulletGlyphMap {
    BulletGlyph dash = BulletGlyph::Dash;
    BulletGlyph star = BulletGlyph::Dot;
    BulletGlyph plus = BulletGlyph::Diamond;

    static BulletGlyphMap parse(const QString &spec);
    QString toSpec() const;
    BulletGlyph glyphFor(QChar marker) const;
    void set(QChar marker, BulletGlyph glyph);

    bool operator==(const BulletGlyphMap &other) const = default;
};

/// Setting names, in the order a chooser lists them.
const QStringList &bulletGlyphNames();
QString bulletGlyphName(BulletGlyph glyph);
/// Display name for a chooser ("Dot", "Ring", ...).
QString bulletGlyphTitle(BulletGlyph glyph);
BulletGlyph bulletGlyphFromName(const QString &name, BulletGlyph fallback);

/// Draws `glyph` centred on `centre`, about the size of a body-text bullet.
/// `marker` and `font` only matter to `AsTyped`.
void paintBulletGlyph(QPainter &painter, BulletGlyph glyph, QChar marker, const QPointF &centre,
                      const QColor &color, const QFont &font);

/// Settings control: one drop-down per bullet marker, each item previewed with
/// its glyph. Emits the new "-=dash,*=dot,+=diamond" spec on a change.
class BulletGlyphChooser : public QWidget
{
    Q_OBJECT
public:
    explicit BulletGlyphChooser(QWidget *parent = nullptr);
    /// Shows `spec` without emitting.
    void setSpec(const QString &spec);
    QString spec() const;
    /// The drop-down for a marker, for the tests.
    QComboBox *comboFor(QChar marker) const;

signals:
    void specChanged(const QString &spec);

private:
    QComboBox *m_combos[3] = {};
};

} // namespace noteAppC
