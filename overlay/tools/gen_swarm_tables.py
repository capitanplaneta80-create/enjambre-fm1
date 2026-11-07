#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Mauricio Vásquez Arias, Universidad EAFIT
"""Tablas de ENJAMBRE (eng_swarm.c): gravedad suavizada, espectros (1/16 de semitono), normalización y ventana
de chirrido. Todo entero: el motor no usa float ni divisiones en el audio.
  python3 tools/gen_swarm_tables.py sw_tables.h"""
import math
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "sw_tables.h"
L = []
w = L.append
w("/* generado por tools/gen_swarm_tables.py: no editar */")
w("#ifndef SW_TABLES_H\n#define SW_TABLES_H")

# gravedad: 1 / (d2 + eps2)^1.5, d2 en Q16 (0..8 en 256 pasos de 1/32), Q8
EPS2 = 0.02
g = []
for i in range(257):
    d2 = i / 32.0
    g.append(int(round(256.0 / (d2 + EPS2) ** 1.5)))
w("static const int32_t SW_GRAV[257] = {" + ", ".join(map(str, g)) + "};")


def st16(r):  # razón de frecuencia -> 1/16 de semitono
    return int(round(16 * 12 * math.log2(r)))


spectra = {}
spectra["ARM"] = [n for n in range(1, 17)]                                   # serie armónica
spectra["MEMB"] = [math.sqrt(a * a + b * b) / math.sqrt(2) for a in range(1, 5) for b in range(1, 5)]  # membrana cuadrada
# Kepler, Harmonices Mundi (1619): intervalos de velocidad angular afelio/perihelio de cada planeta, apilados
kep = [1, 25 / 24, 16 / 15, 6 / 5, 5 / 4, 4 / 3, 3 / 2, 8 / 5, 5 / 3, 15 / 8, 2, 12 / 5, 5 / 2, 3, 4, 5]
spectra["KEPL"] = kep
# Bohr: líneas del hidrógeno, Balmer (n=3..10) y Lyman (n=2..9) relativas a H-alfa
balmer = [(0.25 - 1 / n ** 2) / (5 / 36) for n in range(3, 11)]
lyman = [(1 - 1 / n ** 2) / (5 / 36) / 4 for n in range(2, 10)]          # una 2.ª octava abajo (audible)
spectra["BOHR"] = sorted(balmer + lyman)
names = list(spectra)
w("#define SW_NSPEC %d" % len(names))
w("static const char *const SW_SPEC_NAMES[] = {" + ", ".join('"%s"' % n for n in names) + "};")
w("static const int16_t SW_SPEC[%d][16] = {" % len(names))
for n in names:
    w("    {" + ", ".join(str(st16(r)) for r in sorted(spectra[n])) + "},   /* %s */" % n)
w("};")

# 1/sqrt(n) para n agentes en una voz, Q15
w("static const int16_t SW_NORM[65] = {" + ", ".join(
    str(32767 if n == 0 else int(round(32767 / math.sqrt(n)))) for n in range(65)) + "};")

# ventana de una sílaba de chirrido (coseno elevado), 64 puntos, Q15
w("static const int16_t SW_WIN[65] = {" + ", ".join(
    str(int(round(32767 * 0.5 * (1 - math.cos(2 * math.pi * i / 64))))) for i in range(65)) + "};")
w("#endif")
open(out, "w").write("\n".join(L) + "\n")
print("escrito", out)
