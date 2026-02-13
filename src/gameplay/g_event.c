/*
 * QUICKEN Engine - Game Event Queue
 *
 * Push/clear game events for client consumption (killfeed, hit confirm, etc.).
 */

#include "g_internal.h"

void g_event_push(game_event_queue_t *queue, const game_event_t *event) {
    if (!queue || !event) return;
    if (queue->count >= MAX_GAME_EVENTS_PER_TICK) return;
    queue->events[queue->count++] = *event;
}

void g_event_clear(game_event_queue_t *queue) {
    if (!queue) return;
    queue->count = 0;
}
