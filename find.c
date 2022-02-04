#include <yed/plugin.h>

static yed_plugin *Self;

int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();
    Self = self;

    return 0;
}
