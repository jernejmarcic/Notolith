// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jernej Marčič
#pragma once

#include <QMetaObject>

#include <mutex>
#include <utility>

namespace noteAppC {

/// How a worker thread posts a result back to an object that may be
/// destroyed while the worker runs. Qt drops a queued call whose receiver is
/// already gone, but the call to invokeMethod itself reads the receiver, and
/// racing a delete with it is a use-after-free. A check-then-post is still
/// that race; so posting holds a lock, and the receiver's destructor takes
/// the same lock to call end(): a post has either finished before the
/// destructor goes on, or it never starts.
///
/// Held by shared_ptr, so it outlives the receiver for as long as a worker
/// still has it.
class Lifeline {
public:
    /// Queues `call` on `receiver`'s thread; false once end() has run.
    template <typename Receiver, typename Call>
    bool post(Receiver *receiver, Call &&call)
    {
        const std::lock_guard lock(m_mutex);
        if (!m_alive)
            return false;
        QMetaObject::invokeMethod(receiver, std::forward<Call>(call), Qt::QueuedConnection);
        return true;
    }

    /// First thing in the receiver's destructor.
    void end()
    {
        const std::lock_guard lock(m_mutex);
        m_alive = false;
    }

private:
    std::mutex m_mutex;
    bool m_alive = true;
};

} // namespace noteAppC
