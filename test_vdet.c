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

    /* حالة ابتدائية: NORTH أحمر، SOUTH أخضر */
    sem_lock();
    shm->running      = 1;
    shm->shutdown     = 0;
    shm->light[NORTH] = RED;
    shm->light[SOUTH] = GREEN;
    shm->light[EAST]  = RED;
    shm->light[WEST]  = RED;
    for (int d = 0; d < NUM_DIRECTIONS; d++)
        shm->vehicle_count[d] = 0;
    sem_unlock();
    printf("✅ SHM initial state set\n");

    /* شغّل vehicle_detector */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./vehicle_detector", "./vehicle_detector", NULL);
        perror("execl"); return 1;
    }
    printf("✅ vehicle_detector started (pid=%d)\n", pid);

    /* راقب لمدة 5 ثواني */
    sleep(10);

    /* اقرأ النتائج */
    sem_lock();
    int north = shm->vehicle_count[NORTH];
    int south = shm->vehicle_count[SOUTH];
    int east  = shm->vehicle_count[EAST];
    int west  = shm->vehicle_count[WEST];
    sem_unlock();

    printf("\n--- Vehicle Counts after 5s ---\n");
    printf("NORTH (RED):   %d vehicles\n", north);
    printf("SOUTH (GREEN): %d vehicles\n", south);
    printf("EAST  (RED):   %d vehicles\n", east);
    printf("WEST  (RED):   %d vehicles\n", west);

    /* التحقق: NORTH المفروض يكون >= 0 */
    printf("\n%s NORTH count valid (>= 0)\n", north >= 0 ? "✅" : "❌");
    printf("%s SOUTH count valid (>= 0)\n", south >= 0 ? "✅" : "❌");

    /* وقف */
    sem_lock(); shm->shutdown = 1; sem_unlock();
    sleep(1);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("✅ vehicle_detector stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
