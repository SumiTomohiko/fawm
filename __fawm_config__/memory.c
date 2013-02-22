#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_SIZE  8192

struct Arena {
    struct Arena* next;
    size_t used_size;
    void* ptr;
};

typedef struct Arena Arena;

struct Storage {
    struct Arena* arena;
};

typedef struct Storage Storage;

static Storage storage;

static Arena*
allocate_arena()
{
    Arena* arena = (Arena*)malloc(sizeof(Arena));
    assert(arena != NULL);
    void* ptr = malloc(ARENA_SIZE);
    assert(ptr != NULL);
    memset(ptr, 0xab, ARENA_SIZE);
    arena->next = NULL;
    arena->used_size = 0;
    arena->ptr = ptr;
    return arena;
}

void
memory_initialize()
{
    storage.arena = allocate_arena();
}

void
memory_dispose()
{
    Arena* arena = storage.arena;
    while (arena != NULL) {
        Arena* next = arena->next;
        free(arena->ptr);
        free(arena);
        arena = next;
    }
}

void*
memory_allocate(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    size_t aligned_size = ((size - 1) / sizeof(size_t) + 1) * sizeof(size_t);
    size_t used_size = storage.arena->used_size;
    size_t unused_size = ARENA_SIZE - used_size;
    if (unused_size < aligned_size) {
        Arena* arena = allocate_arena();
        arena->next = storage.arena;
        storage.arena = arena;
        return memory_allocate(size);
    }
    void* ptr = (void*)((uintptr_t)storage.arena->ptr + used_size);
    storage.arena->used_size += aligned_size;
    return ptr;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
