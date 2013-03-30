#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <fawm/private.h>
#include <fawm/private/__fawm_config__.h>

#include "y.tab.h"

static void
parse_config(Config* config, FILE* fpin)
{
    parser_initialize(config);
    extern FILE* yyin;
    yyin = fpin;
    extern int yyparse();
    yyparse();
}

static size_t
align(size_t size)
{
    return ((size - 1) / sizeof(size_t) + 1) * sizeof(size_t);
}

static size_t
compute_size_of_menu_item(MenuItem* item)
{
    size_t size = sizeof(MenuItem);

    MenuItemType type = item->type;
    if ((type == MENU_ITEM_TYPE_EXIT) || (type == MENU_ITEM_TYPE_RELOAD)) {
        return size;
    }
    assert(type == MENU_ITEM_TYPE_EXEC);

    size += align(strlen(item->u.exec.caption.ptr) + 1);
    size += align(strlen(item->u.exec.command.ptr) + 1);
    return size;
}

static size_t
compute_size_of_menu(Menu* menu)
{
    size_t size = sizeof(Menu);
    int i;
    for (i = 0; i < menu->items_num; i++) {
        size += compute_size_of_menu_item(&menu->items.ptr[i]);
    }
    return size;
}

static size_t
compute_size_of_config(Config* config)
{
    Menu* menu = config->menu.ptr;
    return sizeof(Config) + (menu != NULL ? compute_size_of_menu(menu) : 0);
}

static void
serialize_menu_item(MenuItem* new_item, MenuItem* item, uintptr_t base, uintptr_t* pos)
{
    const char* caption;
    const char* command;
    size_t len;
    switch (item->type) {
    case MENU_ITEM_TYPE_EXEC:
        caption = item->u.exec.caption.ptr;
        len = strlen(caption) + 1;
        memcpy((void*)*pos, caption, len);
        new_item->u.exec.caption.offset = *pos - base;
        *pos += align(len);

        command = item->u.exec.command.ptr;
        len = strlen(command) + 1;
        memcpy((void*)*pos, command, len);
        new_item->u.exec.command.offset = *pos - base;
        *pos += align(len);
        break;
    case MENU_ITEM_TYPE_EXIT:
    case MENU_ITEM_TYPE_RELOAD:
        break;
    default:
        assert(false);
    }
}

static void
serialize_config(Config* config, uintptr_t base)
{
    uintptr_t pos = base;

    Config* new_config = (Config*)pos;
    size_t size = sizeof(*config);
    memcpy(new_config, config, size);
    pos += size;

    Menu* new_menu = (Menu*)pos;
    Menu* menu = config->menu.ptr;
    size = sizeof(*menu);
    memcpy(new_menu, menu, size);
    new_config->menu.offset = pos - base;
    pos += size;

    MenuItem* new_items = (MenuItem*)pos;
    MenuItem* items = menu->items.ptr;
    size = sizeof(menu->items.ptr[0]) * menu->items_num;
    memcpy(new_items, items, size);
    new_menu->items.offset = pos - base;
    pos += size;

    int i;
    for (i = 0; i < menu->items_num; i++) {
        serialize_menu_item(&new_items[i], &items[i], base, &pos);
    }
}

static int
write_file(void* dest, size_t size, FILE* fpout)
{
    size_t nbytes = 0;
    while (!ferror(fpout) && (nbytes < size)) {
        void* ptr = (void*)((uintptr_t)dest + nbytes);
        nbytes += fwrite(ptr, size - nbytes, 1, fpout);
    }
    return nbytes == size ? 0 : -1;
}

static int
output_config(Config* config, FILE* fpout)
{
    size_t size = compute_size_of_config(config);
    uintptr_t base = (uintptr_t)alloca(size);
    serialize_config(config, base);

    write_file(&size, sizeof(size), fpout);
    write_file((void*)base, size, fpout);

    return 0;
}

static void
print_error(const char* fmt, ...)
{
    size_t size = strlen(fmt) + 2;
    char* s = (char*)alloca(size);
    snprintf(s, size, "%s\n", fmt);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, s, ap);
    va_end(ap);
}

static int
input_config(Config* config, const char* path)
{
    FILE* fpin = fopen(path, "r");
    if (fpin == NULL) {
        const char* fmt = "Cannot open configuration file: %s: %s";
        print_error(fmt, strerror(errno), path);
        return 1;
    }

    parse_config(config, fpin);

    return fclose(fpin);
}

int
main(int argc, const char* argv[])
{
    if (argc != 2) {
        print_error("Usage: %s <config_file>", argv[0]);
        return 1;
    }
    memory_initialize();

    Config config;
    bzero(&config, sizeof(config));
    if (input_config(&config, argv[1]) != 0) {
        return 1;
    }
    if (output_config(&config, stdout) != 0) {
        return 1;
    }

    return 0;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
