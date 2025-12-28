
#include "menu.h"

void menu_init(menu_t *menu)
{
    menu->current = 0;
    if (menu->items[0].activate) {
        menu->items[0].activate(menu->items[0].ctx);
    }
}

void menu_next(menu_t *menu)
{
    const menu_item_t *old = &menu->items[menu->current];

    if (old->deactivate) {
        old->deactivate(old->ctx);
    }

    menu->current = (menu->current + 1) % menu->item_count;

    const menu_item_t *now = &menu->items[menu->current];
    if (now->activate) {
        now->activate(now->ctx);
    }
}

void menu_draw(menu_t *menu)
{
    const menu_item_t *item = &menu->items[menu->current];
    item->draw(item->ctx);
}

bool menu_handle_event(menu_t *menu, movement_event_t event)
{
    const menu_item_t *item = &menu->items[menu->current];

    if (item->handle_event) {
        if (item->handle_event(event, item->ctx)) {
            return true; /* event consumed */
        }
    }

    /* default menu behavior */
    if (event.event_type== EVENT_ALARM_BUTTON_UP) {
        menu_next(menu);
        return true;
    }

    return false;
}
