// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej
// libzmq on first use. The notebook is the only caller of libzmq, and libzmq
// drags in gnutls, p11-kit, nettle and more: 12 libraries the loader would map
// and relocate before main() for a feature most starts never touch (47 M
// instructions, 4.6 ms). These forwarders are linked ahead of libzmq with
// --as-needed (root CMakeLists.txt), which then leaves libzmq out of the
// executable's NEEDED list; the first notebook call loads it.
//
// A new libzmq function the notebook starts using needs a line here, or the
// link fails with an undefined reference (--as-needed has dropped libzmq).
#include <dlfcn.h>

#include <cerrno>

#include <zmq.h>

namespace {

void *zmqSymbol(const char *name)
{
    static void *handle = dlopen("libzmq.so.5", RTLD_NOW | RTLD_GLOBAL);
    return handle ? dlsym(handle, name) : nullptr;
}

// One forwarder: resolves the real function once, and when libzmq cannot be
// loaded fails the way zmq does (-1 / NULL with errno set).
#define ZMQ_FORWARD(ret, fail, name, params, args)                                  \
    ret name params                                                                 \
    {                                                                               \
        static auto real = reinterpret_cast<ret (*) params>(zmqSymbol(#name));      \
        if (!real) {                                                                \
            errno = ENOSYS;                                                         \
            return fail;                                                            \
        }                                                                           \
        return real args;                                                           \
    }

} // namespace

extern "C" {
ZMQ_FORWARD(int, -1, zmq_close, (void *s), (s))
ZMQ_FORWARD(int, -1, zmq_connect, (void *s, const char *addr), (s, addr))
ZMQ_FORWARD(int, -1, zmq_ctx_destroy, (void *c), (c))
ZMQ_FORWARD(void *, nullptr, zmq_ctx_new, (void), ())
ZMQ_FORWARD(int, ENOSYS, zmq_errno, (void), ())
ZMQ_FORWARD(int, -1, zmq_getsockopt, (void *s, int o, void *v, size_t *l), (s, o, v, l))
ZMQ_FORWARD(int, -1, zmq_msg_close, (zmq_msg_t *m), (m))
ZMQ_FORWARD(void *, nullptr, zmq_msg_data, (zmq_msg_t *m), (m))
ZMQ_FORWARD(int, -1, zmq_msg_init, (zmq_msg_t *m), (m))
ZMQ_FORWARD(int, -1, zmq_msg_recv, (zmq_msg_t *m, void *s, int f), (m, s, f))
ZMQ_FORWARD(size_t, 0, zmq_msg_size, (const zmq_msg_t *m), (m))
ZMQ_FORWARD(int, -1, zmq_poll, (zmq_pollitem_t *i, int n, long t), (i, n, t))
ZMQ_FORWARD(int, -1, zmq_recv, (void *s, void *b, size_t l, int f), (s, b, l, f))
ZMQ_FORWARD(int, -1, zmq_send, (void *s, const void *b, size_t l, int f), (s, b, l, f))
ZMQ_FORWARD(int, -1, zmq_setsockopt, (void *s, int o, const void *v, size_t l), (s, o, v, l))
ZMQ_FORWARD(void *, nullptr, zmq_socket, (void *c, int t), (c, t))
ZMQ_FORWARD(int, -1, zmq_socket_monitor, (void *s, const char *a, int e), (s, a, e))
}
