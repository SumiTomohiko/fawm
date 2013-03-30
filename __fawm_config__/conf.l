%{
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <fawm/private/__fawm_config__.h>

#include "y.tab.h"

static int
yywrap()
{
    return 1;
}

static char string[256];
%}
%start COMMENT STRING
%%
<INITIAL>"end"      return T_END;
<INITIAL>"exec"     return T_EXEC;
<INITIAL>"exit"     return T_EXIT;
<INITIAL>"menu"     return T_MENU;
<INITIAL>"reload"   return T_RELOAD;
<INITIAL>"\n"       return T_NEWLINE;
<INITIAL>"\""       {
    string[0] = '\0';
    BEGIN STRING;
}
<INITIAL>"#"        {
    BEGIN COMMENT;
}
<INITIAL>[ \t]      ;
<INITIAL>.          {
    abort();
}

<STRING>"\""        {
    size_t size = strlen(string) + 1;
    char* ptr = memory_allocate(size);
    memcpy(ptr, string, size);
    yylval.string = ptr;
    BEGIN INITIAL;
    return T_STRING;
}
<STRING>.           {
    assert(strlen(string) < sizeof(string));
    strcat(string, yytext);
}

<COMMENT>"\n"       {
    BEGIN INITIAL;
}
<COMMENT>.
%%
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
