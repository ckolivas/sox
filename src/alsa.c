/* libSoX device driver: ALSA   (c) 2006-2012 SoX contributors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "sox_i.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <semaphore.h>

typedef struct {
  snd_pcm_uframes_t  buf_len, period, read_len, write_len;
  snd_pcm_t          * pcm;
  char               * buf;
  char               * thread_buf;
  size_t	bufsize;
  unsigned int       format;
  sem_t write_sem;
  sem_t rwrite_sem;
  sem_t read_sem;
  sem_t rread_sem;
} priv_t;

static const
  struct {
    unsigned int bits;
    enum _snd_pcm_format alsa_fmt;
    unsigned int bytes; /* occupied in the buffer per sample */
    sox_encoding_t enc;
  } formats[] = {
    /* order by # of bits; within that, preferred first */
    {  8, SND_PCM_FORMAT_S8, 1, SOX_ENCODING_SIGN2 },
    {  8, SND_PCM_FORMAT_U8, 1, SOX_ENCODING_UNSIGNED },
    { 16, SND_PCM_FORMAT_S16, 2, SOX_ENCODING_SIGN2 },
    { 16, SND_PCM_FORMAT_U16, 2, SOX_ENCODING_UNSIGNED },
    { 24, SND_PCM_FORMAT_S24, 4, SOX_ENCODING_SIGN2 },
    { 24, SND_PCM_FORMAT_U24, 4, SOX_ENCODING_UNSIGNED },
    { 24, SND_PCM_FORMAT_S24_3LE, 3, SOX_ENCODING_SIGN2 },
    { 32, SND_PCM_FORMAT_S32, 4, SOX_ENCODING_SIGN2 },
    { 32, SND_PCM_FORMAT_U32, 4, SOX_ENCODING_UNSIGNED },
    {  0, 0, 0, SOX_ENCODING_UNKNOWN } /* end of list */
  };

static int select_format(
    sox_encoding_t              * encoding_,
    unsigned                    * nbits_,
    snd_pcm_format_mask_t const * mask,
    unsigned int                * format)
{
  unsigned int from = 0, to; /* NB: "to" actually points one after the last */
  int cand = -1;

  while (formats[from].bits < *nbits_ && formats[from].bits != 0)
    from++;  /* find the first entry with at least *nbits_ bits */
  for (to = from; formats[to].bits != 0; to++) ;  /* find end of list */

  while (to > 0) {
    unsigned int i, bits_next = 0;
    for (i = from; i < to; i++) {
      lsx_debug_most("select_format: trying #%u", i);
      if (snd_pcm_format_mask_test(mask, formats[i].alsa_fmt)) {
        if (formats[i].enc == *encoding_) {
          cand = i;
          break; /* found a match */
        } else if (cand == -1) /* don't overwrite a candidate that
                                       was earlier in the list */
          cand = i; /* will work, but encoding differs */
      }
    }
    if (cand != -1)
      break;
    /* no candidate found yet; now try formats with less bits: */
    to = from;
    if (from > 0)
      bits_next = formats[from-1].bits;
    while (from && formats[from-1].bits == bits_next)
      from--; /* go back to the first entry with bits_next bits */
  }

  if (cand == -1) {
    lsx_debug("select_format: no suitable ALSA format found");
    return -1;
  }

  if (*nbits_ != formats[cand].bits || *encoding_ != formats[cand].enc) {
    lsx_warn("can't encode %u-bit %s", *nbits_,
        sox_encodings_info[*encoding_].desc);
    *nbits_ = formats[cand].bits;
    *encoding_ = formats[cand].enc;
  }
  lsx_debug("selecting format %d: %s (%s)", cand,
      snd_pcm_format_name(formats[cand].alsa_fmt),
      snd_pcm_format_description(formats[cand].alsa_fmt));
  *format = cand;
  return 0;
}

typedef struct {
	sox_format_t * ft;
	priv_t *p;
} thread_priv_t;

static int recover(sox_format_t * ft, snd_pcm_t * pcm, int err)
{
  if (err == -EPIPE)
    lsx_warn("%s-run", ft->mode == 'r'? "over" : "under");
  else if (err != -ESTRPIPE)
    lsx_warn("%s", snd_strerror(err));
  else while ((err = snd_pcm_resume(pcm)) == -EAGAIN) {
    lsx_report("suspended");
    sleep(1);                  /* Wait until the suspend flag is released */
  }
  if (err < 0 && (err = snd_pcm_recover(pcm, err, 0)) < 0)
    lsx_fail_errno(ft, SOX_EPERM, "%s", snd_strerror(err));
  return err;
}

static void *read_thread(void *arg)
{
	thread_priv_t *tpt = arg;
	sox_format_t *ft = tpt->ft;
	priv_t *p = tpt->p;
	snd_pcm_sframes_t  n;

	sem_wait(&p->rread_sem);
	while (42) {
		size_t len = p->read_len / ft->signal.channels;
		do {
			n = snd_pcm_readi(p->pcm, p->thread_buf, len);
			if (n < 0 && recover(ft, p->pcm, (int)n) < 0)
				break;
		} while (n <= 0);

		sem_wait(&p->rread_sem);
		memcpy(p->buf, p->thread_buf, p->bufsize);
		sem_post(&p->read_sem);
	}

	return NULL;
}

static void *write_thread(void *arg)
{
	thread_priv_t *tpt = arg;
	sox_format_t * ft = tpt->ft;
	priv_t *p = (priv_t *)tpt->ft->priv;
	snd_pcm_sframes_t  actual;
	size_t i, n;

	sem_post(&p->write_sem);
	sem_wait(&p->rwrite_sem);
	while (42) {
		n = p->write_len;
		for (i = 0; i < n; i += actual * ft->signal.channels) do {
			do {
				actual = snd_pcm_writei(p->pcm,
							p->thread_buf + i * formats[p->format].bytes,
				   (n - i) / ft->signal.channels);
			} while (actual == -EAGAIN);
			if (actual < 0 && recover(ft, p->pcm, (int)actual) < 0)
				return 0;
		} while (actual < 0);
		sem_post(&p->write_sem);
		sem_wait(&p->rwrite_sem);
	}

	return NULL;
}

#define _(x,y) do {if ((err = x y) < 0) {lsx_fail_errno(ft, SOX_EPERM, #x " error: %s", snd_strerror(err)); goto error;} } while (0)
static int setup(sox_format_t * ft)
{
  priv_t                 * p = (priv_t *)ft->priv;
  snd_pcm_hw_params_t    * params = NULL;
  snd_pcm_format_mask_t  * mask = NULL;
  snd_pcm_uframes_t      min, max;
  unsigned               n;
  int                    err;
  thread_priv_t *tpt;
  static pthread_t thread;

  _(snd_pcm_open, (&p->pcm, ft->filename, ft->mode == 'r'? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, 0));
  _(snd_pcm_hw_params_malloc, (&params));
  _(snd_pcm_hw_params_any, (p->pcm, params));
#if SND_LIB_VERSION >= 0x010009               /* Disable alsa-lib resampling: */
  _(snd_pcm_hw_params_set_rate_resample, (p->pcm, params, 0));
#endif
  _(snd_pcm_hw_params_set_access, (p->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED));

  _(snd_pcm_format_mask_malloc, (&mask));           /* Set format: */
  snd_pcm_hw_params_get_format_mask(params, mask);
  _(select_format, (&ft->encoding.encoding, &ft->encoding.bits_per_sample, mask, &p->format));
  _(snd_pcm_hw_params_set_format, (p->pcm, params, formats[p->format].alsa_fmt));
  snd_pcm_format_mask_free(mask), mask = NULL;

  n = ft->signal.rate;                              /* Set rate: */
  _(snd_pcm_hw_params_set_rate_near, (p->pcm, params, &n, 0));
  ft->signal.rate = n;

  n = ft->signal.channels;                          /* Set channels: */
  _(snd_pcm_hw_params_set_channels_near, (p->pcm, params, &n));
  ft->signal.channels = n;

  /* Get number of significant bits: */
  if ((err = snd_pcm_hw_params_get_sbits(params)) > 0)
    ft->signal.precision = min(err, SOX_SAMPLE_PRECISION);
  else lsx_debug("snd_pcm_hw_params_get_sbits can't tell precision: %s",
           snd_strerror(err));

  /* Set buf_len > > sox_globals.bufsiz for no underrun: */
  p->buf_len = sox_globals.bufsiz * 8 / formats[p->format].bytes /
      ft->signal.channels;
  _(snd_pcm_hw_params_get_buffer_size_min, (params, &min));
  _(snd_pcm_hw_params_get_buffer_size_max, (params, &max));
  p->period = range_limit(p->buf_len, min, max) / 8;
  p->buf_len = p->period * 8;
  lsx_debug("pcm buffer size min %lu max %lu period %lu len %lu", min,max, p->period, p->buf_len);
  _(snd_pcm_hw_params_set_period_size_near, (p->pcm, params, &p->period, 0));
  _(snd_pcm_hw_params_set_buffer_size_near, (p->pcm, params, &p->buf_len));
  if (p->period * 2 > p->buf_len) {
    lsx_fail_errno(ft, SOX_EPERM, "buffer too small");
    goto error;
  }

  _(snd_pcm_hw_params, (p->pcm, params));           /* Configure ALSA */
  snd_pcm_hw_params_free(params), params = NULL;
  _(snd_pcm_prepare, (p->pcm));
  p->buf_len *= ft->signal.channels;                /* No longer in `frames' */
  p->bufsize = p->buf_len * formats[p->format].bytes;
  p->buf = lsx_malloc(p->bufsize);
  p->thread_buf = lsx_malloc(p->bufsize * 10);
  sem_init(&p->write_sem, 0, 0);
  sem_init(&p->rwrite_sem, 0, 0);
  sem_init(&p->read_sem, 0, 0);
  sem_init(&p->rread_sem, 0, 0);
	tpt = lsx_malloc(sizeof(thread_priv_t));
	tpt->ft = ft;
	tpt->p = p;
  pthread_create(&thread, NULL, read_thread, tpt);
  pthread_create(&thread, NULL, write_thread, tpt);
  return SOX_SUCCESS;

error:
  if (mask) snd_pcm_format_mask_free(mask);
  if (params) snd_pcm_hw_params_free(params);
  return SOX_EOF;
}

static size_t read_(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  priv_t             * p = (priv_t *)ft->priv;
  snd_pcm_sframes_t  i;
  static sox_bool first = sox_true;

  len = min(len, p->buf_len);
  if (first) {
	  p->read_len = len;
	  sem_post(&p->rread_sem);
	  sem_post(&p->rread_sem);
	  first = sox_false;
  } else if (p->read_len != len) {
	  lsx_warn("read len changed from %ld to %ld", p->read_len, len);
	  p->read_len = len;
  }

  sem_wait(&p->read_sem);
  i = len;
    switch (formats[p->format].alsa_fmt) {
      case SND_PCM_FORMAT_S8: {
        int8_t * buf1 = (int8_t *)p->buf;
        while (i--) *buf++ = SOX_SIGNED_8BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U8: {
        uint8_t * buf1 = (uint8_t *)p->buf;
        while (i--) *buf++ = SOX_UNSIGNED_8BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S16: {
        int16_t * buf1 = (int16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf++ = SOX_SIGNED_16BIT_TO_SAMPLE(lsx_swapw(*buf1++),);
        else
          while (i--) *buf++ = SOX_SIGNED_16BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U16: {
        uint16_t * buf1 = (uint16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf++ = SOX_UNSIGNED_16BIT_TO_SAMPLE(lsx_swapw(*buf1++),);
        else
          while (i--) *buf++ = SOX_UNSIGNED_16BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S24: {
        sox_int24_t * buf1 = (sox_int24_t *)p->buf;
        while (i--) *buf++ = SOX_SIGNED_24BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S24_3LE: {
        unsigned char *buf1 = (unsigned char *)p->buf;
        while (i--) {
          uint32_t temp;
          temp  = *buf1++;
          temp |= *buf1++ << 8;
          temp |= *buf1++ << 16;
          *buf++ = SOX_SIGNED_24BIT_TO_SAMPLE((sox_int24_t)temp,);
        }
        break;
      }
      case SND_PCM_FORMAT_U24: {
        sox_uint24_t * buf1 = (sox_uint24_t *)p->buf;
        while (i--) *buf++ = SOX_UNSIGNED_24BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S32: {
        int32_t * buf1 = (int32_t *)p->buf;
        while (i--) *buf++ = SOX_SIGNED_32BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U32: {
        uint32_t * buf1 = (uint32_t *)p->buf;
        while (i--) *buf++ = SOX_UNSIGNED_32BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      default: lsx_fail_errno(ft, SOX_EFMT, "invalid format");
        return 0;
    }
    sem_post(&p->rread_sem);

    return len;
}

static size_t write_(sox_format_t * ft, sox_sample_t const * buf, size_t len)
{
  priv_t             * p = (priv_t *)ft->priv;
  size_t             done, i, n;
  SOX_SAMPLE_LOCALS;

  for (done = 0; done < len; done += n) {
    i = n = min(len - done, p->buf_len);
    switch (formats[p->format].alsa_fmt) {
      case SND_PCM_FORMAT_S8: {
        int8_t * buf1 = (int8_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_8BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U8: {
        uint8_t * buf1 = (uint8_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_8BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S16: {
        int16_t * buf1 = (int16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf1++ = lsx_swapw(SOX_SAMPLE_TO_SIGNED_16BIT(*buf++, ft->clips));
        else
          while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_16BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U16: {
        uint16_t * buf1 = (uint16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf1++ = lsx_swapw(SOX_SAMPLE_TO_UNSIGNED_16BIT(*buf++, ft->clips));
        else
          while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_16BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S24: {
        sox_int24_t * buf1 = (sox_int24_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_24BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S24_3LE: {
        unsigned char *buf1 = (unsigned char *)p->buf;
        while (i--) {
          uint32_t temp = (uint32_t)SOX_SAMPLE_TO_SIGNED_24BIT(*buf++, ft->clips);
          *buf1++ = (temp & 0x000000FF);
          *buf1++ = (temp & 0x0000FF00) >> 8;
          *buf1++ = (temp & 0x00FF0000) >> 16;
        }
        break;
      }
      case SND_PCM_FORMAT_U24: {
        sox_uint24_t * buf1 = (sox_uint24_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_24BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S32: {
        int32_t * buf1 = (int32_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_32BIT(*buf++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U32: {
        uint32_t * buf1 = (uint32_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_32BIT(*buf++, ft->clips);
        break;
      }
      default: lsx_fail_errno(ft, SOX_EFMT, "invalid format");
        return 0;
    }
    p->write_len = n;
    sem_wait(&p->write_sem);
    memcpy(p->thread_buf, p->buf, p->bufsize);
    sem_post(&p->rwrite_sem);
  }
  return len;
}

static int stop(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  snd_pcm_close(p->pcm);
  free(p->buf);
  return SOX_SUCCESS;
}

static int stop_write(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  size_t n = ft->signal.channels * p->period, npad = n - (ft->olength % n);
  sox_sample_t * buf = lsx_calloc(npad, sizeof(*buf)); /* silent samples */

  if (npad != n)                      /* pad to hardware period: */
    write_(ft, buf, npad);
  free(buf);
  snd_pcm_drain(p->pcm);
  return stop(ft);
}

LSX_FORMAT_HANDLER(alsa)
{
  static char const * const names[] = {"alsa", NULL};
  static unsigned const write_encodings[] = {
    SOX_ENCODING_SIGN2   , 32, 24, 16, 8, 0,
    SOX_ENCODING_UNSIGNED, 32, 24, 16, 8, 0,
    0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Advanced Linux Sound Architecture device driver",
    names, SOX_FILE_DEVICE | SOX_FILE_NOSTDIO,
    setup, read_, stop, setup, write_, stop_write,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
