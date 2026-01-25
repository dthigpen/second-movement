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
#include "animation.h"
#include "menu.h"

static uint8_t TICK_COUNT = 20;

// --- High-level screens / modes ---
typedef enum {
DNGN_SCREEN_NONE = 0,
DNGN_SCREEN_TITLE,
DNGN_SCREEN_ANIM,
DNGN_SCREEN_ENCOUNTER,
DNGN_SCREEN_ENCOUNTER_MENU,
DNGN_SCREEN_RUN_AWAY, // short state to start hit or miss anim
DNGN_SCREEN_DESCEND, // short state to start descend anim
DNGN_SCREEN_LOOT,
DNGN_SCREEN_STATUS,
DNGN_SCREEN_GAME_OVER,
DNGN_SCREEN_COUNT
} dngn_screen_t;

// --- Encounter types ---
typedef enum {
DNGN_ROOM_EMPTY = 0,
DNGN_ROOM_ENEMY,
DNGN_ROOM_LOOT
} dngn_room_type_t;


// --- Player actions during encounters ---
typedef enum {
DNGN_ACTION_FIGHT = 0,
DNGN_ACTION_RUN,
DNGN_ACTION_HEAL,
DNGN_ACTION_COUNT
} dngn_action_t;


// --- Items (very small set) ---
typedef enum {
DNGN_ITEM_NONE = 0,
DNGN_ITEM_WEAPON,
DNGN_ITEM_POTION,
// DNGN_ITEM_SHIELD, // TODO
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
    bool has_shield;
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
dngn_room_type_t current_room;


// player stats
// uint8_t score;
dngn_player_t player;

// enemy stats (only valid during encounter)
dngn_enemy_t enemy;


// UI state
dngn_action_t selected_action;
dngn_item_t found_item;


// current animation
animation_state_t animation;

// ticks for thinks like flash or alternating text on the display
uint8_t ticks;
bool screen_changed;

dngn_outcome_event_t outcome;

} dngn_state_t;

// For dungeon generation:
typedef struct {
    uint8_t weight;
    uint8_t value;
} weighted_choice_t;

const int DNGN_HEALING_POTION_HP = 3;
const int DNGN_MAX_POTIONS = 3;

// --- START animation definitions

static bool default_button_handler(movement_event_t event) {
    // Returns true if handled by the default handler, otherwise false
    // indicating it should be handled by the caller
    switch(event.event_type) {
        case EVENT_MODE_BUTTON_UP:
        case EVENT_LIGHT_BUTTON_DOWN:
        case EVENT_LIGHT_BUTTON_UP:
        case EVENT_MODE_LONG_PRESS:
            movement_default_loop_handler(event);
            return true;
    }
    return false;
}
static bool unskippable_anim(animation_state_t *anim, movement_event_t event, void *context) {
    // unskippable meaning it cannot be skipped to reveal content after animation
    // MODE will still change faces
    return default_button_handler(event);
}

static bool skippable_anim(animation_state_t *anim, movement_event_t event, void *context) {
    switch(event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            printf("Animation SKIPPED!\n");
            animation_stop(anim);
            return true;
    }
    return default_button_handler(event);
}

// Descend animation

static void _draw_descend(uint8_t frame_index, void* context) {
    
    dngn_state_t *state = (dngn_state_t *) context;
    char buf[3]; // 2 chars + \0

    switch(frame_index) {
        case 0:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "Flr", "FL");
            snprintf(buf, sizeof buf, "%2d", (int)state->floor);
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

static const animation_frame_t descend_frames[] = {
    { .duration_ticks = 2 },
    { .duration_ticks = 2 },
    { .duration_ticks = 2 },
    { .duration_ticks = 2 },
};

const animation_def_t DNGN_ANIM_DESCEND = {
    .frames = descend_frames,
    .frame_count = sizeof(descend_frames) / sizeof(descend_frames[0]),
    .draw_frame = _draw_descend,
    .handle_event = skippable_anim
};

// Encounter animation

static void _draw_encounter(uint8_t frame_index, void* context) {
    
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
static const animation_frame_t encounter_frames[] = {
    { .duration_ticks = 8 },
    { .duration_ticks = 8 },
};

const animation_def_t DNGN_ANIM_ENCOUNTER = {
    .frames = encounter_frames,
    .frame_count = sizeof(encounter_frames) / sizeof(encounter_frames[0]),
    .draw_frame = _draw_encounter,
    .handle_event = unskippable_anim
};

// Run Away animation

static void _draw_run_away_anim(uint8_t frame_index, void *context) {
    dngn_state_t *state = (dngn_state_t*) context;
    switch(frame_index) {
        case 0:
            watch_clear_display();
            const char* text = "HiT";
            watch_display_text(WATCH_POSITION_BOTTOM, text);
            break;
        case 1:
            watch_clear_display();
            break;
    }
}

static animation_frame_t _run_away_anim_frames[] = {
    { .duration_ticks = 4 },
    { .duration_ticks = 4 },
};

const animation_def_t DNGN_ANIM_RUN_AWAY = {
    .frames = _run_away_anim_frames,
    .frame_count = sizeof(_run_away_anim_frames) / sizeof(_run_away_anim_frames[0]),
    .draw_frame = _draw_run_away_anim,
    .handle_event = skippable_anim,
};
// --- END animation definitions

static void dngn_start_animation(dngn_state_t *state, const animation_def_t* anim_def, int8_t loop) {
    animation_start(&state->animation, anim_def, (void *) state, loop);
    movement_request_tick_frequency(8);
}

static void dngn_start_animation_before_screen(dngn_state_t *state, const animation_def_t* anim_def, int8_t loop, dngn_screen_t next_screen) {
    animation_start(&state->animation, anim_def, (void *) state, loop);
    state->screen = DNGN_SCREEN_ANIM;
    movement_request_tick_frequency(8);
}

// ---------- forward declarations ----------

static void _title_transition(movement_event_t event, void *context);
static void _title_display(movement_event_t event, void *context);

static void _anim_transition(movement_event_t event, void *context);
static void _anim_display(movement_event_t event, void *context);

static void _descend_transition(movement_event_t event, void *context);

static void _run_away_transition(movement_event_t event, void *context);

// static void _descend_display(movement_event_t event, void *context);

static void _floor_transition(movement_event_t event, void *context);
static void _floor_display(movement_event_t event, void *context);

static void _encounter_transition(movement_event_t event, void *context);
static void _encounter_display(movement_event_t event, void *context);

static void _encounter_menu_transition(movement_event_t event, void *context);
static void _encounter_menu_display(movement_event_t event, void *context);

static void _loot_transition(movement_event_t event, void *context);
static void _loot_display(movement_event_t event, void *context);

static void _status_transition(movement_event_t event, void *context);
static void _status_display(movement_event_t event, void *context);

static void _game_over_transition(movement_event_t event, void *context);
static void _game_over_display(movement_event_t event, void *context);

static void _no_op(movement_event_t event, void *context) {};

dngn_enemy_t dngn_generate_enemy(const dngn_state_t *state);
bool dngn_run_hit(const dngn_state_t *state);
uint8_t dngn_potion_heal_amount(const dngn_state_t *state);
uint8_t dngn_score_for_floor(const dngn_state_t *state);
uint8_t dngn_score_for_enemy(const dngn_enemy_t *enemy);



// ---------- helpers ----------
static uint8_t _rand(uint8_t max) {
    return rand() % max;
}

uint8_t weighted_roll(weighted_choice_t *choices, uint8_t count) {
    uint8_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        total += choices[i].weight;
    }

    uint8_t roll = rand() % total;
    for (uint8_t i = 0; i < count; i++) {
        if (roll < choices[i].weight) {
            return choices[i].value;
        }
        roll -= choices[i].weight;
    }

    return choices[count - 1].value;
}

// --- Tunable mechanics ---
dngn_enemy_t dngn_generate_enemy(const dngn_state_t *state) {
    dngn_enemy_t e;

    uint8_t floor = state->floor;

    e.is_boss = (floor % 10 == 0);

    e.hp = 2 + floor / 2;
    e.damage = 1 + floor / 6;

    if (e.is_boss) {
        e.hp += 3;
        e.damage += 1;
    }

    e.gold = e.is_boss ? 12 : 6;
    printf("Generated enemy. is_boss=%d hp=%d damage=%d gold=%d\n", e.is_boss, e.hp, e.damage, e.gold);
    return e;
}

dngn_item_t dngn_generate_loot(const dngn_state_t *state) {
    dngn_item_t loot = { DNGN_ITEM_NONE, 0 };

    uint8_t roll = rand() % 100;

    bool can_get_potion = state->player.potions < DNGN_MAX_POTIONS;
    if (can_get_potion && roll < 40) {
        loot.type = DNGN_ITEM_POTION;
        loot.value = dngn_potion_heal_amount(state);
    } else if (roll < 60) {
        loot.type = DNGN_ITEM_WEAPON;
        loot.value = 1; // +1 damage
    }
    // TODO add more. Max HP, weapn names, gold?
    printf("Generated loot. type=%d value=%d\n", loot.type, loot.value);
    return loot;
}

bool dngn_run_hit(const dngn_state_t *state) {
    uint8_t base = 50;
    uint8_t penalty = state->floor / 5; // gets worse deeper

    uint8_t chance = base + penalty;
    if (chance > 85) chance = 85;

    return (rand() % 100) < chance;
}

uint8_t dngn_potion_heal_amount(const dngn_state_t *state) {
    uint8_t heal = 2;

    if (state->floor > 10) heal = 3;
    if (heal > state->player.max_hp) heal = state->player.max_hp;

    return heal;
}

uint8_t dngn_score_for_floor(const dngn_state_t *state) {
    return state->floor > 0 ? 1 : 0;
}

uint8_t dngn_score_for_enemy(const dngn_enemy_t *enemy) {
    // return enemy->is_boss ? 12 : 6;
    return enemy->gold;
}


static uint8_t _clamp(uint8_t value, uint8_t value_min, uint8_t value_max) {
    if(value < value_min) return value_min;
    if(value > value_max) return value_max;
    return value;
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
        state->player.gold += dngn_score_for_enemy(&state->enemy);
        return true;
    }
    return false;
}
static bool enemy_attack_player(dngn_state_t *state) {
    // returns true if player was defeated
    state->player.hp -= state->enemy.damage;
    printf("Floor %d. Enemy attacks player with %d ATK. Player: %d HP\n", state->floor, state->enemy.damage, state->player.hp);
    if (state->player.hp <= 0) {
        printf("Floor %d. Player dies!\n", state->floor);
        return true;
    }
    return false;
}

static void end_run(dngn_state_t *state) {
    state->screen = DNGN_SCREEN_GAME_OVER;
    state->active = false;
}
// sets state values for a new room of random type and sets the corresponding screen
static void _enter_random_room(dngn_state_t *state) {
    // setup animation
    dngn_start_animation(state, &DNGN_ANIM_DESCEND, 1);

    uint8_t cleared_floor_score = dngn_score_for_floor(state);
    state->player.gold += cleared_floor_score;

    // generate random room
    weighted_choice_t room_weights[][3] = {
        // floors 1-5
        {{ 50, DNGN_ROOM_ENEMY },
        { 30, DNGN_ROOM_LOOT },
        { 20, DNGN_ROOM_EMPTY }},
        // floors 6-10
        {{ 60, DNGN_ROOM_ENEMY },
        { 25, DNGN_ROOM_LOOT },
        { 15, DNGN_ROOM_EMPTY }},
        // floors 11+
        {{ 70, DNGN_ROOM_ENEMY },
        { 20, DNGN_ROOM_LOOT },
        { 10, DNGN_ROOM_EMPTY }},
    };
    uint8_t bracket = ++state->floor <= 5 ? 0 : state->floor <= 10 ? 1 : 2;
    // uint8_t r = _rand(3);
    uint8_t r = weighted_roll(room_weights[bracket], 3);
    state->current_room = (dngn_room_type_t)r;

    if (state->current_room == DNGN_ROOM_ENEMY) {
        state->enemy = dngn_generate_enemy(state);
        state->selected_action = DNGN_ACTION_FIGHT;
        state->screen = DNGN_SCREEN_ENCOUNTER;
    } else if (state->current_room == DNGN_ROOM_LOOT) {
        state->found_item = dngn_generate_loot(state);
        state->screen = DNGN_SCREEN_LOOT;
        printf("Generated loot for room\n");
    } else {
        // empty room
        state->screen = DNGN_SCREEN_STATUS;
    }
}


static void reset_player_state(dngn_state_t* state) {
    state->floor = 0;
    // state->player = {0};
    state->player.max_hp = 10;
    state->player.hp = 10;
    state->player.damage = 2;
    state->player.potions = 0;
    state->player.gold = 0;
}

// ---------- TITLE ----------
static void _title_transition(movement_event_t event, void *context) {
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

static void _title_display(movement_event_t event, void *context) {
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

// ---------- ANIM ----------
static void _anim_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    // when animation is finished move to the next state
    if(!state->animation.active && state->screen_after_anim != DNGN_SCREEN_NONE) {
        state->screen = state->screen_after_anim;
        state->screen_after_anim = DNGN_SCREEN_NONE;
    }
}
static void _anim_display(movement_event_t event, void *context) {
    // nothing to display, animation should be playing
}

// ---------- FLOOR START ----------
static void _floor_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        state->screen = DNGN_SCREEN_DESCEND;
    } else {
        movement_default_loop_handler(event);
    }
}

static void _floor_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    watch_display_text(WATCH_POSITION_TOP, "FLOOR");
    // watch_display_text(WATCH_POSITION_BOTTOM, state->floor);
    watch_display_float_with_best_effort(state->floor, NULL);
}

// ---------- ENCOUNTER ----------
static void _encounter_transition_orig(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    switch (event.event_type) {
        case EVENT_LIGHT_BUTTON_UP:
            state->selected_action = (state->selected_action + 1) % DNGN_ACTION_COUNT;
            break;

        case EVENT_ALARM_BUTTON_UP:
            if (state->selected_action == DNGN_ACTION_FIGHT) {
                printf("Floor %d. Player attacks enemy with %d ATK. Enemy: %d HP\n", state->floor, state->player.damage, state->enemy.hp);
                state->enemy.hp -= state->player.damage;
                
                if (state->enemy.hp <= 0) {
                    printf("Floor %d. Enemy defeated!\n", state->floor);
                    _enter_random_room(state);
                    break;
                }
                state->player.hp -= state->enemy.damage;
                printf("Floor %d. Enemy attacks player with %d ATK. Player: %d HP\n", state->floor, state->enemy.damage, state->player.hp);
            } else if (state->selected_action == DNGN_ACTION_HEAL && state->player.potions > 0) {
                state->player.potions--;
                state->player.hp += DNGN_HEALING_POTION_HP;
                if (state->player.hp > state->player.max_hp)
                    state->player.hp = state->player.max_hp;
                printf("Floor %d. Player drinks healing potion (+3). Player: %d HP\n", state->floor, state->player.hp);
            } else if (state->selected_action == DNGN_ACTION_RUN) {
                state->player.hp -= 1;
                _enter_random_room(state);
                break;
            }

            if (state->player.hp <= 0) {
                state->screen = DNGN_SCREEN_GAME_OVER;
                state->active = false;
                printf("Floor %d. Player dies!\n", state->floor);
            }
            break;

        default:
            movement_default_loop_handler(event);
    }
}


static void _encounter_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(!state->animation.active) {
        printf("Starting encounter anim\n");
        dngn_start_animation(state, &DNGN_ANIM_ENCOUNTER, -1);
    }

    switch (event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            animation_stop(&state->animation);
            state->screen = DNGN_SCREEN_ENCOUNTER_MENU;
            printf("Going to encounter MENU\n");
        default:
            movement_default_loop_handler(event);
    }
}

static void _encounter_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    // taken care of by animation
}

static int calc_heal_amount(dngn_state_t *state) {
    uint8_t floor = state->floor;
    if (floor <= 10) {
        return 3;
    } else if (floor <= 20) {
        return 6;
    } else {
        return 8;
    }
}
// ---------- ENCOUNTER MENU --------

static void _encounter_menu_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    if(state->screen_changed) {
        state->ticks = 0;
    } else {
        state->ticks = (state->ticks + 1) % TICK_COUNT;
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
            state->ticks = 0;
            state->selected_action = (state->selected_action + 1) % DNGN_ACTION_COUNT;
            break;

        case EVENT_ALARM_BUTTON_UP:
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
                const int heal_amount = calc_heal_amount(state);
                state->player.hp = _clamp(state->player.hp + heal_amount, 0, state->player.max_hp);
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
                bool hit = dngn_run_hit(state);
                state->outcome.encounter.hit = hit;
                if(hit) {
                    bool died = enemy_attack_player(state);
                    state->outcome.encounter.died = died;
                }
                state->screen = DNGN_SCREEN_RUN_AWAY;
                break;
            }
            // if an action was taken, reset to the first menu item: fight
            // unless they tried to select heal when they had no healing potions
            const bool failed_heal = state->selected_action == DNGN_ACTION_HEAL && state->player.potions == 0;
            if(!failed_heal) {
                state->selected_action = DNGN_ACTION_FIGHT;
            }
            break;

        default:
            movement_default_loop_handler(event);
    }
    
}

static void _encounter_menu_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    char buf[5];
    watch_clear_display();
    watch_set_indicator(WATCH_INDICATOR_LAP);
    // TODO show lap icon, indicating a menu (e.i looping options)
    switch (state->selected_action) {
        case DNGN_ACTION_FIGHT:
            if (state->ticks < TICK_COUNT / 2) {
                watch_display_text(WATCH_POSITION_BOTTOM, "FitE");
            } else {
                snprintf(buf, sizeof buf, "%4d", (int)state->player.damage);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "Pt");
            }
            break;
        case DNGN_ACTION_HEAL:
            if (state->ticks < TICK_COUNT / 2) {
                watch_display_text(WATCH_POSITION_BOTTOM, "HEAL");
            } else {
                uint8_t heal_amount = dngn_potion_heal_amount(state);
                // print heal amount
                snprintf(buf, sizeof buf, "%4d", heal_amount);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "HP");
                // print num potions left
                snprintf(buf, sizeof buf, "%2d", (int)state->player.potions);
                watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);



            }
            break;
            break;
        case DNGN_ACTION_RUN:
            watch_display_text(WATCH_POSITION_BOTTOM, "run");
            break;
        default:
            break;
    }
    
}

// ---------- LOOT ----------
static void _loot_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;

    // apply item effects on button press, then enter another room
    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        if (state->found_item.type == DNGN_ITEM_WEAPON) {
            state->player.damage += state->found_item.value;
        } else if (state->found_item.type == DNGN_ITEM_POTION) {
            state->player.potions++;
        } else if (state->found_item.type == DNGN_ITEM_MAX_HP_UP) {
            state->player.hp += state->found_item.value;
        } else {
            printf("ERROR: _loot_transition Unhandled loot type: %d\n", state->found_item.type);
        }
        _enter_random_room(state);
    } else {
        default_button_handler(event);
    }
    
}

static void _loot_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    watch_display_text(WATCH_POSITION_TOP, "Lt");
    switch(state->found_item.type) {
        case DNGN_ITEM_WEAPON:
            watch_display_text(WATCH_POSITION_BOTTOM, "dagr");
            break;
        case DNGN_ITEM_POTION:
            watch_display_text(WATCH_POSITION_BOTTOM, "Potn");
            break;
        case DNGN_ITEM_MAX_HP_UP:
            watch_display_text(WATCH_POSITION_BOTTOM, "HPUP");
            break;
        case DNGN_ITEM_NONE:
            watch_display_text(WATCH_POSITION_BOTTOM, "none");
            break;
        default:
            printf("ERROR: Unknown loot type: %d\n", state->found_item.type);
    }
}

// ---------- STATUS ----------
static void _status_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        _enter_random_room(state);
    } else {
        movement_default_loop_handler(event);
    }
}

static void _status_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    watch_display_text(WATCH_POSITION_TOP, "HP");
    // watch_display_number(WATCH_POSITION_BOTTOM, state->player.hp);
    watch_display_float_with_best_effort(state->player.hp, NULL);
}

// ---------- GAME OVER ----------
static void _game_over_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    const int total_ticks = 30;
    if(state->screen_changed) {
        state->ticks = 0;
    } else {
        state->ticks = (state->ticks + 1) % total_ticks; // 3 frames, 10 ticks each
    }
    switch (event.event_type)
    {
    case EVENT_ALARM_BUTTON_UP:
        state->screen = DNGN_SCREEN_TITLE;
        break;
    default:
        default_button_handler(event);
    }
}

static void _game_over_display(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    const int total_ticks = 30;
    const int total_frames = 3;
    const int ticks_per_frame = total_ticks / total_frames;
    const int frame = state->ticks / ticks_per_frame;
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
        printf("ERROR: _game_over_display unhandled frame index: %d\n", frame);
        break;
    }
}

static void _descend_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    // when current animation is finished, play floor descend animation and roll next room
    if(!state->animation.active) {
        dngn_start_animation(state, &DNGN_ANIM_DESCEND, 1);
        _enter_random_room(state);
    }
}

static void _run_away_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if(!state->animation.active) {
        if (state->outcome.encounter.hit) {
            dngn_start_animation(state, &DNGN_ANIM_RUN_AWAY, 2);
        }
    }        
    if(state->player.hp <= 0) {
        state->screen = DNGN_SCREEN_GAME_OVER;
    } else {
        state->screen = DNGN_SCREEN_DESCEND;
    }
}

// ---------- lifecycle ----------
void dungeon_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void)watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(dngn_state_t));
        dngn_state_t *state = (dngn_state_t *)*context_ptr;

        // wire screens
        state->screens[DNGN_SCREEN_TITLE]       = (dngn_screen_def_t){ _title_transition, _title_display };
        state->screens[DNGN_SCREEN_DESCEND]       = (dngn_screen_def_t){ _descend_transition, _no_op };
        state->screens[DNGN_SCREEN_RUN_AWAY]       = (dngn_screen_def_t){ _run_away_transition, _no_op };
        state->screens[DNGN_SCREEN_ANIM]       = (dngn_screen_def_t){ _anim_transition, _anim_display };
        state->screens[DNGN_SCREEN_ENCOUNTER]   = (dngn_screen_def_t){ _encounter_transition, _encounter_display };
        state->screens[DNGN_SCREEN_ENCOUNTER_MENU]   = (dngn_screen_def_t){ _encounter_menu_transition, _encounter_menu_display };
        state->screens[DNGN_SCREEN_LOOT]        = (dngn_screen_def_t){ _loot_transition, _loot_display };
        state->screens[DNGN_SCREEN_STATUS]      = (dngn_screen_def_t){ _status_transition, _status_display };
        state->screens[DNGN_SCREEN_GAME_OVER]   = (dngn_screen_def_t){ _game_over_transition, _game_over_display };
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
    
    // allow animation to handle event first
    // then screen state next
    if (animation_handle_event(&state->animation, event, context)) {
        // animation consumed it
    } else {
        // Call this screen's transition function
        // and set a flag if the screen has changed during it
        dngn_screen_t last_screen = state->screen;
        screen_def->transition(event, state);
        dngn_screen_t new_screen = state->screen;
        state->screen_changed = last_screen != new_screen;
    }

    // --- Display ---
    // allow animation to draw first
    // if no animation drawn then screen state can draw
    if (!animation_draw(&state->animation)) {
        // screen may change during transition
        screen_def = &state->screens[state->screen];
        screen_def->display(event, state);
    }

    // increment animation state
    if(event.event_type == EVENT_TICK) {
        animation_tick(&state->animation);
    }
    return true;
}

void dungeon_face_resign(void *context) {
    (void) context;
}