/*
 * Dreamroq by Mike Melanson
 * Updated by Josh Pearson to add audio support
 *
 * This is the sample Dreamcast player app, designed to be run under
 * the KallistiOS operating system.
 */
/*
	Name: Iaan micheal
	Copyright: 
	Author: Ian micheal
	Date: 12/08/23 05:17
	Description: kos 2.0 up port threading fix and wrappers and all warnings fixed
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include "dreamroqlib.h"
#include "dc_timer.h"
#include "snddrv.h"
#include <dc/sound/sound.h>
#include <stdio.h>

/* Audio Global variables */
#define   PCM_BUF_SIZE (1024 * 1024) 
static unsigned char *pcm_buf = NULL;
static int pcm_size = 0;
static int audio_init = 0;
static mutex_t pcm_mut = MUTEX_INITIALIZER;
/* Video Global variables */
static pvr_ptr_t textures[2];
static int current_frame = 0;
static int graphics_initialized = 0;
static float video_delay;
// Define the target frame rate
#define TARGET_FRAME_RATE 30

static void snd_thd()
{
    do
    {
        unsigned int start_time, end_time;

        // Measure time taken by waiting for AICA Driver request
        start_time = dc_get_time();
        while (snddrv.buf_status != SNDDRV_STATUS_NEEDBUF)
            thd_pass();
        end_time = dc_get_time();
        printf("Wait for AICA Driver: %u ms\n", end_time - start_time);

        // Measure time taken by waiting for RoQ Decoder
        start_time = dc_get_time();
        while (pcm_size < snddrv.pcm_needed)
        {
            if (snddrv.dec_status == SNDDEC_STATUS_DONE)
                goto done;
            thd_pass();
        }
        end_time = dc_get_time();
        printf("Wait for RoQ Decoder: %u ms\n", end_time - start_time);

        // Measure time taken by copying PCM samples
        start_time = dc_get_time();
        mutex_lock(&pcm_mut);
        memcpy(snddrv.pcm_buffer, pcm_buf, snddrv.pcm_needed);
        pcm_size -= snddrv.pcm_needed;
        memmove(pcm_buf, pcm_buf + snddrv.pcm_needed, pcm_size);
        mutex_unlock(&pcm_mut);
        end_time = dc_get_time();
        printf("Copy PCM Samples: %u ms\n", end_time - start_time);

        // Measure time taken by informing AICA Driver
        start_time = dc_get_time();
        snddrv.buf_status = SNDDRV_STATUS_HAVEBUF;
        end_time = dc_get_time();
        printf("Inform AICA Driver: %u ms\n", end_time - start_time);

    } while (snddrv.dec_status == SNDDEC_STATUS_STREAMING);
done:
    snddrv.dec_status = SNDDEC_STATUS_NULL;
}

static int render_cb(unsigned short *buf, int width, int height, int stride,
                     int texture_height)
{
    pvr_poly_cxt_t cxt;
    static pvr_poly_hdr_t hdr[2];
    static pvr_vertex_t vert[4];

    float ratio;
    // screen coordinates of upper left and bottom right corners
    static int ul_x, ul_y, br_x, br_y;

    // Initialize textures, drawing coordinates, and other parameters
    if (!graphics_initialized)
    {
        textures[0] = pvr_mem_malloc(stride * texture_height * 2);
        textures[1] = pvr_mem_malloc(stride * texture_height * 2);
        if (!textures[0] || !textures[1])
        {
            return ROQ_RENDER_PROBLEM;
        }

        // Precompile the poly headers
        for (int i = 0; i < 2; i++) {
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                             stride, texture_height, textures[i], PVR_FILTER_NONE);
            pvr_poly_compile(&hdr[i], &cxt);
        }

        // Calculate drawing coordinates
        ratio = 640.0 / width;
        ul_x = 0;
        br_x = (int)(ratio * stride);
        ul_y = (int)((480 - ratio * height) / 2);
        br_y = ul_y + (int)(ratio * texture_height);

        // Set common vertex properties
        for (int i = 0; i < 4; i++) {
            vert[i].z = 1.0f;
            vert[i].argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
            vert[i].oargb = 0;
            vert[i].flags = (i < 3) ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
        }

        // Initialize vertex coordinates and UV coordinates
        vert[0].x = ul_x;
        vert[0].y = ul_y;
        vert[0].u = 0.0;
        vert[0].v = 0.0;

        vert[1].x = br_x;
        vert[1].y = ul_y;
        vert[1].u = 1.0;
        vert[1].v = 0.0;

        vert[2].x = ul_x;
        vert[2].y = br_y;
        vert[2].u = 0.0;
        vert[2].v = 1.0;

        vert[3].x = br_x;
        vert[3].y = br_y;
        vert[3].u = 1.0;
        vert[3].v = 1.0;

        // Get the current hardware timing
        video_delay = (float)dc_get_time();

        graphics_initialized = 1;
    }

    // Send the video frame as a texture over to video RAM
    pvr_txr_load(buf, textures[current_frame], stride * texture_height * 2);

    // Calculate the elapsed time since the last frame
    unsigned int current_time = dc_get_time();
    unsigned int elapsed_time = current_time - video_delay;
    unsigned int target_frame_time = 1000 / TARGET_FRAME_RATE;

    // If the elapsed time is less than the target frame time, introduce a delay
    if (elapsed_time < target_frame_time) {
        unsigned int delay_time = target_frame_time - elapsed_time;
        thd_sleep(delay_time);
    }

    // Update the hardware timing for the current frame
    video_delay = (float)current_time;

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    // Render the frame using precompiled headers and vertices
    pvr_prim(&hdr[current_frame], sizeof(pvr_poly_hdr_t));
    for (int i = 0; i < 4; i++) {
        pvr_prim(&vert[i], sizeof(pvr_vertex_t));
    }

    pvr_list_finish();
    pvr_scene_finish();

    // Toggle between frames
    current_frame = 1 - current_frame;

    return ROQ_SUCCESS;
}

#define AUDIO_THREAD_PRIO 400  // Adjust this value as needed
static void *snd_thd_wrapper(void *arg)
{
	printf("Audio Thread: Started\n");
    // Capture the start time
    unsigned int start_time = dc_get_time();
    snd_thd();  // Call the actual audio thread function
        // Capture the end time
    unsigned int end_time = dc_get_time();

    // Calculate the time interval
    unsigned int elapsed_time = end_time - start_time;
    printf("Audio Thread: Finished (Time: %u ms)\n", elapsed_time);
    return NULL;
}

static int audio_cb(unsigned char *buf, int size, int channels)
{
    if (!audio_init)
    {
        /* allocate PCM buffer */
        pcm_buf = malloc(PCM_BUF_SIZE);
        if (pcm_buf == NULL)
            return ROQ_NO_MEMORY;

        /* Start AICA Driver */
        snddrv_start(22050, channels);
        snddrv.dec_status = SNDDEC_STATUS_STREAMING;
        printf("Creating Audio Thread\n");
        // Create a thread to stream the samples to the AICA
        thd_create(AUDIO_THREAD_PRIO, snd_thd_wrapper, NULL);
        
            //printf("SNDDRV: Creating Driver Thread\n");


        audio_init = 1;
    }

    /* Copy the decoded PCM samples to our local PCM buffer */
    mutex_lock(&pcm_mut);
    memcpy(pcm_buf + pcm_size, buf, size);
    pcm_size += size;
    mutex_unlock(&pcm_mut);

    return ROQ_SUCCESS;
}

static int quit_cb()
{
    static int frame_count = 0;
    static unsigned int last_time = 0;
    static unsigned int target_frame_time = 1000 / 30; // 30 FPS

    // Calculate time difference since the last frame
    unsigned int current_time = dc_get_time();
    unsigned int elapsed_time = current_time - last_time;

    // Check if the video has ended and the audio decoding status is done
    if (snddrv.dec_status == SNDDEC_STATUS_DONE) {
        printf("Exiting due to audio decoding status\n");
        return 1; // Exit the loop
    }

    // Check if the "Start" button is pressed
    MAPLE_FOREACH_BEGIN(MAPLE_FUNC_CONTROLLER, cont_state_t, st)
    if (st->buttons & CONT_START) {
        printf("Exiting due to Start button\n");
        return 1; // Exit the loop
    }
    MAPLE_FOREACH_END()

    // Delay if necessary to maintain the target frame rate
    if (elapsed_time < target_frame_time) {
        unsigned int delay_time = target_frame_time - elapsed_time;
        thd_sleep(delay_time);
    }

    // Print FPS information every second
    if (elapsed_time >= 1000) {
        double fps = (double)frame_count / (elapsed_time / 1000.0);
     //   printf("FPS: %.2lf\n", fps);

        frame_count = 0;
        last_time = current_time;
    }

  //  printf("Continuing loop\n");
    fflush(stdout); // Flush the output buffer to ensure immediate display
    frame_count++;

    return 0; // Continue the loop
}



int main()
{
    int status = 0;

    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_defaults();

    printf("dreamroq_play(C) Multimedia Mike Melanson & Josh PH3NOM Pearson 2011\n");
    printf("dreamroq_play(C) Ian micheal Up port to Kos2.0 sound fix and threading\n");
    printf("dreamroq_play(C) Ian micheal Kos2.0 free and exit when loop ends 2023\n");
    printf("dreamroq_play(C) Ian micheal redo frame limit code and rendering and comment what it does 2023\n");
    /* To disable a callback, simply replace the function name by 0 */
    status = dreamroq_play("/cd/movie.roq", 0, render_cb, audio_cb, quit_cb);

    printf("dreamroq_play() status = %d\n", status);

    if (audio_init)
    {
        snddrv.dec_status = SNDDEC_STATUS_DONE; /* Signal audio thread to stop */
        while (snddrv.dec_status != SNDDEC_STATUS_NULL)
        {
            thd_pass(1);
            printf("Waiting for audio thread to finish...\n");
        }
        free(pcm_buf);
        pcm_buf = NULL;
        pcm_size = 0;
    }

    if (graphics_initialized)
    {
        pvr_mem_free(textures[0]); /* Free the PVR memory */
        pvr_mem_free(textures[1]);
        printf("Freed PVR memory\n");
    }

    printf("Exiting main()\n");
    return 0;
}


