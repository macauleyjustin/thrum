#include "thrum_rt.h"

TVal thrum_cap_gui_create(TRuntime* rt, int line, int col) {
    thrum_error(rt, line, col, "gui capability is not linked in this build (build with Native Cocoa GUI backend)", NULL);
}
