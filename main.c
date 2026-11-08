/*
 * PORTERO CARRERA - homebrew para PSP (PPSSPP)
 *
 * Controles:
 *   Cruz (X)          Elegir / confirmar
 *   Circulo           Volver / pase filtrado (al tener la pelota)
 *   Cuadrado          Tirarse (el momento del salto importa)
 *   Triangulo         Achicar el angulo (mantener)
 *   Izq / Der         Caminar a lo largo del arco
 *   Start             Pausa
 *
 * Al tener la pelota tras una parada:
 *   Cruz      -> pase corto (a un defensor)
 *   Circulo   -> pase filtrado (por arriba)
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

PSP_MODULE_INFO("PorteroCarrera", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-512);

/* ------------------------------------------------------------------ */
/* Callbacks (salir con el boton Home del emulador/PSP)               */
/* ------------------------------------------------------------------ */
static int exitCallback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callbackThread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setupCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Constantes                                                         */
/* ------------------------------------------------------------------ */
#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define DT (1.0f / 60.0f)

#define SHOTS_PER_MATCH 5
#define SEASON_MATCHES  6
#define WALK_SPEED      1.1f

#define SAVE_DIR   "ms0:/PSP/SAVEDATA/PRTR00001"
#define SAVE_PATH  "ms0:/PSP/SAVEDATA/PRTR00001/DATA.BIN"
#define SAVE_MAGIC 0x31545250u /* 'PRT1' */

#define RGBA(r, g, b, a) ((unsigned int)(((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r)))
#define C_WHITE  RGBA(255, 255, 255, 255)
#define C_YELLOW RGBA(255, 230, 60, 255)
#define C_GREEN  RGBA(90, 255, 120, 255)
#define C_RED    RGBA(255, 80, 80, 255)
#define C_GRAY   RGBA(150, 150, 150, 255)
#define C_DARK   RGBA(0, 0, 0, 170)

static const char *RIVALS[SEASON_MATCHES] = {
    "REAL NORTE", "DEP SUR", "UNION ESTE", "ATL OESTE", "CLUB CENTRO", "RACING PLATA"
};
static const char *DIFF_NAMES[3] = { "FACIL", "NORMAL", "DIFICIL" };
static const float DIFF_DUR[3]   = { 1.5f, 1.25f, 1.0f };   /* segundos de vuelo del tiro */
static const float DIFF_REACH[3] = { 1.2f, 1.0f, 0.85f };   /* alcance del arquero */

/* ------------------------------------------------------------------ */
/* Primitivas de dibujo con GU (sin texturas, por ahora)              */
/* Cuando lleguen las texturas se reemplazan rect()/circle() por      */
/* sprites texturizados.                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned int color;
    float x, y, z;
} Vtx;

static unsigned int __attribute__((aligned(16))) gulist[262144];

static void rect(float x, float y, float w, float h, unsigned int c)
{
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    v[0].color = c; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = c; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static void circle(float cx, float cy, float r, unsigned int c)
{
    const int N = 16;
    Vtx *v = (Vtx *)sceGuGetMemory((N + 2) * sizeof(Vtx));
    v[0].color = c; v[0].x = cx; v[0].y = cy; v[0].z = 0;
    for (int i = 0; i <= N; i++) {
        float a = (float)i * (2.0f * 3.14159265f / (float)N);
        v[i + 1].color = c;
        v[i + 1].x = cx + cosf(a) * r;
        v[i + 1].y = cy + sinf(a) * r;
        v[i + 1].z = 0;
    }
    sceGuDrawArray(GU_TRIANGLE_FAN, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, N + 2, 0, v);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* ------------------------------------------------------------------ */
/* Fuente bitmap 3x5 (mayusculas, numeros y algunos signos)          */
/* ------------------------------------------------------------------ */
static const char *FONT_CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-./! ";
static const unsigned char FONT[42][5] = {
    {0b010,0b101,0b111,0b101,0b101}, /* A */
    {0b110,0b101,0b110,0b101,0b110}, /* B */
    {0b011,0b100,0b100,0b100,0b011}, /* C */
    {0b110,0b101,0b101,0b101,0b110}, /* D */
    {0b111,0b100,0b110,0b100,0b111}, /* E */
    {0b111,0b100,0b110,0b100,0b100}, /* F */
    {0b011,0b100,0b101,0b101,0b011}, /* G */
    {0b101,0b101,0b111,0b101,0b101}, /* H */
    {0b111,0b010,0b010,0b010,0b111}, /* I */
    {0b001,0b001,0b001,0b101,0b010}, /* J */
    {0b101,0b101,0b110,0b101,0b101}, /* K */
    {0b100,0b100,0b100,0b100,0b111}, /* L */
    {0b101,0b111,0b111,0b101,0b101}, /* M */
    {0b101,0b111,0b111,0b111,0b101}, /* N */
    {0b010,0b101,0b101,0b101,0b010}, /* O */
    {0b110,0b101,0b110,0b100,0b100}, /* P */
    {0b010,0b101,0b101,0b110,0b011}, /* Q */
    {0b110,0b101,0b110,0b101,0b101}, /* R */
    {0b011,0b100,0b010,0b001,0b110}, /* S */
    {0b111,0b010,0b010,0b010,0b010}, /* T */
    {0b101,0b101,0b101,0b101,0b111}, /* U */
    {0b101,0b101,0b101,0b101,0b010}, /* V */
    {0b101,0b101,0b111,0b111,0b101}, /* W */
    {0b101,0b101,0b010,0b101,0b101}, /* X */
    {0b101,0b101,0b010,0b010,0b010}, /* Y */
    {0b111,0b001,0b010,0b100,0b111}, /* Z */
    {0b111,0b101,0b101,0b101,0b111}, /* 0 */
    {0b010,0b110,0b010,0b010,0b111}, /* 1 */
    {0b110,0b001,0b010,0b100,0b111}, /* 2 */
    {0b110,0b001,0b010,0b001,0b110}, /* 3 */
    {0b101,0b101,0b111,0b001,0b001}, /* 4 */
    {0b111,0b100,0b110,0b001,0b110}, /* 5 */
    {0b011,0b100,0b110,0b101,0b010}, /* 6 */
    {0b111,0b001,0b010,0b010,0b010}, /* 7 */
    {0b010,0b101,0b010,0b101,0b010}, /* 8 */
    {0b010,0b101,0b011,0b001,0b110}, /* 9 */
    {0b000,0b010,0b000,0b010,0b000}, /* : */
    {0b000,0b000,0b111,0b000,0b000}, /* - */
    {0b000,0b000,0b000,0b000,0b010}, /* . */
    {0b001,0b001,0b010,0b100,0b100}, /* / */
    {0b010,0b010,0b010,0b000,0b010}, /* ! */
    {0b000,0b000,0b000,0b000,0b000}, /* espacio */
};

static int textW(const char *t, int s)
{
    int n = (int)strlen(t);
    return n == 0 ? 0 : n * 4 * s - s;
}

static void drawText(float x, float y, int s, unsigned int c, const char *t)
{
    for (; *t; t++) {
        char ch = *t;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        int idx = 41; /* espacio por defecto */
        for (int i = 0; FONT_CH[i]; i++) {
            if (FONT_CH[i] == ch) { idx = i; break; }
        }
        for (int r = 0; r < 5; r++)
            for (int col = 0; col < 3; col++)
                if ((FONT[idx][r] >> (2 - col)) & 1)
                    rect(x + col * s, y + r * s, (float)s, (float)s, c);
        x += 4 * s;
    }
}

static void drawTextC(float y, int s, unsigned int c, const char *t)
{
    drawText((SCR_W - textW(t, s)) / 2.0f, y, s, c, t);
}

static void drawTextR(float xr, float y, int s, unsigned int c, const char *t)
{
    drawText(xr - textW(t, s), y, s, c, t);
}

/* ------------------------------------------------------------------ */
/* Entrada                                                            */
/* ------------------------------------------------------------------ */
static SceCtrlData pad, prevPad;
#define PRESSED(b) ((pad.Buttons & (b)) && !(prevPad.Buttons & (b)))
#define HELD(b)    (pad.Buttons & (b))

/* ------------------------------------------------------------------ */
/* Datos guardados (save data en ms0:/PSP/SAVEDATA/...)               */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned int magic;
    int version;
    int careerActive;
    int difficulty;
    int season;          /* partidos ya jugados en la carrera (0..6) */
    int wins, draws, losses;
    int points;
    int shotsFaced, saves, goalsConceded;
    int passesOk, passesTotal;
    unsigned int checksum;
} SaveData;

static SaveData sv;

static unsigned int calcSum(const SaveData *d)
{
    const unsigned char *p = (const unsigned char *)d;
    unsigned int s = 0;
    for (size_t i = 0; i < offsetof(SaveData, checksum); i++)
        s = s * 31u + p[i];
    return s;
}

static int saveGame(void)
{
    sv.magic = SAVE_MAGIC;
    sv.version = 1;
    sv.checksum = calcSum(&sv);
    sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);  /* ignorar error si ya existe */
    sceIoMkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_PATH, "wb");
    if (!f) return 0;
    size_t n = fwrite(&sv, sizeof(sv), 1, f);
    fclose(f);
    return n == 1;
}

static int loadGame(void)
{
    SaveData tmp;
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f) return 0;
    size_t n = fread(&tmp, sizeof(tmp), 1, f);
    fclose(f);
    if (n != 1 || tmp.magic != SAVE_MAGIC || tmp.checksum != calcSum(&tmp))
        return 0;
    sv = tmp;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Estado del juego                                                   */
/* ------------------------------------------------------------------ */
enum {
    S_MENU, S_OPTIONS, S_STATS,
    S_INTRO, S_READY, S_FLIGHT, S_RESULT, S_HOLD, S_PASS, /* en partido */
    S_MATCHEND, S_PAUSE
};

enum { P_STAND, P_WALK, P_DIVE, P_PASS_S, P_PASS_F };

static const char *MENU_ITEMS[5] = { "NUEVA CARRERA", "CONTINUAR", "OPCIONES", "ESTADISTICAS", "SALIR" };

static int st = S_MENU;
static float stT = 0.0f;
static int menuSel = 0;
static int pauseSel = 0;
static int pausedFrom = S_INTRO;
static float pausedT = 0.0f;
static int running = 1;

/* partido */
static int mSaves, mGoals, shotNo;
static char msg[32];
static unsigned int msgCol = C_WHITE;

/* tiro */
static float tx;          /* destino del tiro en el arco: -1 (izq) .. 1 (der) */
static int high;          /* tiro alto */
static float shotDur, shotEl;

/* arquero */
static float kx;          /* posicion del arquero: -1..1 */
static float walkT;
static int moving;
static int dove;
static float diveDir;
static float divePressAt; /* cuando se tiro (tiempo dentro del tiro) */
static int shrinking;
static int savedLast;

/* pase */
static int passKind;      /* 0 corto, 1 filtrado */

static void setState(int s)
{
    st = s;
    stT = 0.0f;
}

/* ------------------------------------------------------------------ */
/* Carrera                                                            */
/* ------------------------------------------------------------------ */
static void startShot(void)
{
    static const float TARGETS[6] = { -0.9f, -0.5f, -0.2f, 0.2f, 0.5f, 0.9f };
    tx = TARGETS[rand() % 6] + (float)((rand() % 21) - 10) * 0.01f;
    high = (rand() % 3 == 0);
    shotDur = DIFF_DUR[sv.difficulty];
    shotEl = 0.0f;
    dove = 0;
    shrinking = 0;
    diveDir = 0.0f;
    divePressAt = 0.0f;
    moving = 0;
    setState(S_READY);
}

static void startMatch(void)
{
    shotNo = 0;
    mSaves = 0;
    mGoals = 0;
    kx = 0.0f;
    setState(S_INTRO);
}

static void newCareer(void)
{
    int diff = sv.difficulty;
    memset(&sv, 0, sizeof(sv));
    sv.difficulty = diff;
    sv.careerActive = 1;
    saveGame();
    startMatch();
}

static void endMatch(void)
{
    int pts;
    sv.season++;
    if (mSaves >= 4) {
        sv.wins++;  pts = 3;
        strcpy(msg, "VICTORIA!");   msgCol = C_GREEN;
    } else if (mSaves == 3) {
        sv.draws++; pts = 1;
        strcpy(msg, "EMPATE");      msgCol = C_YELLOW;
    } else {
        sv.losses++; pts = 0;
        strcpy(msg, "DERROTA");     msgCol = C_RED;
    }
    sv.points += pts;
    if (sv.season >= SEASON_MATCHES)
        sv.careerActive = 0;     /* temporada terminada */
    saveGame();
    setState(S_MATCHEND);
}

static void resolveShot(void)
{
    float mul = DIFF_REACH[sv.difficulty];
    float reach = 0.0f;
    float kEff = kx;
    int saved;

    if (dove) {
        /* tiempo entre que se tiro y el contacto de la pelota */
        float tc = shotDur - divePressAt;
        float ext = clampf(2.2f * tc, 0.0f, 0.55f);   /* cuanto llega a estirarse */
        kEff = kx + diveDir * ext;
        if (tc <= 1.0f)
            reach = high ? 0.22f : 0.32f;   /* tiro a tiempo */
        else
            reach = 0.10f;                  /* se tiro demasiado pronto: ya esta en el piso */
    } else {
        if (!high)
            reach = 0.28f + (shrinking ? 0.10f : 0.0f);
        else
            reach = 0.0f;                   /* alto sin tirarse: entra */
    }
    reach *= mul;
    saved = fabsf(tx - kEff) <= reach;

    sv.shotsFaced++;
    shotNo++;
    if (saved) {
        mSaves++;
        sv.saves++;
        savedLast = 1;
        strcpy(msg, "ATAJADA!");
        msgCol = C_GREEN;
        setState(S_HOLD);
    } else {
        mGoals++;
        sv.goalsConceded++;
        savedLast = 0;
        strcpy(msg, "GOL!");
        msgCol = C_RED;
        setState(S_RESULT);
    }
}

static void finishPass(void)
{
    int ok = (rand() % 100) < (passKind == 0 ? 85 : 60);
    sv.passesTotal++;
    if (ok) sv.passesOk++;
    strcpy(msg, ok ? "PASE OK" : "PASE FALLO");
    msgCol = ok ? C_GREEN : C_RED;
    setState(S_RESULT);
}

/* ------------------------------------------------------------------ */
/* Actualizacion                                                      */
/* ------------------------------------------------------------------ */
static void updateMenu(void)
{
    if (PRESSED(PSP_CTRL_UP))   menuSel = (menuSel + 4) % 5;
    if (PRESSED(PSP_CTRL_DOWN)) menuSel = (menuSel + 1) % 5;
    if (PRESSED(PSP_CTRL_CROSS)) {
        switch (menuSel) {
        case 0: newCareer(); break;
        case 1: if (sv.careerActive) startMatch(); break;
        case 2: setState(S_OPTIONS); break;
        case 3: setState(S_STATS); break;
        case 4: running = 0; break;
        }
    }
}

static void updateOptions(void)
{
    if (PRESSED(PSP_CTRL_CROSS)) {
        sv.difficulty = (sv.difficulty + 1) % 3;
        saveGame();
    }
    if (PRESSED(PSP_CTRL_CIRCLE)) setState(S_MENU);
}

static void updateFlight(void)
{
    float mv = 0.0f;
    shotEl += DT;

    if (!dove) {
        if (HELD(PSP_CTRL_LEFT))  mv -= 1.0f;
        if (HELD(PSP_CTRL_RIGHT)) mv += 1.0f;
        kx = clampf(kx + mv * WALK_SPEED * DT, -1.05f, 1.05f);
        shrinking = HELD(PSP_CTRL_TRIANGLE) ? 1 : 0;
    } else {
        shrinking = 0;
    }
    moving = (mv != 0.0f);
    if (moving) walkT += DT;

    if (!dove && PRESSED(PSP_CTRL_SQUARE)) {
        dove = 1;
        divePressAt = shotEl;
        if (HELD(PSP_CTRL_LEFT))       diveDir = -1.0f;
        else if (HELD(PSP_CTRL_RIGHT)) diveDir = 1.0f;
        else                           diveDir = (tx >= kx) ? 1.0f : -1.0f;
    }

    if (shotEl >= shotDur)
        resolveShot();
}

static void updatePause(void)
{
    if (PRESSED(PSP_CTRL_UP))   pauseSel = (pauseSel + 2) % 3;
    if (PRESSED(PSP_CTRL_DOWN)) pauseSel = (pauseSel + 1) % 3;

    if (PRESSED(PSP_CTRL_START)) {
        st = pausedFrom; stT = pausedT;
        return;
    }
    if (PRESSED(PSP_CTRL_CROSS)) {
        if (pauseSel == 0) {
            st = pausedFrom; stT = pausedT;
        } else if (pauseSel == 1) {
            saveGame();
            strcpy(msg, "CARRERA GUARDADA");
            msgCol = C_GREEN;
        } else {
            /* volver al menu: el partido en curso no se guarda, se repite al continuar */
            saveGame();
            setState(S_MENU);
        }
    }
}

static void update(void)
{
    /* Start pausa durante un partido */
    if (st >= S_INTRO && st <= S_PASS && PRESSED(PSP_CTRL_START)) {
        pausedFrom = st;
        pausedT = stT;
        setState(S_PAUSE);
        pauseSel = 0;
        return;
    }

    stT += DT;
    switch (st) {
    case S_MENU:    updateMenu(); break;
    case S_OPTIONS: updateOptions(); break;
    case S_STATS:
        if (PRESSED(PSP_CTRL_CROSS) || PRESSED(PSP_CTRL_CIRCLE)) setState(S_MENU);
        break;
    case S_INTRO:
        if (stT >= 2.0f) startShot();
        break;
    case S_READY:
        if (stT >= 0.8f) { shotEl = 0.0f; setState(S_FLIGHT); }
        break;
    case S_FLIGHT:
        updateFlight();
        break;
    case S_RESULT:
        if (stT >= 1.6f) {
            if (shotNo >= SHOTS_PER_MATCH) endMatch();
            else startShot();
        }
        break;
    case S_HOLD:
        if (PRESSED(PSP_CTRL_CROSS)) {
            passKind = 0; setState(S_PASS);
        } else if (PRESSED(PSP_CTRL_CIRCLE)) {
            passKind = 1; setState(S_PASS);
        } else if (stT >= 2.5f) {
            strcpy(msg, "SIN PASE");
            msgCol = C_GRAY;
            setState(S_RESULT);
        }
        break;
    case S_PASS:
        if (stT >= 0.8f) finishPass();
        break;
    case S_MATCHEND:
        if (PRESSED(PSP_CTRL_CROSS)) {
            if (sv.careerActive) startMatch();
            else setState(S_MENU);
        }
        break;
    case S_PAUSE:
        updatePause();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Dibujo                                                             */
/* ------------------------------------------------------------------ */
static void drawField(void)
{
    rect(0, 0, SCR_W, SCR_H, RGBA(28, 105, 38, 255));
    for (int x = 0; x < SCR_W; x += 60)
        if (((x / 60) & 1) == 0)
            rect((float)x, 0, 60, SCR_H, RGBA(33, 118, 44, 255));
    rect(0, 0, SCR_W, 22, RGBA(18, 18, 30, 255));           /* tribuna */
    rect(40, 200, 400, 2, RGBA(255, 255, 255, 200));        /* linea del area */
    circle(240, 230, 2, C_WHITE);                           /* punto del penal */
}

static void drawGoal(void)
{
    rect(120, 60, 240, 140, RGBA(0, 0, 0, 70));
    for (int x = 120; x <= 360; x += 12)
        rect((float)x, 60, 1, 140, RGBA(255, 255, 255, 70));
    for (int y = 60; y <= 200; y += 12)
        rect(120, (float)y, 240, 1, RGBA(255, 255, 255, 70));
    rect(117, 57, 246, 4, C_WHITE);   /* travesano */
    rect(117, 57, 4, 147, C_WHITE);   /* poste izq */
    rect(359, 57, 4, 147, C_WHITE);   /* poste der */
}

static void drawKeeper(float x, int pose, float dir, float ph)
{
    const unsigned int shirt = RGBA(255, 200, 0, 255);
    const unsigned int pants = RGBA(25, 25, 25, 255);
    const unsigned int glove = RGBA(40, 220, 90, 255);
    const unsigned int skin  = RGBA(240, 190, 140, 255);

    if (pose == P_DIVE) {
        /* cuerpo tendido hacia 'dir' */
        float bx = x + dir * 4.0f;
        float ax = (dir > 0) ? bx + 14 : bx - 36;
        float gx = (dir > 0) ? bx + 34 : bx - 42;
        rect(bx - 14, 170, 28, 11, shirt);                 /* torso */
        rect(ax, 168, 22, 7, shirt);                       /* brazos estirados */
        rect(gx, 166, 8, 11, glove);                       /* guantes */
        circle(bx - dir * 18, 176, 6, skin);               /* cabeza */
        rect(bx - dir * 22 - 7, 180, 14, 7, pants);        /* piernas */
        return;
    }

    float off = (pose == P_WALK) ? sinf(ph) * 4.0f : 0.0f;
    rect(x - 8 + off, 182, 6, 16, pants);                  /* piernas */
    rect(x + 2 - off, 182, 6, 16, pants);
    rect(x - 8, 160, 16, 22, shirt);                       /* torso */
    circle(x, 152, 6, skin);                               /* cabeza */
    rect(x - 14, 160, 6, 20, shirt);                       /* brazo izq */
    rect(x - 14, 178, 6, 6, glove);

    if (pose == P_PASS_S) {
        float s = sinf(ph * 3.14159f);                     /* pase corto: swing lateral */
        rect(x + 8, 160, 6 + 26 * s, 6, shirt);
        rect(x + 14 + 26 * s, 158, 6, 10, glove);
    } else if (pose == P_PASS_F) {
        float s = sinf(ph * 3.14159f);                     /* pase filtrado: brazo arriba */
        rect(x + 8, 160 - 24 * s, 6, 20 + 24 * s, shirt);
        rect(x + 8, 156 - 24 * s, 6, 6, glove);
    } else {
        rect(x + 8, 160, 6, 20, shirt);                    /* brazo der */
        rect(x + 8, 178, 6, 6, glove);
    }
}

static void drawBall(float x, float y, float r)
{
    circle(x + 1, y + 1, r, RGBA(0, 0, 0, 120));
    circle(x, y, r, C_WHITE);
}

static void drawHud(void)
{
    char left[48], right[48];
    rect(0, 0, SCR_W, 22, C_DARK);
    snprintf(left, sizeof(left), "PARTIDO %d  VS %s", sv.season + 1, RIVALS[sv.season]);
    int shotDisp = shotNo + 1;
    if (shotDisp > SHOTS_PER_MATCH) shotDisp = SHOTS_PER_MATCH;
    snprintf(right, sizeof(right), "TIRO %d/%d  PARADAS %d", shotDisp, SHOTS_PER_MATCH, mSaves);
    drawText(6, 7, 2, C_WHITE, left);
    drawTextR(SCR_W - 6, 7, 2, C_WHITE, right);
}

static void renderGame(void)
{
    drawField();
    drawGoal();

    float gx = 240.0f + tx * 120.0f;     /* destino del tiro en pantalla */
    float gy = high ? 80.0f : 185.0f;
    float kscr = 240.0f + kx * 120.0f;   /* arquero en pantalla */
    int pose = P_STAND;
    float dir = 0.0f, ph = 0.0f;
    int showBall = 0;
    float bx = 0, by = 0, br = 0;

    switch (st) {
    case S_INTRO:
    case S_READY:
        showBall = 1; bx = 240; by = 230; br = 4;
        break;
    case S_FLIGHT: {
        float p = clampf(shotEl / shotDur, 0.0f, 1.0f);
        showBall = 1;
        bx = lerpf(240, gx, p);
        by = lerpf(230, gy, p);
        br = 2.0f + 4.0f * p;
        if (dove) {
            pose = P_DIVE;
            dir = diveDir;
            float age = shotEl - divePressAt;
            kscr = 240.0f + (kx + diveDir * clampf(2.2f * age, 0.0f, 0.55f)) * 120.0f;
        } else if (moving) {
            pose = P_WALK;
            ph = walkT * 12.0f;
        }
        break;
    }
    case S_RESULT:
        if (!savedLast) { showBall = 1; bx = gx; by = gy; br = 6; }
        break;
    case S_HOLD:
        showBall = 1; bx = kscr + 8; by = 170; br = 5;
        break;
    case S_PASS: {
        float p = clampf(stT / 0.8f, 0.0f, 1.0f);
        pose = (passKind == 0) ? P_PASS_S : P_PASS_F;
        ph = p;
        float ex = (passKind == 0) ? kscr + 110 : kscr + 30;
        float ey = (passKind == 0) ? 215 : 60;
        showBall = 1;
        bx = lerpf(kscr + 10, ex, p);
        by = lerpf(170, ey, p);
        br = 5;
        break;
    }
    default:
        break;
    }

    drawKeeper(kscr, pose, dir, ph);
    if (showBall) drawBall(bx, by, br);
    drawHud();

    /* textos sobre la escena */
    char buf[48];
    switch (st) {
    case S_INTRO:
        snprintf(buf, sizeof(buf), "PARTIDO %d", sv.season + 1);
        drawTextC(90, 4, C_WHITE, buf);
        snprintf(buf, sizeof(buf), "VS %s", RIVALS[sv.season]);
        drawTextC(140, 3, C_YELLOW, buf);
        drawTextC(200, 2, C_WHITE, "PREPARATE");
        break;
    case S_READY:
        snprintf(buf, sizeof(buf), "TIRO %d DE %d", shotNo + 1, SHOTS_PER_MATCH);
        drawTextC(40, 2, C_YELLOW, buf);
        break;
    case S_FLIGHT:
        if (shrinking) drawText(10, 30, 2, C_YELLOW, "ACHICANDO");
        drawTextC(256, 1, C_WHITE, "IZQ DER: MOVER  CUADRADO: TIRARSE  TRIANGULO: ACHICAR");
        break;
    case S_RESULT:
        drawTextC(96, 4, msgCol, msg);
        break;
    case S_HOLD:
        drawTextC(96, 4, msgCol, msg);
        drawTextC(250, 1, C_WHITE, "CRUZ: PASE CORTO  CIRCULO: PASE FILTRADO");
        break;
    case S_PASS:
        drawTextC(96, 3, C_WHITE, "PASANDO...");
        break;
    default:
        break;
    }
}

static void drawMenu(void)
{
    drawField();
    drawTextC(40, 4, C_YELLOW, "PORTERO CARRERA");
    for (int i = 0; i < 5; i++) {
        float y = 110.0f + i * 26.0f;
        if (i == menuSel)
            rect(120, y - 6, 240, 22, RGBA(0, 0, 0, 150));
        unsigned int c = (i == menuSel) ? C_YELLOW : C_WHITE;
        if (i == 1 && !sv.careerActive) c = C_GRAY;
        drawTextC(y, 2, c, MENU_ITEMS[i]);
    }
    drawTextC(250, 1, C_WHITE, "CRUZ: ELEGIR  ARRIBA/ABAJO: MOVER");
}

static void drawOptions(void)
{
    drawField();
    rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 120));
    drawTextC(60, 3, C_YELLOW, "OPCIONES");
    drawTextC(120, 2, C_WHITE, "DIFICULTAD");
    drawTextC(150, 3, C_GREEN, DIFF_NAMES[sv.difficulty]);
    drawTextC(230, 1, C_WHITE, "CRUZ: CAMBIAR   CIRCULO: VOLVER");
}

static void statRow(float y, const char *label, const char *value)
{
    drawText(60, y, 2, C_WHITE, label);
    drawTextR(420, y, 2, C_YELLOW, value);
}

static void drawStats(void)
{
    char v[16];
    int pct = (sv.shotsFaced > 0) ? (sv.saves * 100 / sv.shotsFaced) : 0;

    drawField();
    rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 150));
    drawTextC(16, 3, C_YELLOW, "ESTADISTICAS");

    float y = 56; const float dy = 19;
    snprintf(v, sizeof(v), "%d", sv.season);         statRow(y, "PARTIDOS JUGADOS", v); y += dy;
    snprintf(v, sizeof(v), "%d", sv.wins);           statRow(y, "VICTORIAS", v);        y += dy;
    snprintf(v, sizeof(v), "%d", sv.draws);          statRow(y, "EMPATES", v);          y += dy;
    snprintf(v, sizeof(v), "%d", sv.losses);         statRow(y, "DERROTAS", v);         y += dy;
    snprintf(v, sizeof(v), "%d", sv.points);         statRow(y, "PUNTOS", v);           y += dy;
    snprintf(v, sizeof(v), "%d", sv.shotsFaced);     statRow(y, "TIROS RECIBIDOS", v);  y += dy;
    snprintf(v, sizeof(v), "%d", sv.saves);          statRow(y, "PARADAS", v);          y += dy;
    snprintf(v, sizeof(v), "%d", sv.goalsConceded);  statRow(y, "GOLES RECIBIDOS", v);  y += dy;
    snprintf(v, sizeof(v), "%d/%d", sv.passesOk, sv.passesTotal); statRow(y, "PASES OK", v); y += dy;
    snprintf(v, sizeof(v), "%d", pct);               statRow(y, "PORC PARADAS", v);     y += dy;

    drawTextC(256, 1, C_WHITE, "CRUZ O CIRCULO: VOLVER");
}

static void drawMatchEnd(void)
{
    char buf[40];
    drawField();
    rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 150));
    drawTextC(40, 3, C_YELLOW, "PARTIDO TERMINADO");
    snprintf(buf, sizeof(buf), "VS %s", RIVALS[sv.season - 1]);
    drawTextC(80, 2, C_WHITE, buf);
    drawTextC(110, 4, msgCol, msg);
    snprintf(buf, sizeof(buf), "PARADAS %d DE %d", mSaves, SHOTS_PER_MATCH);
    drawTextC(160, 2, C_WHITE, buf);
    snprintf(buf, sizeof(buf), "PUNTOS TOTALES %d", sv.points);
    drawTextC(190, 2, C_WHITE, buf);
    if (sv.careerActive)
        drawTextC(240, 1, C_WHITE, "CRUZ: SIGUIENTE PARTIDO");
    else
        drawTextC(240, 1, C_WHITE, "CARRERA TERMINADA  CRUZ: MENU");
}

static void drawPause(void)
{
    rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 170));
    drawTextC(60, 4, C_YELLOW, "PAUSA");
    const char *items[3] = { "CONTINUAR", "GUARDAR", "SALIR AL MENU" };
    for (int i = 0; i < 3; i++) {
        float y = 130.0f + i * 28.0f;
        unsigned int c = (i == pauseSel) ? C_YELLOW : C_WHITE;
        if (i == pauseSel) rect(130, y - 6, 220, 22, RGBA(0, 0, 0, 150));
        drawTextC(y, 2, c, items[i]);
    }
    drawTextC(230, 1, C_GRAY, "EL PARTIDO EN CURSO SE REPITE SI SALIS AL MENU");
}

static void render(void)
{
    switch (st) {
    case S_MENU:      drawMenu(); break;
    case S_OPTIONS:   drawOptions(); break;
    case S_STATS:     drawStats(); break;
    case S_MATCHEND:  drawMatchEnd(); break;
    case S_PAUSE:
        renderGame();
        drawPause();
        break;
    default:
        renderGame();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GU                                                                 */
/* ------------------------------------------------------------------ */
static void initGu(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, gulist);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0x88000, BUF_W);
    sceGuDepthBuffer((void *)0x110000, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void beginFrame(unsigned int clear)
{
    sceGuStart(GU_DIRECT, gulist);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

static void endFrame(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    setupCallbacks();
    initGu();

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    srand(sceKernelGetSystemTimeLow());

    memset(&sv, 0, sizeof(sv));
    sv.difficulty = 1;
    if (!loadGame()) {
        memset(&sv, 0, sizeof(sv));
        sv.difficulty = 1;
    }
    memset(&pad, 0, sizeof(pad));
    memset(&prevPad, 0, sizeof(prevPad));

    while (running) {
        sceCtrlReadBufferPositive(&pad, 1);
        update();

        beginFrame(RGBA(0, 0, 0, 255));
        render();
        endFrame();

        prevPad = pad;
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
