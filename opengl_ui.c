/*
 * opengl_ui.c  —  Anwar Atawna
 * --------------------------------
 * Real-time OpenGL/GLUT visualization of the traffic light system.
 *
 * What is drawn:
 *   - 4-way intersection (grey roads + dashed centre lines).
 *   - One traffic light per direction (3 coloured bulbs).
 *   - ANIMATED vehicles: cars slide toward intersection when GREEN,
 *     decelerate and stop when YELLOW, stay still when RED.
 *   - Status bar: phase + time remaining + vehicle counts.
 *   - Pedestrian crossing: flashing zebra stripes + WALK label.
 *   - Emergency: flashing red overlay + direction label.
 *   - Safety violation: red border + message.
 *
 * Animation design:
 *   g_anim_offset[dir] is a sub-pixel offset (0..CAR_SPACING) that
 *   increases at CAR_SPEED px/s when GREEN and decelerates to 0 when RED.
 *   Each car is drawn at:  stop_line_distance = (i+1)*CAR_SPACING - offset
 *   When that distance ≤ 0 the car has entered the intersection and is
 *   not drawn, giving the illusion of continuous movement.
 *
 * IPC: read-only SHM attach; reads under sem_lock() every timer tick.
 *
 * Compile:  make opengl_ui
 * Run:      ./opengl_ui   (main system must be running first)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <GL/glut.h>

#include "config.h"
#include "ipc.h"
#include "shared.h"

/* ------------------------------------------------------------------ */
/* Window / timing constants                                           */
/* ------------------------------------------------------------------ */
#define WIN_W        900
#define WIN_H        700
#define FPS           20    /* frames per second — smoother animation  */
#define ROAD_W        80.0f /* half-width of each road arm             */

/* ------------------------------------------------------------------ */
/* Car geometry                                                        */
/* ------------------------------------------------------------------ */
#define CAR_LEN      20.0f  /* length along travel direction           */
#define CAR_WID      12.0f  /* width perpendicular to travel           */
#define CAR_GAP       6.0f  /* bumper gap between cars                 */
#define CAR_SPACING  (CAR_LEN + CAR_GAP)   /* 26 px per slot          */
#define CAR_SPEED    40.0f  /* pixels per second when moving           */

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static SharedData  g_snap;
static SharedData *g_shm  = NULL;
static int         g_tick = 0;

/* Per-direction animation offset (0 .. CAR_SPACING, wraps) */
static float g_anim[NUM_DIRECTIONS] = {0.0f, 0.0f, 0.0f, 0.0f};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static void draw_circle(float cx, float cy, float r) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= 32; i++) {
        float a = (float)i * 2.0f * (float)M_PI / 32.0f;
        glVertex2f(cx + r * cosf(a), cy + r * sinf(a));
    }
    glEnd();
}

static void draw_rect(float x, float y, float hw, float hh) {
    glBegin(GL_QUADS);
    glVertex2f(x-hw, y-hh); glVertex2f(x+hw, y-hh);
    glVertex2f(x+hw, y+hh); glVertex2f(x-hw, y+hh);
    glEnd();
}

static void draw_rect_outline(float x, float y, float hw, float hh) {
    glBegin(GL_LINE_LOOP);
    glVertex2f(x-hw, y-hh); glVertex2f(x+hw, y-hh);
    glVertex2f(x+hw, y+hh); glVertex2f(x-hw, y+hh);
    glEnd();
}

static void draw_text(float x, float y, const char *s) {
    glRasterPos2f(x, y);
    for (; *s; s++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *s);
}
static void draw_text18(float x, float y, const char *s) {
    glRasterPos2f(x, y);
    for (; *s; s++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *s);
}

/* ------------------------------------------------------------------ */
/* Traffic light assembly                                              */
/* ------------------------------------------------------------------ */
static void set_bulb_color(int bulb_state, int active) {
    if (!active) { glColor3f(0.15f, 0.15f, 0.15f); return; }
    switch (bulb_state) {
    case RED:    glColor3f(1.0f, 0.05f, 0.05f); break;
    case YELLOW: glColor3f(1.0f, 0.90f, 0.00f); break;
    case GREEN:  glColor3f(0.0f, 0.90f, 0.20f); break;
    default:     glColor3f(0.3f, 0.30f, 0.30f); break;
    }
}

static void draw_traffic_light(float cx, float cy, int dir) {
    int st = g_snap.light[dir];
    float hw = 20.0f, hh = 62.0f, sp = 38.0f, r = 13.0f;

    /* Housing */
    glColor3f(0.12f, 0.12f, 0.12f);
    draw_rect(cx, cy, hw, hh);
    /* Pole */
    glColor3f(0.25f, 0.25f, 0.25f);
    draw_rect(cx, cy - hh - 20.0f, 3.0f, 20.0f);

    set_bulb_color(RED,    st == RED);    draw_circle(cx, cy + sp, r);
    set_bulb_color(YELLOW, st == YELLOW); draw_circle(cx, cy,      r);
    set_bulb_color(GREEN,  st == GREEN);  draw_circle(cx, cy - sp, r);

    /* Direction label */
    glColor3f(1.0f, 1.0f, 1.0f);
    char buf[16];
    snprintf(buf, sizeof(buf), "%s", dir_str(dir));
    draw_text(cx - 12.0f, cy - hh - 15.0f, buf);
    snprintf(buf, sizeof(buf), "Q:%d", g_snap.vehicle_count[dir]);
    glColor3f(1.0f, 1.0f, 0.3f);
    draw_text(cx - 10.0f, cy - hh - 27.0f, buf);
}

/* ------------------------------------------------------------------ */
/* Draw one car (top-down view)                                        */
/* cx,cy  = centre of the car                                         */
/* vert   = 1 if travelling N-S, 0 if E-W                            */
/* idx    = car index (for colour variety)                            */
/* ------------------------------------------------------------------ */
static void draw_car(float cx, float cy, int vert, int idx) {
    float bw = vert ? CAR_WID : CAR_LEN;   /* body half-width  */
    float bh = vert ? CAR_LEN : CAR_WID;   /* body half-height */

    /* Pick a body colour from a small palette */
    static const float palette[8][3] = {
        {0.85f,0.15f,0.15f}, {0.15f,0.40f,0.85f},
        {0.85f,0.70f,0.10f}, {0.85f,0.45f,0.10f},
        {0.30f,0.75f,0.35f}, {0.65f,0.20f,0.80f},
        {0.90f,0.90f,0.90f}, {0.15f,0.65f,0.75f},
    };
    int ci = idx % 8;
    glColor3f(palette[ci][0], palette[ci][1], palette[ci][2]);
    draw_rect(cx, cy, bw/2, bh/2);

    /* Windshield (lighter rectangle) */
    glColor3f(0.7f, 0.85f, 1.0f);
    draw_rect(cx, cy, bw/2 * 0.6f, bh/2 * 0.35f);

    /* Outline */
    glColor3f(0.0f, 0.0f, 0.0f);
    glLineWidth(1.0f);
    draw_rect_outline(cx, cy, bw/2, bh/2);
}

/* ------------------------------------------------------------------ */
/* Draw all cars for one direction with animation                      */
/*                                                                     */
/* The "stop line" is the edge of the intersection box:               */
/*   NORTH: y = cy + ROAD_W   (cars queue upward, positive y)        */
/*   SOUTH: y = cy - ROAD_W   (cars queue downward, negative y)      */
/*   EAST : x = cx + ROAD_W   (cars queue rightward)                 */
/*   WEST : x = cx - ROAD_W   (cars queue leftward)                  */
/*                                                                     */
/* dist = how far a car is from the stop line (always ≥ 0 when       */
/* queued). When GREEN, dist decreases → car moves toward the line.  */
/* When dist ≤ 0 the car has entered the intersection; skip it.      */
/* ------------------------------------------------------------------ */
static void draw_cars(int dir) {
    int count = g_snap.vehicle_count[dir];
    if (count <= 0) return;

    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;
    float off = g_anim[dir];   /* rolling animation offset */

    for (int i = 0; i < count && i < MAX_VEHICLES_PER_DIR; i++) {
        /* Distance from stop line for car i (slot i+1 back in queue) */
        float dist = (float)(i + 1) * CAR_SPACING - off;
        if (dist <= CAR_LEN / 2.0f) continue;  /* entered intersection */

        float x, y;
        switch (dir) {
        case NORTH:
            /* Heading south: queue above intersection, lane left of centre */
            x = cx - ROAD_W / 2.0f;
            y = cy + ROAD_W + dist;
            break;
        case SOUTH:
            /* Heading north: queue below intersection, lane right of centre */
            x = cx + ROAD_W / 2.0f;
            y = cy - ROAD_W - dist;
            break;
        case EAST:
            /* Heading west: queue right of intersection, lane above centre */
            x = cx + ROAD_W + dist;
            y = cy + ROAD_W / 2.0f;
            break;
        case WEST:
            /* Heading east: queue left of intersection, lane below centre */
            x = cx - ROAD_W - dist;
            y = cy - ROAD_W / 2.0f;
            break;
        default: continue;
        }

        int vert = (dir == NORTH || dir == SOUTH);
        draw_car(x, y, vert, i);
    }
}

/* ------------------------------------------------------------------ */
/* Update animation offsets (called every timer tick)                 */
/* ------------------------------------------------------------------ */
static void update_anim(void) {
    float dt = 1.0f / (float)FPS;
    for (int d = 0; d < NUM_DIRECTIONS; d++) {
        if (g_snap.light[d] == GREEN) {
            g_anim[d] += CAR_SPEED * dt;
            if (g_anim[d] >= CAR_SPACING) g_anim[d] -= CAR_SPACING;
        } else {
            /* Decelerate smoothly to 0 */
            if (g_anim[d] > 0.0f) {
                g_anim[d] -= CAR_SPEED * dt * 1.5f;
                if (g_anim[d] < 0.0f) g_anim[d] = 0.0f;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Intersection: roads + lane markings + zebra crossing               */
/* ------------------------------------------------------------------ */
static void draw_intersection(void) {
    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;

    /* Road surface */
    glColor3f(0.30f, 0.30f, 0.30f);
    draw_rect(cx,  WIN_H/2.0f, ROAD_W, WIN_H/2.0f);   /* N-S vertical  */
    draw_rect(WIN_W/2.0f, cy,  WIN_W/2.0f, ROAD_W);   /* E-W horizontal */

    /* Slightly lighter intersection box */
    glColor3f(0.34f, 0.34f, 0.34f);
    draw_rect(cx, cy, ROAD_W, ROAD_W);

    /* Stop lines (white) */
    glColor3f(1.0f, 1.0f, 1.0f);
    glLineWidth(2.5f);
    glBegin(GL_LINES);
    /* NORTH approach stop line */
    glVertex2f(cx - ROAD_W, cy + ROAD_W); glVertex2f(cx, cy + ROAD_W);
    /* SOUTH approach stop line */
    glVertex2f(cx, cy - ROAD_W); glVertex2f(cx + ROAD_W, cy - ROAD_W);
    /* EAST approach stop line */
    glVertex2f(cx + ROAD_W, cy); glVertex2f(cx + ROAD_W, cy + ROAD_W);
    /* WEST approach stop line */
    glVertex2f(cx - ROAD_W, cy - ROAD_W); glVertex2f(cx - ROAD_W, cy);
    glEnd();

    /* Yellow dashed centre lines */
    glColor3f(1.0f, 0.9f, 0.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    for (float y = 0; y < cy - ROAD_W; y += 28.0f) {
        glVertex2f(cx, y); glVertex2f(cx, y + 14.0f);
    }
    for (float y = cy + ROAD_W; y < WIN_H; y += 28.0f) {
        glVertex2f(cx, y); glVertex2f(cx, y + 14.0f);
    }
    for (float x = 0; x < cx - ROAD_W; x += 28.0f) {
        glVertex2f(x, cy); glVertex2f(x + 14.0f, cy);
    }
    for (float x = cx + ROAD_W; x < WIN_W; x += 28.0f) {
        glVertex2f(x, cy); glVertex2f(x + 14.0f, cy);
    }
    glEnd();
    glLineWidth(1.0f);

    /* Pedestrian crossing zebra stripes */
    if (g_snap.pedestrian_active) {
        int blink = (g_tick / (FPS / 2)) % 2;
        float alpha = blink ? 0.85f : 0.45f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        float stripe_h = 10.0f, stripe_gap = 16.0f;
        for (int i = 0; i < 4; i++) {
            float sy = cy - ROAD_W + 8.0f + (float)i * stripe_gap;
            /* West side crossing */
            glBegin(GL_QUADS);
            glVertex2f(cx - ROAD_W - 55, sy);   glVertex2f(cx - ROAD_W, sy);
            glVertex2f(cx - ROAD_W, sy + stripe_h); glVertex2f(cx - ROAD_W - 55, sy + stripe_h);
            glEnd();
            /* East side crossing */
            glBegin(GL_QUADS);
            glVertex2f(cx + ROAD_W, sy);   glVertex2f(cx + ROAD_W + 55, sy);
            glVertex2f(cx + ROAD_W + 55, sy + stripe_h); glVertex2f(cx + ROAD_W, sy + stripe_h);
            glEnd();
        }
        glDisable(GL_BLEND);
    }
}

/* ------------------------------------------------------------------ */
/* Main display callback                                               */
/* ------------------------------------------------------------------ */
static void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);

    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;

    /* Grass background */
    glColor3f(0.13f, 0.24f, 0.13f);
    draw_rect(cx, cy, WIN_W/2.0f, WIN_H/2.0f);

    draw_intersection();

    /* Traffic lights */
    draw_traffic_light(cx,             cy + 215.0f, NORTH);
    draw_traffic_light(cx,             cy - 215.0f, SOUTH);
    draw_traffic_light(cx + 245.0f,    cy,          EAST);
    draw_traffic_light(cx - 245.0f,    cy,          WEST);

    /* Animated vehicle queues */
    draw_cars(NORTH);
    draw_cars(SOUTH);
    draw_cars(EAST);
    draw_cars(WEST);

    /* ---- Status bar ---- */
    glColor3f(0.05f, 0.05f, 0.05f);
    glBegin(GL_QUADS);
    glVertex2f(0, WIN_H-50); glVertex2f(WIN_W, WIN_H-50);
    glVertex2f(WIN_W, WIN_H); glVertex2f(0, WIN_H);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    char status[256];
    snprintf(status, sizeof(status),
             "Phase: %-14s  Time: %ds  |  N=%d  S=%d  E=%d  W=%d vehicles",
             phase_str(g_snap.current_phase),
             g_snap.phase_time_remaining,
             g_snap.vehicle_count[NORTH], g_snap.vehicle_count[SOUTH],
             g_snap.vehicle_count[EAST],  g_snap.vehicle_count[WEST]);
    draw_text18(10.0f, WIN_H - 30.0f, status);

    /* ---- Pedestrian WALK label ---- */
    if (g_snap.pedestrian_active) {
        int blink = (g_tick / (FPS / 2)) % 2;
        if (blink) {
            glColor3f(1.0f, 1.0f, 0.0f);
            draw_text18(cx - 22.0f, cy + 8.0f, "WALK");
        }
    }

    /* ---- Pedestrian request indicator ---- */
    if (g_snap.pedestrian_request && !g_snap.pedestrian_active) {
        glColor3f(1.0f, 0.55f, 0.0f);
        draw_text(cx - 32.0f, cy + 95.0f, "PED REQUEST");
    }

    /* ---- Emergency overlay ---- */
    if (g_snap.emergency_mode) {
        int blink = (g_tick / (FPS / 5)) % 2;
        if (blink) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1.0f, 0.0f, 0.0f, 0.22f);
            draw_rect(cx, cy, WIN_W/2.0f, WIN_H/2.0f);
            glDisable(GL_BLEND);
        }
        glColor3f(1.0f, 0.25f, 0.25f);
        char emsg[64];
        snprintf(emsg, sizeof(emsg), "*** EMERGENCY: %s ***",
                 dir_str(g_snap.emergency_direction));
        draw_text18(cx - 90.0f, WIN_H - 68.0f, emsg);
    }

    /* ---- Safety violation border ---- */
    if (g_snap.safety_violation) {
        glColor3f(1.0f, 0.0f, 0.0f);
        glLineWidth(5.0f);
        draw_rect_outline(cx, (WIN_H - 50.0f) / 2.0f,
                          WIN_W/2.0f - 3.0f, (WIN_H - 50.0f)/2.0f - 3.0f);
        glLineWidth(1.0f);
        glColor3f(1.0f, 0.3f, 0.3f);
        draw_text18(10.0f, WIN_H - 70.0f, g_snap.safety_msg);
    }

    /* ---- Title ---- */
    glColor3f(0.85f, 0.85f, 0.85f);
    draw_text(5.0f, 8.0f,
              "Traffic Light IPC System ---   [Q/ESC to close]");

    glutSwapBuffers();
}

/* ------------------------------------------------------------------ */
/* Timer callback: update SHM snapshot + animation, schedule redraw   */
/* ------------------------------------------------------------------ */
static void timer_cb(int val) {
    (void)val;
    g_tick++;

    if (g_shm) {
        sem_lock();
        g_snap = *g_shm;
        sem_unlock();

        if (g_snap.shutdown) {
            printf("[GLUI] system shutdown — exiting\n");
            ipc_detach(g_shm);
            exit(0);
        }
    }

    update_anim();          /* advance car animation offsets */
    glutPostRedisplay();
    glutTimerFunc(1000 / FPS, timer_cb, 0);
}

static void keyboard_cb(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 27 || key == 'q' || key == 'Q') {
        if (g_shm) ipc_detach(g_shm);
        exit(0);
    }
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int main(int argc, char *argv[]) {
    g_shm = ipc_attach();
    if (!g_shm) {
        fprintf(stderr, "[GLUI] Cannot attach to SHM — run ./main first\n");
        return 1;
    }
    sem_lock(); g_snap = *g_shm; sem_unlock();

    printf("[GLUI] OpenGL UI started — Q or ESC to close\n");
    fflush(stdout);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(WIN_W, WIN_H);
    glutInitWindowPosition(80, 40);
    glutCreateWindow("Traffic Light IPC System");

    glClearColor(0.13f, 0.24f, 0.13f, 1.0f);
    glEnable(GL_LINE_SMOOTH);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WIN_W, 0, WIN_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glutDisplayFunc(display);
    glutKeyboardFunc(keyboard_cb);
    glutTimerFunc(1000 / FPS, timer_cb, 0);
    glutMainLoop();
    return 0;
}