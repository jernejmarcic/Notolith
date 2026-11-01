#pragma once

// What a note may make the machine ask for. No DNS happens here: the fetcher
// resolves a name and checks every address it gets with isInternalAddress.

#include <QHostAddress>
#include <QString>
#include <QUrl>

namespace noteAppC::remoteimage {

/// An address that is not the internet: this machine, its network, link-local (cloud metadata is
/// 169.254.169.254), private, unique-local, shared (CGNAT), multicast, unspecified. Also IPv4 carried inside
/// IPv6 (mapped, compatible, NAT64, 6to4). ReferenceBlock.cpp's check, moved here to be one.
bool isInternalAddress(const QHostAddress &address);

/// True for text that starts with http:// or https://. What decides that an image is a remote one.
bool isRemoteImageTarget(const QString &target);

/// Why `url` may not be fetched, or an empty string when it may: https only, no user:pass, a host,
/// not `localhost` or `*.localhost`, and not an IP literal in an internal range.
QString refusal(const QUrl &url);

/// `scheme://host[:port]`, lower case, the unit "Always load from" remembers. Empty for a URL with no host.
QString originOf(const QUrl &url);

} // namespace noteAppC::remoteimage
