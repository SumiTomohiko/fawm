%{
#include "y.tab.h"
%}
%%
"end"   return T_END;
"exec"  return T_EXEC;
"exit"  return T_EXIT;
"menu"  return T_MENU;
"\n"    return T_NEWLINE;
\".*\"  return T_STRING;
%%
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
