/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Mauricio Vásquez Arias, Escuela de Artes y Humanidades, Universidad EAFIT */
/* ENJAMBRE: un enjambre de insectos como agentes que se mueven en un campo, para Felucca (M-VAVE FM-1).
 *
 * Cada parte tiene hasta SW_MAX agentes (en su memoria de motor, engines.c eng_mem). Cada agente es un insecto
 * que estridula: una portadora senoidal (la nota de la voz por una razón del ESPECTRO) pulsada en sílabas y
 * chirridos. Los agentes se reparten entre las voces que suenan; el campo y el enjambre son uno solo por parte,
 * así que las notas de un acorde se mueven juntas.
 *
 * CAMPO (EDIT 1)
 *   ORBITA  1..3 masas (MASA) que giran entre sí; los insectos orbitan por gravedad suavizada. Radio -> altura
 *           (órbitas internas, parciales altos), velocidad -> brillo, vx -> efecto Doppler de unos cents.
 *   CUANTO  una partícula en una caja 2D: superposición de MASA modos propios que evolucionan en el tiempo
 *           (batidos cuánticos). Los insectos muestrean |psi|^2 con un paseo de Metropolis: se juntan donde la
 *           probabilidad es alta. Celda 4x4 de la caja -> altura: la densidad de probabilidad se vuelve armonía.
 *   ATOMO   un núcleo de Coulomb; los insectos son electrones. Capa de Bohr (r = n^2 a0) -> línea espectral,
 *           el sentido del giro elige serie; su brillo es |psi|^2 de la nube cuántica en su posición.
 * COHE  cohesión del enjambre: atracción al centroide, alineación de velocidades y acople de Kuramoto de los
 *       chirridos (los grillos y las luciérnagas se sincronizan así).
 * TEMP  temperatura (ley de Dolbear): ritmo de chirrido, agitación térmica, velocidad del tiempo cuántico.
 * CHIR  0 tono continuo; más: sílabas pulsadas; sobre 64 las sílabas se agrupan en chirridos.
 * COLP  medición al tocar una nota: CUANTO colapsa a un modo (y decohere de vuelta), ORBITA sincroniza los
 *       chirridos (un eclipse), ATOMO hace saltar a los electrones de capa. Más COLP, colapso más largo.
 *
 * Seguridad y carga. Sin float, sin asignación, sin llamadas a bibliotecas, sin divisiones en los bucles de
 * audio (dos por bloque y parte); posiciones y velocidades acotadas (un agente que escapa rebota), todo bucle
 * acotado por SW_MAX. Un regulador por parte (sw_govern) lee la carga del audio del anfitrión (SW_LOAD_Q8, la
 * media de ~93 ms, y SW_FAST_Q8, la última media de búfer) y fija cuántos agentes suenan: sube despacio bajo
 * SW_LO, baja por pasos sobre SW_HI, recorta un cuarto sobre SW_HARD. Felucca descarta voces a partir del 85 %
 * (audio.c shed_check): el regulador vive por debajo, para que nunca llegue a eso. */
#include "sw_tables.h"

#ifndef SW_MAX
#define SW_MAX 128               /* agentes por parte: el techo estático */
#endif
#define SW_MIN 4
#ifndef SW_LOAD_Q8
#define SW_LOAD_Q8() 0u          /* el anfitrión: carga media del audio, 1/256 */
#endif
#ifndef SW_FAST_Q8
#define SW_FAST_Q8() SW_LOAD_Q8()  /* el anfitrión: carga de la última media de búfer, 1/256 */
#endif
#ifndef SW_COUNT
#define SW_COUNT(kind, n) ((void)0)    /* el anfitrión de pruebas cuenta el trabajo (0 audio, 1 física, 2 chirrido) */
#endif
#define SW_LO   (62u * 256u / 100u)    /* por debajo: crece */
#define SW_HI   (72u * 256u / 100u)    /* por encima: decrece */
#define SW_HARD (80u * 256u / 100u)    /* por encima (última media): recorta un cuarto */
#define SW_ONE (1 << 24)         /* posiciones y velocidades Q24, el campo es [-1, 1]^2 */
#define SW_PHYS 4u               /* bloques por paso de física (128 muestras: 344.5 Hz) */
#define SW_INC_MAX 0x60000000u   /* 0.375 FS: por encima la portadora calla (sin aliasing) */

static const char *const N_SW_FIELD[] = {"ORBITA", "CUANTO", "ATOMO"};
enum { SWF_ORBIT, SWF_QUANT, SWF_ATOM };

typedef struct {
    int32_t x, y, vx, vy;        /* Q24 */
    uint32_t ph;                 /* fase de la portadora */
    uint32_t cph;                /* fase del chirrido (8 sílabas por ciclo) */
    int32_t a, tgt;              /* amplitud al final del bloque anterior y la de este, Q15 */
    int16_t off;                 /* altura sobre la nota, 1/16 de semitono (espectro + Doppler) */
    int16_t lvl;                 /* nivel del campo, Q15 */
    int16_t det;                 /* desvío propio del ritmo de chirrido, Q15 de +-1/8 */
    uint8_t idx, pad;
} sw_agent_t;

typedef struct {
    sw_agent_t ag[SW_MAX];
    int32_t mx[3], my[3];        /* las masas (Q24) */
    uint32_t mang;               /* el giro de las masas */
    uint32_t qth[3];             /* las fases de los modos cuánticos */
    int32_t coh;                 /* colapso, Q15 */
    int32_t kc, ks;              /* el parámetro de orden de Kuramoto (media de cos, sen de los chirridos), Q15 */
    int32_t cx, cy, cvx, cvy;    /* centroide y velocidad media */
    int32_t rng;
    uint32_t blocks;
    uint16_t ncap;               /* el regulador: agentes permitidos */
    uint16_t nlive;              /* agentes que suenan o se apagan (los primeros nlive) */
    uint8_t nv;                  /* voces que suenan */
    uint8_t gcool;               /* bloques de espera del regulador */
    uint8_t field, init;
    uint8_t kstar;               /* el modo al que colapsó */
} sw_part_t;
ENG_MEM_FITS("ENJAMBRE", sizeof(sw_part_t));

static sw_part_t *sw_of(const track_t *t) { return (sw_part_t *)ENG_MEM((uint32_t)(t - trk) % NPART); }
static inline uint32_t sw_rnd(sw_part_t *P) { return noise32(&P->rng); }
static inline int32_t sw_rs(sw_part_t *P) { return (int32_t)(sw_rnd(P) >> 16) - 32768; }   /* -32768..32767 */
static inline int32_t sw_cos(uint32_t ph) { return SINE[(ph + 0x40000000u) >> 22]; }
static inline int32_t sw_sin(uint32_t ph) { return SINE[ph >> 22]; }

static uint32_t sw_isqrt(uint32_t v)            /* raíz entera (solo al iniciar órbitas, fuera de los bucles) */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* la gravedad: a = SW_G / 65536 * dx / d^3 por paso (unidades del campo); ~1.2 s por vuelta a r = 0.5 */
#define SW_G 2
/* una órbita circular alrededor del centro, masa gm (en SW_G): v = sqrt(G / r). Fuera de los bucles de audio:
 * solo al cambiar de campo o en un salto cuántico (dos divisiones de 32 bits por agente) */
static void sw_orbit_v(sw_agent_t *a, int32_t gm)
{
    int32_t xs = a->x >> 12, ys = a->y >> 12, ux, uy;         /* Q12 */
    uint32_t r = sw_isqrt((uint32_t)(xs * xs + ys * ys)), v;   /* Q12 */
    if (r < 64u)
        r = 64u;
    v = sw_isqrt(((uint32_t)gm << 24) / r) << 10;             /* Q24: 2^16 sqrt(gm 4096 / r) */
    ux = xs * 4096 / (int32_t)r;
    uy = ys * 4096 / (int32_t)r;
    a->vx = -(((int32_t)(v >> 4) * uy) >> 8);
    a->vy = ((int32_t)(v >> 4) * ux) >> 8;
}

static void sw_seed(sw_part_t *P, const track_t *t)
{
    uint32_t i;
    if (!P->rng)
        P->rng = 0x5EEDC0DE + (int32_t)(t - trk) * 7919;
    for (i = 0; i < SW_MAX; i++) {
        sw_agent_t *a = &P->ag[i];
        uint32_t ang = sw_rnd(P);
        int32_t r = (int32_t)(0x00380000 + (sw_rnd(P) >> 9));   /* 0.22 .. ~0.72 */
        a->x = ((r >> 8) * sw_cos(ang)) >> 7;
        a->y = ((r >> 8) * sw_sin(ang)) >> 7;
        a->ph = sw_rnd(P);
        a->cph = sw_rnd(P);
        a->det = (int16_t)(sw_rs(P) >> 3);
        a->a = a->tgt = 0;
        sw_orbit_v(a, SW_G);
    }
    P->ncap = 32;
    P->init = 1;
}

/* ---------------------------------------------------------------- el regulador --- */
static void sw_govern(sw_part_t *P)
{
    uint32_t L = SW_LOAD_Q8(), F = SW_FAST_Q8();
    if (P->gcool) {
        P->gcool--;
        return;
    }
    if (F > SW_HARD) {                           /* una media de búfer cerca del límite: un cuarto menos ya */
        P->ncap = (uint16_t)(P->ncap - (P->ncap >> 2));
        P->gcool = 4;                            /* espera la siguiente media (4 bloques = 128 muestras) */
    } else if (L > SW_HI) {
        P->ncap = (uint16_t)(P->ncap > 2u ? P->ncap - 2u : 0u);
        P->gcool = 8;
    } else if (L < SW_LO && F < SW_HI && P->ncap < SW_MAX) {
        P->ncap++;                               /* +1 cada 16 bloques: ~86 agentes por segundo */
        P->gcool = 16;
    }
    if (P->ncap < SW_MIN)
        P->ncap = SW_MIN;
}

/* ------------------------------------------------------------- el campo cuántico --- */
static const uint8_t SW_QN[3][2] = {{1, 2}, {2, 2}, {3, 1}};     /* (nx, ny); E = nx^2 + ny^2: 5, 8, 10 */
/* |psi|^2 en (u, v) (Q16 en [0, 1]), Q15; *kd el modo dominante */
static int32_t sw_psi2(const sw_part_t *P, uint32_t u, uint32_t v, uint32_t nm, const int32_t *c, uint32_t *kd)
{
    int32_t re = 0, im = 0, best = -1;
    uint32_t k;
    for (k = 0; k < nm; k++) {
        int32_t f = (sw_sin(u * SW_QN[k][0] << 15) * sw_sin(v * SW_QN[k][1] << 15)) >> 15;   /* sin(nx pi u) sin(ny pi v) */
        int32_t w = (f * c[k]) >> 15, aw = w < 0 ? -w : w;
        re += (w * sw_cos(P->qth[k])) >> 15;
        im -= (w * sw_sin(P->qth[k])) >> 15;
        if (aw > best) {
            best = aw;
            *kd = k;
        }
    }
    re = clamp(re, -65535, 65535) >> 1;
    im = clamp(im, -65535, 65535) >> 1;
    return (re * re + im * im) >> 13;
}
static inline uint32_t sw_u(int32_t x)          /* Q24 [-1, 1] -> Q16 [0, 1) */
{
    return (uint32_t)clamp((x + SW_ONE) >> 9, 0, 65535);
}

/* --------------------------------------------------------------------- la física --- */
static const int32_t SW_RING[15] = {   /* r^2 entre anillos (0.04 .. 1.1, geométricos), Q24 */
    41759, 64962, 101059, 157211, 244565, 380457, 591857, 920721, 1432317, 2228179, 3466260, 5392277, 8388479, 13049511, 20300431};
static inline uint32_t sw_ring(int32_t d2)      /* el anillo de r^2: 0 (dentro) .. 15, búsqueda binaria */
{
    uint32_t lo = 0, hi = 15;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (d2 > SW_RING[mid])
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static void sw_physics(sw_part_t *P, const track_t *t, uint32_t n)
{
    const int16_t *p = t->p;
    uint32_t field = (uint32_t)p[P_E0], nm = (uint32_t)clamp(p[P_E4], 1, 3), i, k;
    int32_t coh = p[P_E2], temp = p[P_E3];
    int32_t qc[3], cq = SW_NORM[nm];
    int32_t sx = 0, sy = 0, svx = 0, svy = 0;               /* sumas en Q16: 128 x 2^17 cabe */
    /* las masas: una al centro; dos o tres giran en torno al centro (sistema binario / triple) */
    P->mang += 0x00200000u + (uint32_t)temp * 0x4000u;
    for (k = 0; k < 3; k++) {
        if (nm == 1 || field == SWF_ATOM) {
            P->mx[k] = P->my[k] = 0;
        } else {
            uint32_t a = P->mang + k * (0xFFFFFFFFu / nm);
            P->mx[k] = sw_cos(a) * 192;                       /* r = 0.375: Q15 x 192 = Q24 */
            P->my[k] = sw_sin(a) * 192;
        }
    }
    /* los modos cuánticos: fase E_k * velocidad; el colapso pesa el modo medido */
    for (k = 0; k < 3; k++) {
        uint32_t e = (uint32_t)(SW_QN[k][0] * SW_QN[k][0] + SW_QN[k][1] * SW_QN[k][1]);
        P->qth[k] += e * (0x00100000u + (uint32_t)temp * 0x6000u);
        qc[k] = (cq * (32767 - P->coh) + (k == P->kstar ? 32767 * P->coh : 0)) >> 15;
    }
    for (i = 0; i < n; i++) {
        sw_agent_t *a = &P->ag[i];
        uint32_t kd = 0;
        if (field == SWF_QUANT) {
            /* Metropolis sobre |psi|^2, con un sesgo al centroide (COHE); el paso crece con TEMP */
            int32_t step = 0x2000 + temp * 0x180;
            int32_t nx = a->x + ((sw_rs(P) * step) >> 10) + (((P->cx - a->x) >> 7) * coh >> 7);
            int32_t ny = a->y + ((sw_rs(P) * step) >> 10) + (((P->cy - a->y) >> 7) * coh >> 7);
            int32_t p0, p1;
            uint32_t kd1 = 0;
            nx = clamp(nx, -SW_ONE + 1, SW_ONE - 1);
            ny = clamp(ny, -SW_ONE + 1, SW_ONE - 1);
            p0 = sw_psi2(P, sw_u(a->x), sw_u(a->y), nm, qc, &kd);
            p1 = sw_psi2(P, sw_u(nx), sw_u(ny), nm, qc, &kd1);
            if ((int32_t)(((sw_rnd(P) >> 17) * (uint32_t)p0) >> 15) <= p1 + 64) {
                a->vx = nx - a->x;
                a->vy = ny - a->y;
                a->x = nx;
                a->y = ny;
                p0 = p1;
            } else
                a->vx = a->vy = 0;
            a->lvl = (int16_t)clamp(p0 * 2, 0, 32767);
            a->idx = (uint8_t)((sw_u(a->x) >> 14) * 4u + (sw_u(a->y) >> 14));     /* la celda 4x4 */
            a->off = 0;
        } else {
            /* ORBITA / ATOMO: gravedad (o Coulomb) suavizada desde 1..3 masas, Euler semiimplícito */
            int32_t ax = 0, ay = 0, d2min = 0x7FFFFFFF, sp;
            uint32_t nmass = field == SWF_ATOM ? 1u : nm;
            for (k = 0; k < nmass; k++) {
                int32_t dx = (P->mx[k] - a->x) >> 12, dy = (P->my[k] - a->y) >> 12;   /* Q12 */
                int32_t d2 = dx * dx + dy * dy;                                        /* Q24 */
                int32_t g = SW_GRAV[d2 >> 19 > 256 ? 256 : d2 >> 19];                  /* Q8 de 1/(d2+e2)^1.5 */
                ax += (dx * g) >> 8;                                                   /* Q12 de dx/d^3 */
                ay += (dy * g) >> 8;
                if (d2 < d2min)
                    d2min = d2;
            }
            a->vx += (ax * SW_G) >> 4;
            a->vy += (ay * SW_G) >> 4;
            /* el enjambre: al centroide y a la velocidad media (COHE); agitación térmica (TEMP) */
            a->vx += ((P->cx - a->x) >> 16) * coh >> 8;
            a->vy += ((P->cy - a->y) >> 16) * coh >> 8;
            a->vx += (sw_rs(P) * temp) >> 12;
            a->vy += (sw_rs(P) * temp) >> 12;
            a->vx = clamp(a->vx, -0x80000, 0x80000);                                   /* |v| < 1/32 por paso */
            a->vy = clamp(a->vy, -0x80000, 0x80000);
            a->x += a->vx;
            a->y += a->vy;
            if (a->x > SW_ONE || a->x < -SW_ONE) {                                     /* rebota en el borde */
                a->x = a->x > 0 ? SW_ONE : -SW_ONE;
                a->vx = -(a->vx >> 1);
            }
            if (a->y > SW_ONE || a->y < -SW_ONE) {
                a->y = a->y > 0 ? SW_ONE : -SW_ONE;
                a->vy = -(a->vy >> 1);
            }
            sp = (a->vx < 0 ? -a->vx : a->vx) + (a->vy < 0 ? -a->vy : a->vy);         /* |v| (norma 1) */
            if (field == SWF_ORBIT) {
                a->idx = (uint8_t)(15u - sw_ring(d2min));                              /* órbita interna: alto */
                a->lvl = (int16_t)clamp(9000 + (sp >> 3), 0, 32767);                   /* rápido: brillante */
                a->off = (int16_t)clamp(a->vx >> 14, -12, 12);                         /* Doppler: +-0.75 st */
            } else {
                /* capas de Bohr r_n = n^2 a0, a0 = 1/80: n = 1..8, el sentido del giro elige serie */
                static const int32_t SH[7] = {     /* r^2 entre las capas n y n+1 (r_n = n^2 / 80), Q24 */
                    16384, 110756, 409600, 1101660, 2438644, 4734976, 8368292};
                int32_t L = (a->x >> 12) * (a->vy >> 12) - (a->y >> 12) * (a->vx >> 12);
                uint32_t sh = 0;
                while (sh < 7u && d2min > SH[sh])
                    sh++;
                a->idx = (uint8_t)(sh + (L > 0 ? 8u : 0u));
                a->lvl = (int16_t)clamp(4000 + sw_psi2(P, sw_u(a->x), sw_u(a->y), nm, qc, &kd) * 2, 0, 32767);
                a->off = 0;
            }
        }
        sx += a->x >> 8;
        sy += a->y >> 8;
        svx += a->vx >> 8;
        svy += a->vy >> 8;
    }
    if (n) {
        P->cx = sx / (int32_t)n * 256;           /* cuatro divisiones por paso de física */
        P->cy = sy / (int32_t)n * 256;
        P->cvx = svx / (int32_t)n * 256;
        P->cvy = svy / (int32_t)n * 256;
    }
}

/* ------------------------------------------------------- los chirridos, por bloque --- */
static void sw_chirp(sw_part_t *P, const track_t *t, uint32_t n)
{
    const int16_t *p = t->p;
    int32_t temp = p[P_E3], chir = p[P_E6], coh = p[P_E2];
    /* Dolbear: más calor, más chirridos. 0.4 .. 6 ciclos/s (x8 sílabas: 3 .. 48 Hz), por bloque de 32 */
    uint32_t base = 1250000u + (uint32_t)temp * 137000u;
    int32_t depth = clamp(chir * 2, 0, 127), kq = (int32_t)(base >> 8);
    uint32_t nsyl = chir > 64 ? (uint32_t)(8 - ((chir - 64) >> 3)) : 8u;   /* sílabas que suenan por chirrido */
    int32_t sc = 0, ss = 0, inv;
    uint32_t i;
    if (nsyl < 2u)
        nsyl = 2u;
    for (i = 0; i < n; i++) {
        sw_agent_t *a = &P->ag[i];
        int32_t c = sw_cos(a->cph), s = sw_sin(a->cph), env;
        uint32_t syl;
        /* Kuramoto: d(theta)/dt = w_i + K R sin(Psi - theta) = w_i + K (S cos - C sin) */
        int32_t pull = (P->ks * c - P->kc * s) >> 15;
        sc += c;
        ss += s;
        a->cph += base + (uint32_t)((int32_t)(base >> 15) * a->det) + (uint32_t)(kq * ((pull * coh) >> 15));
        syl = a->cph >> 29;
        if (syl >= nsyl)
            env = 0;
        else
            env = 32767 - ((depth * (32767 - SW_WIN[(a->cph >> 23) & 63u])) >> 7);
        a->tgt = (env * a->lvl) >> 15;
    }
    for (; i < P->nlive; i++) {                  /* sobre el tope: se apagan en ~3 ms */
        sw_agent_t *a = &P->ag[i];
        a->tgt = a->a - (a->a >> 2);
        if (a->tgt < 64)
            a->tgt = 0;
    }
    inv = n ? 32768 / (int32_t)n : 0;            /* la otra división del bloque */
    P->kc = (sc * inv) >> 15;
    P->ks = (ss * inv) >> 15;
}

static void swarm_block(track_t *t)
{
    sw_part_t *P = sw_of(t);
    const int16_t *p = t->p;
    uint32_t want, n, i, nv = 0;
    if (!P->init)
        sw_seed(P, t);
    if (P->field != (uint8_t)p[P_E0]) {          /* otro campo: las órbitas arrancan circulares */
        P->field = (uint8_t)p[P_E0];
        if (P->field != SWF_QUANT)
            for (i = 0; i < SW_MAX; i++)
                sw_orbit_v(&P->ag[i], SW_G * (P->field == SWF_ORBIT ? clamp(p[P_E4], 1, 3) : 1));
    }
    sw_govern(P);
    want = SW_MIN + (((uint32_t)p[P_E1] * (SW_MAX - SW_MIN)) >> 7);
    n = want < P->ncap ? want : P->ncap;
    for (i = 0; i < NVOICE; i++)
        nv += t->v[i].active;
    P->nv = (uint8_t)(nv ? nv : 1u);
    if (P->blocks++ % SW_PHYS == 0u) {
        sw_physics(P, t, n);
        SW_COUNT(1 + 2 * (p[P_E0] == SWF_QUANT), n);
    }
    sw_chirp(P, t, n);
    SW_COUNT(2, n);
    /* nlive: los primeros n más los que aún se apagan */
    i = P->nlive > n ? P->nlive : n;
    while (i > n && P->ag[i - 1u].a == 0 && P->ag[i - 1u].tgt == 0)
        i--;
    P->nlive = (uint16_t)i;
    if (P->coh)                                  /* la decoherencia: COLP alto, colapso largo (23 ms .. 3 s) */
        P->coh -= (P->coh >> (5 + (p[P_E7] >> 4))) + 1;
    if (P->coh < 0)
        P->coh = 0;
}

static void swarm_note_on(track_t *t, voice_t *v)
{
    sw_part_t *P = sw_of(t);
    int32_t c = t->p[P_E7] * 258;                /* Q15 */
    uint32_t i, n = P->nlive;
    v->s[0] = 0;
    if (!P->init || !c)
        return;
    if (c > P->coh)
        P->coh = c;
    P->kstar = (uint8_t)(sw_rnd(P) % (uint32_t)clamp(t->p[P_E4], 1, 3));   /* la medición: pesos iguales */
    if (P->field == SWF_ORBIT) {                 /* un eclipse: los chirridos se alinean */
        for (i = 0; i < n; i++)
            P->ag[i].cph = (P->ag[i].cph >> 15) * (uint32_t)(32767 - c);
    } else if (P->field == SWF_ATOM) {           /* saltos cuánticos: cada electrón cambia de radio */
        for (i = 0; i < n; i++) {
            sw_agent_t *a = &P->ag[i];
            int32_t f = clamp(32768 + ((sw_rs(P) * c) >> 15), 0, 65000);   /* x 0.0 .. 2.0 */
            a->x = ((a->x >> 10) * f) >> 5;
            a->y = ((a->y >> 10) * f) >> 5;
            a->x = clamp(a->x, -SW_ONE, SW_ONE);
            a->y = clamp(a->y, -SW_ONE, SW_ONE);
            sw_orbit_v(a, SW_G);
        }
    }
}

static void swarm_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    sw_part_t *P = sw_of(t);
    const int16_t *spec = SW_SPEC[clamp(t->p[P_E5], 0, SW_NSPEC - 1)];
    int32_t acc[CTL];
    uint32_t rank = 0, i, k, cnt = 0, nv = P->nv;
    int32_t g;
    for (i = 0; i < NVOICE && &t->v[i] != v; i++)
        rank += t->v[i].active;
    if (nv < rank + 1u)
        nv = rank + 1u;
    for (k = 0; k < n; k++)
        acc[k] = 0;
    for (i = rank; i < P->nlive; i += nv) {
        sw_agent_t *a = &P->ag[i];
        uint32_t inc, ph;
        int32_t amp, da;
        if (!a->a && !a->tgt)
            continue;
        inc = pitch_inc((uint32_t)clamp(m->pitch16 + spec[a->idx & 15u] + a->off, 0, 2047));
        if (inc > SW_INC_MAX) {                  /* sobre 0.375 FS: calla */
            a->a = a->tgt = 0;
            continue;
        }
        amp = a->a << 8;                         /* Q23, rampa de a a tgt a lo largo del bloque */
        da = ((a->tgt - a->a) * 256) >> CTL_LOG2;
        ph = a->ph;
        for (k = 0; k < n; k++) {
            ph += inc;
            amp += da;
            acc[k] += (SINE[ph >> 22] * (amp >> 8)) >> 15;
        }
        a->ph = ph;
        a->a = a->tgt;
        cnt++;
        SW_COUNT(0, n);
    }
    if (!cnt)
        return;
    g = (SW_NORM[cnt > 64u ? 64u : cnt] * 3) >> 5;   /* 1.5 / sqrt(n), Q11 */
    for (k = 0; k < n; k++)                      /* acotado antes de multiplicar; saturación suave (sincronías) */
        out[k] += voice_amp(softclip(clamp((clamp(acc[k], -699000, 699000) * g) >> 11, -65535, 65535)), m, k);
}

static const preset_t SWARM_PRESETS[] = {
    /* name, {CAMP, AGEN, COHE, TEMP, MASA, ESPC, CHIR, COLP}, {A D S R}, fenv, mono */
    {"GRILLOS", {0, 70, 60, 70, 1, 0, 100, 50}, {10, 90, 120, 70}, 0, 0, FX(0, 0, 20, 60), PAT(0)},
    {"KEPLER", {0, 90, 20, 30, 3, 2, 30, 40}, {60, 90, 120, 100}, 0, 0, FX(0, 30, 30, 85), PAT(0)},
    {"LUCIERNAG", {1, 100, 110, 50, 2, 1, 80, 90}, {30, 90, 120, 90}, 0, 0, FX(0, 20, 40, 80), PAT(0)},
    {"NUBE PSI", {1, 127, 30, 40, 3, 1, 0, 110}, {80, 90, 127, 110}, 0, 0, FX(0, 40, 20, 95), PAT(0)},
    {"ATOMO H", {2, 80, 10, 50, 3, 3, 40, 100}, {20, 90, 120, 90}, 0, 0, FX(0, 0, 30, 75), PAT(0)},
    {"CIGARRAS", {2, 127, 90, 110, 2, 0, 70, 30}, {10, 90, 120, 60}, 0, 0, FX(10, 0, 10, 50), PAT(0)},
};

static const engine_t ENG_SWARM = {
    .name = "ENJAMB",
    .page_title = {"CAMPO", "VOZ"},
    .edit = {
        {"CAMP", F_ENUM, 0, 2, 0, N_SW_FIELD, 0},
        {"AGEN", F_PCT, 0, 127, 64, 0, 0},
        {"COHE", F_PCT, 0, 127, 50, 0, 0},
        {"TEMP", F_PCT, 0, 127, 60, 0, 0},
        {"MASA", F_INT, 1, 3, 1, 0, 0},
        {"ESPC", F_ENUM, 0, SW_NSPEC - 1, 0, SW_SPEC_NAMES, 0},
        {"CHIR", F_PCT, 0, 127, 90, 0, 0},
        {"COLP", F_PCT, 0, 127, 60, 0, 0},
    },
    .presets = SWARM_PRESETS,
    .npresets = NELEM(SWARM_PRESETS),
    .note_on = swarm_note_on,
    .render = swarm_render,
    .knob = {P_E1, P_E2, P_E3, P_E6},
    .block = swarm_block,
    .mem = sizeof(sw_part_t),
};
