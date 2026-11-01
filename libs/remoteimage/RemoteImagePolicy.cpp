#include "RemoteImagePolicy.h"

#include <QCoreApplication>

#include <algorithm>

namespace noteAppC::remoteimage {

namespace {

bool isInternalV4(quint32 v4)
{
    // Qt's isSiteLocal/isGlobal know no private IPv4: the ranges are spelled out
    const auto in = [v4](quint32 net, int bits) { return (v4 >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) || in(0x7F000000, 8)
           || in(0xA9FE0000, 16) || in(0xAC100000, 12) || in(0xC0000000, 24) || in(0xC0A80000, 16)
           || in(0xC6120000, 15) || v4 >= 0xE0000000; // multicast, reserved, broadcast
}

} // namespace

bool isInternalAddress(const QHostAddress &address)
{
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

bool isRemoteImageTarget(const QString &target)
{
    return target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
           || target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

QString refusal(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http"))
        return QCoreApplication::translate("RemoteImage", "Not loaded: http is refused, only https.");
    if (scheme != QLatin1String("https"))
        return QCoreApplication::translate("RemoteImage", "Not loaded: not a web address.");
    if (!url.userInfo().isEmpty())
        return QCoreApplication::translate("RemoteImage", "Not loaded: the address has a user name or password.");
    QString host = url.host().toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return QCoreApplication::translate("RemoteImage", "Not loaded: the address has no host.");
    const QString local = QCoreApplication::translate("RemoteImage", "Not loaded: %1 is a local address.").arg(host);
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")))
        return local;
    const QHostAddress literal(host); // an IPv6 literal comes without its brackets from QUrl::host
    if (!literal.isNull() && isInternalAddress(literal))
        return local;
    return {};
}

QString originOf(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return {};
    QString origin = url.scheme().toLower() + QStringLiteral("://") + (host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host);
    const int port = url.port(-1);
    if (port > 0 && !(port == 443 && url.scheme().toLower() == QLatin1String("https")))
        origin += QLatin1Char(':') + QString::number(port);
    return origin;
}

} // namespace noteAppC::remoteimage
