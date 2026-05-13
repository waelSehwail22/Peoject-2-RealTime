/*
 * pedestrian.c  —  Anwar Atawna
 * --------------------------------
 * Pedestrian crossing request process.
 *
 * Two operating modes:
 *   interactive (default): press ENTER in this terminal to request crossing.
 *   automatic (--auto)   : sends a random request every 20–60 seconds.
 *
 * IPC decisions:
 *   MSG_PEDESTRIAN: carries the crossing request to the controller.
 *   SHM pedestrian_request: set here so the safety monitor also sees it.
 *   SHM pedestrian_active : polled to detect when the controller grants
 *                           the walk signal (request fulfilled).
 *
 *   The process accepts only one request at a time — it waits until
 *   pedestrian_active is cleared before accepting a new one.
 *
 * select() on stdin: avoids blocking on fgets() while still allowing
 * the SHM shutdown flag to be checked every 500 ms.
 *
 * Fix: walk_announced flag prevents WALK SIGNAL from printing every 500ms.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

static SharedData            *g_shm    = NULL;
static volatile sig_atomic_t  g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void send_request(void) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_PEDESTRIAN;
    m.source    = SRC_PEDESTRIAN;
    m.direction = -1;
    m.value     = 1;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "Pedestrian crossing request");
    msg_send(&m);

    sem_lock();
    g_shm->pedestrian_request = 1;
    sem_unlock();
}

/* Non-blocking check whether ENTER was pressed */
static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

int main(int argc, char *argv[]) {
    int auto_mode = (argc > 1 && strcmp(argv[1], "--auto") == 0);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    g_shm = ipc_attach();
    if (!g_shm) { fprintf(stderr, "[PED] ipc_attach failed\n"); return 1; }

    printf("[PED] pedestrian process started (pid=%d)\n", getpid());
    if (auto_mode)
        printf("[PED] auto mode — random requests every 20-60 s\n");
    else
        printf("[PED] interactive — press ENTER to request crossing\n");
    fflush(stdout);

    int pending         = 0;  /* 1 = request sent, waiting for walk signal  */
    int walk_announced  = 0;  /* 1 = already printed WALK SIGNAL this cycle */

    while (g_running) {
        sem_lock();
        int shutdown = g_shm->shutdown;
        int req      = g_shm->pedestrian_request;
        int active   = g_shm->pedestrian_active;
        sem_unlock();
        if (shutdown) break;

        /* Detect when a previous request has been fully served */
        if (pending && !req && !active) {
            pending        = 0;
            walk_announced = 0;
            printf("[PED] crossing complete — ready for next request\n");
            fflush(stdout);
        }

        /* Show walk signal ONCE when it becomes active ✅ */
        if (active) {
            if (!walk_announced) {
                printf("[PED] ** WALK SIGNAL ACTIVE **\n");
                fflush(stdout);
                walk_announced = 1;
            }
        } else {
            walk_announced = 0;
        }

        if (!pending) {
            if (auto_mode) {
                /* Automatic mode: sleep 20-60 s then fire */
                int wait = 20 + rand() % 40;
                printf("[PED] next automatic request in %d seconds\n", wait);
                fflush(stdout);
                for (int t = 0; t < wait && g_running; t++) {
                    sleep(1);
                    sem_lock(); int sd = g_shm->shutdown; sem_unlock();
                    if (sd) break;
                }
                if (!g_running) break;
                printf("[PED] sending automatic pedestrian request!\n");
                fflush(stdout);
                send_request();
                pending = 1;
            } else {
                /* Interactive mode: non-blocking key check */
                if (kbhit()) {
                    char buf[64];
                    if (fgets(buf, sizeof(buf), stdin)) {
                        printf("[PED] request sent — waiting for walk signal...\n");
                        fflush(stdout);
                        send_request();
                        pending = 1;
                    }
                }
            }
        }

        usleep(500 * 1000);   /* 500 ms poll interval */
    }

    printf("[PED] pedestrian process shutting down\n");
    ipc_detach(g_shm);
    return 0;
}
