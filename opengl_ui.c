/*
 * opengl_ui.c  —  Anwar Atawna
 * --------------------------------
 * Real-time OpenGL/GLUT visualization of the traffic light system.
 *
 * What is drawn:
 *   - A 4-way intersection (grey road surface + white lane markings).
 *   - One traffic light pole per direction, each with 3 circles (R/Y/G).
 *     The active light is bright; the inactive lights are dark.
 *   - Vehicle queues: small coloured rectangles at each approach,
 *     one per vehicle in the queue.
 *   - Status bar at the top: current phase + time remaining.
 *   - Pedestrian indicator: flashing "WALK" overlay in the crossing area.
 *   - Emergency alert: full-screen red flash when emergency_mode = 1.
 *   - Safety violation: red border + text when safety_violation = 1.
 *
 * IPC decisions:
 *   Read-only: attaches to SHM, never writes.
 *   Reads under sem_lock() on every GLUT timer tick (every 100 ms).
 *   Never sends messages — pure observer.
 *
 * Compile:
 *   gcc opengl_ui.c ipc.c -o opengl_ui -lGL -lGLU -lglut -lm
 *   (or use: make opengl_ui)
 *
 * Run:  ./opengl_ui   (the main system must already be running)
 */

#include <math.h>
#include <signal.h>
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
/* Window / display parameters                                         */
/* ------------------------------------------------------------------ */
#define WIN_W   900
#define WIN_H   700
#define FPS      10      /* timer ticks per second */

/* ------------------------------------------------------------------ */
/* Snapshot of SHM (filled every timer tick, read by display)         */
/* ------------------------------------------------------------------ */
static SharedData  g_snap;
static SharedData *g_shm     = NULL;
static int         g_tick    = 0;   /* incremented each timer call     */
/* g_running is set by shutdown detection in timer_cb */

/* ------------------------------------------------------------------ */
/* Colour helpers                                                       */
/* ------------------------------------------------------------------ */
static void set_color_for_light(int state, int is_active) {
    if (!is_active) { glColor3f(0.2f, 0.2f, 0.2f); return; }
    switch (state) {
    case RED:    glColor3f(1.0f, 0.0f, 0.0f); break;
    case YELLOW: glColor3f(1.0f, 0.9f, 0.0f); break;
    case GREEN:  glColor3f(0.0f, 0.9f, 0.2f); break;
    default:     glColor3f(0.4f, 0.4f, 0.4f); break;
    }
}

/* ------------------------------------------------------------------ */
/* Draw a filled circle at (cx, cy) with radius r                     */
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

/* ------------------------------------------------------------------ */
/* Draw a string at position (x, y)                                   */
/* ------------------------------------------------------------------ */
static void draw_text(float x, float y, const char *str) {
    glRasterPos2f(x, y);
    for (const char *c = str; *c; c++)
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
}

static void draw_text18(float x, float y, const char *str) {
    glRasterPos2f(x, y);
    for (const char *c = str; *c; c++)
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *c);
}

/* ------------------------------------------------------------------ */
/* Draw a single traffic light assembly                                */
/*                                                                      */
/* cx, cy = centre of the traffic light housing                        */
/* dir    = which direction (determines which state is active)        */
/* ------------------------------------------------------------------ */
static void draw_traffic_light(float cx, float cy, int dir) {
    int state = g_snap.light[dir];

    /* Housing (dark grey box) */
    float hw = 22.0f, hh = 66.0f;
    glColor3f(0.15f, 0.15f, 0.15f);
    glBegin(GL_QUADS);
    glVertex2f(cx - hw, cy - hh);
    glVertex2f(cx + hw, cy - hh);
    glVertex2f(cx + hw, cy + hh);
    glVertex2f(cx - hw, cy + hh);
    glEnd();

    /* Three lights: top=RED, middle=YELLOW, bottom=GREEN */
    float spacing = 40.0f;
    float r = 14.0f;

    /* RED bulb */
    set_color_for_light(RED,    state == RED);
    draw_circle(cx, cy + spacing, r);

    /* YELLOW bulb */
    set_color_for_light(YELLOW, state == YELLOW);
    draw_circle(cx, cy,           r);

    /* GREEN bulb */
    set_color_for_light(GREEN,  state == GREEN);
    draw_circle(cx, cy - spacing, r);

    /* Direction label below housing */
    glColor3f(1.0f, 1.0f, 1.0f);
    char label[16];
    snprintf(label, sizeof(label), "%s", dir_str(dir));
    draw_text(cx - 12.0f, cy - hh - 16.0f, label);

    /* Vehicle count */
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "Q:%d", g_snap.vehicle_count[dir]);
    glColor3f(1.0f, 1.0f, 0.4f);
    draw_text(cx - 12.0f, cy - hh - 28.0f, cnt);
}

/* ------------------------------------------------------------------ */
/* Draw the vehicle queue for a direction as small coloured blocks    */
/* ------------------------------------------------------------------ */
static void draw_vehicle_queue(float start_x, float start_y,
                                float dx, float dy, int dir) {
    int count = g_snap.vehicle_count[dir];
    if (count <= 0) return;

    int state = g_snap.light[dir];
    float r, g, b;
    switch (state) {
    case GREEN:  r=0.0f; g=0.8f; b=0.2f; break;
    case YELLOW: r=0.9f; g=0.8f; b=0.0f; break;
    default:     r=0.8f; g=0.1f; b=0.1f; break;
    }

    for (int i = 0; i < count && i < MAX_VEHICLES_PER_DIR; i++) {
        float x = start_x + dx * (float)i;
        float y = start_y + dy * (float)i;
        glColor3f(r, g, b);
        glBegin(GL_QUADS);
        glVertex2f(x - 7, y - 4);
        glVertex2f(x + 7, y - 4);
        glVertex2f(x + 7, y + 4);
        glVertex2f(x - 7, y + 4);
        glEnd();
        /* Car outline */
        glColor3f(0.0f, 0.0f, 0.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(x - 7, y - 4);
        glVertex2f(x + 7, y - 4);
        glVertex2f(x + 7, y + 4);
        glVertex2f(x - 7, y + 4);
        glEnd();
    }
}

/* ------------------------------------------------------------------ */
/* Draw the intersection (roads + lane markings)                       */
/* ------------------------------------------------------------------ */
static void draw_intersection(void) {
    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;
    float road_w = 80.0f;

    /* Road surface */
    glColor3f(0.3f, 0.3f, 0.3f);
    /* Vertical road (N-S) */
    glBegin(GL_QUADS);
    glVertex2f(cx - road_w, 0);       glVertex2f(cx + road_w, 0);
    glVertex2f(cx + road_w, WIN_H);   glVertex2f(cx - road_w, WIN_H);
    glEnd();
    /* Horizontal road (E-W) */
    glBegin(GL_QUADS);
    glVertex2f(0,       cy - road_w); glVertex2f(WIN_W,    cy - road_w);
    glVertex2f(WIN_W,   cy + road_w); glVertex2f(0,        cy + road_w);
    glEnd();

    /* Centre box */
    glColor3f(0.35f, 0.35f, 0.35f);
    glBegin(GL_QUADS);
    glVertex2f(cx - road_w, cy - road_w);
    glVertex2f(cx + road_w, cy - road_w);
    glVertex2f(cx + road_w, cy + road_w);
    glVertex2f(cx - road_w, cy + road_w);
    glEnd();

    /* White centre lines (dashed effect via segments) */
    glColor3f(1.0f, 1.0f, 0.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    /* N-S centre */
    for (float y = 0; y < cy - road_w; y += 30.0f) {
        glVertex2f(cx, y);
        glVertex2f(cx, y + 15.0f);
    }
    for (float y = cy + road_w; y < WIN_H; y += 30.0f) {
        glVertex2f(cx, y);
        glVertex2f(cx, y + 15.0f);
    }
    /* E-W centre */
    for (float x = 0; x < cx - road_w; x += 30.0f) {
        glVertex2f(x,        cy);
        glVertex2f(x + 15.0f, cy);
    }
    for (float x = cx + road_w; x < WIN_W; x += 30.0f) {
        glVertex2f(x,        cy);
        glVertex2f(x + 15.0f, cy);
    }
    glEnd();
    glLineWidth(1.0f);

    /* Pedestrian crossing stripes (zebra) */
    if (g_snap.pedestrian_active) {
        /* Flash at 1 Hz */
        int blink = (g_tick / (FPS / 2)) % 2;
        if (blink) glColor4f(1.0f, 1.0f, 1.0f, 0.7f);
        else       glColor4f(0.8f, 0.8f, 0.8f, 0.5f);

        for (int i = 0; i < 4; i++) {
            float stripe_y = cy - road_w + 10.0f + (float)i * 18.0f;
            glBegin(GL_QUADS);
            glVertex2f(cx - road_w - 50.0f, stripe_y);
            glVertex2f(cx - road_w,         stripe_y);
            glVertex2f(cx - road_w,         stripe_y + 10.0f);
            glVertex2f(cx - road_w - 50.0f, stripe_y + 10.0f);
            glEnd();
            glBegin(GL_QUADS);
            glVertex2f(cx + road_w,         stripe_y);
            glVertex2f(cx + road_w + 50.0f, stripe_y);
            glVertex2f(cx + road_w + 50.0f, stripe_y + 10.0f);
            glVertex2f(cx + road_w,         stripe_y + 10.0f);
            glEnd();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main display callback                                               */
/* ------------------------------------------------------------------ */
static void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);

    float cx = WIN_W / 2.0f;
    float cy = WIN_H / 2.0f;

    /* --- Background --- */
    glColor3f(0.15f, 0.25f, 0.15f);   /* grass green */
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(WIN_W, 0);
    glVertex2f(WIN_W, WIN_H); glVertex2f(0, WIN_H);
    glEnd();

    /* --- Intersection roads --- */
    draw_intersection();

    /* --- Traffic lights --- */
    /*  NORTH: above intersection   */  draw_traffic_light(cx,        cy + 220.0f, NORTH);
    /*  SOUTH: below intersection   */  draw_traffic_light(cx,        cy - 220.0f, SOUTH);
    /*  EAST:  right of intersection*/  draw_traffic_light(cx + 250.0f, cy,        EAST);
    /*  WEST:  left of intersection */  draw_traffic_light(cx - 250.0f, cy,        WEST);

    /* --- Vehicle queues --- */
    /* NORTH queue: cars lined up coming from north */
    draw_vehicle_queue(cx - 30.0f, cy + 110.0f,  0.0f,  18.0f, NORTH);
    /* SOUTH queue */
    draw_vehicle_queue(cx + 30.0f, cy - 110.0f,  0.0f, -18.0f, SOUTH);
    /* EAST queue */
    draw_vehicle_queue(cx + 110.0f, cy + 20.0f,  18.0f, 0.0f,  EAST);
    /* WEST queue */
    draw_vehicle_queue(cx - 110.0f, cy - 20.0f, -18.0f, 0.0f,  WEST);

    /* --- Status bar at top --- */
    glColor3f(0.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, WIN_H - 50); glVertex2f(WIN_W, WIN_H - 50);
    glVertex2f(WIN_W, WIN_H);  glVertex2f(0, WIN_H);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    char status[256];
    snprintf(status, sizeof(status),
             "Phase: %-14s  Time remaining: %ds  |  "
             "Veh: N=%d S=%d E=%d W=%d",
             phase_str(g_snap.current_phase),
             g_snap.phase_time_remaining,
             g_snap.vehicle_count[NORTH],
             g_snap.vehicle_count[SOUTH],
             g_snap.vehicle_count[EAST],
             g_snap.vehicle_count[WEST]);
    draw_text18(10.0f, WIN_H - 30.0f, status);

    /* --- Pedestrian WALK label in crossing area --- */
    if (g_snap.pedestrian_active) {
        int blink = (g_tick / (FPS / 2)) % 2;
        if (blink) {
            glColor3f(1.0f, 1.0f, 0.0f);
            draw_text18(cx - 25.0f, cy + 5.0f, "WALK");
        }
    }

    /* --- Pedestrian request pending indicator --- */
    if (g_snap.pedestrian_request && !g_snap.pedestrian_active) {
        glColor3f(1.0f, 0.6f, 0.0f);
        draw_text(cx - 30.0f, cy + 110.0f, "PED REQUEST");
    }

    /* --- Emergency overlay --- */
    if (g_snap.emergency_mode) {
        int blink = (g_tick / (FPS / 4)) % 2;
        if (blink) {
            glColor4f(1.0f, 0.0f, 0.0f, 0.25f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBegin(GL_QUADS);
            glVertex2f(0, 0); glVertex2f(WIN_W, 0);
            glVertex2f(WIN_W, WIN_H); glVertex2f(0, WIN_H);
            glEnd();
            glDisable(GL_BLEND);
        }
        glColor3f(1.0f, 0.2f, 0.2f);
        char emsg[64];
        snprintf(emsg, sizeof(emsg), "EMERGENCY: %s",
                 dir_str(g_snap.emergency_direction));
        draw_text18(cx - 60.0f, WIN_H - 70.0f, emsg);
    }

    /* --- Safety violation border --- */
    if (g_snap.safety_violation) {
        glColor3f(1.0f, 0.0f, 0.0f);
        glLineWidth(6.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(3, 3); glVertex2f(WIN_W - 3, 3);
        glVertex2f(WIN_W - 3, WIN_H - 55); glVertex2f(3, WIN_H - 55);
        glEnd();
        glLineWidth(1.0f);
        glColor3f(1.0f, 0.3f, 0.3f);
        draw_text18(10.0f, WIN_H - 70.0f, g_snap.safety_msg);
    }

    /* --- Title --- */
    glColor3f(0.9f, 0.9f, 0.9f);
    draw_text(5.0f, 10.0f, "Traffic Light IPC System — Anwar Atawna  [Ctrl+C to stop]");

    glutSwapBuffers();
}

/* ------------------------------------------------------------------ */
/* Timer: refresh SHM snapshot and schedule redraw                    */
/* ------------------------------------------------------------------ */
static void timer_cb(int val) {
    (void)val;
    g_tick++;

    if (g_shm) {
        sem_lock();
        g_snap = *g_shm;
        sem_unlock();

        if (g_snap.shutdown) {
            printf("[GLUI] system shutdown detected — exiting\n");
            ipc_detach(g_shm);
            exit(0);
        }
    }

    glutPostRedisplay();
    glutTimerFunc(1000 / FPS, timer_cb, 0);
}

/* ------------------------------------------------------------------ */
/* Keyboard callback                                                   */
/* ------------------------------------------------------------------ */
static void keyboard_cb(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 27 || key == 'q' || key == 'Q') {
        printf("[GLUI] quit\n");
        if (g_shm) ipc_detach(g_shm);
        exit(0);
    }
}

/* ================================================================== */
/* main                                                                 */
/* ================================================================== */
int main(int argc, char *argv[]) {
    /* Attach to IPC (system must be running first) */
    g_shm = ipc_attach();
    if (!g_shm) {
        fprintf(stderr, "[GLUI] Could not attach to SHM.\n"
                        "[GLUI] Start the system with ./main first.\n");
        return 1;
    }

    /* Take initial snapshot */
    sem_lock(); g_snap = *g_shm; sem_unlock();

    printf("[GLUI] OpenGL UI started — press Q or ESC to close\n");
    fflush(stdout);

    /* GLUT initialisation */
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(WIN_W, WIN_H);
    glutInitWindowPosition(100, 50);
    glutCreateWindow("Traffic Light IPC System — Anwar Atawna");

    glClearColor(0.15f, 0.25f, 0.15f, 1.0f);

    /* Orthographic 2-D projection matching the window size */
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
