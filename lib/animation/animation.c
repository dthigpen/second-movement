#include "animation.h"

void animation_start(
    animation_state_t *anim,
    const animation_def_t *def,
    void *context
) {
    anim->active = true;
    anim->current_frame = 0;
    anim->current_tick = 0;
    anim->def = def;
    anim->context = context;
}

void animation_stop(animation_state_t *anim) {
    anim->active = false;
}

void animation_tick(animation_state_t *anim) {
    if (!anim->active || !anim->def) return;

    anim->current_tick++;

    const animation_frame_t *frame =
        &anim->def->frames[anim->current_frame];

    if (anim->current_tick >= frame->duration_ticks) {
        anim->current_tick = 0;
        anim->current_frame++;

        if (anim->current_frame >= anim->def->frame_count) {
            anim->active = false;
        }
    }
}

bool animation_draw(animation_state_t *anim) {
    if (!anim->active || !anim->def) return false;

    anim->def->draw_frame(anim->current_frame, anim->context);
    return true;
}
