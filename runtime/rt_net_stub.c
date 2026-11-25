#include "thrum_rt.h"

TVal thrum_cap_net_create(TRuntime* rt, int line, int col) {
    thrum_error(rt, line, col, "net capability is not linked in this build", NULL);
}
