#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "ipc.h"
#include "shared.h"
#include "config.h"

int main() {
    /* شغّل النظام كامل بـ --auto */
    pid_t pid = fork();
    if (pid == 0) {
        char *a[] = {"./main", "--auto", NULL};
        execv("./main", a);
        perror("execv"); return 1;
    }
    printf("✅ System started (main pid=%d)\n", pid);

    /* انتظر 15 ثانية للنظام يشتغل */
    sleep(15);

    /* اتصل بالـ SHM وتحقق */
    SharedData *shm = ipc_attach();
    if (!shm) { printf("❌ ipc_attach failed\n"); return 1; }

    sem_lock();
    int running  = shm->running;
    int phase    = shm->current_phase;
    int north    = shm->light[NORTH];
    int south    = shm->light[SOUTH];
    int east     = shm->light[EAST];
    int west     = shm->light[WEST];
    int safe_vio = shm->safety_violation;
    sem_unlock();

    printf("\n--- System State after 15s ---\n");
    printf("%s running = %d\n",          running  ? "✅" : "❌", running);
    printf("%s phase = %s\n",            "✅", phase_str(phase));
    printf("   NORTH=%s SOUTH=%s EAST=%s WEST=%s\n",
           light_str(north), light_str(south),
           light_str(east),  light_str(west));
    printf("%s No safety violations\n",  !safe_vio ? "✅" : "❌");

    /* تحقق إن N-S و E-W ما كلهم أخضر */
    int ns = (north==GREEN || south==GREEN);
    int ew = (east==GREEN  || west==GREEN);
    printf("%s No conflicting lights\n", !(ns&&ew) ? "✅" : "❌");

    ipc_detach(shm);

    /* وقف النظام */
    kill(pid, SIGINT);
    waitpid(pid, NULL, 0);
    printf("\n✅ System stopped cleanly\n");
    return 0;
}
