#include <stdio.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <string.h>

typedef struct {
    long mtype;
    char text[64];
} Message;

int main() {
    int msgid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    if (msgid < 0) { perror("msgget failed"); return 1; }
    printf("Queue created: msgid=%d\n", msgid);

    Message m;
    m.mtype = 1;
    strcpy(m.text, "GREEN");
    msgsnd(msgid, &m, sizeof(m.text), 0);
    printf("Sent: '%s'\n", m.text);

    Message recv;
    msgrcv(msgid, &recv, sizeof(recv.text), 1, 0);
    printf("Received: '%s'\n", recv.text);

    msgctl(msgid, IPC_RMID, NULL);
    printf("Queue deleted cleanly\n");
    return 0;
}
