#if !defined(FAWM_PRIVATE___FAWM_CONFIG___H)
#define FAWM_PRIVATE___FAWM_CONFIG___H

#include <sys/types.h>

#include <fawm/private.h>

struct MenuItemList {
    struct MenuItemList* next;
    struct MenuItem* item;
};

typedef struct MenuItemList MenuItemList;

void* memory_allocate(size_t);
void memory_dispose();
void memory_initialize();
void parser_initialize(Config*);

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
