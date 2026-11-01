// An LD_PRELOAD library for the tests of the default build (tests/CMakeLists.txt): the process exits with status
// 86, with a line and a backtrace on stderr, the moment it reaches for the network. The default build has no
// networking (docs/OFFLINE.md), and the link-time checks cannot see a socket made by code that is loaded later or
// a library opened with dlopen. Unix sockets pass: the single-instance socket and the notebook's ipc:// endpoints
// are the app's own.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

// The trap is for the process under test, not for what it starts: a Jupyter kernel, pandoc or bubblewrap are other
// programs (a kernel binds its own loopback ports), and the app's own sockets are the ones that matter.
__attribute__((constructor)) static void scrubPreload(void)
{
    unsetenv("LD_PRELOAD");
}

static void trap(const char *what, const char *detail)
{
    fprintf(stderr, "nettrap: %s %s\n", what, detail ? detail : "");
    void *frames[24];
    backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
    _exit(86);
}

static int inetFamily(int family)
{
    return family == AF_INET || family == AF_INET6;
}

int socket(int domain, int type, int protocol)
{
    static int (*real)(int, int, int);
    if (inetFamily(domain))
        trap("socket(AF_INET or AF_INET6)", NULL);
    if (!real)
        real = (int (*)(int, int, int))dlsym(RTLD_NEXT, "socket");
    return real(domain, type, protocol);
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    static int (*real)(int, const struct sockaddr *, socklen_t);
    if (address && address->sa_family != AF_UNIX && address->sa_family != AF_UNSPEC)
        trap("connect() to a socket that is not AF_UNIX", NULL);
    if (!real)
        real = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    return real(fd, address, length);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
    (void)service;
    (void)hints;
    (void)result;
    trap("getaddrinfo", node);
    return EAI_FAIL;
}

// libzmq asks for the peer of an ipc:// connection too, which is a path and needs no lookup: only an inet address traps.
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t hostLength, char *service,
                socklen_t serviceLength, int flags)
{
    static int (*real)(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
    if (address && inetFamily(address->sa_family))
        trap("getnameinfo", NULL);
    if (!real)
        real = (int (*)(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int))dlsym(
            RTLD_NEXT, "getnameinfo");
    return real(address, length, host, hostLength, service, serviceLength, flags);
}

struct hostent *gethostbyname(const char *name)
{
    trap("gethostbyname", name);
    return NULL;
}

struct hostent *gethostbyname2(const char *name, int family)
{
    (void)family;
    trap("gethostbyname2", name);
    return NULL;
}

int gethostbyname_r(const char *name, struct hostent *result, char *buffer, size_t bufferLength,
                    struct hostent **out, int *error)
{
    (void)result;
    (void)buffer;
    (void)bufferLength;
    (void)out;
    (void)error;
    trap("gethostbyname_r", name);
    return -1;
}

// What a network client would bring: a library of the network or crypto stack, or a Qt TLS backend.
void *dlopen(const char *file, int flags)
{
    static void *(*real)(const char *, int);
    static const char *const networkLibraries[] = {"libcurl", "libssl", "libgnutls", "libdatachannel", "libjuice",
                                                   "libusrsctp", "libavahi", "libqopensslbackend", "libqschannelbackend"};
    if (file) {
        for (unsigned i = 0; i < sizeof networkLibraries / sizeof *networkLibraries; ++i) {
            if (strstr(file, networkLibraries[i]))
                trap("dlopen", file);
        }
    }
    if (!real)
        real = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    return real(file, flags);
}
