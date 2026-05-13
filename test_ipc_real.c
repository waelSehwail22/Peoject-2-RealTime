#include <stdio.h>
#include <string.h>
#include "ipc.h"
#include "shared.h"
#include "config.h"

int main() {
    /* 1. init */
    SharedData *shm = ipc_init();
    if (!shm) { fprintf(stderr, "ipc_init failed\n"); return 1; }
    printf("✅ ipc_init OK\n");

    /* 2. كتابة SHM تحت SEM_MAIN */
    sem_lock();
    shm->light[NORTH] = GREEN;
    shm->light[SOUTH] = GREEN;
    shm->light[EAST]  = RED;
    shm->light[WEST]  = RED;
    shm->running = 1;
    sem_unlock();
    printf("✅ SHM write OK\n");

    /* 3. قراءة وتحقق */
    sem_lock();
    int ok = (shm->light[NORTH] == GREEN &&
              shm->light[EAST]  == RED);
    sem_unlock();
    printf("%s SHM read — NORTH=%s EAST=%s\n",
           ok ? "✅" : "❌",
           light_str(shm->light[NORTH]),
           light_str(shm->light[EAST]));

    /* 4. تست كل الـ mtype الموجودة */
    printf("\n--- Message Queue Tests ---\n");
    Message m, recv;

    /* CMD للـ traffic lights */
    int dirs[] = {NORTH, SOUTH, EAST, WEST};
    int states[] = {GREEN, GREEN, RED, RED};
    for (int i = 0; i < 4; i++) {
        build_cmd_message(&m, dirs[i], states[i]);
        msg_send(&m);
        printf("✅ Sent CMD → %s = %s  (mtype=%ld)\n",
               dir_str(dirs[i]), light_str(states[i]), m.mtype);
    }

    /* استقبل بترتيب عكسي — لأثبت إن mtype بيفلتر صح */
    msg_recv(&recv, MSG_CMD_FOR(WEST),  1);
    printf("✅ WEST  got: %s\n", light_str(recv.value));

    msg_recv(&recv, MSG_CMD_FOR(EAST),  1);
    printf("✅ EAST  got: %s\n", light_str(recv.value));

    msg_recv(&recv, MSG_CMD_FOR(SOUTH), 1);
    printf("✅ SOUTH got: %s\n", light_str(recv.value));

    msg_recv(&recv, MSG_CMD_FOR(NORTH), 1);
    printf("✅ NORTH got: %s\n", light_str(recv.value));

    /* تست MSG_PEDESTRIAN */
    memset(&m, 0, sizeof(m));
    m.mtype   = MSG_PEDESTRIAN;
    m.source  = SRC_PEDESTRIAN;
    m.value   = 1;
    msg_send(&m);
    msg_recv(&recv, MSG_PEDESTRIAN, 1);
    printf("✅ PEDESTRIAN msg OK — value=%d\n", recv.value);

    /* تست MSG_EMERGENCY */
    memset(&m, 0, sizeof(m));
    m.mtype     = MSG_EMERGENCY;
    m.source    = SRC_EMERGENCY;
    m.direction = SOUTH;
    m.priority  = 1;
    msg_send(&m);
    msg_recv(&recv, MSG_EMERGENCY, 1);
    printf("✅ EMERGENCY msg OK — dir=%s\n", dir_str(recv.direction));

    /* 5. destroy */
    ipc_destroy();
    printf("\n✅ ipc_destroy OK\n");
    return 0;
}
