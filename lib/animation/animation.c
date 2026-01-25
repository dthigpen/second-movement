#include "animation.h"

void animation_start(
    animation_state_t *anim,
    const animation_def_t *def,
    void *context,
    int8_t loop
) {
    anim->active = true;
    anim->current_frame = 0;
    anim->current_tick = 0;
    anim->current_loop = 0;
    anim->def = def;
    anim->context = context;
    anim->loop = loop;
}

void animation_stop(animation_state_t *anim) {
    anim->active = false;
}

void animation_tick(animation_state_t *anim) {
    if (!anim->active || !anim->def) return;

    anim->current_tick++;

    const animation_frame_t *frame =
        &anim->def->frames[anim->current_frame];

    // after last tick in frame
    if (anim->current_tick >= frame->duration_ticks) {
        anim->current_tick = 0;
        anim->current_frame++;
        // after last frame in animation
        if (anim->current_frame >= anim->def->frame_count) {
            if(anim->loop >= 0) {
                anim->current_loop++;
            }

            // after last finite loop iteration
            if(anim->loop >= 0 && anim->current_loop >= anim->loop) {
                anim->active = false;
            } else {
                // reset animation for next loop iteration
                uint8_t cur = anim->current_loop;
                animation_start(anim, anim->def, anim->context, anim->loop);
                anim->current_loop = cur;
            }
        }
    }
    // printf("animation_tick active=%d tick=%d/%d frame=%d/%d iter=%d/%d\n",anim->active, anim->current_tick, frame->duration_ticks, anim->current_frame, anim->def->frame_count, anim->current_loop, anim->loop);
}

bool animation_draw(animation_state_t *anim) {
    if (!anim->active || !anim->def) return false;

    anim->def->draw_frame(anim->current_frame, anim->context);
    return true;
}

bool animation_handle_event(
    animation_state_t *anim,
    movement_event_t event,
    void *context
) {
    if (!anim->active || !anim->def || !anim->def->handle_event) {
        return false;
    }

    return anim->def->handle_event(anim, event, context);
}