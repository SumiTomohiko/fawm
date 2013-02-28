%{
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include <fawm/private/__fawm_config__.h>

static Config* parser_config;

void
parser_initialize(Config* config)
{
    parser_config = config;
}

/**
 * If the declaration of yylex() does not exit, clang warns "implicait
 * declaration". yacc does not have any options to declare it. I dislike to add
 * this manually.
 */
extern int yylex();

static int
yyerror(const char* msg)
{
    fprintf(stderr, "parser error: %s\n", msg);
    return 0;   /* No one use this value? */
}

static MenuItemList*
find_last_menu_item(MenuItemList* list)
{
    assert(list != NULL);
    MenuItemList* l;
    for (l = list; l->next != NULL; l = l->next) {
    }
    return l;
}

static MenuItemList*
allocate_menu_item_list(MenuItem* item)
{
    MenuItemList* list = (MenuItemList*)memory_allocate(sizeof(MenuItemList));
    list->next = NULL;
    list->item = item;
    return list;
}

static MenuItemList*
add_menu_item(MenuItemList* list, MenuItem* item)
{
    assert(list != NULL);
    find_last_menu_item(list)->next = allocate_menu_item_list(item);
    return list;
}

static MenuItem*
allocate_menu_item(MenuItemType type)
{
    MenuItem* item = memory_allocate(sizeof(MenuItem));
    item->type = type;
    return item;
}

static int
count_menu_items(MenuItemList* list)
{
    int n = 0;
    MenuItemList* l;
    for (l = list; l != NULL; l = l->next) {
        n++;
    }
    return n;
}

static Menu*
allocate_menu(MenuItemList* list)
{
    int items_num = count_menu_items(list);

    MenuItem* items = (MenuItem*)memory_allocate(sizeof(MenuItem) * items_num);
    int i;
    MenuItemList* l;
    for (i = 0, l = list; i < items_num; i++, l = l->next) {
        memcpy(&items[i], l->item, sizeof(MenuItem));
    }

    Menu* menu = (Menu*)memory_allocate(sizeof(Menu));
    menu->items.ptr = items;
    menu->items_num = items_num;

    return menu;
}
%}
%union {
    Menu* menu;
    MenuItem* menu_item;
    MenuItemList* menu_items;
    char* string;
}
%token T_END T_EXEC T_EXIT T_MENU T_NEWLINE
%type<menu> menu
%type<menu_item> menu_item
%type<menu_items> menu_items
%token<string> T_STRING
%%
config  : config T_NEWLINE section
        | section
        ;
section : menu {
            parser_config->menu.ptr = $1;
        }
        | /* empty */
        ;
menu    : T_MENU T_NEWLINE menu_items T_NEWLINE T_END {
            $$ = allocate_menu($3);
        }
        ;
menu_items
        : menu_items T_NEWLINE menu_item {
            bool pred = $1 != NULL;
            $$ = pred ? add_menu_item($1, $3) : allocate_menu_item_list($3);
        }
        | menu_item {
            $$ = allocate_menu_item_list($1);
        }
        ;
menu_item
        : T_EXEC T_STRING T_STRING {
            $$ = allocate_menu_item(MENU_ITEM_TYPE_EXEC);
            $$->u.exec.caption.ptr = $2;
            $$->u.exec.command.ptr = $3;
        }
        | T_EXIT {
            $$ = allocate_menu_item(MENU_ITEM_TYPE_EXIT);
        }
        | /* empty */ {
            $$ = NULL;
        }
        ;
%%
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
