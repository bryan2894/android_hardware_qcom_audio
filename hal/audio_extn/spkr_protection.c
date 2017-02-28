/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "audio_hw_spkr_prot"
/*#define LOG_NDEBUG 0*/
#define LOG_NDDEBUG 0

#include <errno.h>
#include <math.h>
#include <cutils/log.h>
#include <fcntl.h>
#include "audio_hw.h"
#include "platform.h"
#include "platform_api.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <math.h>
#include <cutils/properties.h>
#include "audio_extn.h"
#include <linux/msm_audio_acdb.h>

#ifdef SPKR_PROT_ENABLED

/*Range of spkr temparatures -30C to 80C*/
#define MIN_SPKR_TEMP_Q6 (-30 * (1 << 6))
#define MAX_SPKR_TEMP_Q6 (80 * (1 << 6))

/*Set safe temp value to 40C*/
#define SAFE_SPKR_TEMP 40
#define SAFE_SPKR_TEMP_Q6 (SAFE_SPKR_TEMP * (1 << 6))

/*Range of resistance values 2ohms to 40 ohms*/
#define MIN_RESISTANCE_SPKR_Q24 (2 * (1 << 24))
#define MAX_RESISTANCE_SPKR_Q24 (40 * (1 << 24))

/*Path where the calibration file will be stored*/
#define CALIB_FILE "/data/misc/audio/audio.cal"

/*Time between retries for calibartion or intial wait time
  after boot up*/
#define WAIT_TIME_SPKR_CALIB (60 * 1000 * 1000)

#define MIN_SPKR_IDLE_SEC (60 * 30)

/*Once calibration is started sleep for 1 sec to allow
  the calibration to kick off*/
#define SLEEP_AFTER_CALIB_START (3000)

/*If calibration is in progress wait for 200 msec before querying
  for status again*/
#define WAIT_FOR_GET_CALIB_STATUS (200 * 1000)

/*Speaker states*/
#define SPKR_NOT_CALIBRATED -1
#define SPKR_CALIBRATED 1

/*Speaker processing state*/
#define SPKR_PROCESSING_IN_PROGRESS 1
#define SPKR_PROCESSING_IN_IDLE 0

/*Modes of Speaker Protection*/
enum speaker_protection_mode {
    SPKR_PROTECTION_DISABLED = -1,
    SPKR_PROTECTION_MODE_PROCESSING = 0,
    SPKR_PROTECTION_MODE_CALIBRATE = 1,
};

struct speaker_prot_session {
    int spkr_prot_mode;
    int spkr_processing_state;
    int thermal_client_handle;
    pthread_mutex_t mutex_spkr_prot;
    pthread_t spkr_calibration_thread;
    pthread_mutex_t spkr_prot_thermalsync_mutex;
    pthread_cond_t spkr_prot_thermalsync;
    int cancel_spkr_calib;
    pthread_cond_t spkr_calib_cancel;
    pthread_mutex_t spkr_calib_cancelack_mutex;
    pthread_cond_t spkr_calibcancel_ack;
    pthread_t speaker_prot_threadid;
    void *thermal_handle;
    void *adev_handle;
    int spkr_prot_t0;
    struct pcm *pcm_rx;
    struct pcm *pcm_tx;
    int (*client_register_callback)
    (char *client_name, int (*callback)(int), void *data);
    void (*thermal_client_unregister_callback)(int handle);
    int (*thermal_client_request)(char *client_name, int req_data);
    bool spkr_prot_enable;
    bool spkr_in_use;
   struct timespec spkr_last_time_used;
};

static struct pcm_config pcm_config_skr_prot = {
    .channels = 2,
    .rate = 48000,
    .period_size = 256,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .stop_threshold = INT_MAX,
    .avail_min = 0,
};

static struct speaker_prot_session handle;

static void spkr_prot_set_spkrstatus(bool enable)
{
    struct timespec ts;
    if (enable)
       handle.spkr_in_use = true;
    else {
       handle.spkr_in_use = false;
       clock_gettime(CLOCK_MONOTONIC, &handle.spkr_last_time_used);
   }
}

static void spkr_prot_calib_cancel(void *adev)
{
    pthread_t threadid;
    struct audio_usecase *uc_info;
    int count = 0;
    threadid = pthread_self();
    ALOGV("%s: Entry", __func__);
    if (pthread_equal(handle.speaker_prot_threadid, threadid) || !adev) {
        ALOGE("%s: Invalid params", __func__);
        return;
    }
    uc_info = get_usecase_from_list(adev, USECASE_AUDIO_SPKR_CALIB_RX);
    if (uc_info) {
            pthread_mutex_lock(&handle.mutex_spkr_prot);
            pthread_mutex_lock(&handle.spkr_calib_cancelack_mutex);
            handle.cancel_spkr_calib = 1;
            pthread_cond_signal(&handle.spkr_calib_cancel);
            pthread_mutex_unlock(&handle.mutex_spkr_prot);
            pthread_cond_wait(&handle.spkr_calibcancel_ack,
            &handle.spkr_calib_cancelack_mutex);
            pthread_mutex_unlock(&handle.spkr_calib_cancelack_mutex);
            pthread_mutex_unlock(&handle.mutex_spkr_prot);
    }
    ALOGV("%s: Exit", __func__);
}

static bool is_speaker_in_use(unsigned long *sec)
{
    struct timespec temp;
    if (!sec) {
        ALOGE("%s: Invalid params", __func__);
        return true;
    }
     if (handle.spkr_in_use) {
        *sec = 0;
         return true;
     } else {
         clock_gettime(CLOCK_MONOTONIC, &temp);
         *sec = temp.tv_sec - handle.spkr_last_time_used.tv_sec;
         return false;
     }
}


static int spkr_calibrate(int t0)
{
    struct audio_device *adev = handle.adev_handle;
    struct msm_spk_prot_cfg protCfg;
    struct msm_spk_prot_status status;
    bool cleanup = false, disable_rx = false, disable_tx = false;
    int acdb_fd = -1;
    struct audio_usecase *uc_info_rx = NULL, *uc_info_tx = NULL;
    int32_t pcm_dev_rx_id = -1, pcm_dev_tx_id = -1;
    struct timespec ts;

    if (!adev) {
        ALOGE("%s: Invalid params", __func__);
        return -EINVAL;
    }
    if (!list_empty(&adev->usecase_list)) {
        ALOGD("%s: Usecase present retry speaker protection", __func__);
        return -EAGAIN;
    }
    acdb_fd = open("/dev/msm_acdb",O_RDWR | O_NONBLOCK);
    if (acdb_fd < 0) {
        ALOGE("%s: spkr_prot_thread open msm_acdb failed", __func__);
        return -ENODEV;
    } else {
        protCfg.mode = MSM_SPKR_PROT_CALIBRATION_IN_PROGRESS;
        protCfg.t0 = t0;
        if (ioctl(acdb_fd, AUDIO_SET_SPEAKER_PROT, &protCfg)) {
            ALOGE("%s: spkr_prot_thread set failed AUDIO_SET_SPEAKER_PROT",
            __func__);
            status.status = -ENODEV;
            goto exit;
        }
    }
    uc_info_rx = (struct audio_usecase *)calloc(1, sizeof(struct audio_usecase));
    if (!uc_info_rx) {
        return -ENOMEM;
    }
    uc_info_rx->id = USECASE_AUDIO_SPKR_CALIB_RX;
    uc_info_rx->type = PCM_PLAYBACK;
    uc_info_rx->in_snd_device = SND_DEVICE_NONE;
    uc_info_rx->out_snd_device = SND_DEVICE_OUT_SPEAKER_PROTECTED;
    pthread_mutex_lock(&adev->lock);
    disable_rx = true;
    enable_snd_device(adev, SND_DEVICE_OUT_SPEAKER_PROTECTED);
    enable_audio_route(adev, uc_info_rx);
    pthread_mutex_unlock(&adev->lock);

    pcm_dev_rx_id = platform_get_pcm_device_id(uc_info_rx->id, PCM_PLAYBACK);
    ALOGV("%s: pcm device id %d", __func__, pcm_dev_rx_id);
    if (pcm_dev_rx_id < 0) {
        ALOGE("%s: Invalid pcm device for usecase (%d)",
              __func__, uc_info_rx->id);
        status.status = -ENODEV;
        goto exit;
    }
    handle.pcm_rx = handle.pcm_tx = NULL;
    handle.pcm_rx = pcm_open(adev->snd_card,
                             pcm_dev_rx_id,
                             PCM_OUT, &pcm_config_skr_prot);
    if (handle.pcm_rx && !pcm_is_ready(handle.pcm_rx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(handle.pcm_rx));
        status.status = -EIO;
        goto exit;
    }
    uc_info_tx = (struct audio_usecase *)
    calloc(1, sizeof(struct audio_usecase));
    if (!uc_info_tx) {
        status.status = -ENOMEM;
        goto exit;
    }
    uc_info_tx->id = USECASE_AUDIO_SPKR_CALIB_TX;
    uc_info_tx->type = PCM_CAPTURE;
    uc_info_tx->in_snd_device = SND_DEVICE_NONE;
    uc_info_tx->out_snd_device = SND_DEVICE_NONE;

    pthread_mutex_lock(&adev->lock);
    disable_tx = true;
    enable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
    enable_audio_route(adev, uc_info_tx);
    pthread_mutex_unlock(&adev->lock);

    pcm_dev_tx_id = platform_get_pcm_device_id(uc_info_tx->id, PCM_CAPTURE);
    if (pcm_dev_tx_id < 0) {
        ALOGE("%s: Invalid pcm device for usecase (%d)",
              __func__, uc_info_tx->id);
        status.status = -ENODEV;
        goto exit;
    }
    handle.pcm_tx = pcm_open(adev->snd_card,
                             pcm_dev_tx_id,
                             PCM_IN, &pcm_config_skr_prot);
    if (handle.pcm_tx && !pcm_is_ready(handle.pcm_tx)) {
        ALOGE("%s: %s", __func__, pcm_get_error(handle.pcm_tx));
        status.status = -EIO;
        goto exit;
    }
    if (pcm_start(handle.pcm_rx) < 0) {
        ALOGE("%s: pcm start for RX failed", __func__);
        status.status = -EINVAL;
        goto exit;
    }
    if (pcm_start(handle.pcm_tx) < 0) {
        ALOGE("%s: pcm start for TX failed", __func__);
        status.status = -EINVAL;
        goto exit;
    }
    cleanup = true;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (SLEEP_AFTER_CALIB_START/1000);
    ts.tv_nsec = 0;
    (void)pthread_cond_timedwait(&handle.spkr_calib_cancel,
        &handle.mutex_spkr_prot, &ts);
    ALOGD("%s: Speaker calibration done", __func__);
    cleanup = true;
    pthread_mutex_lock(&handle.spkr_calib_cancelack_mutex);
    if (handle.cancel_spkr_calib) {
        status.status = -EAGAIN;
        goto exit;
    }
    if (acdb_fd > 0) {
        status.status = -EINVAL;
        while (!ioctl(acdb_fd, AUDIO_GET_SPEAKER_PROT,&status)) {
            /*sleep for 200 ms to check for status check*/
            if (!status.status) {
                ALOGD("%s: spkr_prot_thread calib Success R0 %d",
                 __func__, status.r0);
                FILE *fp;
                fp = fopen(CALIB_FILE,"wb");
                if (!fp) {
                    ALOGE("%s: spkr_prot_thread File open failed %s",
                    __func__, strerror(errno));
                    status.status = -ENODEV;
                } else {
                    fwrite(&status.r0, sizeof(status.r0),1,fp);
                    fwrite(&protCfg.t0, sizeof(protCfg.t0),1,fp);
                    fclose(fp);
                }
                break;
            } else if (status.status == -EAGAIN) {
                  ALOGD("%s: spkr_prot_thread try again", __func__);
                  usleep(WAIT_FOR_GET_CALIB_STATUS);
            } else {
                ALOGE("%s: spkr_prot_thread get failed status %d",
                __func__, status.status);
                break;
            }
        }
exit:
        if (handle.pcm_rx)
            pcm_close(handle.pcm_rx);
        handle.pcm_rx = NULL;
        if (handle.pcm_tx)
            pcm_close(handle.pcm_tx);
        handle.pcm_tx = NULL;
        pthread_mutex_lock(&adev->lock);
        if (disable_rx) {
            disable_snd_device(adev, SND_DEVICE_OUT_SPEAKER_PROTECTED);
            disable_audio_route(adev, uc_info_rx);
        }
        if (disable_tx) {
            disable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
            disable_audio_route(adev, uc_info_tx);
        }
        pthread_mutex_unlock(&adev->lock);

        if (!status.status) {
            protCfg.mode = MSM_SPKR_PROT_CALIBRATED;
            protCfg.r0 = status.r0;
            if (ioctl(acdb_fd, AUDIO_SET_SPEAKER_PROT, &protCfg))
                ALOGE("%s: spkr_prot_thread disable calib mode", __func__);
            else
                handle.spkr_prot_mode = MSM_SPKR_PROT_CALIBRATED;
        } else {
            protCfg.mode = MSM_SPKR_PROT_NOT_CALIBRATED;
            handle.spkr_prot_mode = MSM_SPKR_PROT_NOT_CALIBRATED;
            if (ioctl(acdb_fd, AUDIO_SET_SPEAKER_PROT, &protCfg))
                ALOGE("%s: spkr_prot_thread disable calib mode failed", __func__);
        }
        if (acdb_fd > 0)
            close(acdb_fd);
        if (uc_info_rx) free(uc_info_rx);
        if (uc_info_tx) free(uc_info_tx);
        if (cleanup) {
            if (handle.cancel_spkr_calib)
                pthread_cond_signal(&handle.spkr_calibcancel_ack);
            handle.cancel_spkr_calib = 0;
            pthread_mutex_unlock(&handle.spkr_calib_cancelack_mutex);
        }
    }
    return status.status;
}

static void* spkr_calibration_thread()
{
    unsigned long sec = 0;
    int t0;
    bool goahead = false;
    struct msm_spk_prot_cfg protCfg;
    FILE *fp;
    int acdb_fd;
    struct audio_device *adev = handle.adev_handle;

    handle.speaker_prot_threadid = pthread_self();
    ALOGD("spkr_prot_thread enable prot Entry");
    acdb_fd = open("/dev/msm_acdb",O_RDWR | O_NONBLOCK);
    if (acdb_fd > 0) {
        /*Set processing mode with t0/r0*/
        protCfg.mode = MSM_SPKR_PROT_NOT_CALIBRATED;
        if (ioctl(acdb_fd, AUDIO_SET_SPEAKER_PROT, &protCfg)) {
            ALOGE("%s: spkr_prot_thread enable prot failed", __func__);
            handle.spkr_prot_mode = MSM_SPKR_PROT_DISABLED;
            close(acdb_fd);
        } else
            handle.spkr_prot_mode = MSM_SPKR_PROT_NOT_CALIBRATED;
    } else {
        handle.spkr_prot_mode = MSM_SPKR_PROT_DISABLED;
        ALOGE("%s: Failed to open acdb node", __func__);
    }
    if (handle.spkr_prot_mode == MSM_SPKR_PROT_DISABLED) {
        ALOGD("%s: Speaker protection disabled", __func__);
        pthread_exit(0);
        return NULL;
    }

    fp = fopen(CALIB_FILE,"rb");
    if (fp) {
        fread(&protCfg.r0,sizeof(protCfg.r0),1,fp);
        ALOGD("%s: spkr_prot_thread r0 value %d", __func__, protCfg.r0);
        fread(&protCfg.t0, sizeof(protCfg.t0), 1, fp);
        ALOGD("%s: spkr_prot_thread t0 value %d", __func__, protCfg.t0);
        fclose(fp);
        /*Valid tempature range: -30C to 80C(in q6 format)
          Valid Resistance range: 2 ohms to 40 ohms(in q24 format)*/
        if (protCfg.t0 > MIN_SPKR_TEMP_Q6 &&
            protCfg.t0 < MAX_SPKR_TEMP_Q6 &&
            protCfg.r0 >= MIN_RESISTANCE_SPKR_Q24
            && protCfg.r0 < MAX_RESISTANCE_SPKR_Q24) {
            ALOGD("%s: Spkr calibrated", __func__);
            protCfg.mode = MSM_SPKR_PROT_CALIBRATED;
            if (ioctl(acdb_fd, AUDIO_SET_SPEAKER_PROT, &protCfg)) {
                ALOGE("%s: enable prot failed", __func__);
                handle.spkr_prot_mode = MSM_SPKR_PROT_DISABLED;
            } else
                handle.spkr_prot_mode = MSM_SPKR_PROT_CALIBRATED;
            close(acdb_fd);
            pthread_exit(0);
            return NULL;
        }
        close(acdb_fd);
    }

    while (1) {
        ALOGV("%s: start calibration", __func__);
        if (!handle.thermal_client_request("spkr",1)) {
            ALOGD("%s: wait for callback from thermal daemon", __func__);
            pthread_mutex_lock(&handle.spkr_prot_thermalsync_mutex);
            pthread_cond_wait(&handle.spkr_prot_thermalsync,
            &handle.spkr_prot_thermalsync_mutex);
            /*Convert temp into q6 format*/
            t0 = (handle.spkr_prot_t0 * (1 << 6));
            pthread_mutex_unlock(&handle.spkr_prot_thermalsync_mutex);
            if (t0 < MIN_SPKR_TEMP_Q6 || t0 > MAX_SPKR_TEMP_Q6) {
                ALOGE("%s: Calibration temparature error %d", __func__,
                      handle.spkr_prot_t0);
                continue;
            }
            ALOGD("%s: Request t0 success value %d", __func__,
            handle.spkr_prot_t0);
        } else {
            ALOGE("%s: Request t0 failed", __func__);
            /*Assume safe value for temparature*/
            t0 = SAFE_SPKR_TEMP_Q6;
        }
        goahead = false;
        pthread_mutex_lock(&handle.mutex_spkr_prot);
        if (is_speaker_in_use(&sec)) {
            ALOGD("%s: Speaker in use retry calibration", __func__);
            pthread_mutex_unlock(&handle.mutex_spkr_prot);
            continue;
        } else {
            ALOGD("%s: speaker idle %ld", __func__, sec);
            if (sec < MIN_SPKR_IDLE_SEC) {
                ALOGD("%s: speaker idle is less retry", __func__);
                pthread_mutex_unlock(&handle.mutex_spkr_prot);
                continue;
            }
            goahead = true;
        }
        if (!list_empty(&adev->usecase_list)) {
            ALOGD("%s: Usecase active re-try calibration", __func__);
            goahead = false;
            pthread_mutex_unlock(&handle.mutex_spkr_prot);
        }
        if (goahead) {
                int status;
                status = spkr_calibrate(t0);
                if (status == -EAGAIN) {
                    ALOGE("%s: failed to calibrate try again %s",
                    __func__, strerror(status));
                    pthread_mutex_unlock(&handle.mutex_spkr_prot);
                    continue;
                } else {
                    ALOGE("%s: calibrate status %s", __func__, strerror(status));
                }
                ALOGD("%s: spkr_prot_thread end calibration", __func__);
                pthread_mutex_unlock(&handle.mutex_spkr_prot);
                break;
        }
    }
    if (handle.thermal_client_handle)
        handle.thermal_client_unregister_callback(handle.thermal_client_handle);
    handle.thermal_client_handle = 0;
    if (handle.thermal_handle)
        dlclose(handle.thermal_handle);
    handle.thermal_handle = NULL;
    pthread_exit(0);
    return NULL;
}

static int thermal_client_callback(int temp)
{
    pthread_mutex_lock(&handle.spkr_prot_thermalsync_mutex);
    ALOGD("%s: spkr_prot set t0 %d and signal", __func__, temp);
    if (handle.spkr_prot_mode == MSM_SPKR_PROT_NOT_CALIBRATED)
        handle.spkr_prot_t0 = temp;
    pthread_cond_signal(&handle.spkr_prot_thermalsync);
    pthread_mutex_unlock(&handle.spkr_prot_thermalsync_mutex);
    return 0;
}

void audio_extn_spkr_prot_init(void *adev)
{
    char value[PROPERTY_VALUE_MAX];
    ALOGD("%s: Initialize speaker protection module", __func__);
    memset(&handle, 0, sizeof(handle));
    if (!adev) {
        ALOGE("%s: Invalid params", __func__);
        return;
    }
    property_get("persist.speaker.prot.enable", value, "");
    handle.spkr_prot_enable = false;
    if (!strncmp("true", value, 4))
       handle.spkr_prot_enable = true;
    if (!handle.spkr_prot_enable) {
        ALOGD("%s: Speaker protection disabled", __func__);
        return;
    }
    handle.adev_handle = adev;
    handle.spkr_prot_mode = MSM_SPKR_PROT_DISABLED;
    handle.spkr_processing_state = SPKR_PROCESSING_IN_IDLE;
    handle.spkr_prot_t0 = -1;
    pthread_cond_init(&handle.spkr_prot_thermalsync, NULL);
    pthread_cond_init(&handle.spkr_calib_cancel, NULL);
    pthread_cond_init(&handle.spkr_calibcancel_ack, NULL);
    pthread_mutex_init(&handle.mutex_spkr_prot, NULL);
    pthread_mutex_init(&handle.spkr_calib_cancelack_mutex, NULL);
    pthread_mutex_init(&handle.spkr_prot_thermalsync_mutex, NULL);
    handle.thermal_handle = dlopen("/vendor/lib/libthermalclient.so",
            RTLD_NOW);
    if (!handle.thermal_handle) {
        ALOGE("%s: DLOPEN for thermal client failed", __func__);
    } else {
        /*Query callback function symbol*/
        handle.client_register_callback =
       (int (*)(char *, int (*)(int),void *))
        dlsym(handle.thermal_handle, "thermal_client_register_callback");
        handle.thermal_client_unregister_callback =
        (void (*)(int) )
        dlsym(handle.thermal_handle, "thermal_client_unregister_callback");
        if (!handle.client_register_callback ||
            !handle.thermal_client_unregister_callback) {
            ALOGE("%s: DLSYM thermal_client_register_callback failed", __func__);
        } else {
            /*Register callback function*/
            handle.thermal_client_handle =
            handle.client_register_callback("spkr", thermal_client_callback, NULL);
            if (!handle.thermal_client_handle) {
                ALOGE("%s: client_register_callback failed", __func__);
            } else {
                ALOGD("%s: spkr_prot client_register_callback success", __func__);
                handle.thermal_client_request = (int (*)(char *, int))
                dlsym(handle.thermal_handle, "thermal_client_request");
            }
        }
    }
    if (handle.thermal_client_request) {
        ALOGD("%s: Create calibration thread", __func__);
        (void)pthread_create(&handle.spkr_calibration_thread,
        (const pthread_attr_t *) NULL, spkr_calibration_thread, &handle);
    } else {
        ALOGE("%s: thermal_client_request failed", __func__);
        if (handle.thermal_client_handle)
            handle.thermal_client_unregister_callback(handle.thermal_client_handle);
        if (handle.thermal_handle)
            dlclose(handle.thermal_handle);
        handle.thermal_handle = NULL;
        handle.spkr_prot_enable = false;
    }

    if (handle.spkr_prot_enable) {
        char platform[PROPERTY_VALUE_MAX];
        property_get("ro.board.platform", platform, "");
        if (!strncmp("apq8084", platform, sizeof("apq8084"))) {
            platform_set_snd_device_backend(SND_DEVICE_OUT_VOICE_SPEAKER,
                                            "speaker-protected");
        }
    }
}

int audio_extn_spkr_prot_start_processing(snd_device_t snd_device)
{
    struct audio_usecase uc_info_tx;
    struct audio_device *adev = handle.adev_handle;
    int32_t pcm_dev_tx_id = -1, ret = 0;

    ALOGV("%s: Entry", __func__);
    /* cancel speaker calibration */
    if (!adev) {
       ALOGE("%s: Invalid params", __func__);
       return -EINVAL;
    }
    spkr_prot_calib_cancel(adev);
    spkr_prot_set_spkrstatus(true);
    if (platform_send_audio_calibration(adev->platform,
        SND_DEVICE_OUT_SPEAKER_PROTECTED) < 0) {
        adev->snd_dev_ref_cnt[snd_device]--;
        return -EINVAL;
    }
    ALOGV("%s: snd_device(%d: %s)", __func__, snd_device,
         platform_get_snd_device_name(SND_DEVICE_OUT_SPEAKER_PROTECTED));
    audio_route_apply_and_update_path(adev->audio_route,
        platform_get_snd_device_name(SND_DEVICE_OUT_SPEAKER_PROTECTED));

    pthread_mutex_lock(&handle.mutex_spkr_prot);
    if (handle.spkr_processing_state == SPKR_PROCESSING_IN_IDLE) {
        memset(&uc_info_tx, 0 , sizeof(uc_info_tx));
        uc_info_tx.id = USECASE_AUDIO_SPKR_CALIB_TX;
        uc_info_tx.type = PCM_CAPTURE;
        uc_info_tx.in_snd_device = SND_DEVICE_NONE;
        uc_info_tx.out_snd_device = SND_DEVICE_NONE;
        handle.pcm_tx = NULL;

        enable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
        enable_audio_route(adev, &uc_info_tx);

        pcm_dev_tx_id = platform_get_pcm_device_id(uc_info_tx.id, PCM_CAPTURE);
        if (pcm_dev_tx_id < 0) {
            ALOGE("%s: Invalid pcm device for usecase (%d)",
                  __func__, uc_info_tx.id);
            ret = -ENODEV;
            goto exit;
        }
        handle.pcm_tx = pcm_open(adev->snd_card,
                                 pcm_dev_tx_id,
                                 PCM_IN, &pcm_config_skr_prot);
        if (handle.pcm_tx && !pcm_is_ready(handle.pcm_tx)) {
            ALOGE("%s: %s", __func__, pcm_get_error(handle.pcm_tx));
            ret = -EIO;
            goto exit;
        }
        if (pcm_start(handle.pcm_tx) < 0) {
            ALOGE("%s: pcm start for TX failed", __func__);
            ret = -EINVAL;
        }
    }

exit:
   /* Clear VI feedback cal and replace with handset MIC  */
   platform_send_audio_calibration(adev->platform,
        SND_DEVICE_IN_HANDSET_MIC);
    if (ret) {
        if (handle.pcm_tx)
            pcm_close(handle.pcm_tx);
        handle.pcm_tx = NULL;
        disable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
        disable_audio_route(adev, &uc_info_tx);
    } else
        handle.spkr_processing_state = SPKR_PROCESSING_IN_PROGRESS;
    pthread_mutex_unlock(&handle.mutex_spkr_prot);
    ALOGV("%s: Exit", __func__);
    return ret;
}

void audio_extn_spkr_prot_stop_processing()
{
    struct audio_usecase uc_info_tx;
    struct audio_device *adev = handle.adev_handle;
    ALOGV("%s: Entry", __func__);
    spkr_prot_set_spkrstatus(false);
    pthread_mutex_lock(&handle.mutex_spkr_prot);
    if (adev && handle.spkr_processing_state == SPKR_PROCESSING_IN_PROGRESS) {
        memset(&uc_info_tx, 0 , sizeof(uc_info_tx));
        uc_info_tx.id = USECASE_AUDIO_SPKR_CALIB_TX;
        uc_info_tx.type = PCM_CAPTURE;
        uc_info_tx.in_snd_device = SND_DEVICE_NONE;
        uc_info_tx.out_snd_device = SND_DEVICE_NONE;
        if (handle.pcm_tx)
            pcm_close(handle.pcm_tx);
        handle.pcm_tx = NULL;
        disable_snd_device(adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);
        disable_audio_route(adev, &uc_info_tx);
    }
    handle.spkr_processing_state = SPKR_PROCESSING_IN_IDLE;
    pthread_mutex_unlock(&handle.mutex_spkr_prot);
    audio_route_reset_and_update_path(adev->audio_route,
    if (adev)
        audio_route_reset_and_update_path(adev->audio_route,
            platform_get_snd_device_name(SND_DEVICE_OUT_SPEAKER_PROTECTED));
    ALOGV("%s: Exit", __func__);
}

bool audio_extn_spkr_prot_is_enabled()
{
    return handle.spkr_prot_enable;
}
#endif /*SPKR_PROT_ENABLED*/
