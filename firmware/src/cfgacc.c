/* cfgacc.c — the settings record and its accessors, in common code.
 *
 * config.c (loading and saving) is in a code bank, but cfg_get() is called
 * from everywhere, often, and from the deepest paths: here it is an ordinary
 * call from any bank, not a banked one through the trampoline. */
#include "board.h"
#include "config.h"

__xdata struct cfg cfg_live;
__xdata const struct cfg *cfg_get(void) { return &cfg_live; }
__xdata struct cfg *cfg_edit(void) { return &cfg_live; }
