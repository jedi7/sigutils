/*

  Copyright (C) 2016 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, version 3.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#include <string.h>
#include "sampling.h"
#include "log.h"
#include "types.h"
#include "pll.h"
#include "taps.h"
#include "coef.h"

void
su_pll_finalize(su_pll_t *pll)
{

}

SUBOOL
su_pll_init(su_pll_t *pll, SUFLOAT fhint, SUFLOAT fc)
{
  SUFLOAT dinv;

  memset(pll, 0, sizeof(su_pll_t));

  fc = SU_NORM2ANG_FREQ(fc);

  /* Settings taken from GNU Radio */
  dinv = 1.f / (1.f + 2.f * .707f * fc + fc * fc);

  pll->alpha = 4 * fc * fc * dinv;
  pll->beta  = 4 * 0.707 * fc * dinv;

  su_ncqo_init(&pll->ncqo, fhint);

  return SU_TRUE;

fail:
  su_pll_finalize(pll);

  return SU_FALSE;
}

SUCOMPLEX
su_pll_track(su_pll_t *pll, SUCOMPLEX x)
{
  SUCOMPLEX ref = su_ncqo_read(&pll->ncqo);
  SUCOMPLEX mix = x * SU_C_CONJ(ref);
  SUFLOAT   phase = su_ncqo_get_phase(&pll->ncqo);
  SUFLOAT   error = su_phase_adjust_one_cycle(SU_C_ARG(x) - phase);

  su_ncqo_inc_angfreq(&pll->ncqo, pll->alpha * error);
  su_ncqo_inc_phase  (&pll->ncqo, pll->beta  * error);

  return mix;
}

void
su_pll_feed(su_pll_t *pll, SUFLOAT x)
{
  SUCOMPLEX s;

  SUFLOAT lck = 0;
  SUFLOAT err = 0;

  s = su_ncqo_read(&pll->ncqo);

  err = -x * SU_C_IMAG(s); /* Error signal: projection against Q */
  lck =  x * SU_C_REAL(s); /* Lock: projection against I */

  pll->lock += pll->beta * (2 * lck - pll->lock);

  if (pll->ncqo.omega > -pll->alpha * err) {
    su_ncqo_inc_angfreq(&pll->ncqo, pll->alpha * err);
  }

  su_ncqo_inc_phase(&pll->ncqo, pll->beta * err);
}

/**************** QPSK Costas Filter implementation **************************/
void
su_costas_finalize(su_costas_t *costas)
{
  su_iir_filt_finalize(&costas->af);
}

void
su_costas_set_loop_gain(su_costas_t *costas, SUFLOAT gain)
{
  costas->gain = gain;
}

SUBOOL
su_costas_init(
    su_costas_t *costas,
    enum sigutils_costas_kind kind,
    SUFLOAT fhint,
    SUFLOAT arm_bw,
    unsigned int arm_order,
    SUFLOAT loop_bw)
{
  SUFLOAT *b = NULL;
  SUFLOAT *a = NULL;
  SUFLOAT scaling;
  unsigned int i = 0;

  memset(costas, 0, sizeof(su_costas_t));

  /* Make LPF filter critically damped (Eric Hagemann) */
  costas->a = SU_NORM2ANG_FREQ(loop_bw);
  costas->b = .5 * costas->a * costas->a;
  costas->y_alpha = 1;
  costas->kind = kind;
  costas->gain = 1;

  su_ncqo_init(&costas->ncqo, fhint);

  /* Initialize arm filters */
  if (arm_order == 0)
    arm_order = 1;

  if (arm_order == 1 || arm_order >= SU_COSTAS_FIR_ORDER_THRESHOLD) {
    if ((b = malloc(sizeof (SUFLOAT) * arm_order)) == NULL)
      goto fail;

    if (arm_order == 1)
      b[0] = 1; /* No filtering */
    else
      su_taps_brickwall_lp_init(b, arm_bw, arm_order);
  } else {
    /* If arm filter order is small, try to build a IIR filter */
    if ((a = su_dcof_bwlp(arm_order - 1, arm_bw)) == NULL)
      goto fail;

    if ((b = su_ccof_bwlp(arm_order - 1)) == NULL)
      goto fail;

    scaling = su_sf_bwlp(arm_order - 1, arm_bw);

    for (i = 0; i < arm_order; ++i)
      b[i] *= scaling;
  }

  if (!__su_iir_filt_init(
      &costas->af,
      a == NULL ? 0 : arm_order,
      a,
      arm_order,
      b,
      SU_FALSE))
    goto fail;

  b = NULL;
  a = NULL;

  return SU_TRUE;

fail:
  su_costas_finalize(costas);

  if (b != NULL)
    free(b);

  if (a != NULL)
    free(a);

  return SU_FALSE;
}

SUCOMPLEX
su_costas_feed(su_costas_t *costas, SUCOMPLEX x)
{
  SUCOMPLEX s;
  SUCOMPLEX L;
  SUFLOAT e = 0;

  s = su_ncqo_read(&costas->ncqo);
  /*
   * s = cos(wt) + sin(wt). Signal sQ be 90 deg delayed wrt sI, therefore
   * we must multiply by conj(s).
   */
  costas->z = costas->gain * su_iir_filt_feed(&costas->af, SU_C_CONJ(s) * x);

  switch (costas->kind) {
    case SU_COSTAS_KIND_NONE:
      SU_ERROR("Invalid Costas loop\n");
      return 0;

    case SU_COSTAS_KIND_BPSK:
      /* Taken directly from Wikipedia */
      e = -SU_C_REAL(costas->z) * SU_C_IMAG(costas->z);
      break;

    case SU_COSTAS_KIND_QPSK:
      /* Compute limiter output */
      L = SU_C_SGN(costas->z);

      /*
       * Error signal taken from Maarten Tytgat's paper "Time Domain Model
       * for Costas Loop Based QPSK Receiver.
       */
      e =  SU_C_REAL(L) * SU_C_IMAG(costas->z)
          -SU_C_IMAG(L) * SU_C_REAL(costas->z);
      break;

    case SU_COSTAS_KIND_8PSK:
      /*
       * The following phase detector was shamelessly borrowed from
       * GNU Radio's Costas Loop implementation. I'm keeping the
       * original comment for referece:
       *
       * -----------8<--------------------------------------------------
       * This technique splits the 8PSK constellation into 2 squashed
       * QPSK constellations, one when I is larger than Q and one
       * where Q is larger than I. The error is then calculated
       * proportionally to these squashed constellations by the const
       * K = sqrt(2)-1.
       *
       * The signal magnitude must be > 1 or K will incorrectly bias
       * the error value.
       *
       * Ref: Z. Huang, Z. Yi, M. Zhang, K. Wang, "8PSK demodulation for
       * new generation DVB-S2", IEEE Proc. Int. Conf. Communications,
       * Circuits and Systems, Vol. 2, pp. 1447 - 1450, 2004.
       * -----------8<--------------------------------------------------
       */

      L = SU_C_SGN(costas->z);

      if (SU_ABS(SU_C_REAL(costas->z)) >= SU_ABS(SU_C_IMAG(costas->z)))
        e =  SU_C_REAL(L) * SU_C_IMAG(costas->z)
            -SU_C_IMAG(L) * SU_C_REAL(costas->z) * (SU_SQRT2 - 1);
      else
        e =  SU_C_REAL(L) * SU_C_IMAG(costas->z) * (SU_SQRT2 - 1)
            -SU_C_IMAG(L) * SU_C_REAL(costas->z);
      break;

    default:
      SU_ERROR("Unsupported Costas loop kind\n");
      return 0;
  }

  costas->lock += costas->a * (1 - e - costas->lock);
  costas->y += costas->y_alpha * (costas->z - costas->y);

  /* IIR loop filter suggested by Eric Hagemann */
  su_ncqo_inc_angfreq(&costas->ncqo, costas->b * e);
  su_ncqo_inc_phase(&costas->ncqo, costas->a * e);

  return costas->y;
}


void
su_costas_set_kind(su_costas_t *costas, enum sigutils_costas_kind kind)
{
  costas->kind = kind;
}
