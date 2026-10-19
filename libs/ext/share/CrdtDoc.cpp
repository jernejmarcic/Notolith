#include "CrdtDoc.h"

namespace noteAppC {

std::unique_ptr<CrdtDoc> makeYrsDoc(quint64 clientID); // CrdtDocYrs.cpp

bool CrdtDoc::available(Backend backend)
{
    return backend == Backend::Yrs; // this library only exists with yrs (CMakeLists.txt)
}

std::unique_ptr<CrdtDoc> CrdtDoc::create(Backend backend, quint64 clientID)
{
    return backend == Backend::Yrs ? makeYrsDoc(clientID) : nullptr;
}

} // namespace noteAppC
