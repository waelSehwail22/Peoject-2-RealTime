#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "ipc.h"
#include "shared.h"
#include "config.h"

int main() {
    SharedData *shm = ipc_init();
    if (!shm) return 1;
    printf("✅ ipc_init OK\n");

    sem_lock();
    shm->running  = 1;
    shm->shutdown = 0;
    for (int d = 0; d < NUM_DIRECTIONS; d++) shm->vehicle_count[d] = 0;
    sem_unlock();

    /* شغّل controller */
    pid_t ctrl = fork();
    if (ctrl == 0) {
        execl("./controller", "./controller", NULL);
        perror("execl controller"); return 1;
    }
    printf("✅ controller started (pid=%d)\n", ctrl);

    /* انتظر N-S GREEN يبدأ */
    sleep(2);
    sem_lock();
    int ph = shm->current_phase;
    int ln = shm->light[NORTH];
    sem_unlock();
    printf("%s Phase after start: %s\n",
           ph == PHASE_NS_GREEN ? "✅" : "❌", phase_str(ph));
    printf("%s NORTH light: %s\n",
           ln == GREEN ? "✅" : "❌", light_str(ln));

    /* بعث pedestrian request */
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype  = MSG_PEDESTRIAN;
    m.source = SRC_PEDESTRIAN;
    m.value  = 1;
    msg_send(&m);
    printf("✅ Pedestrian request sent\n");

    sleep(5);
    sem_lock();
    int ped = shm->pedestrian_active;
    sem_unlock();
    printf("%s Pedestrian active: %d\n", ped ? "✅" : "⚠️ ", ped);

    /* بعث emergency */
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_EMERGENCY;
    m.source    = SRC_EMERGENCY;
    m.direction = SOUTH;
    m.priority  = 1;
    msg_send(&m);
    printf("✅ Emergency request sent (SOUTH)\n");

    sleep(5);
    sem_lock();
    int emrg = shm->emergency_mode;
    int edir = shm->emergency_direction;
    int ls   = shm->light[SOUTH];
    sem_unlock();
    printf("%s Emergency mode: %d\n", emrg ? "✅" : "⚠️ ", emrg);
    printf("%s Emergency dir: %s\n",
           edir == SOUTH ? "✅" : "❌", dir_str(edir));
    printf("%s SOUTH light: %s\n",
           ls == GREEN ? "✅" : "❌", light_str(ls));

    /* وقف */
    sem_lock(); shm->shutdown = 1; sem_unlock();
    sleep(2);
    kill(ctrl, SIGTERM);
    waitpid(ctrl, NULL, 0);
    printf("✅ controller stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
