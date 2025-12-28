
#include <stdbool.h>
#include "movement.h"

typedef struct menu_item {
    void (*activate)(void *ctx);
    void (*deactivate)(void *ctx);
    void (*draw)(void *ctx);
    bool (*handle_event)(movement_event_t event, void *ctx); /* optional */

    void *ctx;
} menu_item_t;


typedef struct menu {
    const menu_item_t *items;
    uint8_t item_count;
    uint8_t current;
} menu_t;


void menu_init(menu_t *menu);

void menu_next(menu_t *menu);

void menu_draw(menu_t *menu);

bool menu_handle_event(menu_t *menu, movement_event_t event);