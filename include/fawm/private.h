#if !defined(FAWM_PRIVATE_H_INCLUDED)
#define FAWM_PRIVATE_H_INCLUDED

#include <stddef.h>

enum MenuItemType {
    MENU_ITEM_TYPE_EXEC,
    MENU_ITEM_TYPE_EXIT
};

typedef enum MenuItemType MenuItemType;

#define DEFINE_MEMBER(type, name) \
    union { \
        ptrdiff_t offset; \
        type* ptr; \
    } name;

struct MenuItem {
    enum MenuItemType type;
    union {
        struct {
            DEFINE_MEMBER(char, caption);
            DEFINE_MEMBER(char, command);
        } exec;
    } u;
};

typedef struct MenuItem MenuItem;

struct Menu {
    DEFINE_MEMBER(MenuItem, items);
    int items_num;
};

typedef struct Menu Menu;

struct Config {
    DEFINE_MEMBER(Menu, menu);
};

typedef struct Config Config;

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
