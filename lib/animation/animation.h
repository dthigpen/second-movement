#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * A single animation frame definition.
 * Extendable later with flags, effects, etc.
 */
typedef struct {
    uint8_t duration_ticks;
} animation_frame_t;

/*
 * Animation definition (immutable, reusable).
 */
typedef struct {
    const animation_frame_t *frames;
    uint8_t frame_count;

    void (*draw_frame)(uint8_t frame_index, void *context);
} animation_def_t;

/*
 * Animation runtime state (mutable).
 */
typedef struct {
    bool active;
    int8_t loop; // -1 for infinite, 0 for default 1 iteration, n > 0 for n iterations
    
    uint8_t current_frame;
    uint8_t current_tick;
    uint8_t current_loop;

    const animation_def_t *def;
    void *context;
} animation_state_t;

/* API */
void animation_start(
    animation_state_t *anim,
    const animation_def_t *def,
    void *context,
    int8_t loop
);

void animation_stop(animation_state_t *anim);

void animation_tick(animation_state_t *anim);

/*
 * Draws current frame.
 * Returns true if an animation was drawn.
 */
bool animation_draw(animation_state_t *anim);
