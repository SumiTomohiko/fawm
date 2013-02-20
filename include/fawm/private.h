#if !defined(FAWM_PRIVATE_H_INCLUDED)
#define FAWM_PRIVATE_H_INCLUDED

typedef int pos_t;

struct Item {
    enum {
        TYPE_EXEC,
        TYPE_EXIT
    } type;
    pos_t caption;
    pos_t command;
};

typedef struct Item Item;

#define X_OF_ITEM(menu, item, x)    ((uintptr_t)(menu) + (item)->x)
#define CAPTION_OF_ITEM(menu, item) (char*)X_OF_ITEM((menu), (item), caption)
#define COMMAND_OF_ITEM(menu, item) (char*)X_OF_ITEM((menu), (item), command)

struct Menu {
    pos_t items;
    int items_num;
};

typedef struct Menu Menu;

#define ITEMS_OF_MENU(menu) (Item*)((uintptr_t)(menu) + (menu)->items)

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
