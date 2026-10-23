#pragma once

#include <QtPlugin>

namespace noteAppC {

struct ModuleHost;

/// What a loadable module implements (spec section 4). One method: the
/// module registers its views, commands and settings rows with the host.
/// Called on the UI thread, once per registry, on first use. A process can hold
/// several windows, each with its own registry and ModuleHost, but the library is
/// mapped once and QPluginLoader hands every registry the same object: a real
/// module keeps no state of its own, or keeps it per host (in the lambdas and
/// views it registers), because a second window's attach() runs on the object
/// the first window's attach() ran on.
class NotolithModule {
public:
    virtual ~NotolithModule() = default;
    virtual void attach(ModuleHost &host) = 0;
};

} // namespace noteAppC

#define NotolithModule_iid "io.github.jernejmarcic.Notolith.Module/1"
Q_DECLARE_INTERFACE(noteAppC::NotolithModule, NotolithModule_iid)
