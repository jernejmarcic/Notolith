// Makes one call that tests/nettrap.c must stop, or the AF_UNIX one it must let through (the argument names it).
// Run under the trap: the first four exit 86, "unix" exits 0.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "inet")) {
        socket(AF_INET, SOCK_STREAM, 0);
    } else if (!strcmp(what, "connect")) {
        // A raw AF_INET connect that skipped socket(): the address family alone is enough.
        struct sockaddr_in address = {.sin_family = AF_INET};
        connect(-1, (struct sockaddr *)&address, sizeof address);
    } else if (!strcmp(what, "resolve")) {
        struct addrinfo *result;
        getaddrinfo("localhost", NULL, NULL, &result);
    } else if (!strcmp(what, "dlopen")) {
        dlopen("libcurl.so.4", RTLD_LAZY);
    } else if (!strcmp(what, "unix")) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = "/nonexistent/nettrap"};
        // Refused for lack of a listener, not by the trap.
        int status = connect(fd, (struct sockaddr *)&address, sizeof address);
        return fd >= 0 && status == -1 ? 0 : 2;
    }
    return 0; // reached only when the trap let the call through
}
