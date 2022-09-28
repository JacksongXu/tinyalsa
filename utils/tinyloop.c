/* tinycap.c
**
** Copyright 2011, The Android Open Source Project
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of The Android Open Source Project nor the names of
**       its contributors may be used to endorse or promote products derived
**       from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY The Android Open Source Project ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL The Android Open Source Project BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
** DAMAGE.
*/

#include <tinyalsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <pthread.h>


#define FORMAT_PCM 1

typedef uint32_t snd_pcm_uframes_t;

typedef struct loopback_dev {
  uint32_t card;
  uint32_t device;
  struct pcm *pcm;
}loopback_dev_t;

typedef struct loopback {
  loopback_dev_t capt;
  loopback_dev_t play;
  /*shared frame fifo*/
  int8_t *buf;
  snd_pcm_uframes_t buf_size;
}loopback_t;


static uint32_t snd_loopback(loopback_t* looper, struct pcm_config *config,
                                       uint32_t show_time_overheads);

static int capturing = 1;


void sigint_handler(int sig __unused)
{
    capturing = 0;
}

int main(int argc, char **argv)
{
    uint32_t capt_card = 0, play_card = 0;
    uint32_t capt_device = 0, play_device = 0;
    uint32_t channels = 2;
    uint32_t rate = 44100;
    uint32_t bits = 16;
    uint32_t frames;
    uint32_t period_size = 1024;
    uint32_t period_count = 4;
    uint32_t show_time_overheads = 0;
  
    enum pcm_format format;

    if (argc < 1) {
        fprintf(stderr, "Usage: %s [-C hw:card,device] [-P hw:card,device]"
                " [-c channels] [-r rate] [-b bits] [-p period_size]"
                " [-n n_periods] [ -o time overheads]\n"
                "\ne.g."
                "\n   %s -C hw:0,0 -P hw:1,0"
                "\nwhich will loop audio from snd card 0 device 0 to snd card 1 device 0\n"
                , argv[0], argv[0]);
        return 1;
    }

    /* parse command line arguments */
    argv += 1;
    while (*argv) {
        if (strcmp(*argv, "-C") == 0) {
            argv++;
            if (*argv)
                sscanf(*argv, "hw:%d,%d", &capt_card, &capt_device);
        } else if (strcmp(*argv, "-P") == 0) {
            argv++;
            if (*argv)
                sscanf(*argv, "hw:%d,%d", &play_card, &play_device);
        } else if (strcmp(*argv, "-c") == 0) {
            argv++;
            if (*argv)
                channels = atoi(*argv);
        } else if (strcmp(*argv, "-r") == 0) {
            argv++;
            if (*argv)
                rate = atoi(*argv);
        } else if (strcmp(*argv, "-b") == 0) {
            argv++;
            if (*argv)
                bits = atoi(*argv);
        } else if (strcmp(*argv, "-p") == 0) {
            argv++;
            if (*argv)
                period_size = atoi(*argv);
        } else if (strcmp(*argv, "-n") == 0) {
            argv++;
            if (*argv)
                period_count = atoi(*argv);
        } else if (strcmp(*argv, "-o") == 0) {
            argv++;
            if (*argv)
                show_time_overheads = atoi(*argv);
        }

        if (*argv)
            argv++;
    }

    switch (bits) {
    case 32:
        format = PCM_FORMAT_S32_LE;
        break;
    case 24:
        format = PCM_FORMAT_S24_LE;
        break;
    case 16:
        format = PCM_FORMAT_S16_LE;
        break;
    default:
        fprintf(stderr, "%u bits is not supported.\n", bits);
        return 1;
    }

    struct pcm_config config;

    memset(&config, 0, sizeof(config));
    config.channels = channels;
    config.rate = rate;
    config.period_size = period_size;
    config.period_count = period_count;
    config.format = format;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    /* install signal handler and begin capturing */
    signal(SIGINT, sigint_handler);
    signal(SIGHUP, sigint_handler);
    signal(SIGTERM, sigint_handler);

    loopback_t looper;
    memset(&looper, 0, sizeof(looper));
    looper.capt.card = capt_card;
    looper.capt.device = capt_device;
    looper.play.card = play_card;
    looper.play.device = play_device;

    frames = snd_loopback(&looper, &config, show_time_overheads);

    printf("\nLooped %u frames\n", frames);

    return 0;
}

static int pcm_open_wrapper(loopback_dev_t* dev, uint32_t pcm_flag, struct pcm_config *pcm_cfg)
{
  dev->pcm = pcm_open(dev->card, dev->device, pcm_flag, pcm_cfg);
  if (!dev->pcm || !pcm_is_ready(dev->pcm)) {
    fprintf(stderr, "Unable to open PCM device hw:%d;%d (%s)\n",
            dev->card, dev->device, pcm_get_error(dev->pcm));
    return -1;
  }

  return 0;
}

uint32_t snd_loopback(loopback_t* looper, struct pcm_config* config,
                                      uint32_t show_time_overheads)
{
    uint32_t capt_size, play_size;
    uint32_t bytes_read = 0;
    uint32_t frames = 0;
    struct timespec end;
    struct timespec now;

    // looper.play.cfg.start_threshold = period_size;
    // looper.play.cfg.stop_threshold = period_size * period_count;


    if(pcm_open_wrapper(&looper->capt, PCM_IN, config) < 0  ||
          pcm_open_wrapper(&looper->play, PCM_OUT, config) < 0)
    {
      goto __loop_end;
    }


    capt_size = pcm_frames_to_bytes(looper->capt.pcm, pcm_get_buffer_size(looper->capt.pcm));
    play_size = pcm_frames_to_bytes(looper->play.pcm, pcm_get_buffer_size(looper->play.pcm));
    if(capt_size != play_size)
    {
      fprintf(stderr, "Uneq pcm frame size, [capt %d; play %d] bytes\n", capt_size, play_size);
      goto __loop_end;
    }
    

    looper->buf = malloc(capt_size);
    if (!looper->buf) {
        fprintf(stderr, "Unable to allocate %u bytes\n", capt_size);
        goto __loop_end;
    }

    printf("loop hw:%d,%d to hw:%d,%d: %u ch, %u hz, %u bit, %u buffer size\n", 
           looper->capt.card, looper->capt.device, looper->play.card, looper->play.device,
           config->channels, config->rate, pcm_format_to_bits(config->format), capt_size);

    long sec, nsec;
    double capt_us, play_us;
    while (capturing) {
       
        if(show_time_overheads){
          clock_gettime(CLOCK_MONOTONIC, &now);
        }
        
        if(pcm_read(looper->capt.pcm, looper->buf, capt_size) < 0){
            fprintf(stderr,"Error capturing sample(%s)\n", pcm_get_error(looper->capt.pcm));
            break;
        }

        if(show_time_overheads){
          clock_gettime(CLOCK_MONOTONIC, &end);
          sec = end.tv_sec - now.tv_sec;
          nsec =  end.tv_nsec - now.tv_nsec;
          capt_us = sec * 1000000 + nsec *1e-3;
          fprintf(stderr,"capture time overheads: %f us\n", capt_us);

          clock_gettime(CLOCK_MONOTONIC, &now);
        }

        if (pcm_write(looper->play.pcm, looper->buf, capt_size)) {
            fprintf(stderr,"Error playing sample(%s)\n", pcm_get_error(looper->play.pcm));
            break;
        }

        if(show_time_overheads){
          clock_gettime(CLOCK_MONOTONIC, &end);
          sec = end.tv_sec - now.tv_sec;
          nsec =  end.tv_nsec - now.tv_nsec;
          play_us = sec * 1000000 + nsec *1e-3;
          fprintf(stderr,"play timer overheads: %f us\n", capt_us);
        }

        bytes_read += capt_size;
    }

    frames = pcm_bytes_to_frames(looper->capt.pcm, bytes_read);

__loop_end:
    if(looper->buf)
      free(looper->buf);

    if(looper->capt.pcm)
      pcm_close(looper->capt.pcm);

    if(looper->play.pcm)
     pcm_close(looper->play.pcm);

    return frames;
}
