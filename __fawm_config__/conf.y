%{
%}
%token T_END T_EXEC T_EXIT T_MENU T_NEWLINE T_STRING
%%
configure   : menu
            ;
menu    : T_MENU T_NEWLINE entries T_NEWLINE T_END
        ;
entries : entries T_NEWLINE entry
        | entry
        ;
entry   : T_EXEC T_STRING T_STRING
        | T_EXIT
        | /* empty */
        ;
%%
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
