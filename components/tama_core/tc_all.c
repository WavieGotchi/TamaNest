/* One translation unit for the whole core, so the compiler can inline. */
#include "tc_mem.c"
#include "tc_periph.c"
#include "tc_lcd.c"
#include "tc_core.c"
#include "tc_fast.c"
#include "tc_hle.c"
#include "tc_turbo.c"
#ifdef TC_JIT
#include "tc_jit.c"
#endif
