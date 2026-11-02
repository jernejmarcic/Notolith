// SPDX-License-Identifier: GPL-3.0-or-later
// The Native CrdtDoc backend: a run-length RGA with Lamport timestamps, plain
// C++ on Qt6::Core, no Rust. See docs/SHARE.md for the why and the limits.
//
// The text is a sequence of items, one per UTF-16 unit. An item is named by
// (client, clock): clocks run 0, 1, 2... per client, one per unit, so a
// replica's state is one number per client. Each item records its origin, the
// item it was typed after (or ROOT), and a Lamport timestamp `ts`. A new item
// goes directly after its origin, skipping every item that follows it with a
// greater (ts, client) key and stopping before the first smaller one; that is
// RGA, and every replica that has the same items ends with the same order.
// Items are stored as runs: item k of a run is (client, clock+k) with
// timestamp ts+k and origin item k-1, so text typed or pasted in one go is one
// run. Deleting marks items as tombstones; they are never removed.
//
// Invariants (the fuzz test sends you back here):
//  1. Lamport: m_lamport is the largest ts of any *integrated* item (bumped on
//     every integrate, including runs promoted out of the pending queue). A
//     local insert takes ts = m_lamport + 1, which is greater than every
//     integrated key, so integrate() skips nothing and the item lands directly
//     after its origin, which is visible offset `at`. Pending items do not
//     count.
//  2. Run atomicity: within a run, item k has ts+k, clock+k and origin item
//     k-1, so keys inside a run strictly increase and nothing can land between
//     item k and k+1 except an item whose origin *is* item k. Hence (a) the
//     skip loop compares only a run's first key, and (b) the encoder may
//     re-merge fragmented runs.
//  3. Canonical merge condition (continues(), used by the encoder and the
//     local append): same client, R2.clock == R1.clock + R1.len, R2.ts ==
//     R1.ts + R1.len, R2.origin == (R1.client, R1.clock + R1.len - 1). Split
//     and trim shift clock and ts and give the right part the left part's last
//     item as origin.
//  4. Two empties: empty bytes as an update is malformed; empty bytes as a
//     state vector means "everything". An up-to-date diff is header-only, not
//     empty, and applies as a no-op.
//  5. The state vector omits clients whose clock is 0.
//  6. Deletes are idempotent: a range over a deleted run changes nothing, so
//     the full delete set a diff carries yields zero edits when known.
//
// Positions (encodePosition) are their own small format, described there.
//
// Wire format, all integers LEB128 varints of at most 10 bytes (u32 fields
// are rejected above UINT32_MAX), nothing trails:
//   update := 'N' 0x01  runCount  run*  dsClientCount  (client rangeCount (clock len)*)*
//   run    := client clock ts flags [originClient] [originClock] units bytes utf8text
//             flags: bit0 has an origin, bit1 the origin's client is the run's
//             own (then originClient is absent); bit1 needs bit0
//   state vector := count (client clock)*       clients ascending, clock > 0
// Runs are sorted by (client, clock) and merged by invariant 3, ranges are
// sorted and merged, so encodeState() depends only on the set of items and
// deletions, not on how a replica fragmented its runs. Every count is checked
// against the bytes left before anything is reserved.

#include "CrdtDoc.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noteAppC {
namespace {

constexpr quint64 kMaxU32 = 0xffffffffu;
// What may wait for edits that have not arrived, in units plus kRunCost per
// run (a run costs about that much memory however short): an update beyond it
// is refused whole. Also the largest update a replica takes at all.
constexpr qsizetype kPendingCap = qsizetype(1) << 25;
constexpr qsizetype kRunCost = 64;

struct RunData {
    quint64 client = 0;
    quint32 clock = 0;
    quint32 ts = 0;
    bool hasOrigin = false; // false: ROOT
    quint64 originClient = 0;
    quint32 originClock = 0;
    QString text;
    quint32 len() const { return quint32(text.size()); }
};

struct Run : RunData {
    bool deleted = false;
    Run *prev = nullptr, *next = nullptr; // document order, tombstones included
    bool fresh = false, justDeleted = false; // transient, for the delta walk
};

struct Range {
    quint64 client;
    quint32 clock, len;
};

struct Decoded {
    std::vector<RunData> runs;
    std::vector<Range> ranges;
};

using Need = std::pair<quint64, quint32>; // (client, clock): waits until that client's state is past clock

bool continues(const RunData &a, const RunData &b)
{
    return a.client == b.client && quint64(a.clock) + a.len() == b.clock && quint64(a.ts) + a.len() == b.ts
        && b.hasOrigin && b.originClient == a.client && quint64(b.originClock) + 1 == quint64(a.clock) + a.len();
}

/// `r` without its first `skip` items: what a peer that already has them lacks.
RunData tail(const RunData &r, quint32 skip)
{
    RunData t = r;
    if (skip == 0)
        return t;
    t.text = r.text.mid(skip);
    t.clock += skip;
    t.ts += skip;
    t.hasOrigin = true;
    t.originClient = r.client;
    t.originClock = t.clock - 1;
    return t;
}

void normalize(std::vector<Range> &ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const Range &a, const Range &b) {
        return a.client != b.client ? a.client < b.client : a.clock < b.clock;
    });
    size_t out = 0;
    for (const Range &r : ranges) {
        if (out && ranges[out - 1].client == r.client
            && quint64(r.clock) <= quint64(ranges[out - 1].clock) + ranges[out - 1].len) {
            Range &last = ranges[out - 1];
            last.len = quint32(std::max<quint64>(quint64(last.clock) + last.len, quint64(r.clock) + r.len) - last.clock);
        } else {
            ranges[out++] = r;
        }
    }
    ranges.resize(out);
}

void putVarint(QByteArray &out, quint64 v)
{
    while (v >= 0x80) {
        out.append(char(v | 0x80));
        v >>= 7;
    }
    out.append(char(v));
}

class Reader {
public:
    explicit Reader(const QByteArray &bytes)
        : m_p(reinterpret_cast<const uchar *>(bytes.constData())), m_end(m_p + bytes.size()) {}

    qsizetype remaining() const { return m_end - m_p; }

    bool byte(uchar &v)
    {
        if (m_p == m_end)
            return false;
        v = *m_p++;
        return true;
    }

    bool varint(quint64 &v)
    {
        v = 0;
        for (int shift = 0; shift <= 63; shift += 7) {
            uchar b;
            if (!byte(b) || (shift == 63 && b > 1))
                return false;
            v |= quint64(b & 0x7f) << shift;
            if (!(b & 0x80))
                return true;
        }
        return false;
    }

    bool u32(quint32 &v)
    {
        quint64 w;
        if (!varint(w) || w > kMaxU32)
            return false;
        v = quint32(w);
        return true;
    }

    /// The next `n` bytes, which the caller has checked are there.
    const char *take(quint64 n)
    {
        const char *p = reinterpret_cast<const char *>(m_p);
        m_p += n;
        return p;
    }

private:
    const uchar *m_p, *m_end;
};

bool decodeRanges(Reader &in, std::vector<Range> &ranges)
{
    quint64 clients;
    if (!in.varint(clients) || clients > quint64(in.remaining()) / 4)
        return false;
    for (quint64 i = 0; i < clients; ++i) {
        quint64 client, count;
        if (!in.varint(client) || !in.varint(count) || count > quint64(in.remaining()) / 2)
            return false;
        for (quint64 j = 0; j < count; ++j) {
            Range r{client, 0, 0};
            if (!in.u32(r.clock) || !in.u32(r.len) || r.len == 0 || quint64(r.clock) + r.len > kMaxU32)
                return false;
            ranges.push_back(r);
        }
    }
    return true;
}

bool decodeUpdate(const QByteArray &bytes, Decoded &out)
{
    Reader in(bytes);
    uchar magic, version;
    quint64 runCount;
    if (!in.byte(magic) || magic != 'N' || !in.byte(version) || version != 1 || !in.varint(runCount)
        || runCount > quint64(in.remaining()) / 6) // a run is at least 6 bytes
        return false;
    out.runs.reserve(size_t(runCount));
    for (quint64 i = 0; i < runCount; ++i) {
        RunData r;
        uchar flags;
        quint64 units, nbytes;
        if (!in.varint(r.client) || !in.u32(r.clock) || !in.u32(r.ts) || !in.byte(flags) || (flags & ~3)
            || ((flags & 2) && !(flags & 1)))
            return false;
        r.hasOrigin = flags & 1;
        if (r.hasOrigin) {
            r.originClient = r.client;
            if (!(flags & 2) && !in.varint(r.originClient))
                return false;
            if (!in.u32(r.originClock) || (r.originClient == r.client && r.originClock >= r.clock))
                return false;
        }
        // UTF-8 never has fewer bytes than the UTF-16 units it spells.
        if (!in.varint(units) || !in.varint(nbytes) || units == 0 || units > nbytes
            || nbytes > quint64(in.remaining()) || quint64(r.clock) + units > kMaxU32
            || quint64(r.ts) + units > kMaxU32)
            return false;
        r.text = QString::fromUtf8(in.take(nbytes), qsizetype(nbytes));
        if (quint64(r.text.size()) != units)
            return false;
        out.runs.push_back(std::move(r));
    }
    return decodeRanges(in, out.ranges) && in.remaining() == 0;
}

/// `runs` sorted and merged, `ranges` in any order.
QByteArray encodeUpdate(const std::vector<RunData> &runs, std::vector<Range> ranges)
{
    QByteArray out("N\x01", 2);
    putVarint(out, runs.size());
    for (const RunData &r : runs) {
        const QByteArray utf8 = r.text.toUtf8();
        putVarint(out, r.client);
        putVarint(out, r.clock);
        putVarint(out, r.ts);
        const bool sameClient = r.hasOrigin && r.originClient == r.client;
        out.append(char((r.hasOrigin ? 1 : 0) | (sameClient ? 2 : 0)));
        if (r.hasOrigin) {
            if (!sameClient)
                putVarint(out, r.originClient);
            putVarint(out, r.originClock);
        }
        putVarint(out, r.len());
        putVarint(out, utf8.size());
        out.append(utf8);
    }
    normalize(ranges);
    qsizetype clients = 0;
    for (size_t i = 0; i < ranges.size(); ++i)
        clients += i == 0 || ranges[i].client != ranges[i - 1].client;
    putVarint(out, clients);
    for (size_t i = 0; i < ranges.size();) {
        size_t end = i;
        while (end < ranges.size() && ranges[end].client == ranges[i].client)
            ++end;
        putVarint(out, ranges[i].client);
        putVarint(out, end - i);
        for (; i < end; ++i) {
            putVarint(out, ranges[i].clock);
            putVarint(out, ranges[i].len);
        }
    }
    return out;
}

bool decodeVector(const QByteArray &bytes, std::unordered_map<quint64, quint32> &out)
{
    if (bytes.isEmpty())
        return true; // an empty vector: has seen nothing
    Reader in(bytes);
    quint64 count;
    if (!in.varint(count) || count > quint64(in.remaining()) / 2)
        return false;
    for (quint64 i = 0; i < count; ++i) {
        quint64 client;
        quint32 clock;
        if (!in.varint(client) || !in.u32(clock))
            return false;
        out[client] = clock;
    }
    return in.remaining() == 0;
}

class NativeDoc final : public CrdtDoc {
public:
    explicit NativeDoc(quint64 client) : m_client(client) {}

    QString text() const override
    {
        QString s;
        s.reserve(m_visibleLen);
        for (const Run *r = m_head; r; r = r->next)
            if (!r->deleted)
                s += r->text;
        return s;
    }

    QByteArray localEdit(qsizetype at, qsizetype removed, const QString &inserted) override
    {
        if (at < 0 || removed < 0 || at > m_visibleLen || removed > m_visibleLen - at
            || (removed == 0 && inserted.isEmpty()))
            return {};
        const QString text = sanitized(inserted);
        const quint64 n = quint64(text.size());
        if (quint64(stateOf(m_client)) + n > kMaxU32 || quint64(m_lamport) + n + 1 > kMaxU32)
            return {}; // out of clock; refused before anything changed

        std::vector<Range> ranges;
        if (removed > 0)
            removeVisible(at, removed, ranges);

        std::vector<RunData> runs;
        if (n > 0) {
            RunData r;
            r.client = m_client;
            r.clock = stateOf(m_client);
            r.ts = m_lamport + 1;
            r.text = text;
            Run *origin = nullptr;
            qsizetype off = 0;
            if (at > 0) {
                origin = locate(at - 1, &off);
                r.hasOrigin = true;
                r.originClient = origin->client;
                r.originClock = origin->clock + quint32(off);
            }
            runs.push_back(r);
            if (origin && continues(*origin, r)) {
                // Typing on at the end of our own run: grow it (amortised O(1)).
                origin->text += r.text;
                m_state[m_client] = r.clock + quint32(n);
                m_lamport += quint32(n);
                m_visibleLen += qsizetype(n);
            } else {
                // The remote path, so local and remote placement are identical.
                Run *placed = integrate(std::move(r), false);
                m_cursorRun = placed;
                m_cursorBefore = at;
            }
        }
        return encodeUpdate(runs, std::move(ranges));
    }

    QList<CrdtEdit> applyUpdate(const QByteArray &update, bool *ok) override
    {
        Decoded d;
        qsizetype weight = 0;
        bool good = decodeUpdate(update, d);
        if (good) {
            for (const RunData &r : d.runs)
                weight += qsizetype(r.len()) + kRunCost;
            weight += qsizetype(d.ranges.size()) * kRunCost;
            good = weight <= kPendingCap && m_pendingWeight + weight <= kPendingCap;
        }
        if (ok)
            *ok = good;
        if (!good)
            return {};

        for (RunData &r : d.runs)
            admit(std::move(r));
        for (const Range &g : d.ranges)
            admitRange(g);
        drain();
        return m_touched ? deltaWalk() : QList<CrdtEdit>();
    }

    QByteArray stateVector() const override
    {
        std::vector<std::pair<quint64, quint32>> entries;
        for (const auto &kv : m_state)
            if (kv.second > 0)
                entries.push_back(kv);
        std::sort(entries.begin(), entries.end());
        QByteArray out;
        putVarint(out, entries.size());
        for (const auto &e : entries) {
            putVarint(out, e.first);
            putVarint(out, e.second);
        }
        return out;
    }

    QByteArray diffSince(const QByteArray &remoteStateVector) const override
    {
        std::unordered_map<quint64, quint32> have;
        if (!decodeVector(remoteStateVector, have))
            return {};
        std::vector<quint64> clients;
        for (const auto &kv : m_byClient)
            clients.push_back(kv.first);
        std::sort(clients.begin(), clients.end());
        std::vector<RunData> runs;
        std::vector<Range> deleted;
        for (const quint64 client : clients) {
            const auto it = have.find(client);
            const quint32 from = it == have.end() ? 0 : it->second;
            for (const auto &kv : m_byClient.at(client)) {
                const Run &r = *kv.second;
                if (r.deleted)
                    deleted.push_back({client, r.clock, r.len()});
                if (quint64(r.clock) + r.len() <= from)
                    continue;
                RunData d = tail(r, r.clock < from ? from - r.clock : 0);
                if (!runs.empty() && continues(runs.back(), d))
                    runs.back().text += d.text;
                else
                    runs.push_back(std::move(d));
            }
        }
        // Limit: the whole delete set goes into every diff, so a diff is
        // never smaller than the history of deletions; a per-client delete
        // clock in the state vector would bound it.
        return encodeUpdate(runs, std::move(deleted));
    }

    QByteArray encodeState() const override { return diffSince({}); }

    // A position is 'P' 0x01 kind, kind 0: the start of the note, kind 1 then
    // `client clock`: right after that item (a varint and a u32, like a run's).
    // Limit: both walk the runs from the head, O(runs); a caret goes out at
    // most ten times a second, so a long note is fine, an order-statistics
    // tree over the runs (see locate) would make it O(log runs) if a profile asks.
    QByteArray encodePosition(qsizetype offset) const override
    {
        QByteArray out("P\x01", 2);
        if (offset < 0 || offset > m_visibleLen)
            return {};
        if (offset == 0)
            return out + char(0);
        qsizetype before = 0;
        for (const Run *r = m_head; r; r = r->next) {
            if (r->deleted)
                continue;
            if (offset <= before + qsizetype(r->len())) {
                out += char(1);
                putVarint(out, r->client);
                putVarint(out, r->clock + quint32(offset - before - 1));
                return out;
            }
            before += r->len();
        }
        return {};
    }

    qsizetype resolvePosition(const QByteArray &position) const override
    {
        Reader in(position);
        uchar magic, version, kind;
        if (!in.byte(magic) || magic != 'P' || !in.byte(version) || version != 1 || !in.byte(kind) || kind > 1)
            return -1;
        if (kind == 0)
            return in.remaining() == 0 ? 0 : -1;
        quint64 client;
        quint32 clock;
        qsizetype off;
        if (!in.varint(client) || !in.u32(clock) || in.remaining() != 0)
            return -1;
        const Run *item = findRun(client, clock, &off);
        if (!item)
            return -1;
        qsizetype before = 0;
        for (const Run *r = m_head; r != item; r = r->next)
            if (!r->deleted)
                before += r->len();
        return item->deleted ? before : before + off + 1;
    }

    bool loadState(const QByteArray &state) override
    {
        bool ok = false;
        applyUpdate(state, &ok);
        return ok;
    }

private:
    quint32 stateOf(quint64 client) const
    {
        const auto it = m_state.find(client);
        return it == m_state.end() ? 0 : it->second;
    }

    static QString sanitized(const QString &s)
    {
        QString out = s;
        for (qsizetype i = 0; i < s.size(); ++i) {
            if (s.at(i).isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate())
                ++i;
            else if (s.at(i).isSurrogate())
                out[i] = QChar::ReplacementCharacter; // detaches only here
        }
        return out;
    }

    Run *findRun(quint64 client, quint32 clock, qsizetype *off) const
    {
        const auto it = m_byClient.find(client);
        if (it == m_byClient.end())
            return nullptr;
        auto j = it->second.upper_bound(clock);
        if (j == it->second.begin())
            return nullptr;
        Run *r = std::prev(j)->second;
        if (quint64(clock) - r->clock >= r->len())
            return nullptr;
        *off = qsizetype(clock - r->clock);
        return r;
    }

    /// Cut `r` after its first `k` items (0 < k < len); returns the right part.
    Run *splitAt(Run *r, qsizetype k)
    {
        Run &n = m_arena.emplace_back(); // Limit: the arena never shrinks, there is no garbage collection of tombstones
        n.client = r->client;
        n.clock = r->clock + quint32(k);
        n.ts = r->ts + quint32(k);
        n.hasOrigin = true;
        n.originClient = r->client;
        n.originClock = n.clock - 1;
        n.text = r->text.mid(k);
        r->text.truncate(k);
        n.deleted = r->deleted;
        n.fresh = r->fresh;
        n.justDeleted = r->justDeleted;
        n.prev = r;
        n.next = r->next;
        (r->next ? r->next->prev : m_tail) = &n;
        r->next = &n;
        m_byClient[n.client][n.clock] = &n;
        return &n;
    }

    /// The visible run holding visible unit `idx` (0 <= idx < m_visibleLen).
    /// Limit: walks runs from the cursor (where the last edit was), so
    /// typing in one place is O(1) and a jump is O(runs); an order-statistics
    /// tree over the runs would make jumps O(log runs) if a profile asks.
    Run *locate(qsizetype idx, qsizetype *off)
    {
        Run *r = m_cursorRun;
        qsizetype before = m_cursorBefore;
        if (!r) {
            r = m_head;
            before = 0;
            while (r->deleted)
                r = r->next;
        }
        while (idx < before) {
            do
                r = r->prev;
            while (r->deleted);
            before -= r->len();
        }
        while (idx >= before + qsizetype(r->len())) {
            before += r->len();
            do
                r = r->next;
            while (r->deleted);
        }
        m_cursorRun = r;
        m_cursorBefore = before;
        *off = idx - before;
        return r;
    }

    /// Tombstone `n` visible units from visible offset `at`.
    void removeVisible(qsizetype at, qsizetype n, std::vector<Range> &ranges)
    {
        qsizetype off;
        Run *r = locate(at, &off);
        if (off > 0)
            r = splitAt(r, off);
        Run *first = r;
        while (n > 0 && r) {
            if (!r->deleted) {
                if (qsizetype(r->len()) > n)
                    splitAt(r, n);
                r->deleted = true;
                ranges.push_back({r->client, r->clock, r->len()});
                m_visibleLen -= r->len();
                n -= r->len();
            }
            r = r->next;
        }
        // Leave the cursor on the visible run just left of the hole.
        Run *left = first->prev;
        while (left && left->deleted)
            left = left->prev;
        m_cursorRun = left;
        m_cursorBefore = left ? at - qsizetype(left->len()) : 0;
    }

    /// Place a run whose clock is next in line and whose origin is known.
    Run *integrate(RunData &&data, bool remote)
    {
        Run *cur = m_head;
        if (data.hasOrigin) {
            qsizetype off;
            Run *o = findRun(data.originClient, data.originClock, &off);
            if (!o)
                return nullptr;
            if (off + 1 < qsizetype(o->len()))
                splitAt(o, off + 1);
            cur = o->next;
        }
        while (cur && (cur->ts != data.ts ? cur->ts > data.ts : cur->client > data.client))
            cur = cur->next;
        Run &n = m_arena.emplace_back();
        static_cast<RunData &>(n) = std::move(data);
        n.fresh = remote;
        Run *prev = cur ? cur->prev : m_tail;
        n.prev = prev;
        n.next = cur;
        (prev ? prev->next : m_head) = &n;
        (cur ? cur->prev : m_tail) = &n;
        m_byClient[n.client][n.clock] = &n;
        m_state[n.client] = n.clock + n.len();
        m_lamport = std::max<quint32>(m_lamport, n.ts + n.len() - 1);
        m_visibleLen += n.len();
        if (remote) {
            m_touched = true;
            m_advanced.push_back(n.client);
        }
        return &n;
    }

    /// Take in a remote run: drop what is known, queue it if it cannot go in
    /// yet, otherwise integrate it.
    void admit(RunData r)
    {
        const quint32 have = stateOf(r.client);
        if (quint64(r.clock) + r.len() <= have)
            return;
        if (r.clock < have)
            r = tail(r, have - r.clock);
        if (r.clock > have)
            return block({r.client, r.clock - 1}, std::move(r));
        if (r.hasOrigin && stateOf(r.originClient) <= r.originClock)
            return block({r.originClient, r.originClock}, std::move(r));
        integrate(std::move(r), true);
    }

    void block(Need need, RunData &&r)
    {
        m_pendingWeight += qsizetype(r.len()) + kRunCost;
        m_waitRuns.emplace(need, std::move(r));
    }

    void admitRange(Range g)
    {
        const quint64 end = quint64(g.clock) + g.len;
        const quint32 have = stateOf(g.client);
        if (have > g.clock)
            deleteKnown(g.client, g.clock, quint32(std::min<quint64>(end, have) - g.clock));
        if (end > have) {
            const quint32 from = std::max(g.clock, have);
            const auto put = m_waitDel.emplace(Need{g.client, quint32(end - 1)}, Range{g.client, from, quint32(end - from)});
            if (put.second)
                m_pendingWeight += kRunCost;
            else if (from < put.first->second.clock) // the same wait, keep the wider range
                put.first->second = {g.client, from, quint32(end - from)};
        }
    }

    /// Tombstone [clock, clock+len) of a client, all of which is known.
    void deleteKnown(quint64 client, quint32 clock, quint32 len)
    {
        const quint64 end = quint64(clock) + len;
        while (clock < end) {
            qsizetype off;
            Run *r = findRun(client, clock, &off);
            if (!r)
                return;
            const qsizetype take = qsizetype(std::min<quint64>(end - clock, r->len() - quint64(off)));
            if (!r->deleted) {
                if (off > 0)
                    r = splitAt(r, off);
                if (take < qsizetype(r->len()))
                    splitAt(r, take);
                r->deleted = r->justDeleted = true;
                m_visibleLen -= r->len();
                m_touched = true;
            }
            clock += quint32(take);
        }
    }

    /// Everything waiting on a client whose state moved, until nothing moves.
    /// Each queued run wakes at most three times, so this is O(n log n) even
    /// for a hostile chain of dependencies.
    void drain()
    {
        while (!m_advanced.empty()) {
            const quint64 client = m_advanced.back();
            m_advanced.pop_back();
            const quint32 have = stateOf(client);
            const auto lo = m_waitRuns.lower_bound(Need{client, 0}), hi = m_waitRuns.lower_bound(Need{client, have});
            std::vector<RunData> woke;
            for (auto it = lo; it != hi; ++it) {
                m_pendingWeight -= qsizetype(it->second.len()) + kRunCost;
                woke.push_back(std::move(it->second));
            }
            m_waitRuns.erase(lo, hi);
            const auto dlo = m_waitDel.lower_bound(Need{client, 0}), dhi = m_waitDel.lower_bound(Need{client, have});
            std::vector<Range> deletes;
            for (auto it = dlo; it != dhi; ++it) {
                m_pendingWeight -= kRunCost;
                deletes.push_back(it->second);
            }
            m_waitDel.erase(dlo, dhi);
            for (RunData &r : woke)
                admit(std::move(r));
            for (const Range &g : deletes)
                deleteKnown(g.client, g.clock, g.len);
        }
    }

    /// What an update changed in the text, in the order and coordinates the
    /// editor applies it: a retain moves `pos`, an insert is an edit at `pos`
    /// that moves it past the new text, a delete is an edit at `pos`. Also
    /// clears the transient flags and re-anchors the cursor.
    /// Limit: O(runs) per remote update, the price of not keeping an index.
    QList<CrdtEdit> deltaWalk()
    {
        QList<CrdtEdit> out;
        qsizetype pos = 0;
        for (Run *r = m_head; r; r = r->next) {
            if (r == m_cursorRun) {
                if (r->deleted)
                    m_cursorRun = nullptr;
                else
                    m_cursorBefore = pos;
            }
            if (r->fresh && !r->justDeleted) {
                if (!out.isEmpty() && out.last().removed == 0 && out.last().at + out.last().inserted.size() == pos)
                    out.last().inserted += r->text;
                else
                    out.append({pos, 0, r->text});
                pos += r->len();
            } else if (r->justDeleted && !r->fresh) {
                if (!out.isEmpty() && out.last().inserted.isEmpty() && out.last().at == pos)
                    out.last().removed += r->len();
                else
                    out.append({pos, qsizetype(r->len()), QString()});
            } else if (!r->deleted && !r->fresh) {
                pos += r->len();
            } // else born dead in this update, or an old tombstone
            r->fresh = r->justDeleted = false;
        }
        m_touched = false;
        return out;
    }

    quint64 m_client;
    quint32 m_lamport = 0;
    std::deque<Run> m_arena; // stable addresses
    Run *m_head = nullptr, *m_tail = nullptr;
    std::unordered_map<quint64, std::map<quint32, Run *>> m_byClient; // first clock -> run
    std::unordered_map<quint64, quint32> m_state; // next clock per client; absent is 0
    qsizetype m_visibleLen = 0;
    Run *m_cursorRun = nullptr; // a visible run, or null
    qsizetype m_cursorBefore = 0; // visible units before m_cursorRun

    // Runs and deletions that wait for edits not seen yet, keyed by what they
    // wait for; m_pendingWeight is bounded by kPendingCap.
    std::multimap<Need, RunData> m_waitRuns;
    std::map<Need, Range> m_waitDel;
    qsizetype m_pendingWeight = 0;
    // Scratch of one applyUpdate().
    std::vector<quint64> m_advanced;
    bool m_touched = false;
};

} // namespace

std::unique_ptr<CrdtDoc> makeNativeDoc(quint64 clientID)
{
    return std::make_unique<NativeDoc>(clientID);
}

} // namespace noteAppC
