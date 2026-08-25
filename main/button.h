#pragma once

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS,
} button_event_t;

void button_init(void);

/* Call periodically (e.g. every 20-50ms) -- timing is wall-clock based, but
 * a press shorter than the polling interval can be missed entirely.
 * Edge-triggered: each event fires once per press. */
button_event_t button_poll(void);
