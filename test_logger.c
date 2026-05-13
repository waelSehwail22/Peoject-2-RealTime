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
    sem_unlock();

    /* شغّل logger */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./logger", "./logger", NULL);
        perror("execl"); return 1;
    }
    printf("✅ logger started (pid=%d)\n", pid);
    sleep(1);

    /* بعث messages من sources مختلفة */
    Message m;

    memset(&m, 0, sizeof(m));
    m.mtype = MSG_LOG; m.source = SRC_CONTROLLER;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "Controller test message");
    msg_send(&m);
    printf("✅ Sent: CONTROLLER log\n");

    memset(&m, 0, sizeof(m));
    m.mtype = MSG_LOG; m.source = SRC_EMERGENCY;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "Emergency test message");
    msg_send(&m);
    printf("✅ Sent: EMERGENCY log\n");

    memset(&m, 0, sizeof(m));
    m.mtype = MSG_LOG; m.source = SRC_SAFETY;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "*** SAFETY VIOLATION: test ***");
    msg_send(&m);
    printf("✅ Sent: SAFETY log\n");

    memset(&m, 0, sizeof(m));
    m.mtype = MSG_LOG; m.source = SRC_TRAFFIC_LIGHT + NORTH;
    m.timestamp = time(NULL);
    snprintf(m.message, sizeof(m.message), "[LIGHT-NORTH] state → GREEN");
    msg_send(&m);
    printf("✅ Sent: TRAFFIC_LIGHT log\n");

    sleep(1);

    /* تحقق من log.txt */
    printf("\n--- log.txt contents ---\n");
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), f)) {
            printf("%s", line);
            count++;
        }
        fclose(f);
        printf("%s log.txt has %d lines\n", count>0?"✅":"❌", count);
    } else {
        printf("❌ log.txt not found\n");
    }

    /* وقف */
    sem_lock(); shm->shutdown = 1; sem_unlock();
    sleep(1);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    printf("✅ logger stopped\n");

    ipc_destroy();
    printf("✅ Done\n");
    return 0;
}
