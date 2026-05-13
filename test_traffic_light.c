#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h> 
#include <unistd.h>
#include "ipc.h"
#include "shared.h"
#include "config.h"

int main() {
    SharedData *shm = ipc_init();
    if (!shm) return 1;
    printf("✅ ipc_init OK\n");

    /* اكتب حالة ابتدائية على SHM */
    sem_lock();
    shm->light[NORTH] = GREEN;
    shm->light[SOUTH] = GREEN;
    shm->light[EAST]  = RED;
    shm->light[WEST]  = RED;
    shm->running = 1;
    sem_unlock();
    printf("✅ SHM initial state set\n");

    /* شغّل traffic_light NORTH كـ child process */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./traffic_light", "./traffic_light", "0", NULL);
        perror("execl failed");
        return 1;
    }
    printf("✅ traffic_light NORTH started (pid=%d)\n", pid);

    sleep(1);  /* خلّيه يبدأ */

    /* بعث CMD: NORTH → YELLOW */
    Message m;
    build_cmd_message(&m, NORTH, YELLOW);
    sem_lock();
    shm->light[NORTH] = YELLOW;
    sem_unlock();
    msg_send(&m);
    printf("✅ Sent CMD: NORTH → YELLOW\n");

    sleep(1);

    /* بعث CMD: NORTH → RED */
    build_cmd_message(&m, NORTH, RED);
    sem_lock();
    shm->light[NORTH] = RED;
    sem_unlock();
    msg_send(&m);
    printf("✅ Sent CMD: NORTH → RED\n");

    sleep(1);

    /* تحقق من الـ confirm */
    Message recv;
    int got = msg_recv(&recv, MSG_CONFIRM, 0);
    if (got > 0)
        printf("✅ CONFIRM received: dir=%s state=%s\n",
               dir_str(recv.direction), light_str(recv.value));
    else
        printf("⚠️  No confirm yet (may be in log queue)\n");

    /* اوقف الـ process */
    sem_lock();
    shm->shutdown = 1;
    sem_unlock();
    sleep(1);

    kill(pid, SIGTERM);
    wait(NULL);
    printf("✅ traffic_light NORTH stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
