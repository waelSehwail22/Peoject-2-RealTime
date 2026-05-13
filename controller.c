/*
 * controller.c  —  Anwar Atawna
 * ------------------------------
 * The "brain" of the traffic light system.
 *
 * Normal cycle (fixed sequence, times are adaptive):
 *   PHASE_NS_GREEN  → PHASE_NS_YELLOW → PHASE_ALL_RED_1 →
 *   PHASE_EW_GREEN  → PHASE_EW_YELLOW → PHASE_ALL_RED_2 → (repeat)
 *
 * Interrupts (highest-priority first):
 *   EMERGENCY  — preempts any phase; clears intersection via YELLOW+ALL-RED
 *                before granting green to the emergency direction.
 *                After emergency ends: YELLOW → ALL-RED → NS-GREEN.
 *   PEDESTRIAN — preempts GREEN phases; goes YELLOW→ALL-RED→PEDESTRIAN.
 *
 * IPC decisions:
 *   SHM        : controller is the ONLY process that writes light[] and
 *                phase fields. All writes happen under sem_lock().
 *   MSG_CMD_FOR: after writing SHM, send a directed message to each
 *                traffic_light process (mtype 11-14) for confirmation.
 *   MSG_PEDESTRIAN / MSG_EMERGENCY:
 *                non-blocking receive in check_msgs(), called every
 *                second in the green loop → response ≤ 1 second.
 *   MSG_LOG    : log every phase transition and special event.
 *
 * Adaptive green:
 *   duration = GREEN_DURATION + vehicle_count × GREEN_TIME_PER_VEHICLE
 *   clamped to [MIN_GREEN_DURATION, MAX_GREEN_DURATION].
 *
 * Safety guarantee:
 *   safe_to_allred() always inserts YELLOW before ALL-RED.
 *   emergency_to_allred() inserts YELLOW after emergency GREEN.
 *   GREEN → RED without YELLOW is structurally impossible.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData            *g_shm    = NULL;
static volatile sig_atomic_t  g_running = 1;

/* Pending special-event flags (written by check_msgs, read by main loop) */
static int g_want_pedestrian = 0;
static int g_want_emergency  = 0;
static int g_emergency_dir   = -1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Logging helper (printf + MSG_LOG to logger process)                 */
/* ------------------------------------------------------------------ */
static void log_event(const char *fmt, ...) {
    Message m;
    va_list ap;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_LOG;
    m.source    = SRC_CONTROLLER;
    m.timestamp = time(NULL);
    va_start(ap, fmt);
    vsnprintf(m.message, sizeof(m.message), fmt, ap);
    va_end(ap);
    msg_send(&m);
    printf("[CTRL] %s\n", m.message);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* apply_phase: write SHM + send CMD to all four traffic lights        */
/* ------------------------------------------------------------------ */
static void apply_phase(int phase) {
    sem_lock();
    g_shm->current_phase    = phase;
    g_shm->phase_start_time = time(NULL);
    g_shm->last_update      = time(NULL);

    switch (phase) {
    case PHASE_NS_GREEN:
        g_shm->light[NORTH] = GREEN;  g_shm->light[SOUTH] = GREEN;
        g_shm->light[EAST]  = RED;    g_shm->light[WEST]  = RED;
        g_shm->pedestrian_active = 0; g_shm->emergency_mode = 0;
        break;
    case PHASE_NS_YELLOW:
        g_shm->light[NORTH] = YELLOW; g_shm->light[SOUTH] = YELLOW;
        g_shm->light[EAST]  = RED;    g_shm->light[WEST]  = RED;
        break;
    case PHASE_ALL_RED_1:
    case PHASE_ALL_RED_2:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        break;
    case PHASE_EW_GREEN:
        g_shm->light[NORTH] = RED;    g_shm->light[SOUTH] = RED;
        g_shm->light[EAST]  = GREEN;  g_shm->light[WEST]  = GREEN;
        g_shm->pedestrian_active = 0; g_shm->emergency_mode = 0;
        break;
    case PHASE_EW_YELLOW:
        g_shm->light[NORTH] = RED;    g_shm->light[SOUTH] = RED;
        g_shm->light[EAST]  = YELLOW; g_shm->light[WEST]  = YELLOW;
        break;
    case PHASE_PEDESTRIAN:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->pedestrian_active  = 1;
        g_shm->pedestrian_request = 0;
        g_shm->emergency_mode     = 0;
        break;
    case PHASE_EMERGENCY:
        for (int d = 0; d < NUM_DIRECTIONS; d++) g_shm->light[d] = RED;
        g_shm->light[g_emergency_dir] = GREEN;
        g_shm->emergency_mode         = 1;
        g_shm->emergency_direction    = g_emergency_dir;
        g_shm->pedestrian_active      = 0;
        g_shm->pedestrian_request     = 0;
        break;
    }

    /* Snapshot light states before releasing the lock */
    int lights[NUM_DIRECTIONS];
    for (int d = 0; d < NUM_DIRECTIONS; d++) lights[d] = g_shm->light[d];
    sem_unlock();

    /* Notify each traffic_light process via its own message type */
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        Message cmd;
        build_cmd_message(&cmd, d, lights[d]);
        msg_send(&cmd);
    }

    log_event("Phase → %s", phase_str(phase));
}

/* ------------------------------------------------------------------ */
/* check_msgs: drain pending requests (non-blocking, call every sec)   */
/* ------------------------------------------------------------------ */
static void check_msgs(void) {
    Message m;

    /* Pedestrian requests */
    while (msg_recv(&m, MSG_PEDESTRIAN, 0) > 0) {
        if (!g_want_pedestrian && !g_want_emergency) {
            g_want_pedestrian = 1;
            log_event("Pedestrian request received — will serve at next opportunity");
            sem_lock();
            g_shm->pedestrian_request = 1;
            sem_unlock();
        }
    }

    /* Emergency requests — highest priority */
    while (msg_recv(&m, MSG_EMERGENCY, 0) > 0) {
        g_want_emergency = 1;
        g_emergency_dir  = m.direction;
        log_event("EMERGENCY vehicle from %s — preempting cycle!",
                  dir_str(m.direction));
    }
}

/* ------------------------------------------------------------------ */
/* Adaptive green duration based on vehicle queue lengths              */
/* ------------------------------------------------------------------ */
static int green_duration(int d1, int d2) {
    sem_lock();
    int n = g_shm->vehicle_count[d1] + g_shm->vehicle_count[d2];
    sem_unlock();
    int t = GREEN_DURATION + n * GREEN_TIME_PER_VEHICLE;
    if (t < MIN_GREEN_DURATION) t = MIN_GREEN_DURATION;
    if (t > MAX_GREEN_DURATION) t = MAX_GREEN_DURATION;
    return t;
}

/* ------------------------------------------------------------------ */
/* safe_to_allred: enforce GREEN→YELLOW→RED rule before every ALL-RED  */
/* ------------------------------------------------------------------ */
static void safe_to_allred(int cur_phase) {
    switch (cur_phase) {
    case PHASE_NS_GREEN:
        apply_phase(PHASE_NS_YELLOW);
        for (int t = 0; t < YELLOW_DURATION && g_running; t++) sleep(1);
        apply_phase(PHASE_ALL_RED_1);
        for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
        break;
    case PHASE_NS_YELLOW:
        apply_phase(PHASE_ALL_RED_1);
        for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
        break;
    case PHASE_EW_GREEN:
        apply_phase(PHASE_EW_YELLOW);
        for (int t = 0; t < YELLOW_DURATION && g_running; t++) sleep(1);
        apply_phase(PHASE_ALL_RED_2);
        for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
        break;
    case PHASE_EW_YELLOW:
        apply_phase(PHASE_ALL_RED_2);
        for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
        break;
    default:
        /* Already ALL-RED or special phase — no action needed */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* emergency_to_allred: YELLOW → ALL-RED after emergency GREEN         */
/* FIX: prevents GREEN→RED without YELLOW (safety violation)           */
/* ------------------------------------------------------------------ */
static void emergency_to_allred(void) {
    /* Set YELLOW only for the emergency direction */
    sem_lock();
    g_shm->light[g_emergency_dir] = YELLOW;
    g_shm->current_phase          = PHASE_NS_YELLOW; /* closest phase */
    g_shm->last_update            = time(NULL);
    sem_unlock();

    /* Notify the traffic_light for that direction */
    Message cmd;
    build_cmd_message(&cmd, g_emergency_dir, YELLOW);
    msg_send(&cmd);
    log_event("Phase → EMERGENCY YELLOW [%s]", dir_str(g_emergency_dir));

    for (int t = 0; t < YELLOW_DURATION && g_running; t++) sleep(1);

    /* Now safe to go ALL-RED */
    apply_phase(PHASE_ALL_RED_1);
    for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
}

/* ------------------------------------------------------------------ */
/* Handle emergency: safe clear → emergency green → yellow → resume   */
/* ------------------------------------------------------------------ */
static void handle_emergency(int *cur) {
    g_want_emergency = 0;
    log_event("Handling EMERGENCY [%s] — clearing intersection",
              dir_str(g_emergency_dir));

    /* Step 1: clear whatever phase is running → ALL-RED */
    safe_to_allred(*cur);
    if (!g_running) return;

    /* Step 2: emergency GREEN for the priority direction */
    apply_phase(PHASE_EMERGENCY);
    *cur = PHASE_EMERGENCY;

    for (int t = 0; t < EMERGENCY_DURATION && g_running; t++) {
        sleep(1);
        check_msgs();   /* a second emergency can be queued here */
    }

    /* Step 3: YELLOW → ALL-RED (prevents GREEN→RED violation) ✅ */
    emergency_to_allred();

    /* Step 4: resume normal cycle from N-S GREEN */
    apply_phase(PHASE_NS_GREEN);
    *cur = PHASE_NS_GREEN;
    log_event("Emergency cleared — resuming normal cycle");
}

/* ------------------------------------------------------------------ */
/* Handle pedestrian: safe clear → walk signal → resume               */
/* ------------------------------------------------------------------ */
static void handle_pedestrian(int *cur) {
    g_want_pedestrian = 0;
    log_event("Handling PEDESTRIAN — clearing intersection");

    safe_to_allred(*cur);
    if (!g_running) return;

    apply_phase(PHASE_PEDESTRIAN);
    *cur = PHASE_PEDESTRIAN;

    for (int t = 0; t < PEDESTRIAN_DURATION && g_running; t++) {
        sleep(1);
        check_msgs();
        if (g_want_emergency) return;   /* emergency interrupts crossing */
    }

    apply_phase(PHASE_ALL_RED_1);
    for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
    apply_phase(PHASE_NS_GREEN);
    *cur = PHASE_NS_GREEN;
    log_event("Pedestrian crossing complete — resuming normal cycle");
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[CTRL] ipc_attach failed\n"); return 1; }

    sem_lock();
    g_shm->controller_pid = getpid();
    g_shm->running        = 1;
    sem_unlock();

    printf("[CTRL] controller started (pid=%d)\n", getpid());
    log_event("Controller started — beginning traffic cycle");

    /* Start with N-S green */
    apply_phase(PHASE_NS_GREEN);
    int cur = PHASE_NS_GREEN;

    while (g_running) {
        sem_lock(); int shutdown = g_shm->shutdown; sem_unlock();
        if (shutdown) break;

        /* Emergency — highest priority */
        if (g_want_emergency) { handle_emergency(&cur); continue; }

        /* Pedestrian — interrupts only during green */
        if (g_want_pedestrian &&
            (cur == PHASE_NS_GREEN || cur == PHASE_EW_GREEN)) {
            handle_pedestrian(&cur);
            continue;
        }

        /* Normal cycle */
        switch (cur) {

        case PHASE_NS_GREEN: {
            int dur = green_duration(NORTH, SOUTH);
            log_event("N-S GREEN  dur=%ds  (N:%d S:%d vehicles)", dur,
                      g_shm->vehicle_count[NORTH], g_shm->vehicle_count[SOUTH]);
            for (int t = 0; t < dur && g_running; t++) {
                sleep(1);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                sem_lock();
                g_shm->phase_time_remaining = dur - t - 1;
                sem_unlock();
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_NS_YELLOW); cur = PHASE_NS_YELLOW;
            }
            break;
        }

        case PHASE_NS_YELLOW:
            for (int t = 0; t < YELLOW_DURATION && g_running; t++) sleep(1);
            apply_phase(PHASE_ALL_RED_1); cur = PHASE_ALL_RED_1;
            break;

        case PHASE_ALL_RED_1:
            for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
            apply_phase(PHASE_EW_GREEN); cur = PHASE_EW_GREEN;
            break;

        case PHASE_EW_GREEN: {
            int dur = green_duration(EAST, WEST);
            log_event("E-W GREEN  dur=%ds  (E:%d W:%d vehicles)", dur,
                      g_shm->vehicle_count[EAST], g_shm->vehicle_count[WEST]);
            for (int t = 0; t < dur && g_running; t++) {
                sleep(1);
                check_msgs();
                if (g_want_emergency || g_want_pedestrian) break;
                sem_lock();
                g_shm->phase_time_remaining = dur - t - 1;
                sem_unlock();
            }
            if (!g_want_emergency && !g_want_pedestrian) {
                apply_phase(PHASE_EW_YELLOW); cur = PHASE_EW_YELLOW;
            }
            break;
        }

        case PHASE_EW_YELLOW:
            for (int t = 0; t < YELLOW_DURATION && g_running; t++) sleep(1);
            apply_phase(PHASE_ALL_RED_2); cur = PHASE_ALL_RED_2;
            break;

        case PHASE_ALL_RED_2:
            for (int t = 0; t < ALL_RED_DURATION && g_running; t++) sleep(1);
            apply_phase(PHASE_NS_GREEN); cur = PHASE_NS_GREEN;
            break;

        default:
            log_event("Unexpected phase %d — recovering", cur);
            apply_phase(PHASE_ALL_RED_1); cur = PHASE_ALL_RED_1;
            break;
        }
    }

    /* All lights to RED on exit */
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        Message cmd;
        sem_lock(); g_shm->light[d] = RED; sem_unlock();
        build_cmd_message(&cmd, d, RED);
        msg_send(&cmd);
    }
    sem_lock(); g_shm->running = 0; sem_unlock();

    log_event("Controller shut down — all lights RED");
    ipc_detach(g_shm);
    return 0;
}

