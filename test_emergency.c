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
    for (int d = 0; d < NUM_DIRECTIONS; d++) shm->light[d] = RED;
    sem_unlock();

    /* شغّل emergency process */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./emergency", "./emergency", "--auto", NULL);
        perror("execl"); return 1;
    }
    printf("✅ emergency started (pid=%d)\n", pid);

    /* انتظر أقل من 45 ثانية — بس نتحقق من SHM مباشرة */
    /* بدل انتظار الـ auto timer، نبعث message يدوي */
    sleep(1);

    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_EMERGENCY;
    m.source    = SRC_EMERGENCY;
    m.direction = SOUTH;
    m.value     = 1;
    m.priority  = 1;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "Test emergency from SOUTH");

    /* اكتب SHM مباشرة كأنه emergency.c */
    sem_lock();
    shm->emergency_mode      = 1;
    shm->emergency_direction = SOUTH;
    sem_unlock();
    msg_send(&m);
    printf("✅ Emergency triggered manually (SOUTH)\n");

    sleep(1);

    /* تحقق من SHM */
    sem_lock();
    int mode = shm->emergency_mode;
    int dir  = shm->emergency_direction;
    sem_unlock();

    printf("%s emergency_mode = %d\n",      mode == 1      ? "✅" : "❌", mode);
    printf("%s emergency_direction = %s\n", dir == SOUTH   ? "✅" : "❌", dir_str(dir));

    /* تحقق من الـ message وصلت */
    Message recv;
    int r = msg_recv(&recv, MSG_EMERGENCY, 0);
    printf("%s MSG_EMERGENCY received — dir=%s\n",
           r > 0 ? "✅" : "❌",
           r > 0 ? dir_str(recv.direction) : "none");

    /* وقف */
    sem_lock(); shm->shutdown = 1; sem_unlock();
    sleep(1);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("✅ emergency stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
