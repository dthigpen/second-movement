/*
 * MIT License
 *
 * Copyright (c) 2022 David Thigpen (https://github.com/dthigpen)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include "dungeon_face.h"
#include "watch.h"
#include <time.h>

#define ROOM_BAG_SIZE 10
#define LOOT_BAG_SIZE 10

// --- High-level screens / modes ---
typedef enum {
DNGN_SCREEN_NONE = 0,
DNGN_SCREEN_TITLE,
DNGN_SCREEN_ENCOUNTER,
DNGN_SCREEN_ENCOUNTER_MENU,
DNGN_SCREEN_RUN_AWAY, // short state to start hit or miss anim
DNGN_SCREEN_DESCEND, // short state to start descend anim
DNGN_SCREEN_LOOT,
DNGN_SCREEN_EMPTY_ROOM,
DNGN_SCREEN_GAME_OVER,
DNGN_SCREEN_COUNT
} dngn_screen_t;

// --- Encounter types ---
typedef enum {
DNGN_ROOM_EMPTY = 0,
DNGN_ROOM_ENEMY,
DNGN_ROOM_LOOT
} dngn_room_type_t;

// --- Enemy types ---
typedef enum {
DNGN_ENEMY_TYPE_BRUTE = 0,
DNGN_ENEMY_TYPE_STRIKER,
DNGN_ENEMY_TYPE_SWING,
DNGN_ENEMY_TYPE_COUNT,
} dngn_enemy_type_t;


// --- Player actions during encounters ---
typedef enum {
DNGN_ACTION_FIGHT = 0,
DNGN_ACTION_RUN,
DNGN_ACTION_HEAL,
DNGN_ACTION_EQUIP_SHEILD,
DNGN_ACTION_COUNT
} dngn_action_t;


// --- Items (very small set) ---
typedef enum {
DNGN_ITEM_NONE = 0,
DNGN_ITEM_WEAPON,
DNGN_ITEM_POTION,
DNGN_ITEM_SHIELD,
DNGN_ITEM_GOLD,
DNGN_ITEM_HP_UP,
DNGN_ITEM_MAX_HP_UP
} dngn_item_type_t;


// --- Game elements ---

typedef struct {
    int8_t hp;
    uint8_t damage;
    bool is_boss;
    uint8_t gold;
} dngn_enemy_t;

typedef struct {
    int8_t hp;
    int8_t max_hp;
    uint8_t damage;
    uint8_t gold;
    uint8_t potions;
    uint8_t shields;
    bool shield_equipped;
} dngn_player_t;

typedef struct {
    dngn_item_type_t type;
    int8_t value;   // +hp, +damage, etc.
} dngn_item_t;


// --- Screen function table ---
typedef struct {
void (*transition)(movement_event_t event, void *context);
void (*display)(movement_event_t event, void *context);
} dngn_screen_def_t;

// outcome data, used to animate an interaction that just occurred
typedef struct {
    union {
        struct {
            bool hit;
            bool died;
        } encounter;
    };
} dngn_outcome_event_t;


typedef struct {
    uint8_t current_tick;
    uint8_t ticks_per_frame;
    uint8_t total_frames;
    bool active;
    bool loop;
} dngn_animation_t;
// --- Main persistent game state ---
typedef struct {
// meta
dngn_screen_t last_screen;
dngn_screen_t screen;
dngn_screen_t screen_after_anim;
dngn_screen_def_t screens[DNGN_SCREEN_COUNT];

// whether a run is currently in progress
bool active;

// dungeon progression
uint8_t floor;

// Room related
dngn_room_type_t room_bag[ROOM_BAG_SIZE];
int room_bag_count;
dngn_room_type_t current_room;

// Loot related
dngn_item_type_t loot_bag[LOOT_BAG_SIZE];
int loot_bag_count;


// player stats
// uint8_t score;
dngn_player_t player;

// enemy stats (only valid during encounter)
dngn_enemy_t enemy;


// UI state
dngn_action_t selected_action;
dngn_item_t found_item;


// current animation
// animation_state_t animation;

bool screen_changed;

dngn_outcome_event_t outcome;

dngn_animation_t anim;
} dngn_state_t;


// For dungeon generation:
typedef struct {
    uint8_t weight;
    uint8_t value;
} weighted_choice_t;

const int DNGN_MAX_POTIONS = 6;
const int DNGN_MAX_SHIELDS = 2;
const int DNGN_MAX_HP_UP_AMOUNT = 2;

static uint8_t get_anim_frame(dngn_animation_t *anim) {
    return anim->ticks_per_frame == 0 ? 0 : anim->current_tick / anim->ticks_per_frame;
}

static void start_anim(dngn_animation_t *anim, uint8_t ticks_per_frame, uint8_t total_frames, bool loop) {
    anim->current_tick = 0;
    anim->active = true;
    anim->ticks_per_frame = ticks_per_frame;
    anim->total_frames = total_frames;
    anim->loop = loop;
}

static void stop_anim(dngn_animation_t *anim) {
    anim->current_tick = 0;
    anim->active = false;
    anim->ticks_per_frame = 0;
    anim->total_frames = 0;
    anim->loop = false;
}

static void tick_anim(dngn_animation_t *anim) {
    anim->current_tick++;
    if(anim->current_tick >= anim->ticks_per_frame * anim->total_frames) {
        if(anim->loop) {
            start_anim(anim, anim->ticks_per_frame, anim->total_frames, anim->loop);
        } else {
            stop_anim(anim);
        }
    }

}


static bool default_button_handler(movement_event_t event) {
    // Returns true if handled by the default handler, otherwise false
    // indicating it should be handled by the caller
    switch(event.event_type) {
        case EVENT_MODE_BUTTON_UP:
        case EVENT_LIGHT_BUTTON_DOWN:
        case EVENT_LIGHT_BUTTON_UP:
        case EVENT_MODE_LONG_PRESS:
            // printf("Default handler called for event_type=%d\n", event.event_type);
            movement_default_loop_handler(event);
            return true;
    }
    return false;
}

static bool skipped_anim(movement_event_t event, dngn_state_t *state) {
    if(event.event_type == EVENT_ALARM_BUTTON_UP) {
        stop_anim(&state->anim);
        return true;
    }
    return false;
}

// Descend animation

static void draw_descend(uint8_t frame_index, void* context) {
    
    dngn_state_t *state = (dngn_state_t *) context;
    char buf[3]; // 2 chars + \0

    switch(frame_index) {
        case 0:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "Flr", "FL");
            snprintf(buf, sizeof buf, "%2d", (int)state->floor + 1);
            watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
            watch_set_pixel(3, 16);
            break;
        case 1:
            watch_set_pixel(2, 16);
            break;
        case 2:
            watch_set_pixel(2, 15);
            break;
        case 3:
            watch_set_pixel(1, 14);
            break;
    }
}

static void draw_encounter(uint8_t frame_index, void* context) {
    
    dngn_state_t *state = (dngn_state_t *) context;
    char buf[5]; // 4 chars + \0
    switch(frame_index) {
        case 0:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "ENEMY", "EN");
            snprintf(buf, sizeof buf, "%4d", (int)state->enemy.hp);
            watch_display_text(WATCH_POSITION_BOTTOM, buf);
            watch_display_text(WATCH_POSITION_SECONDS, "HP");
            break;
        case 1:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "PLyr", "PL");
            snprintf(buf, sizeof buf, "%4d", (int)state->player.hp);
            watch_display_text(WATCH_POSITION_BOTTOM, buf);
            watch_display_text(WATCH_POSITION_SECONDS, "HP");
            break;
    }
}

static void draw_run_away_anim(uint8_t frame_index, void *context) {
    dngn_state_t *state = (dngn_state_t*) context;
    const char* text = "HiT";
    watch_clear_display();
    switch(frame_index) {
        case 0:
        case 2:
        case 3: // draw last frame too so it doesn't end on blank
            watch_display_text(WATCH_POSITION_BOTTOM, text);
            break;
    }
}

// ---------- forward declarations ----------

static void title_transition(movement_event_t event, void *context);
static void title_display(movement_event_t event, void *context);

static void descend_transition(movement_event_t event, void *context);
static void descend_display(movement_event_t event, void *context);

static void run_away_transition(movement_event_t event, void *context);
static void run_away_display(movement_event_t event, void *context);

static void encounter_transition(movement_event_t event, void *context);
static void encounter_display(movement_event_t event, void *context);

static void encounter_menu_transition(movement_event_t event, void *context);
static void encounter_menu_display(movement_event_t event, void *context);

static void loot_transition(movement_event_t event, void *context);
static void loot_display(movement_event_t event, void *context);

static void empty_room_transition(movement_event_t event, void *context);
static void empty_room_display(movement_event_t event, void *context);

static void game_over_transition(movement_event_t event, void *context);
static void game_over_display(movement_event_t event, void *context);

static void no_op(movement_event_t event, void *context) {};

static dngn_enemy_t generate_enemy(const dngn_state_t *state);
static bool run_hit(const dngn_state_t *state);
static uint8_t potion_heal_amount(const dngn_state_t *state);
static void apply_rewards_for_clearing_floor(dngn_state_t *state);
static uint8_t weighted_roll(weighted_choice_t *choices, uint8_t count);
static dngn_item_t generate_loot(const dngn_state_t *state);

static uint8_t get_anim_frame(dngn_animation_t *anim);
static void start_anim(dngn_animation_t *anim, uint8_t ticks_per_frame, uint8_t total_frames, bool loop);
static void stop_anim(dngn_animation_t *anim);
static void tick_anim(dngn_animation_t *anim);
static void refill_room_bag(dngn_state_t *state);
static void refill_loot_bag(dngn_state_t *state);

static dngn_room_type_t get_next_room_type();
static dngn_item_type_t get_next_loot_type();

// ---------- helpers ----------
void init_random() {
    srand((unsigned int)time(NULL));
}

static uint8_t roll(uint8_t min_inclusive, uint8_t max_inclusive) {
    return rand() % (max_inclusive - min_inclusive + 1) + min_inclusive;
}
static uint8_t weighted_roll(weighted_choice_t *choices, uint8_t count) {
    uint8_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        total += choices[i].weight;
    }

    uint8_t roll_val = rand() % total;
    for (uint8_t i = 0; i < count; i++) {
        if (roll_val < choices[i].weight) {
            return choices[i].value;
        }
        roll_val -= choices[i].weight;
    }

    return choices[count - 1].value;
}

// --- Tunable mechanics ---
static dngn_enemy_t generate_enemy(const dngn_state_t *state) {
    dngn_enemy_t e;

    uint8_t floor = state->floor;

    // TODO bring back as random chance for an "elite" enemy
    e.is_boss = roll(0, 100) <= 5;

    const uint8_t enemy_type = roll(0, DNGN_ENEMY_TYPE_COUNT);
    switch (enemy_type)
    {
    case DNGN_ENEMY_TYPE_BRUTE:
        e.hp  = 4 + state->floor * 2;
        e.damage = 1 + state->floor / 4;
        break;
    case DNGN_ENEMY_TYPE_STRIKER:
        e.hp  = 2 + state->floor;
        e.damage = 2 + state->floor / 3;
        break;
    case DNGN_ENEMY_TYPE_SWING:
        e.hp = 3 + state->floor * 3 / 2;
        e.damage = 1 + state->floor / 4; // base damage
        e.damage += roll(0, 1 + state->floor / 6); // extra
        break;
    default:
        printf("ERROR Unknown enemy type while generating enemy: %d\n", enemy_type);
        break;
    }

    e.hp = 2 + floor / 2;
    e.damage = 1 + floor / 5;

    if (e.is_boss) {
        e.hp += 3;
        e.damage += 1;
    }

    e.gold = e.is_boss ? 12 : 6;
    printf("Generated enemy. is_boss=%d hp=%d damage=%d gold=%d\n", e.is_boss, e.hp, e.damage, e.gold);
    return e;
}

static dngn_item_t generate_loot(const dngn_state_t *state) {
    dngn_item_t loot = { DNGN_ITEM_NONE, 0 };

    loot.type = get_next_loot_type(state);

    // convert to gold if roll cannot be used
    bool at_max_potions = state->player.potions >= DNGN_MAX_POTIONS;
    bool at_max_shields = state->player.shields >= DNGN_MAX_SHIELDS;
    bool at_max_hp = state->player.hp >= state->player.max_hp;
    if((loot.type == DNGN_ITEM_POTION && at_max_potions) ||
        (loot.type == DNGN_ITEM_SHIELD && at_max_shields) ||
        (loot.type == DNGN_ITEM_HP_UP && at_max_hp)) {
            printf("WARN Got unusable item type %d, converting to Gold.\n", loot.type);
            loot.type = DNGN_ITEM_GOLD;
    }
    switch (loot.type)
    {
    case DNGN_ITEM_WEAPON:
        loot.value = 1;
        break;
    case DNGN_ITEM_POTION:
        loot.value = potion_heal_amount(state);
        break;
    case DNGN_ITEM_SHIELD:
        loot.value = 1; // value not used right now
        break;
    case DNGN_ITEM_GOLD:
        loot.value = 4;
        break;
    case DNGN_ITEM_HP_UP:
        loot.value = DNGN_MAX_HP_UP_AMOUNT; // TODO change to its own amount
        break;
    case DNGN_ITEM_MAX_HP_UP:
        loot.value = DNGN_MAX_HP_UP_AMOUNT;
        break;
    default:
        printf("ERROR Unhandled type of loot generated: %d\n", loot.type);
        break;
    }
    
    printf("Generated loot type=%d value=%d\n", loot.type, loot.value);
    return loot;
}

static bool run_hit(const dngn_state_t *state) {
    uint8_t base = 50;
    uint8_t penalty = state->floor / 5; // gets worse deeper

    uint8_t chance = base + penalty;
    if (chance > 85) chance = 85;

    return (rand() % 100) < chance;
}

static uint8_t potion_heal_amount(const dngn_state_t *state) {
    // uint8_t min_heal = 2;
    // uint8_t max_heal = 4;
    // uint8_t base_heal = min_heal + rand() % (max_heal - min_heal + 1);
    uint8_t base_heal = 2;
    dngn_enemy_t enemy = state->enemy.damage > 0 ? state->enemy : generate_enemy(state);
    return enemy.damage + base_heal;
}

static void apply_rewards_for_clearing_floor(dngn_state_t *state) {
    int gold = state->floor > 0 ? 1 : 0;
    state->player.gold += gold;
}

static uint8_t clamp(uint8_t value, uint8_t value_min, uint8_t value_max) {
    if(value < value_min) return value_min;
    if(value > value_max) return value_max;
    return value;
}

void refill_room_bag(dngn_state_t *state) {
    int i = 0;
    int encounter_count = 6;
    int loot_count = 3;
    int empty_count = 2;

    for (int j = 0; j < encounter_count; j++)
        state->room_bag[i++] = DNGN_ROOM_ENEMY;

    for (int j = 0; j < loot_count; j++)
        state->room_bag[i++] = DNGN_ROOM_LOOT;

    for (int j = 0; j < empty_count; j++)
        state->room_bag[i++] = DNGN_ROOM_EMPTY;

    state->room_bag_count = ROOM_BAG_SIZE;
}

/* ---------------------- */
/* Get Next Room */
/* ---------------------- */
dngn_room_type_t get_next_room_type(dngn_state_t *state) {

    // Refill if empty
    if (state->room_bag_count == 0) {
        printf("DEBUG Room bag empty, refilling\n");
        refill_room_bag(state);
    }

    // Pick random index from remaining entries
    int r = rand() % state->room_bag_count;

    dngn_room_type_t chosen = state->room_bag[r];
    
    // Swap chosen with last element
    state->room_bag[r] = state->room_bag[state->room_bag_count - 1];
    
    // Shrink bag
    state->room_bag_count--;
    printf("DEBUG Chose index %d, room type %d. %d choices remain\n", r, chosen, state->room_bag_count);

    return chosen;
}

/* ---------------------- */
/* refill_loot_bag        */
/* ---------------------- */
static void refill_loot_bag(dngn_state_t *state)
{
    int i = 0;
    int weapon_count = 2;
    int potion_count = 2;
    int hp_count = 2;
    int max_hp_count = 1;
    int shield_count = 1;
    int gold_count = 2;
    for (int j = 0; j < weapon_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_WEAPON;

    for (int j = 0; j < potion_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_POTION;

    for (int j = 0; j < hp_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_HP_UP;

    for (int j = 0; j < max_hp_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_MAX_HP_UP;

    for (int j = 0; j < shield_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_SHIELD;

    for (int j = 0; j < gold_count; j++)
        state->loot_bag[i++] = DNGN_ITEM_GOLD;

    state->loot_bag_count = LOOT_BAG_SIZE;
}


/* ---------------------- */
/* get_next_loot          */
/* ---------------------- */
static dngn_item_type_t get_next_loot_type(dngn_state_t *state)
{
    if (state->loot_bag_count == 0) {
        refill_loot_bag(state);
    }

    int r = rand() % state->loot_bag_count;

    dngn_item_type_t chosen = state->loot_bag[r];

    // swap with last
    state->loot_bag[r] = state->loot_bag[state->loot_bag_count - 1];

    state->loot_bag_count--;

    return chosen;
}
static bool is_player_dead(dngn_state_t *state) {
    return state->player.hp <= 0;
}

static bool player_attack_enemy(dngn_state_t *state) {
    // returns true if enemy was defeated
    state->enemy.hp -= state->player.damage;
    printf("Floor %d. Player attacks enemy with %d ATK. Enemy: %d HP\n", state->floor, state->player.damage, state->enemy.hp);
    if (state->enemy.hp <= 0) {
        printf("Floor %d. Enemy defeated!\n", state->floor);
        state->player.gold += state->enemy.gold;
        return true;
    }
    return false;
}
static bool enemy_attack_player(dngn_state_t *state) {
    // returns true if player was defeated
    bool player_using_shield = state->player.shields > 0 && state->player.shield_equipped;
    if(player_using_shield) {
        state->player.shields--;
        state->player.shield_equipped = false;
        printf("Used shield to block %d damage. Shield broke. \n", state->enemy.damage);
        // TODO Show shield breaking or blocking attack
    } else {
        bool damage = state->enemy.damage;
        if (damage >= state->player.hp) {
            state->player.hp = 0;
            printf("Floor %d. Player dies! Gold: %d\n", state->floor, state->player.gold);
            return true;
        } else {
            state->player.hp -= damage;
        }
    }
    return false;
}

static void end_run(dngn_state_t *state) {
    state->screen = DNGN_SCREEN_GAME_OVER;
    state->active = false;
}
// sets state values for a new room of random type and sets the corresponding screen
static void enter_random_room(dngn_state_t *state) {
    // setup animation
    // clear previous (current for now) floor
    apply_rewards_for_clearing_floor(state);
    state->floor++;
    state->current_room = get_next_room_type(state);

    if (state->current_room == DNGN_ROOM_ENEMY) {
        state->enemy = generate_enemy(state);
        state->selected_action = DNGN_ACTION_FIGHT;
        state->screen = DNGN_SCREEN_ENCOUNTER;
    } else if (state->current_room == DNGN_ROOM_LOOT) {
        state->found_item = generate_loot(state);
        state->screen = DNGN_SCREEN_LOOT;
    } else if (state->current_room == DNGN_ROOM_EMPTY) {
        state->screen = DNGN_SCREEN_EMPTY_ROOM;
    } else {
        printf("ERROR enter_random_room Unhandled room type: %d\n", state->current_room);
        state->screen = DNGN_SCREEN_EMPTY_ROOM;
    }
    printf("enter_random_room new screen: %d\n", state->screen);

}

static void reset_player_state(dngn_state_t* state) {
    state->floor = 0;
    state->player.max_hp = 4;
    state->player.hp = 40;
    state->player.damage = 20;
    state->player.potions = 1;
    state->player.shields = 1;
    state->player.gold = 0;
}

// ---------- TITLE ----------
static void title_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    bool reset_state = false;
    if(event.event_type != EVENT_TICK) printf("_title_transition event_type=%d active=%d\n", event.event_type, state->active);

    bool start_new_run = !state->active && event.event_type == EVENT_ALARM_BUTTON_UP;
    bool reset_old_run = state->active && event.event_type == EVENT_ALARM_LONG_PRESS;
    bool play = event.event_type == EVENT_ALARM_BUTTON_UP || reset_old_run;

    if(start_new_run || reset_old_run) {
        // initialize player for new run
        reset_player_state(state);
    }
    if (play) {
        state->active = true;
        state->screen = DNGN_SCREEN_DESCEND;
    }
}

static void title_display(movement_event_t event, void *context) {
    (void)event;
    (void)context;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    watch_display_text_with_fallback(WATCH_POSITION_TOP, "dngn","dn");
    if(state->active) {
        watch_display_text(WATCH_POSITION_BOTTOM, "Cont");
    } else {
        watch_display_text(WATCH_POSITION_BOTTOM, "START");
    }
}

static void encounter_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(state->screen_changed) {
        start_anim(&state->anim, 6, 2, true);
    }
    if(skipped_anim(event, state)) {
        state->screen = DNGN_SCREEN_ENCOUNTER_MENU;
        printf("Going to encounter MENU\n");
    } else {
        default_button_handler(event);
    }
}

static void encounter_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    if(state->anim.active) {
        const uint8_t frame = get_anim_frame(&state->anim);
        draw_encounter(frame, context);
    }
}

// ---------- ENCOUNTER MENU --------

static void encounter_menu_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    if(state->screen_changed) {
        start_anim(&state->anim, 4, 2, true);
    }

    /*
    FIGHT
        Player attacks enemy with their weapon. Enemy HP - Player Weapon DMG
        Enemy attacks player. Player - Enemy DMG
    HEAL
        Player uses 1 healing potion. Player HP + Healing Potion HP
        Enemy attacks player. Player - Enemy DMG
    RUN
        Player attempts to flee.
        50% Enemy attacks player. Player - Enemy DMG
        50% Enemy misses player.
    */
    switch (event.event_type) {
        case EVENT_LIGHT_BUTTON_UP:
            // switch to the next menu item and reset ticks for it
            state->anim.current_tick = 0;
            bool invalid_choice = true;
            // select next valid item in enum. E.g. only show Heal option if player has potions
            do {
                state->selected_action = (state->selected_action + 1) % DNGN_ACTION_COUNT;
                invalid_choice = (state->selected_action == DNGN_ACTION_HEAL && state->player.potions == 0) || (state->selected_action == DNGN_ACTION_EQUIP_SHEILD && state->player.shields == 0);
                // if(invalid_choice) {
                //     printf("Player does not meet conditions for action type: %d. Skipping\n", state->selected_action);
                // } else {
                //     printf("Valid action type: %d\n", state->selected_action);
                // }
            } while(invalid_choice);
            break;

        case EVENT_ALARM_BUTTON_UP:
            // perform current selected action
            if (state->selected_action == DNGN_ACTION_FIGHT) {
                if (player_attack_enemy(state)) {
                    state->screen = DNGN_SCREEN_DESCEND;
                    break;
                }
                if(enemy_attack_player(state)) {
                    end_run(state);
                    break;
                }
                // TODO play attack animation
                // go back to enemy encounter screen
                state->screen = DNGN_SCREEN_ENCOUNTER;
            } else if (state->selected_action == DNGN_ACTION_HEAL && state->player.potions > 0) {
                state->player.potions--;
                const int heal_amount = potion_heal_amount(state);
                state->player.hp = clamp(state->player.hp + heal_amount, 0, state->player.max_hp);
                printf("Floor %d. Player drinks healing potion (+%d). Player: %d HP\n", state->floor, heal_amount, state->player.hp);
                // TODO play healing animation
                // TODO show +X HP
                // state->player.hp -= state->enemy.damage;
                printf("Floor %d. Enemy attacks player after healing with %d ATK. Player: %d HP\n", state->floor, state->enemy.damage, state->player.hp);
                if(enemy_attack_player(state)) {
                    end_run(state);
                    break;
                }
                // TODO play attack animation
                // go back to enemy encounter screen
                state->screen = DNGN_SCREEN_ENCOUNTER;
            } else if (state->selected_action == DNGN_ACTION_RUN) {
                // TODO show anim for - X HP and MISS
                bool hit = run_hit(state);
                state->outcome.encounter.hit = hit;
                if(hit) {
                    bool died = enemy_attack_player(state);
                    state->outcome.encounter.died = died;
                }
                state->screen = DNGN_SCREEN_RUN_AWAY;
            } else if (state->selected_action == DNGN_ACTION_EQUIP_SHEILD) {
                state->player.shield_equipped = state->player.shields > 0 && !state->player.shield_equipped;
            }
            // reset choice to beginning unless shield equip option
            if(state->selected_action != DNGN_ACTION_EQUIP_SHEILD) {
                state->selected_action = DNGN_ACTION_FIGHT;
            }
            break;

        default:
            default_button_handler(event);
    }
    
}

static void encounter_menu_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    char buf[5];
    watch_clear_display();
    watch_set_indicator(WATCH_INDICATOR_LAP);
    // TODO show lap icon, indicating a menu (e.i looping options)
    const uint8_t current_frame = get_anim_frame(&state->anim);
    switch (state->selected_action) {
        case DNGN_ACTION_FIGHT:
            if (current_frame == 0) {
                watch_display_text(WATCH_POSITION_BOTTOM, "FitE");
            } else {
                snprintf(buf, sizeof buf, "%4d", (int)state->player.damage);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "Pt");
            }
            break;
        case DNGN_ACTION_HEAL:
            if (current_frame == 0) {
                watch_display_text(WATCH_POSITION_BOTTOM, "HEAL");
            } else {
                uint8_t heal_amount = potion_heal_amount(state);
                // print heal amount
                snprintf(buf, sizeof buf, "%4d", heal_amount);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "HP");
                // print num potions left
                snprintf(buf, sizeof buf, "%2d", (int)state->player.potions);
                watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
            }
            break;
        case DNGN_ACTION_RUN:
            watch_display_text(WATCH_POSITION_BOTTOM, "run");
            break;
        case DNGN_ACTION_EQUIP_SHEILD:
            watch_display_text(WATCH_POSITION_BOTTOM, "SHiELD");
            watch_display_text(WATCH_POSITION_TOP, state->player.shield_equipped ? "On" : "no");
            // print num shields left
            snprintf(buf, sizeof buf, "%2d", (int)state->player.shields);
            watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
            break;
        default:
            printf("ERROR Unhandled display menu item: %d\n", state->selected_action);
            break;
    }
    
}

void dngn_apply_loot(dngn_state_t *state, dngn_item_t loot) {
    switch (loot.type) {
        case DNGN_ITEM_POTION:
            state->player.potions++;
            break;

        case DNGN_ITEM_WEAPON:
            state->player.damage += loot.value;
            break;

        case DNGN_ITEM_SHIELD:
            if (state->player.shields < DNGN_MAX_SHIELDS ) {
                state->player.shields++;
            }
            break;

        case DNGN_ITEM_MAX_HP_UP:
            state->player.max_hp += loot.value;
            break;

        case DNGN_ITEM_GOLD:
            state->player.gold += loot.value;
            break;

        default:
            break;
    }
}


// ---------- LOOT ----------
static void loot_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    // apply item effects on button press, then enter another room
    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        if (state->found_item.type == DNGN_ITEM_WEAPON) {
            state->player.damage += state->found_item.value;
        } else if (state->found_item.type == DNGN_ITEM_POTION) {
            state->player.potions++;
        } else if (state->found_item.type == DNGN_ITEM_MAX_HP_UP) {
            state->player.max_hp += state->found_item.value;
            // give their HP a littel boost too
            state->player.hp = clamp(state->player.hp + state->found_item.value, 0, state->player.max_hp);
        } else if (state->found_item.type == DNGN_ITEM_GOLD) {
            state->player.gold += state->found_item.value;
        } else if (state->found_item.type == DNGN_ITEM_NONE) {
            // no nothing
        } else {
            printf("ERROR: Unhandled loot type: %d\n", state->found_item.type);
        }
        state->screen = DNGN_SCREEN_DESCEND;
    } else {
        default_button_handler(event);
    }
    
}

static void loot_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    char buf[3]; // 2 chars + \0
    watch_clear_display();
    switch(state->found_item.type) {
        case DNGN_ITEM_WEAPON:
            watch_display_text(WATCH_POSITION_BOTTOM, "dagr");
            break;
        case DNGN_ITEM_POTION:
            watch_display_text(WATCH_POSITION_BOTTOM, "Potn");
            break;
        case DNGN_ITEM_SHIELD:
            watch_display_text(WATCH_POSITION_BOTTOM, "SHiELD");
            break;
        case DNGN_ITEM_GOLD:
            snprintf(buf, sizeof buf, "%2d", (int)state->found_item.value);
            watch_display_text(WATCH_POSITION_BOTTOM, "Gold");
            break;
        case DNGN_ITEM_MAX_HP_UP:
            watch_display_text(WATCH_POSITION_BOTTOM, "HPUP");
            break;
        case DNGN_ITEM_NONE:
            watch_display_text(WATCH_POSITION_TOP, "No");
            watch_display_text(WATCH_POSITION_BOTTOM, "Loot");
            break;
        default:
            printf("ERROR: Unhandled display loot type: %d\n", state->found_item.type);
    }
}

// ---------- STATUS ----------
static void empty_room_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        enter_random_room(state);
    } else {
        default_button_handler(event);
    }
}

static void empty_room_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    // watch_display_text(WATCH_POSITION_TOP, "HP");
    watch_display_text(WATCH_POSITION_BOTTOM, "EmPty");
    // watch_display_number(WATCH_POSITION_BOTTOM, state->player.hp);
    // watch_display_float_with_best_effort(state->player.hp, NULL);
}

// ---------- GAME OVER ----------
static void game_over_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(state->screen_changed) {
        start_anim(&state->anim, 4, 3, true);
    }
    switch (event.event_type)
    {
    case EVENT_ALARM_BUTTON_UP:
        stop_anim(&state->anim);
        state->screen = DNGN_SCREEN_TITLE;
        break;
    default:
        default_button_handler(event);
    }
}

static void game_over_display(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(!state->anim.active) {
        return;
    }
    const int frame = get_anim_frame(&state->anim);
    char buf[5]; // 4 chars + \0
    switch (frame)
    {
    case 0:
        watch_clear_display();
        watch_display_text(WATCH_POSITION_TOP, "U");
        watch_display_text(WATCH_POSITION_BOTTOM, "died");
        break;
    case 1:
        watch_clear_display();
        watch_display_text_with_fallback(WATCH_POSITION_TOP, "Flr", "Fl");
        snprintf(buf, sizeof buf, "%4d", (int)state->floor);
        watch_display_text(WATCH_POSITION_BOTTOM, buf);
        break;
    case 2:
        watch_clear_display();
        watch_display_text_with_fallback(WATCH_POSITION_TOP, "gld", "gl");
        snprintf(buf, sizeof buf, "%4d", (int)state->player.gold);
        watch_display_text(WATCH_POSITION_BOTTOM, buf);
        break;
    default:
        printf("ERROR: game_over_display unhandled frame index: %d\n", frame);
        break;
    }
}

static void descend_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    // when current animation is finished, play floor descend animation and roll next room
    // anim starting settings
    if (state->screen_changed) {
        start_anim(&state->anim, 2, 4, false);
    }

    switch(event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            stop_anim(&state->anim);
            break;
        default:
            default_button_handler(event);
    }
    // after animation finishes, go to the next screen/room
    if(!state->anim.active) {
        enter_random_room(state);
    }
}

static void descend_display(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(!state->anim.active) {
        return;
    }
    const uint8_t frame = get_anim_frame(&state->anim);
    draw_descend(frame, context);
}

static void run_away_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(state->screen_changed) {
        stop_anim(&state->anim);
        if (state->outcome.encounter.hit) {
            start_anim(&state->anim, 2, 4, false);
        }
    }
    if(!state->anim.active) {
        if(state->player.hp <= 0) {
            state->screen = DNGN_SCREEN_GAME_OVER;
        } else {
            state->screen = DNGN_SCREEN_DESCEND;
        }
    }
}

static void run_away_display(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(!state->anim.active) {
        return;
    }
    const uint8_t frame = get_anim_frame(&state->anim);
    draw_run_away_anim(frame, context);
}

// ---------- lifecycle ----------
void dungeon_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void)watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(dngn_state_t));
        dngn_state_t *state = (dngn_state_t *)*context_ptr;

        // wire screens
        state->screens[DNGN_SCREEN_TITLE]       = (dngn_screen_def_t){ title_transition, title_display };
        state->screens[DNGN_SCREEN_DESCEND]       = (dngn_screen_def_t){ descend_transition, descend_display };
        state->screens[DNGN_SCREEN_RUN_AWAY]       = (dngn_screen_def_t){ run_away_transition, run_away_display };
        state->screens[DNGN_SCREEN_ENCOUNTER]   = (dngn_screen_def_t){ encounter_transition, encounter_display };
        state->screens[DNGN_SCREEN_ENCOUNTER_MENU]   = (dngn_screen_def_t){ encounter_menu_transition, encounter_menu_display };
        state->screens[DNGN_SCREEN_LOOT]        = (dngn_screen_def_t){ loot_transition, loot_display };
        state->screens[DNGN_SCREEN_EMPTY_ROOM]      = (dngn_screen_def_t){ empty_room_transition, empty_room_display };
        state->screens[DNGN_SCREEN_GAME_OVER]   = (dngn_screen_def_t){ game_over_transition, game_over_display };
    }
}

void dungeon_face_activate(void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    // save off current screen
    // set to title screen and offer continue/reset
    if(state->screen != DNGN_SCREEN_TITLE) {
        state->last_screen = state->screen;
        printf("Last screen was: %d\n", state->last_screen);
    }
    state->screen = DNGN_SCREEN_TITLE;
    movement_request_tick_frequency(4);
}

bool dungeon_face_loop(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    // --- Event handling (and state logic) ---
    dngn_screen_def_t *screen_def = &state->screens[state->screen];
    
    dngn_screen_t last = state->screen;
    screen_def->transition(event, state);
    state->screen_changed = (last != state->screen);

    // Draw
    screen_def = &state->screens[state->screen];
    screen_def->display(event, state);

    // Tick animation
    if (event.event_type == EVENT_TICK) {
        tick_anim(&state->anim);
    }
    return true;
}

void dungeon_face_resign(void *context) {
    (void) context;
}