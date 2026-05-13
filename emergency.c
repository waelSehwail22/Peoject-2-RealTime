/*
 * emergency.c  —  Anwar Atawna
 * --------------------------------
 * Emergency vehicle detection process.
 *
 * Two operating modes:
 *   interactive (default): enter direction number (0-3) and press ENTER.
 *   automatic   (--auto) : random emergency every 45-120 seconds.
 *
 * IPC decisions:
 *   MSG_EMERGENCY: sent with the approaching direction so the controller
 *   immediately preempts the normal cycle (response ≤ 1 s).
 *
 *   SHM emergency_mode / emergency_direction: set here before sending
 *   the message so the safety monitor detects the emergency state even
 *   if the controller is momentarily busy.
 *
 *   After triggering, this process waits EMERGENCY_DURATION + margin
 *   before allowing the next event (prevents overlapping emergencies).
 */

#include <signal.h>
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

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void trigger_emergency(int dir) {
    /* Set SHM immediately — safety monitor sees it without delay */
    sem_lock();
    g_shm->emergency_mode      = 1;
    g_shm->emergency_direction = dir;
    sem_unlock();

    /* Send message to controller */
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_EMERGENCY;
    m.source    = SRC_EMERGENCY;
    m.direction = dir;
    m.value     = 1;
    m.priority  = 1;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message),
             "EMERGENCY vehicle approaching from %s!", dir_str(dir));
    msg_send(&m);

    /* Log it */
    Message log;
    memset(&log, 0, sizeof(log));
    log.mtype     = MSG_LOG;
    log.source    = SRC_EMERGENCY;
    log.timestamp = time(NULL);
    snprintf(log.message, sizeof(log.message),
             "Emergency vehicle from %s — preempting traffic", dir_str(dir));
    msg_send(&log);

    printf("[EMRG] *** EMERGENCY from %-5s — controller notified ***\n",
           dir_str(dir));
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    int auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[EMRG] ipc_attach failed\n"); return 1; }

    printf("[EMRG] emergency process started (pid=%d)\n", getpid());
    if (auto_mode)
        printf("[EMRG] auto mode — random emergencies every 45-120 s\n");
    else {
        printf("[EMRG] interactive mode\n");
        printf("[EMRG] Enter direction (0=N 1=S 2=E 3=W) then ENTER, -1 to quit\n");
    }
    fflush(stdout);

    if (auto_mode) {
        while (g_running) {
            sem_lock(); int sd = g_shm->shutdown; sem_unlock();
            if (sd) break;

            int wait = 45 + rand() % 75;
            printf("[EMRG] next emergency in %d seconds...\n", wait);
            fflush(stdout);

            for (int t = 0; t < wait && g_running; t++) {
                sleep(1);
                sem_lock(); int sd2 = g_shm->shutdown; sem_unlock();
                if (sd2) { g_running = 0; break; }
            }
            if (!g_running) break;

            trigger_emergency(rand() % NUM_DIRECTIONS);

            /* Wait for the emergency to fully clear before the next one */
            sleep(EMERGENCY_DURATION + ALL_RED_DURATION + 5);
        }
    } else {
        while (g_running) {
            printf("Direction (0-3, -1=quit): ");
            fflush(stdout);

            int dir;
            if (scanf("%d", &dir) != 1) break;
            if (dir == -1) break;

            if (dir < 0 || dir >= NUM_DIRECTIONS) {
                printf("Invalid. Use 0=N 1=S 2=E 3=W\n");
                continue;
            }
            trigger_emergency(dir);
        }
    }

    printf("[EMRG] emergency process shutting down\n");
    ipc_detach(g_shm);
    return 0;
}

