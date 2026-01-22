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
DNGN_SCREEN_FLOOR_START,
DNGN_SCREEN_ENCOUNTER,
DNGN_SCREEN_ENCOUNTER_MENU,
DNGN_SCREEN_LOOT,
DNGN_SCREEN_STATUS,
DNGN_SCREEN_GAME_OVER,
DNGN_SCREEN_COUNT
} dngn_screen_t;


typedef enum {
    DNGN_OUTCOME_NONE = 0,
    DNGN_OUTCOME_ENC_FIGHT,
    DNGN_OUTCOME_ENC_HEAL_LIVE,
    DNGN_OUTCOME_ENC_HEAL_DIE,
    DNGN_OUTCOME_ENC_RUN_HIT_LIVE,
    DNGN_OUTCOME_ENC_RUN_HIT_DIE,
    DNGN_OUTCOME_ENC_RUN_MISS,
} dngn_outcome_t;


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
DNGN_ITEM_SWORD,
DNGN_ITEM_POTION,
DNGN_ITEM_MAX_HP_UP
} dngn_item_t;


// --- Screen function table ---
typedef struct {
void (*transition)(movement_event_t event, void *context);
void (*display)(movement_event_t event, void *context);
} dngn_screen_def_t;




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
int8_t player_hp;
int8_t player_max_hp;
int8_t player_attack;
uint8_t potions;


// enemy stats (only valid during encounter)
int8_t enemy_hp;
int8_t enemy_attack;


// UI state
dngn_action_t selected_action;
dngn_item_t found_item;

// current animation
animation_state_t animation;

// ticks for thinks like flash or alternating text on the display
uint8_t ticks;
bool screen_changed;

// outcome of each transition
dngn_outcome_t pending_outcome;

} dngn_state_t;

// For dungeon generation:
typedef struct {
    uint8_t weight;
    uint8_t value;
} weighted_choice_t;

const int DNGN_HEALING_POTION_HP = 3;

// --- START animation definitions

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

static bool unskippable_anim(animation_state_t *anim, movement_event_t event, void *context) {
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

static bool skippable_anim(animation_state_t *anim, movement_event_t event, void *context) {
    switch(event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            printf("Animation SKIPPED!\n");
            animation_stop(anim);
            return true;
        case EVENT_MODE_BUTTON_UP:
        case EVENT_LIGHT_BUTTON_DOWN:
        case EVENT_LIGHT_BUTTON_UP:
        case EVENT_MODE_LONG_PRESS:
            movement_default_loop_handler(event);
            return true;
    }
    return false;
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

static bool _handle_event_encounter_anim(animation_state_t *anim, movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *) context;
    switch(event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            state->screen = DNGN_SCREEN_ENCOUNTER_MENU;
            return true;
        case EVENT_MODE_LONG_PRESS:
            movement_default_loop_handler(event);
            return true;
    }
    return false;
}
static void _draw_encounter(uint8_t frame_index, void* context) {
    
    dngn_state_t *state = (dngn_state_t *) context;
    char buf[5]; // 4 chars + \0
    switch(frame_index) {
        case 0:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "ENEMY", "EN");
            snprintf(buf, sizeof buf, "%4d", (int)state->enemy_hp);
            watch_display_text(WATCH_POSITION_BOTTOM, buf);
            watch_display_text(WATCH_POSITION_SECONDS, "HP");
            break;
        case 1:
            watch_clear_display();
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "PLyr", "PL");
            snprintf(buf, sizeof buf, "%4d", (int)state->player_hp);
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

static void _generate_enemy(dngn_state_t *state) {
    state->enemy_hp = (4 + state->floor / 2) + rand() % 3;
    state->enemy_attack = 1 + state->floor / 6;
}

static uint8_t _clamp(uint8_t value, uint8_t value_min, uint8_t value_max) {
    if(value < value_min) return value_min;
    if(value > value_max) return value_max;
    return value;
}
static void _generate_loot(dngn_state_t *state) {
    weighted_choice_t item_weights[] = {
        {50, DNGN_ITEM_POTION},
        {30, DNGN_ITEM_SWORD},
        {15, DNGN_ITEM_MAX_HP_UP},
        {5, DNGN_ITEM_NONE},
    };
    state->found_item = (dngn_item_t)weighted_roll(item_weights, 4);

    if(state->found_item == DNGN_ITEM_POTION) {
        state->potions += 1;
    } else if(state->found_item == DNGN_ITEM_SWORD) {
        // sword increases attack by +1, or if floor 15+ then chance to be +2
        state->player_attack += state->floor >= 15 && _rand(100) > 70 ? 2 : 1;
    } else if(state->found_item == DNGN_ITEM_MAX_HP_UP) {
        // increase max hp by 2, heal 2hp and stop at max
        state->player_max_hp += 2;
        state->player_hp = _clamp(state->player_hp + 2, 0, state->player_max_hp);
    } else if(state->found_item == DNGN_ITEM_NONE) {
        // no changes to state
    }
}

static bool is_player_dead(dngn_state_t *state) {
    return state->player_hp <= 0;
}

static bool player_attack_enemy(dngn_state_t *state) {
    // returns true if enemy was defeated
    state->enemy_hp -= state->player_attack;
    printf("Floor %d. Player attacks enemy with %d ATK. Enemy: %d HP\n", state->floor, state->player_attack, state->enemy_hp);
    if (state->enemy_hp <= 0) {
        printf("Floor %d. Enemy defeated!\n", state->floor);
        return true;
    }
    return false;
}
static bool enemy_attack_player(dngn_state_t *state) {
    // returns true if player was defeated
    state->player_hp -= state->enemy_attack;
    printf("Floor %d. Enemy attacks player with %d ATK. Player: %d HP\n", state->floor, state->enemy_attack, state->player_hp);
    if (state->player_hp <= 0) {
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
        _generate_enemy(state);
        state->selected_action = DNGN_ACTION_FIGHT;
    } else if (state->current_room == DNGN_ROOM_LOOT) {
        _generate_loot(state);
    }

    // set the screen to the one corresponding to the room type
    state->screen = (state->current_room == DNGN_ROOM_ENEMY)
            ? DNGN_SCREEN_ENCOUNTER
            : (state->current_room == DNGN_ROOM_LOOT)
                ? DNGN_SCREEN_LOOT
                : DNGN_SCREEN_STATUS;
}


static void reset_player_state(dngn_state_t* state) {
    state->floor = 0;
    state->player_max_hp = 10;
    state->player_hp = 10;
    state->player_attack = 2;
    state->potions = 1;
}


// ---------- lifecycle ----------
void dungeon_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void)watch_face_index;

    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(dngn_state_t));
        dngn_state_t *state = (dngn_state_t *)*context_ptr;

        // wire screens
        state->screens[DNGN_SCREEN_TITLE]       = (dngn_screen_def_t){ _title_transition, _title_display };
        state->screens[DNGN_SCREEN_ANIM]       = (dngn_screen_def_t){ _anim_transition, _anim_display };
        state->screens[DNGN_SCREEN_FLOOR_START] = (dngn_screen_def_t){ _floor_transition, _floor_display };
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

    // if(event.event_type != EVENT_TICK) printf("dungeon_face_loop screen=%d, last_screen=%d, event_type=%d, active=%d\n", state->screen, state->last_screen, event.event_type, state->active);
    
    // --- Event handling (and state logic) ---
    dngn_screen_def_t *screen = &state->screens[state->screen];
    // allow animation to handle event first
    // then screen state next
    if (animation_handle_event(&state->animation, event, context)) {
        // animation consumed it
    } else {
        // Call this screen's transition function
        // and set a flag if the screen has changed during it
        dngn_screen_t last_screen = state->screen;
        screen->transition(event, state);
        dngn_screen_t new_screen = state->screen;
        state->screen_changed = last_screen != new_screen;
    }

    // --- Display ---
    // allow animation to draw first
    // if no animation drawn then screen state can draw
    if (!animation_draw(&state->animation)) {
        // screen may change during transition
        screen = &state->screens[state->screen];
        screen->display(event, state);
    }

    // increment animation state
    if(event.event_type == EVENT_TICK) {
        animation_tick(&state->animation);
    }
    return true;
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
        _enter_random_room(state);
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
        _enter_random_room(state);
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
                printf("Floor %d. Player attacks enemy with %d ATK. Enemy: %d HP\n", state->floor, state->player_attack, state->enemy_hp);
                state->enemy_hp -= state->player_attack;
                
                if (state->enemy_hp <= 0) {
                    printf("Floor %d. Enemy defeated!\n", state->floor);
                    _enter_random_room(state);
                    break;
                }
                state->player_hp -= state->enemy_attack;
                printf("Floor %d. Enemy attacks player with %d ATK. Player: %d HP\n", state->floor, state->enemy_attack, state->player_hp);
            } else if (state->selected_action == DNGN_ACTION_HEAL && state->potions > 0) {
                state->potions--;
                state->player_hp += DNGN_HEALING_POTION_HP;
                if (state->player_hp > state->player_max_hp)
                    state->player_hp = state->player_max_hp;
                printf("Floor %d. Player drinks healing potion (+3). Player: %d HP\n", state->floor, state->player_hp);
            } else if (state->selected_action == DNGN_ACTION_RUN) {
                state->player_hp -= 1;
                _enter_random_room(state);
                break;
            }

            if (state->player_hp <= 0) {
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
        dngn_start_animation(state, &DNGN_ANIM_ENCOUNTER, 1);
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
                    _enter_random_room(state);
                    break;
                }
                if(enemy_attack_player(state)) {
                    end_run(state);
                    break;
                }
                // TODO play attack animation
                // go back to enemy encounter screen
                state->screen = DNGN_SCREEN_ENCOUNTER;
            } else if (state->selected_action == DNGN_ACTION_HEAL && state->potions > 0) {
                state->potions--;
                const int heal_amount = calc_heal_amount(state);
                state->player_hp = _clamp(state->player_hp + heal_amount, 0, state->player_max_hp);
                printf("Floor %d. Player drinks healing potion (+%d). Player: %d HP\n", state->floor, heal_amount, state->player_hp);
                // TODO play healing animation
                // TODO show +X HP
                state->player_hp -= state->enemy_attack;
                printf("Floor %d. Enemy attacks player after healing with %d ATK. Player: %d HP\n", state->floor, state->enemy_attack, state->player_hp);
                // TODO play attack animation
                // go back to enemy encounter screen
                state->screen = DNGN_SCREEN_ENCOUNTER;
            } else if (state->selected_action == DNGN_ACTION_RUN) {
                // TODO show anim for - X HP and MISS
                const int ATK = 0;
                const int MISS = 1;
                weighted_choice_t choices[] = {
                    {50, ATK}, {50, MISS}
                };
                const int roll = weighted_roll(choices, sizeof choices);
                if(roll == ATK) {
                    if(enemy_attack_player(state)) {
                        end_run(state);
                        break;
                    }
                }
                _enter_random_room(state);
                break;
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
                snprintf(buf, sizeof buf, "%4d", (int)state->player_attack);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "Pt");
            }
            break;
        case DNGN_ACTION_HEAL:
            if (state->ticks < TICK_COUNT / 2) {
                watch_display_text(WATCH_POSITION_BOTTOM, "HEAL");
            } else {
                snprintf(buf, sizeof buf, "%4d", DNGN_HEALING_POTION_HP);
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
                watch_display_text(WATCH_POSITION_SECONDS, "HP");
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

    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        if (state->found_item == DNGN_ITEM_SWORD) {
            state->player_attack++;
        } else if (state->found_item == DNGN_ITEM_POTION) {
            state->potions++;
        }
        _enter_random_room(state);
    } else {
        movement_default_loop_handler(event);
    }
}

static void _loot_display(movement_event_t event, void *context) {
    (void)event;
    dngn_state_t *state = (dngn_state_t *)context;
    watch_clear_display();
    watch_display_text(WATCH_POSITION_TOP, "LOOT");
    watch_display_text(WATCH_POSITION_BOTTOM,
        state->found_item == DNGN_ITEM_SWORD ? "dagr" : "potn");
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
    // watch_display_number(WATCH_POSITION_BOTTOM, state->player_hp);
    watch_display_float_with_best_effort(state->player_hp, NULL);
}

// ---------- GAME OVER ----------
static void _game_over_transition(movement_event_t event, void *context) {
    dngn_state_t *state = (dngn_state_t *)context;
    if (event.event_type == EVENT_ALARM_BUTTON_UP) {
        dungeon_face_activate(state);
    } else {
        movement_default_loop_handler(event);
    }
}

static void _game_over_display(movement_event_t event, void *context) {
    (void)event;
    (void)context;
    watch_clear_display();
    watch_display_text(WATCH_POSITION_TOP, "GAME");
    watch_display_text(WATCH_POSITION_BOTTOM, "OVER");
}

void dungeon_face_resign(void *context) {
    (void) context;
}
