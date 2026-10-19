#pragma once

#include <memory>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace noteAppC {

/// One change to the text, in the shape MarkdownEditor::applyExternalEdit
/// (and its edited() signal) already speaks. `at` and `removed` count UTF-16
/// units, the unit of QString.
struct CrdtEdit {
    qsizetype at = 0;
    qsizetype removed = 0;
    QString inserted;
};

/// The text of one shared note as a CRDT: replicas that have seen the same
/// updates hold the same text, whatever order the updates arrived in.
///
/// Backend-neutral on purpose - nothing of the engine behind it leaks past
/// this header, and tests/test_share_crdt.cpp runs one suite over every
/// backend. Yrs is the Rust yrs library behind a small C surface
/// (libs/ext/share/yrs-ffi); Native is reserved for a C++ implementation of
/// the same contract. See docs/SHARE.md.
///
/// A document is single-threaded: call it from one thread (the UI thread).
/// Updates and state vectors are opaque byte strings in the Yjs v1 encoding;
/// two backends of the same kind interoperate, a Yrs and a Native one are
/// not required to.
class CrdtDoc {
public:
    enum class Backend { Yrs, Native };

    virtual ~CrdtDoc() = default;

    /// Whether this build contains the backend. Never changes at run time.
    static bool available(Backend backend);

    /// An empty document. `clientID` names this replica and must be unique
    /// among everyone editing the note, and below 2^53; two replicas that
    /// share one corrupt each other (a random 53-bit value will do). Null if
    /// the backend is not in this build.
    static std::unique_ptr<CrdtDoc> create(Backend backend, quint64 clientID);

    virtual QString text() const = 0;

    /// Replace `removed` units at `at` with `inserted`, and return the update
    /// to broadcast to the other replicas (empty if the edit changed nothing).
    /// A range outside the text is refused: nothing changes, the result is
    /// empty. Offsets must not split a surrogate pair, and a lone surrogate in
    /// `inserted` is stored as U+FFFD (one unit, so offsets stay in step) -
    /// the editor produces neither.
    virtual QByteArray localEdit(qsizetype at, qsizetype removed, const QString &inserted) = 0;

    /// Merge a remote update. Returns what changed in the text, each edit
    /// applied to the text the previous one left, ready to feed
    /// applyExternalEdit in order. An update that does not decode sets *ok to
    /// false and changes nothing; an update that refers to edits not seen yet
    /// is kept pending (ok stays true, no edits) and takes effect when the
    /// missing ones arrive. `ok` may be null.
    virtual QList<CrdtEdit> applyUpdate(const QByteArray &update, bool *ok = nullptr) = 0;

    /// What this replica has seen; send it to a peer to learn what it lacks.
    virtual QByteArray stateVector() const = 0;

    /// Everything a peer whose state vector is `remoteStateVector` has not
    /// seen yet (an empty vector: everything). Empty if the vector does not
    /// decode - a real diff is never empty.
    virtual QByteArray diffSince(const QByteArray &remoteStateVector) const = 0;

    /// The whole document, for storing on disk.
    virtual QByteArray encodeState() const = 0;

    /// Merge a state written by encodeState() into this document (meant for
    /// a fresh one). False, and no change, if it does not decode.
    virtual bool loadState(const QByteArray &state) = 0;
};

} // namespace noteAppC
