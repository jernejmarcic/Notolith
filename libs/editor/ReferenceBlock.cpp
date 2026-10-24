/// A ```` ```reference ```` block (Code Styler's file reference, found by the
/// parser only when the advanced-code-blocks plugin is on): a vault file or an
/// `http(s)` URL shown as a highlighted listing under the file's name, the
/// lines `start` to `end` of it. Read this file's neighbour CSVembed.cpp first;
/// the cache, the watcher and the delivery are the same contract.
///
/// Nothing is read or fetched in layout. A vault file is read on a worker
/// thread (QtConcurrent) and a URL by QNetworkAccessManager, which is
/// asynchronous itself; the block says "Loading..." until deliverReference
/// lands. A QFileSystemWatcher (made with the first file, debounced 200 ms)
/// relays out the blocks whose file changed, so an edit in another program shows
/// with nothing else happening.
///
/// A URL is fetched only while setRemoteReferences is on (the window's
/// setting, off by default), at most once per five minutes per URL across every
/// editor, and a copy older than that is shown while the next one comes. Only
/// http and https are ever asked for, a redirect never leaves https for http,
/// and a body is cut at kReferenceMaxBytes.

#include "MarkdownEditor.h"
#include "MarkdownEditor_p.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace noteAppC {

namespace {

constexpr qint64 kReferenceMaxBytes = 2 * 1024 * 1024;
// ponytail: the listing is a QTextLayout built on the UI thread on every
// relayout, so what is shown is cut here; a viewport-only layout is the upgrade.
constexpr int kReferenceMaxLines = 500;
constexpr qint64 kRemoteFreshMs = 5 * 60 * 1000;
constexpr int kRemoteTimeoutMs = 15'000;

// setReferenceReadHookForTests': read by worker threads, so atomic.
std::atomic<void (*)(const QString &)> gReferenceReadHook{nullptr};

std::atomic<int> gReferenceScans{0};
QString gAllowedAddress; // the tests' server; UI thread only
constexpr int kMaxRemoteCopies = 32;
constexpr int kMaxRedirects = 5;
constexpr int kReferenceMaxLineChars = 1000;
constexpr int kReferenceMaxScanLines = 50000; ///< lines a start:/end: pattern is tried on

struct RemoteCopy {
    QString text;
    QString error;
    qint64 fetchedAt = 0;
};

/// Every URL any editor in the process has fetched: UI thread only.
QHash<QString, RemoteCopy> &remoteCopies()
{
    static QHash<QString, RemoteCopy> copies;
    return copies;
}

/// Every URL's text is kept for five minutes at most and 32 at a time.
void rememberRemote(const QString &key, RemoteCopy copy)
{
    auto &copies = remoteCopies();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = copies.begin(); it != copies.end();) {
        it = now - it->fetchedAt >= kRemoteFreshMs ? copies.erase(it) : std::next(it);
    }
    while (copies.size() >= kMaxRemoteCopies) {
        auto oldest = copies.begin();
        for (auto it = copies.begin(); it != copies.end(); ++it) {
            if (it->fetchedAt < oldest->fetchedAt)
                oldest = it;
        }
        copies.erase(oldest);
    }
    copies.insert(key, std::move(copy));
}

/// An address that is not the internet: this machine, its network, link-local
/// (cloud metadata is 169.254.169.254), private, unique-local, shared (CGNAT),
/// multicast, unspecified. A note must not make the machine ask them.
bool isInternalV4(quint32 v4)
{
    // Qt's isSiteLocal/isGlobal know no private IPv4: the ranges are spelled out
    const auto in = [v4](quint32 net, int bits) { return (v4 >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8)
           || in(0xA9FE0000, 16) || in(0xAC100000, 12) || in(0xC0000000, 24) || in(0xC0A80000, 16)
           || in(0xC6120000, 15) || v4 >= 0xE0000000; // multicast, reserved, broadcast
}

bool isInternalAddress(const QHostAddress &address)
{
    if (!gAllowedAddress.isEmpty() && gAllowedAddress.split(QLatin1Char(',')).contains(address.toString()))
        return false;
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4); // also an IPv4-mapped IPv6 address
    if (isV4)
        return isInternalV4(v4);
    // IPv4 carried inside IPv6 reaches the same hosts: ::a.b.c.d (compatible),
    // 64:ff9b::a.b.c.d (NAT64 on a network that has a gateway) and 2002:a.b:c.d:: (6to4)
    const Q_IPV6ADDR six = address.toIPv6Address();
    const auto low = [&six] {
        return (quint32(six[12]) << 24) | (quint32(six[13]) << 16) | (quint32(six[14]) << 8) | quint32(six[15]);
    };
    bool zeros = true;
    for (int n = 0; n < 12; ++n)
        zeros = zeros && six[n] == 0;
    static const uchar nat64[12] = {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    if ((zeros && address.protocol() == QAbstractSocket::IPv6Protocol && low() > 1)
        || std::equal(nat64, nat64 + 12, six.c)) {
        if (isInternalV4(low()))
            return true;
    }
    if (six[0] == 0x20 && six[1] == 0x02) {
        const quint32 embedded = (quint32(six[2]) << 24) | (quint32(six[3]) << 16) | (quint32(six[4]) << 8) | quint32(six[5]);
        if (isInternalV4(embedded))
            return true;
    }
    return address.isNull() || !address.isGlobal() || address.isLoopback() || address.isLinkLocal()
           || address.isSiteLocal() || address.isUniqueLocalUnicast() || address.isMulticast();
}

bool isRemote(const QString &target)
{
    return target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
           || target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

/// The address a hosting site's page for a file is a view of: GitHub's
/// `/blob/` and GitLab's `/-/blob/` pages are HTML, the raw file is elsewhere.
QString rawURL(const QString &link)
{
    const QUrl url(link);
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if ((url.host() == QLatin1String("github.com") || url.host() == QLatin1String("www.github.com")) && parts.size() > 4 && parts.at(2) == QLatin1String("blob"))
        return QStringLiteral("https://raw.githubusercontent.com/") + parts.mid(0, 2).join(QLatin1Char('/'))
               + QLatin1Char('/') + parts.mid(3).join(QLatin1Char('/'));
    if (url.host().startsWith(QLatin1String("gitlab")) && url.path().contains(QLatin1String("/-/blob/"))) {
        QString path = url.path();
        path.replace(QLatin1String("/-/blob/"), QLatin1String("/-/raw/"));
        QUrl raw = url;
        raw.setPath(path);
        return raw.toString();
    }
    return link;
}

/// Bytes to text the way a note is read: UTF-8, LF line ends; empty with
/// `error` set for a file that is not text.
QString decode(QByteArray bytes, QString &error, const QString &binary)
{
    if (bytes.left(8192).contains('\0')) {
        error = binary;
        return {};
    }
    if (bytes.size() >= kReferenceMaxBytes) {
        // whole lines only: the cut is somewhere in the middle of one
        const qsizetype lastBreak = bytes.lastIndexOf('\n');
        if (lastBreak >= 0)
            bytes.truncate(lastBreak + 1);
    }
    QString text = QString::fromUtf8(bytes);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    return text;
}

/// The 1-based line a `start`/`end` spec names, searching from `from`: a
/// number is itself, `/regex/` and any other word the first line (at or after
/// `from`) that matches or contains it. 0 when no line does.
int lineMatching(const QString &text, const QString &spec, int from)
{
    bool isNumber = false;
    const int number = spec.toInt(&isNumber);
    if (isNumber)
        return qMax(1, number);
    std::optional<QRegularExpression> pattern;
    const bool isRegex = spec.size() > 2 && spec.startsWith(QLatin1Char('/')) && spec.endsWith(QLatin1Char('/'));
    if (isRegex) {
        pattern = userRegularExpression(spec.mid(1, spec.size() - 2));
        if (!pattern)
            return 0; // invalid, too long, or one that sets its own budget
    }
    int line = 1;
    qsizetype at = 0;
    while (at <= text.size()) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), at);
        if (end < 0)
            end = text.size();
        if (line > kReferenceMaxScanLines && isRegex)
            return 0; // the pattern has had its share: a worker does the scan, but not for ever
        if (line >= from) {
            const QStringView row = QStringView(text).mid(at, end - at);
            if (isRegex ? pattern->match(row).hasMatch() : row.contains(spec))
                return line;
        }
        if (end >= text.size())
            break;
        at = end + 1;
        ++line;
    }
    return 0;
}

/// Lines `start` to `end` of `text`; `end` empty is the end of the file and
/// `+N` is N lines past `start`.
ReferenceExcerpt excerptOf(const QString &text, const QString &start, const QString &end)
{
    ++gReferenceScans;
    ReferenceExcerpt out;
    out.ranged = !start.isEmpty() || !end.isEmpty();
    if (!start.isEmpty()) {
        out.first = lineMatching(text, start, 1);
        if (out.first == 0) {
            out.error = QObject::tr("No line matches start: %1").arg(start);
            return out;
        }
    }
    int last = std::numeric_limits<int>::max();
    if (end.startsWith(QLatin1Char('+')) && end.mid(1).toInt() > 0) {
        last = out.first + end.mid(1).toInt();
    } else if (!end.isEmpty()) {
        const int found = lineMatching(text, end, out.first);
        if (found == 0) {
            out.error = QObject::tr("No line matches end: %1").arg(end);
            return out;
        }
        last = qMax(found, out.first);
    }
    bool capped = false;
    if (last - out.first + 1 > kReferenceMaxLines) {
        last = out.first + kReferenceMaxLines - 1;
        capped = true;
    }

    int line = 1;
    qsizetype at = 0;
    qsizetype from = -1;
    qsizetype to = text.size();
    while (at <= text.size()) {
        qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), at);
        const bool lastLine = lineEnd < 0;
        if (lastLine)
            lineEnd = text.size();
        if (line == out.first)
            from = at;
        if (from >= 0) {
            out.last = line;
            to = lineEnd;
            if (line >= last)
                break;
        }
        if (lastLine)
            break;
        at = lineEnd + 1;
        ++line;
    }
    if (from < 0) {
        out.error = QObject::tr("The file has only %1 lines").arg(line);
        return out;
    }
    out.text = text.mid(from, to - from);
    // a minified file is one line of megabytes: the listing wraps what it shows,
    // so a line is cut where nobody reads on
    if (out.text.size() > kReferenceMaxLineChars) {
        QStringList lines = out.text.split(QLatin1Char('\n'));
        for (QString &row : lines) {
            if (row.size() > kReferenceMaxLineChars)
                row = row.left(kReferenceMaxLineChars) + QChar(0x2026);
        }
        out.text = lines.join(QLatin1Char('\n'));
    }
    // a trailing newline is the end of the last line, not more lines after it
    out.cut = capped && to + 1 < text.size();
    return out;
}

} // namespace

void MarkdownEditor::setVaultRootProvider(std::function<QString()> provider)
{
    d->m_vaultRoot = std::move(provider);
}

void MarkdownEditor::dropRemoteReferences()
{
    // what a reply in flight would deliver is no longer wanted: cut it loose first
    if (d->m_referenceNet) {
        for (QNetworkReply *reply : d->m_referenceNet->findChildren<QNetworkReply *>()) {
            reply->disconnect();
            reply->abort();
        }
        d->m_referenceNet->deleteLater();
        d->m_referenceNet = nullptr;
    }
    for (auto it = d->m_referenceCache.begin(); it != d->m_referenceCache.end();)
        it = isRemote(it.key()) ? d->m_referenceCache.erase(it) : std::next(it);
    remoteCopies().clear();
}

void MarkdownEditor::dropReferenceCache()
{
    dropRemoteReferences();
    d->m_referenceCache.clear();
    d->m_referenceGone.clear();
    d->m_referenceWatched.clear();
    // the watcher goes too: its inotify instance is released, and the next
    // reference block makes a new one
    delete d->m_referenceWatcher;
    d->m_referenceWatcher = nullptr;
    delete d->m_referenceRefresh;
    d->m_referenceRefresh = nullptr;
}

void MarkdownEditor::setRemoteReferences(bool on)
{
    if (d->m_remoteReferences == on)
        return;
    d->m_remoteReferences = on;
    if (!on)
        dropRemoteReferences();
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || block.extras->referenceTarget.isEmpty())
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
    }
    updateLayout();
    viewport()->update();
}

bool MarkdownEditor::referencesPending() const
{
    for (const ReferenceCacheEntry &entry : std::as_const(d->m_referenceCache)) {
        if (entry.pending)
            return true;
    }
    return false;
}

int MarkdownEditor::referenceCacheSize() const
{
    return int(d->m_referenceCache.size());
}

int MarkdownEditor::panelFormatCountAt(qsizetype offset) const
{
    const qsizetype index = blockAt(offset);
    if (index < 0 || index >= qsizetype(d->m_blocks.size()) || !d->m_blocks[size_t(index)].layout)
        return 0;
    return int(d->m_blocks[size_t(index)].layout->formats().size());
}

QString MarkdownEditor::rawReferenceURL(const QString &link)
{
    return rawURL(link);
}

int MarkdownEditor::referenceWatchedForTests() const
{
    return d->m_referenceWatcher ? int(d->m_referenceWatcher->files().size() + d->m_referenceWatcher->directories().size())
                                 : 0;
}

int MarkdownEditor::remoteReferenceCopiesForTests()
{
    return int(remoteCopies().size());
}

int MarkdownEditor::referenceScansForTests()
{
    return gReferenceScans.load();
}

void MarkdownEditor::allowReferenceAddressForTests(const QString &address)
{
    gAllowedAddress = address;
}

void MarkdownEditor::setReferenceReadHookForTests(void (*hook)(const QString &))
{
    gReferenceReadHook.store(hook);
}

bool MarkdownEditor::layoutReference(BlockView &view)
{
    if (view.span.kind != BlockSpan::Kind::Code || d->m_decorations.references.isEmpty())
        return false;
    const CodeFenceSpan *fence = codeFenceOf(view);
    if (!fence)
        return false;
    const ReferenceSpan *reference = nullptr;
    for (const ReferenceSpan &candidate : std::as_const(d->m_decorations.references)) {
        if (candidate.block.start == fence->block.start) {
            reference = &candidate;
            break;
        }
    }
    if (!reference)
        return false;

    const bool remote = isRemote(reference->target);
    QString target; // what a click on the header opens
    QString key;    // what the read is cached under
    QString name;
    bool outside = false;
    if (remote) {
        target = reference->target;
        key = rawURL(target);
        // the host in the title: it is where a click goes
        name = QUrl(key).host();
        if (!QUrl(key).fileName().isEmpty())
            name = QUrl(key).fileName() + QStringLiteral(" (") + name + QLatin1Char(')');
    } else {
        QString written = reference->target.trimmed();
        if (written.startsWith(QLatin1String("[[")) && written.endsWith(QLatin1String("]]"))) {
            written = written.mid(2, written.size() - 4);
            written = written.section(QLatin1Char('|'), 0, 0).section(QLatin1Char('#'), 0, 0);
        }
        if (written.startsWith(QLatin1String("@/")))
            written.remove(0, 2);
        // Only the vault's own files, symlinks resolved: a note from elsewhere
        // must not make `file: /etc/passwd` or `../../.ssh/id_ed25519` show up.
        // The window's resolver finds a file wherever it is (beside the note,
        // under the root, outside both); the check is here.
        const QString found = m_resolver ? m_resolver(written) : QString();
        const QString real = found.isEmpty() ? QString() : QFileInfo(found).canonicalFilePath();
        const QString provided = d->m_vaultRoot ? d->m_vaultRoot() : QString();
        const QString root = provided.isEmpty() ? QString() : QDir(provided).canonicalPath();
        if (!real.isEmpty() && (root.isEmpty() || !real.startsWith(root + QLatin1Char('/')))) {
            outside = true;
            name = written; // not the file's own name: it is not the reader's to learn
        } else {
            key = real;
            target = key;
            name = real.isEmpty() ? written : QFileInfo(real).fileName();
        }
    }

    // The panel every outcome but the listing itself uses: a header that opens
    // the target, and a line of text.
    const auto panel = [&](const QString &label, const QString &body, bool monospace) {
        layoutTextPanel(view, label, body, monospace);
        view.isPluginBlock = true;
        view.extrasMut().referenceTarget = target;
        view.extrasMut().referenceKey = key;
        return true;
    };

    if (outside)
        return panel(name, tr("Outside the vault: %1").arg(reference->target), false);
    if (key.isEmpty() || (!remote && !QFileInfo(key).isFile()))
        return panel(name, tr("Could not find %1").arg(reference->target), false);
    if (remote && !d->m_remoteReferences)
        return panel(name, tr("Fetching remote files is off. Turn on \"Fetch remote references\" in Settings to show this file."), false);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = d->m_referenceCache.find(key);
    const auto start = [&](ReferenceCacheEntry &entry) {
        entry.pending = true;
        entry.generation = ++d->m_referenceGeneration;
        if (!entry.latest)
            entry.latest = std::make_shared<std::atomic<quint64>>();
        entry.latest->store(entry.generation);
    };
    if (remote) {
        if (it == d->m_referenceCache.end()) {
            it = d->m_referenceCache.insert(key, ReferenceCacheEntry());
            const auto known = remoteCopies().constFind(key);
            if (known != remoteCopies().cend() && now - known->fetchedAt < kRemoteFreshMs) {
                it->pending = false;
                it->text = known->text;
                it->error = known->error;
                it->fetchedAt = known->fetchedAt;
            } else {
                start(*it);
                it->fetchedAt = now;
                fetchReference(key, key, it->generation, 0);
            }
        } else if (!it->pending && now - it->fetchedAt >= kRemoteFreshMs && d->m_referenceDelivering != key) {
            start(*it); // the old text stays on screen until the new arrives
            it->fetchedAt = now;
            fetchReference(key, key, it->generation, 0);
        }
    } else {
        const QFileInfo info(key);
        watchReference(key);
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        const qint64 size = info.size();
        // A read that failed is never kept as the answer (the file may be
        // readable now - a chmod changes neither mtime nor size).
        if (it != d->m_referenceCache.end() && !it->error.isEmpty() && !it->permanent && d->m_referenceDelivering != key)
            it->mtime = -1;
        if (it == d->m_referenceCache.end() || it->mtime != mtime || it->size != size) {
            if (it == d->m_referenceCache.end())
                it = d->m_referenceCache.insert(key, ReferenceCacheEntry());
            it->mtime = mtime;
            it->size = size;
            start(*it);
            const quint64 generation = it->generation;
            const auto latest = it->latest;
            const QString unreadable = tr("Could not read %1").arg(info.fileName());
            const QString binary = tr("%1 is not a text file").arg(info.fileName());
            // Through m_lifeline, as every other worker: the editor may be gone
            // by the time the file has been read.
            (void)QtConcurrent::run([this, lifeline = d->m_lifeline, latest, key, generation, mtime, size, unreadable,
                                     binary] {
                if (latest->load() != generation)
                    return; // a newer read of this file has been asked for
                if (const auto hook = gReferenceReadHook.load())
                    hook(key);
                ReferenceCacheEntry result;
                result.pending = false;
                result.mtime = mtime;
                result.size = size;
                result.generation = generation;
                try {
                    QFile file(key);
                    if (!file.open(QIODevice::ReadOnly))
                        result.error = unreadable;
                    else
                        result.text = decode(file.read(kReferenceMaxBytes), result.error, binary);
                    result.permanent = result.error == binary; // the file would read the same again
                } catch (...) { // out of memory, say: an error to show, never "Loading..." for good
                    result.text.clear();
                    result.error = unreadable;
                }
                lifeline->post(this, [this, key, generation, result = std::move(result)]() mutable {
                    deliverReference(key, generation, std::move(result));
                });
            });
        }
    }

    if (it->text.isEmpty() && !it->error.isEmpty())
        return panel(name, it->error, false);
    if (it->text.isEmpty() && it->pending)
        return panel(name, tr("Loading…"), false);

    // Cut to the block's lines and coloured once per file, range and theme:
    // a relayout (a width change, a delivery elsewhere) takes the kept one.
    const QString language = !reference->language.isEmpty() ? reference->language : QFileInfo(name).suffix();
    const QString memoKey = reference->start + QLatin1Char('\x1f') + reference->end + QLatin1Char('\x1f') + language
                            + QLatin1Char('\x1f') + embedThemeTag();
    auto memo = it->excerpts.find(memoKey);
    if (memo == it->excerpts.end()) {
        // The scan (a start:/end: regex over the whole file) is a worker's job,
        // next to the read: the UI thread never runs a user pattern.
        if (!it->scanning.contains(memoKey)) {
            it->scanning.insert(memoKey);
            const quint64 generation = it->generation;
            (void)QtConcurrent::run([this, lifeline = d->m_lifeline, key, memoKey, language, generation,
                                     text = it->text, start = reference->start, end = reference->end] {
                ReferenceExcerpt made;
                try {
                    made = excerptOf(text, start, end);
                    while (made.text.endsWith(QLatin1Char('\n')))
                        made.text.chop(1);
                } catch (...) { // out of memory, say: an error to show, never "Loading..." for good
                    made = ReferenceExcerpt();
                    made.error = QObject::tr("Could not read the file");
                }
                lifeline->post(this, [this, key, memoKey, language, generation, made = std::move(made)]() mutable {
                    deliverExcerpt(key, generation, memoKey, language, std::move(made));
                });
            });
        }
        return panel(name, tr("Loading…"), false);
    }
    const ReferenceExcerpt &excerpt = *memo;
    if (!excerpt.error.isEmpty())
        return panel(name, excerpt.error, false);
    if (excerpt.text.isEmpty())
        return panel(name, tr("Empty file"), false);

    QString label = name;
    if (excerpt.ranged)
        label += excerpt.first == excerpt.last ? tr(":%1").arg(excerpt.first)
                                               : tr(":%1-%2").arg(excerpt.first).arg(excerpt.last);
    QString body = excerpt.text;
    if (excerpt.cut)
        body += QLatin1Char('\n') + tr("… more lines not shown");
    panel(label, body, true);
    view.layout->setFormats(excerpt.formats);
    return true;
}

void MarkdownEditor::fetchReference(const QString &key, const QString &url, quint64 generation, int hops)
{
    // An answer that is a refusal: delivered from the event loop, never from
    // inside the layout that asked (it relays that very block out).
    const auto fail = [this, key, generation](const QString &why) {
        QTimer::singleShot(0, this, [this, key, generation, why] {
            ReferenceCacheEntry result;
            result.pending = false;
            result.generation = generation;
            result.fetchedAt = QDateTime::currentMSecsSinceEpoch();
            result.error = why;
            deliverReference(key, generation, std::move(result));
        });
    };
    const QUrl parsed(url);
    const QString scheme = parsed.scheme().toLower();
    if ((scheme != QLatin1String("http") && scheme != QLatin1String("https")) || parsed.host().isEmpty()) {
        fail(tr("Not a web address: %1").arg(url));
        return;
    }
    const QString host = parsed.host();
    const auto get = [this, key, url, generation, hops] {
        // a lookup that ends after the setting, the plugin or the block went away sends nothing
        if (!d->m_remoteReferences || !d->m_referenceCache.contains(key))
            return;
        if (!d->m_referenceNet)
            d->m_referenceNet = new QNetworkAccessManager(this);
        QNetworkRequest request{QUrl(url)};
        // followed here, hop by hop, so every host is checked
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(kRemoteTimeoutMs);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Notolith"));
        QNetworkReply *reply = d->m_referenceNet->get(request);
        // set by the progress handler, read by finished: both live in the reply's closures
        const auto large = std::make_shared<bool>(false);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply, large](qint64 received, qint64) {
            if (received > kReferenceMaxBytes) {
                *large = true;
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, key, url, generation, hops, large] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400 && reply->hasRawHeader("Location")) {
                const QUrl next = reply->url().resolved(QUrl(QString::fromUtf8(reply->rawHeader("Location"))));
                const bool downgrade = reply->url().scheme().toLower() == QLatin1String("https")
                                       && next.scheme().toLower() != QLatin1String("https");
                ReferenceCacheEntry refused;
                refused.pending = false;
                refused.generation = generation;
                refused.fetchedAt = QDateTime::currentMSecsSinceEpoch();
                if (hops >= kMaxRedirects)
                    refused.error = tr("Too many redirects from %1").arg(url);
                else if (downgrade)
                    refused.error = tr("%1 redirects from https to http").arg(url);
                if (!refused.error.isEmpty()) {
                    deliverReference(key, generation, std::move(refused));
                    return;
                }
                fetchReference(key, next.toString(), generation, hops + 1);
                return;
            }
            ReferenceCacheEntry result;
            result.pending = false;
            result.generation = generation;
            result.fetchedAt = QDateTime::currentMSecsSinceEpoch();
            if (*large) {
                result.error = tr("%1 is larger than %2 MB").arg(url).arg(kReferenceMaxBytes >> 20);
            } else if (reply->error() != QNetworkReply::NoError || status >= 400) {
                result.error = tr("Could not fetch %1 (%2)").arg(url, status >= 400 ? QString::number(status) : reply->errorString());
            } else {
                result.text = decode(reply->readAll(), result.error, tr("%1 is not a text file").arg(url));
            }
            deliverReference(key, generation, std::move(result));
        });
    };
    const QString refusal = tr("%1 is a local address: not fetched").arg(host);
    // ponytail: the name is resolved here for the check and again by the
    // network manager for the request, so a name that changes in between (DNS
    // rebinding) gets through; pinning the address needs a socket of our own.
    const QHostAddress literal(host);
    if (!literal.isNull()) {
        if (isInternalAddress(literal))
            fail(refusal);
        else
            get();
        return;
    }
    QHostInfo::lookupHost(host, this, [this, fail, get, refusal, host](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            fail(tr("Could not resolve %1").arg(host));
            return;
        }
        for (const QHostAddress &address : info.addresses()) {
            if (isInternalAddress(address)) {
                fail(refusal);
                return;
            }
        }
        get();
    });
}

void MarkdownEditor::deliverReference(const QString &key, quint64 generation, ReferenceCacheEntry result)
{
    auto it = d->m_referenceCache.find(key);
    if (it == d->m_referenceCache.end() || it->generation != generation)
        return; // it changed again, and a newer read is on its way
    const auto latest = it->latest;
    // a failed fetch keeps the text an older one got
    if (result.text.isEmpty() && !result.error.isEmpty() && !it->text.isEmpty() && isRemote(key)) {
        result.text = it->text;
        result.error.clear();
    }
    *it = std::move(result);
    it->latest = latest;
    if (isRemote(key))
        rememberRemote(key, {it->text, it->error, it->fetchedAt});
    d->m_referenceDelivering = key;
    relayReferenceBlocks(key);
    d->m_referenceDelivering.clear();
}

void MarkdownEditor::deliverExcerpt(const QString &key, quint64 generation, const QString &memoKey,
                                    const QString &language, ReferenceExcerpt made)
{
    auto it = d->m_referenceCache.find(key);
    if (it == d->m_referenceCache.end() || it->generation != generation)
        return; // the file changed or the cache was dropped meanwhile
    it->scanning.remove(memoKey);
    if (made.error.isEmpty() && !made.text.isEmpty()) {
        SyntaxMap syntax;
        const bool mapped = syntaxMapFor(styleOfLanguage(language), &syntax);
        made.formats = d->m_code.highlight(made.text, language, mapped ? &syntax : nullptr);
    }
    it->excerpts.insert(memoKey, std::move(made));
    d->m_referenceDelivering = key;
    relayReferenceBlocks(key);
    d->m_referenceDelivering.clear();
}

void MarkdownEditor::relayReferenceBlocks(const QString &key)
{
    bool touched = false;
    for (BlockView &block : d->m_blocks) {
        if (!block.extras || block.extras->referenceKey != key)
            continue;
        block.layout.reset();
        block.laidOut = false;
        block.heightEstimated = false;
        touched = true;
    }
    if (touched) {
        updateLayout();
        viewport()->update();
    }
}

void MarkdownEditor::watchReference(const QString &path)
{
    if (d->m_referenceWatched.contains(path))
        return; // asked at every layout: the watcher's lists are not copied for nothing
    if (!d->m_referenceWatcher) {
        d->m_referenceWatcher = new QFileSystemWatcher(this);
        d->m_referenceRefresh = new QTimer(this);
        d->m_referenceRefresh->setSingleShot(true);
        d->m_referenceRefresh->setInterval(200); // a write in place can come in more than one go
        connect(d->m_referenceWatcher, &QFileSystemWatcher::fileChanged, this, &MarkdownEditor::referenceFileEvent);
        connect(d->m_referenceWatcher, &QFileSystemWatcher::directoryChanged, this, &MarkdownEditor::referenceFileEvent);
        connect(d->m_referenceRefresh, &QTimer::timeout, this, &MarkdownEditor::refreshReferences);
    }
    // The file, and its folder: a program that saves by writing a new file
    // and renaming it over the old one makes inotify drop the file's watch.
    const QString folder = QFileInfo(path).absolutePath();
    if (!d->m_referenceWatcher->directories().contains(folder))
        d->m_referenceWatcher->addPath(folder);
    if (!d->m_referenceWatcher->files().contains(path))
        d->m_referenceWatcher->addPath(path);
    d->m_referenceWatched.insert(path);
}

void MarkdownEditor::referenceFileEvent()
{
    if (!d->m_referencePendingSince.isValid())
        d->m_referencePendingSince.start();
    // A debounce, but a file appended to every 100 ms must still show up.
    if (!d->m_referenceRefresh->isActive() || d->m_referencePendingSince.elapsed() < 1000)
        d->m_referenceRefresh->start();
}

void MarkdownEditor::refreshReferences()
{
    d->m_referencePendingSince.invalidate();
    const QStringList keys = d->m_referenceCache.keys();
    for (const QString &key : keys) {
        if (isRemote(key))
            continue;
        const QFileInfo info(key);
        const auto cached = d->m_referenceCache.constFind(key);
        if (!info.isFile()) {
            // Gone: its read goes too, or a restore with the same mtime and
            // size would look like nothing happened.
            d->m_referenceCache.remove(key);
            d->m_referenceGone.insert(key);
            relayReferenceBlocks(key);
            continue;
        }
        d->m_referenceWatched.remove(key);
        watchReference(key); // put back what a rename-over-save took away
        if (info.lastModified().toMSecsSinceEpoch() != cached->mtime || info.size() != cached->size
            || !cached->error.isEmpty())
            relayReferenceBlocks(key);
    }
    for (auto it = d->m_referenceGone.begin(); it != d->m_referenceGone.end();) {
        if (QFileInfo(*it).isFile()) {
            relayReferenceBlocks(*it);
            it = d->m_referenceGone.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace noteAppC
