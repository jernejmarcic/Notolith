// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cmath>

namespace noteAppC::bases {

/// One value inside a Bases expression. Obsidian's own type set is wider
/// (objects, links, icons, files) but everything Bases needs to filter, sort
/// or show a property boils down to these five - see the module's report for
/// what is deliberately left out.
///
/// A list holds values of any type: `file.tags` and list properties are
/// lists of strings, but `map()` makes numbers, and `reduce()` has to add
/// them rather than join their text.
struct Value {
    /// `Link` is a note (or URL) reference - `[[Note]]`, `file.links`,
    /// `link()`, `file.asLink()`, `this` - and also stands in for Obsidian's
    /// separate `File` type: `.asFile()` returns the same Link unchanged (a
    /// File is "only usable in specific functions" per the docs, and every
    /// one of those - `==`, `.asLink()` - already works the same way on a
    /// Link here), so this module has one reference type, not two.
    enum class Type { Empty, Bool, Number, String, Date, List, Link };

    Type type = Type::Empty;
    bool boolean = false;
    double number = 0.0;
    QString text;      ///< String: the text. Link: the raw target, as typed.
    QDateTime date;
    QList<Value> list;
    /// Link only: an explicit alias (`[[target|display]]`, `link(x, "y")`).
    /// Empty means "no alias" - toDisplayString() then shows `text` itself,
    /// the same as Obsidian rendering an un-aliased `[[target]]`.
    QString linkDisplay;

    Value() = default;
    static Value empty() { return Value(); }
    static Value ofBool(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }
    static Value ofNumber(double n) { Value v; v.type = Type::Number; v.number = n; return v; }
    static Value ofString(QString s) { Value v; v.type = Type::String; v.text = std::move(s); return v; }
    static Value ofDate(QDateTime d) { Value v; v.type = Type::Date; v.date = std::move(d); return v; }
    static Value ofLink(QString target, QString display = QString())
    {
        Value v;
        v.type = Type::Link;
        v.text = std::move(target);
        v.linkDisplay = std::move(display);
        return v;
    }
    static Value ofList(const QStringList &l)
    {
        Value v;
        v.type = Type::List;
        v.list.reserve(l.size());
        for (const QString &s : l)
            v.list << ofString(s);
        return v;
    }
    static Value ofValues(QList<Value> l) { Value v; v.type = Type::List; v.list = std::move(l); return v; }
    /// A list's elements as text, the form contains()/join() compare in.
    QStringList strings() const
    {
        QStringList out;
        out.reserve(list.size());
        for (const Value &item : list)
            out << item.toDisplayString();
        return out;
    }

    bool isEmpty() const { return type == Type::Empty; }

    /// Obsidian's own truthiness: empty/false/0/"" are falsy, everything else
    /// (including a non-empty list) is truthy.
    bool truthy() const
    {
        switch (type) {
        case Type::Empty: return false;
        case Type::Bool: return boolean;
        case Type::Number: return number != 0.0;
        case Type::String: return !text.isEmpty();
        case Type::Date: return date.isValid();
        case Type::List: return !list.isEmpty();
        case Type::Link: return !text.isEmpty();
        }
        return false;
    }

    /// Best-effort text, for display, concatenation and `toString()`.
    QString toDisplayString() const
    {
        switch (type) {
        case Type::Empty: return {};
        case Type::Bool: return boolean ? QStringLiteral("true") : QStringLiteral("false");
        case Type::Number: {
            // Obsidian shows an integer without a trailing ".0".
            if (number == std::floor(number) && std::abs(number) < 1e15)
                return QString::number(qint64(number));
            return QString::number(number, 'g', 15);
        }
        case Type::String: return text;
        case Type::Date: return date.toString(Qt::ISODate);
        case Type::List: return strings().join(QStringLiteral(", "));
        case Type::Link: return linkDisplay.isEmpty() ? text : linkDisplay;
        }
        return {};
    }

    static QLatin1String typeName(Type t)
    {
        switch (t) {
        case Type::Empty: return QLatin1String("empty");
        case Type::Bool: return QLatin1String("boolean");
        case Type::Number: return QLatin1String("number");
        case Type::String: return QLatin1String("string");
        case Type::Date: return QLatin1String("date");
        case Type::List: return QLatin1String("list");
        case Type::Link: return QLatin1String("link");
        }
        return QLatin1String("empty");
    }
};

/// A total order between two same-or-different-type values, for sorting a
/// view: same-type values compare naturally, cross-type ones fall back to
/// their display text. Unlike an expression's own `<`/`>` (Expression.cpp's
/// `compareOrder`), this never refuses a pair - a UI sort needs *a* stable
/// order, not a mathematically meaningful one - so it is not used to
/// evaluate `<`/`>` in a filter or formula.
///
/// Deliberately says nothing about where an empty value sorts: that is a
/// spreadsheet convention (blanks last, regardless of direction), which
/// belongs in the caller's comparator, not in a value-to-value ordering.
inline int compareForSort(const Value &a, const Value &b)
{
    if (a.type == b.type) {
        switch (a.type) {
        case Value::Type::Empty: return 0;
        case Value::Type::Bool: return int(a.boolean) - int(b.boolean);
        case Value::Type::Number: return a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
        case Value::Type::String: return a.text.compare(b.text);
        case Value::Type::Date: return a.date < b.date ? -1 : (a.date > b.date ? 1 : 0);
        case Value::Type::List: return a.strings().join(QLatin1Char(',')).compare(b.strings().join(QLatin1Char(',')));
        case Value::Type::Link: return a.toDisplayString().compare(b.toDisplayString());
        }
    }
    return a.toDisplayString().compare(b.toDisplayString());
}

} // namespace noteAppC::bases
