// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrdtDoc.h"

namespace noteAppC {

std::unique_ptr<CrdtDoc> makeNativeDoc(quint64 clientID); // CrdtDocNative.cpp
#ifdef NOTEAPPC_HAVE_YRS
std::unique_ptr<CrdtDoc> makeYrsDoc(quint64 clientID); // CrdtDocYrs.cpp
#endif

bool CrdtDoc::available(Backend backend)
{
    if (backend == Backend::Native)
        return true;
#ifdef NOTEAPPC_HAVE_YRS
    return true;
#else
    return false; // Yrs needs cargo (libs/ext/share/CMakeLists.txt)
#endif
}

std::unique_ptr<CrdtDoc> CrdtDoc::create(Backend backend, quint64 clientID)
{
    if (backend == Backend::Native)
        return makeNativeDoc(clientID);
#ifdef NOTEAPPC_HAVE_YRS
    return makeYrsDoc(clientID);
#else
    return nullptr;
#endif
}

} // namespace noteAppC
