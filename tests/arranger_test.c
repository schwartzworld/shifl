/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../firmware/src/arranger.h"

#define SET_ENTRY(e,sc,ba) do { uint32_t _k; for(_k=0;_k<ARR_TRACKS;_k++) (e)->track[_k]=(uint8_t)(sc); (e)->bars=(ba); } while(0)

int main(void)
{
    arr_config_t c;
    arr_clock_t r;
    unsigned long samples;
    unsigned transitions = 0;
    int event;
    arr_defaults(&c);
    assert(!arr_valid(&c, 7));                 /* entry[3] needs slot 3 (bit 3), missing from ready=7 */
    assert(arr_begin(&r, &c, 7) == ARR_INVALID && !r.running);
    c.count = 3;
    SET_ENTRY(&c.entry[0], 0, 1);
    SET_ENTRY(&c.entry[1], 2, 2);
    SET_ENTRY(&c.entry[2], 0, 1);             /* repeated scenes are valid */
    assert(arr_begin(&r, &c, 5) == 0);        /* returns first entry index (0) */
    /* 137 BPM has a fractional samples-per-bar: transitions must never drift
     * by more than one 32-sample audio block, even over multiple sections. */
    for (samples = 0; r.running; samples += 32) {
        event = arr_next(&r, &c, 44118);
        if (event != ARR_NONE) {
            unsigned bars = transitions == 0 ? 1 : transitions == 1 ? 3 : 4;
            double expected = 44118.0 * 240.0 * bars / 137.0;
            assert(samples >= expected && samples - expected < 32.0);
            /* arr_next now returns entry index (1, 2) not scene index (2, 0) */
            assert(event == (transitions == 0 ? 1 : transitions == 1 ? 2 : ARR_DONE));
            transitions++;
        }
        arr_elapse(&r, 32, 137);
    }
    assert(transitions == 3);
    c.count = 1; c.entry[0].bars = 1; c.loop = 1;
    assert(arr_begin(&r, &c, 1) == 0);
    arr_elapse(&r, 44118, 120);                /* half bar at 120 BPM */
    assert(arr_next(&r, &c, 44118) == ARR_NONE);
    arr_elapse(&r, 88236, 60);                 /* tempo change preserves musical position */
    assert(arr_next(&r, &c, 44118) == 0 && r.running);   /* loop wraps: entry index 0 */
    c.entry[0].bars = 0; assert(!arr_valid(&c, 15));
    c.entry[0].bars = 65; assert(!arr_valid(&c, 15));
    c.entry[0].bars = 1; c.entry[0].track[0] = ARR_MUTE + 1u; assert(!arr_valid(&c, 15));
    c.entry[0].track[0] = 0; c.count = 17; assert(!arr_valid(&c, 15));
    puts("arranger: order, repeats, stop, loop, tempo change, fractional timing and invalid scenes PASS");

    /* Per-track: each track independently picks a scene or mutes */
    {
        uint32_t k;
        arr_config_t pt; arr_clock_t pr; int ev;
        arr_defaults(&pt);
        pt.count = 1; pt.loop = 0;
        pt.entry[0].bars = 1;
        pt.entry[0].track[0] = 0;          /* T1 -> A */
        pt.entry[0].track[1] = 1;          /* T2 -> B */
        pt.entry[0].track[2] = 2;          /* T3 -> C */
        pt.entry[0].track[3] = ARR_MUTE;   /* T4 muted: no slot needed */
        assert(arr_valid(&pt, 7));          /* slots A,B,C ready (bits 0,1,2); T4 muted needs nothing */
        assert(!arr_valid(&pt, 3));         /* slot C (bit 2) missing -> T3 invalid */
        assert(arr_begin(&pr, &pt, 7) == 0);
        arr_elapse(&pr, 44100u * 240u, 120);   /* one full bar at 120 BPM */
        ev = arr_next(&pr, &pt, 44100);
        assert(ev == ARR_DONE);

        /* All-muted entry is valid with any ready mask (no slots required) */
        for (k = 0; k < ARR_TRACKS; k++) pt.entry[0].track[k] = ARR_MUTE;
        assert(arr_valid(&pt, 0));

        /* Track scene value out of range is rejected */
        pt.entry[0].track[2] = ARR_SCENES + 1u;
        assert(!arr_valid(&pt, 15));
    }
    puts("arranger: per-track scenes, mute, independent validation PASS");
    return 0;
}
