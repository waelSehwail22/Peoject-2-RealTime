#ifndef SHARED_H
#define SHARED_H
#include "config.h"
#include <time.h>
typedef struct {
    int light[NUM_DIRECTIONS];
    int current_phase;
    int phase_time_remaining;
    int vehicle_count[NUM_DIRECTIONS];
    int pedestrian_request;
    int pedestrian_active;
    int emergency_mode;
    int emergency_direction;
    int safety_violation;
    char safety_msg[128];
    time_t phase_start_time;
    time_t last_update;
    int controller_pid;
    int running;
    int shutdown;
} SharedData;
#endif /* SHARED_H */
