#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "ipc.h"
#include "shared.h"
#include "config.h"

/* استقبل كل الـ MSG_LOG ودوّر على violations */
static int count_violations(const char *keyword) {
    int count = 0;
    Message m;
    while (msg_recv(&m, MSG_LOG, 0) > 0) {
        if (strstr(m.message, keyword)) count++;
    }
    return count;
}

int main() {
    SharedData *shm = ipc_init();
    if (!shm) return 1;
    printf("✅ ipc_init OK\n");

    sem_lock();
    shm->running = 1; shm->shutdown = 0;
    shm->safety_violation = 0;
    for (int d = 0; d < NUM_DIRECTIONS; d++) shm->light[d] = RED;
    shm->pedestrian_active = 0;
    sem_unlock();

    pid_t pid = fork();
    if (pid == 0) {
        execl("./safety_monitor", "./safety_monitor", NULL);
        perror("execl"); return 1;
    }
    printf("✅ safety_monitor started (pid=%d)\n", pid);
    sleep(1);

    /* Rule 1 */
    printf("\n--- Test Rule 1: conflicting GREEN ---\n");
    sem_lock();
    shm->light[NORTH] = GREEN;
    shm->light[EAST]  = GREEN;
    sem_unlock();
    sleep(1);
    int v = count_violations("N-S and E-W");
    printf("%s Rule 1 violation detected (%d times)\n", v>0?"✅":"❌", v);

    sem_lock();
    shm->light[NORTH] = YELLOW; shm->light[EAST] = YELLOW;
    sem_unlock(); sleep(1);
    sem_lock();
    shm->light[NORTH] = RED; shm->light[EAST] = RED;
    sem_unlock(); sleep(1);

    /* Rule 2 */
    printf("\n--- Test Rule 2: pedestrian + GREEN ---\n");
    sem_lock();
    shm->light[NORTH]      = GREEN;
    shm->pedestrian_active = 1;
    sem_unlock();
    sleep(1);
    v = count_violations("Pedestrian");
    printf("%s Rule 2 violation detected (%d times)\n", v>0?"✅":"❌", v);

    sem_lock();
    shm->light[NORTH]      = YELLOW;
    shm->pedestrian_active = 0;
    sem_unlock(); sleep(1);
    sem_lock();
    shm->light[NORTH] = RED;
    sem_unlock(); sleep(1);

    /* Rule 3a: GREEN→YELLOW→RED صح */
    printf("\n--- Test Rule 3a: GREEN→YELLOW→RED (no violation) ---\n");
    sem_lock(); shm->light[SOUTH] = GREEN; sem_unlock(); sleep(1);
    sem_lock(); shm->light[SOUTH] = YELLOW; sem_unlock(); sleep(1);
    sem_lock(); shm->light[SOUTH] = RED; sem_unlock(); sleep(1);
    v = count_violations("SOUTH went GREEN");
    printf("%s Rule 3a no violation (%d)\n", v==0?"✅":"❌", v);

    /* Rule 3b: GREEN→RED مباشرة */
    printf("\n--- Test Rule 3b: GREEN→RED skip ---\n");
    sem_lock(); shm->light[WEST] = GREEN; sem_unlock(); sleep(1);
    sem_lock(); shm->light[WEST] = RED; sem_unlock(); sleep(1);
    v = count_violations("WEST went GREEN");
    printf("%s Rule 3b violation detected (%d times)\n", v>0?"✅":"❌", v);

    /* وقف */
    sem_lock(); shm->shutdown = 1; sem_unlock();
    sleep(1);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("\n✅ safety_monitor stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
