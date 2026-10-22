#pragma once

#include <QtPlugin>

namespace noteAppC {

struct ModuleHost;

/// What a loadable module implements (spec section 4). One method: the
/// module registers its views, commands and settings rows with the host.
/// Called on the UI thread, once, on first use.
class NotolithModule {
public:
    virtual ~NotolithModule() = default;
    virtual void attach(ModuleHost &host) = 0;
};

} // namespace noteAppC

#define NotolithModule_iid "io.github.jernejmarcic.Notolith.Module/1"
Q_DECLARE_INTERFACE(noteAppC::NotolithModule, NotolithModule_iid)
