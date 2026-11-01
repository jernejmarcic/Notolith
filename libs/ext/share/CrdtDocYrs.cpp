// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
#include "CrdtDoc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// The C surface of libs/ext/share/yrs-ffi/src/lib.rs.
extern "C" {
struct NHandle;
struct NBuf {
    uint8_t *ptr;
    size_t len;
};
NHandle *ncrdt_new(uint64_t clientID);
void ncrdt_free(NHandle *);
void ncrdt_buf_free(uint8_t *ptr, size_t len);
int ncrdt_text(NHandle *, NBuf *out);
int ncrdt_local_edit(NHandle *, uint32_t at, uint32_t removed, const uint8_t *ins, size_t insLen, NBuf *update);
int ncrdt_apply(NHandle *, const uint8_t *update, size_t len, NBuf *delta);
int ncrdt_state_vector(NHandle *, NBuf *out);
int ncrdt_diff(NHandle *, const uint8_t *sv, size_t svLen, NBuf *out);
int ncrdt_encode_position(NHandle *, uint32_t offset, NBuf *out);
int ncrdt_resolve_position(NHandle *, const uint8_t *pos, size_t len, uint32_t *at);
}

namespace noteAppC {
namespace {

/// A buffer Rust handed out, given back on scope exit.
struct Owned {
    NBuf buf{nullptr, 0};
    ~Owned() { ncrdt_buf_free(buf.ptr, buf.len); }
    QByteArray bytes() const { return QByteArray(reinterpret_cast<const char *>(buf.ptr), qsizetype(buf.len)); }
};

const uint8_t *raw(const QByteArray &bytes)
{
    return reinterpret_cast<const uint8_t *>(bytes.constData());
}

constexpr uint32_t kMaxUnits = 0x7fffffff; // yrs counts in u32

class YrsDoc final : public CrdtDoc {
public:
    explicit YrsDoc(NHandle *handle) : m_h(handle) {}
    ~YrsDoc() override { ncrdt_free(m_h); }

    QString text() const override
    {
        Owned out;
        return ncrdt_text(m_h, &out.buf) == 0 ? QString::fromUtf8(out.bytes()) : QString();
    }

    QByteArray localEdit(qsizetype at, qsizetype removed, const QString &inserted) override
    {
        if (at < 0 || removed < 0 || at > kMaxUnits || removed > kMaxUnits || inserted.size() > kMaxUnits)
            return {};
        const QByteArray utf8 = inserted.toUtf8();
        Owned update;
        // A failure halfway still hands over what changed (empty otherwise).
        ncrdt_local_edit(m_h, uint32_t(at), uint32_t(removed), raw(utf8), size_t(utf8.size()), &update.buf);
        return update.bytes();
    }

    QList<CrdtEdit> applyUpdate(const QByteArray &update, bool *ok) override
    {
        Owned delta;
        const bool good = ncrdt_apply(m_h, raw(update), size_t(update.size()), &delta.buf) == 0;
        if (ok)
            *ok = good;
        return decodeDelta(delta.bytes()); // after a failure: the part of the update that did land
    }

    QByteArray stateVector() const override
    {
        Owned out;
        return ncrdt_state_vector(m_h, &out.buf) == 0 ? out.bytes() : QByteArray();
    }

    QByteArray diffSince(const QByteArray &remoteStateVector) const override
    {
        Owned out;
        return ncrdt_diff(m_h, raw(remoteStateVector), size_t(remoteStateVector.size()), &out.buf) == 0
            ? out.bytes() : QByteArray();
    }

    QByteArray encodeState() const override { return diffSince({}); }

    QByteArray encodePosition(qsizetype offset) const override
    {
        Owned out;
        if (offset < 0 || offset > kMaxUnits || ncrdt_encode_position(m_h, uint32_t(offset), &out.buf) != 0)
            return {};
        return out.bytes();
    }

    qsizetype resolvePosition(const QByteArray &position) const override
    {
        uint32_t at = 0;
        return ncrdt_resolve_position(m_h, raw(position), size_t(position.size()), &at) == 0 ? qsizetype(at) : -1;
    }

    bool loadState(const QByteArray &state) override
    {
        bool ok = false;
        applyUpdate(state, &ok);
        return ok;
    }

private:
    // The stream is documented above encode_delta() in lib.rs: a retain moves
    // the cursor, a delete is an edit at the cursor, an insert is an edit at
    // the cursor that moves it past the new text. Doing it in this order is
    // what makes every edit apply to the text the previous one left.
    static QList<CrdtEdit> decodeDelta(const QByteArray &stream)
    {
        QList<CrdtEdit> edits;
        const char *p = stream.constData();
        const char *end = p + stream.size();
        auto u32 = [&p] {
            uint32_t v;
            std::memcpy(&v, p, 4); // little endian, like every target this builds for
            p += 4;
            return v;
        };
        qsizetype pos = 0;
        while (end - p >= 5) {
            const char tag = *p++;
            const uint32_t units = u32();
            if (tag == 0) {
                pos += units;
            } else if (tag == 1) {
                edits.append({pos, qsizetype(units), QString()});
            } else if (end - p >= 4) {
                const uint32_t bytes = u32();
                if (end - p < qsizetype(bytes))
                    break;
                QString inserted = QString::fromUtf8(p, qsizetype(bytes));
                p += bytes;
                const qsizetype at = pos;
                pos += inserted.size();
                edits.append({at, 0, std::move(inserted)});
            }
        }
        return edits;
    }

    NHandle *m_h;
};

} // namespace

std::unique_ptr<CrdtDoc> makeYrsDoc(quint64 clientID)
{
    NHandle *handle = ncrdt_new(clientID);
    return handle ? std::make_unique<YrsDoc>(handle) : nullptr;
}

} // namespace noteAppC
