/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "msm8660_platform"
#define LOG_NDEBUG 0
/*#define LOG_NDDEBUG 0*/

#include <stdlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <cutils/log.h>
#include <sys/ioctl.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <audio_hw.h>
#include <platform_api.h>
#include "platform.h"
#include "audio_extn.h"
#include "voice_extn.h"
#include "sound/compress_params.h"
#ifdef HWDEP_CAL_ENABLED
#include "sound/msmcal-hwdep.h"
#endif

#define UNUSED(a) ((void)(a))

#define MIXER_XML_PATH "/system/etc/mixer_paths.xml"
#define LIB_ACDB_LOADER "libacdbloader.so"
#define AUDIO_DATA_BLOCK_MIXER_CTL "HDMI EDID"

#define MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE (256 * 1024)
#define MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE (2 * 1024)
#define COMPRESS_OFFLOAD_FRAGMENT_SIZE_FOR_AV_STREAMING (2 * 1024)
#define COMPRESS_OFFLOAD_FRAGMENT_SIZE (32 * 1024)

/* Used in calculating fragment size for pcm offload */
#define PCM_OFFLOAD_BUFFER_DURATION_FOR_AV 1000 /* 1 sec */
#define PCM_OFFLOAD_BUFFER_DURATION_FOR_AV_STREAMING 80 /* 80 millisecs */

/* MAX PCM fragment size cannot be increased  further due
 * to flinger's cblk size of 1mb,and it has to be a multiple of
 * 24 - lcm of channels supported by DSP
 */
#define MAX_PCM_OFFLOAD_FRAGMENT_SIZE (240 * 1024)
#define MIN_PCM_OFFLOAD_FRAGMENT_SIZE (4 * 1024)

#define ALIGN( num, to ) (((num) + (to-1)) & (~(to-1)))
/*
 * This file will have a maximum of 38 bytes:
 *
 * 4 bytes: number of audio blocks
 * 4 bytes: total length of Short Audio Descriptor (SAD) blocks
 * Maximum 10 * 3 bytes: SAD blocks
 */
#define MAX_SAD_BLOCKS      10
#define SAD_BLOCK_SIZE      3

/* EDID format ID for LPCM audio */
#define EDID_FORMAT_LPCM    1

/* Retry for delay in FW loading*/
#define RETRY_NUMBER 10
#define RETRY_US 500000
#define MAX_SND_CARD 1

#define SAMPLE_RATE_8KHZ  8000
#define SAMPLE_RATE_16KHZ 16000

#define AUDIO_PARAMETER_KEY_FLUENCE_TYPE  "fluence"
#define AUDIO_PARAMETER_KEY_BTSCO         "bt_samplerate"
#define AUDIO_PARAMETER_KEY_SLOWTALK      "st_enable"
#define AUDIO_PARAMETER_KEY_VOLUME_BOOST  "volume_boost"
#define MAX_CAL_NAME 20

#ifdef HWDEP_CAL_ENABLED
char cal_name_info[WCD9XXX_MAX_CAL][MAX_CAL_NAME] = {
        [WCD9XXX_ANC_CAL] = "anc_cal",
        [WCD9XXX_MBHC_CAL] = "mbhc_cal",
};
#endif

#define TOSTRING_(x) #x
#define TOSTRING(x) TOSTRING_(x)

struct audio_block_header
{
    int reserved;
    int length;
};

/* Audio calibration related functions */
typedef void (*acdb_deallocate_t)();
typedef int  (*acdb_init_t)();
typedef void (*acdb_send_audio_cal_t)(int, int);
typedef void (*acdb_send_voice_cal_t)(int, int);
typedef int (*acdb_loader_get_calibration_t)(char *attr, int size, void *data);
typedef void (*acdb_mapper_get_acdb_id_from_dev_name_t)(char *name, int *id);
acdb_loader_get_calibration_t acdb_loader_get_calibration;
acdb_mapper_get_acdb_id_from_dev_name_t acdb_mapper_get_acdb_id_from_dev_name;

struct platform_data {
    struct audio_device *adev;
    bool fluence_in_spkr_mode;
    bool fluence_in_voice_call;
    bool fluence_in_voice_rec;
    bool fluence_in_audio_rec;
    int  fluence_type;
    char fluence_cap[PROPERTY_VALUE_MAX];
    int  btsco_sample_rate;
    bool slowtalk;
    /* Audio calibration related functions */
    void                       *acdb_handle;
    acdb_init_t                acdb_init;
    acdb_deallocate_t          acdb_deallocate;
    acdb_send_audio_cal_t      acdb_send_audio_cal;
    acdb_send_voice_cal_t      acdb_send_voice_cal;

    void *hw_info;
    struct msm_data *msm;
    struct device_table *device_list;

    int max_vol_index;
};

static int pcm_device_table[AUDIO_USECASE_MAX][2] = {
    [USECASE_AUDIO_PLAYBACK_DEEP_BUFFER] = {DEEP_BUFFER_PCM_DEVICE,
                                            DEEP_BUFFER_PCM_DEVICE},
    [USECASE_AUDIO_PLAYBACK_LOW_LATENCY] = {LOWLATENCY_PCM_DEVICE,
                                           LOWLATENCY_PCM_DEVICE},
    [USECASE_AUDIO_PLAYBACK_MULTI_CH] = {MULTIMEDIA2_PCM_DEVICE,
                                        MULTIMEDIA2_PCM_DEVICE},
    [USECASE_AUDIO_RECORD] = {AUDIO_RECORD_PCM_DEVICE, AUDIO_RECORD_PCM_DEVICE},
    [USECASE_AUDIO_RECORD_LOW_LATENCY] = {LOWLATENCY_PCM_DEVICE,
                                          LOWLATENCY_PCM_DEVICE},
    [USECASE_VOICE_CALL] = {VOICE_CALL_PCM_DEVICE, VOICE_CALL_PCM_DEVICE},
};

/* Array to store sound devices */
static char * device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = "none",
    /* Playback sound devices */
    [SND_DEVICE_OUT_HANDSET] = "handset",
    [SND_DEVICE_OUT_SPEAKER] = "speaker",
    [SND_DEVICE_OUT_SPEAKER_REVERSE] = "speaker-reverse",
    [SND_DEVICE_OUT_HEADPHONES] = "headphones",
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = "speaker-and-headphones",
    [SND_DEVICE_OUT_VOICE_HANDSET] = "voice-handset",
    [SND_DEVICE_OUT_VOICE_SPEAKER] = "voice-speaker",
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = "voice-headphones",
    [SND_DEVICE_OUT_HDMI] = "hdmi",
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = "speaker-and-hdmi",
    [SND_DEVICE_OUT_BT_SCO] = "bt-sco-headset",
    [SND_DEVICE_OUT_BT_SCO_WB] = "bt-sco-headset-wb",
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = "voice-tty-full-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = "voice-tty-vco-headphones",
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = "voice-tty-hco-handset",
    [SND_DEVICE_OUT_VOIP_HANDSET] = "voip-handset",
    [SND_DEVICE_OUT_VOIP_SPEAKER] = "voip-speaker",
    [SND_DEVICE_OUT_VOIP_HEADPHONES] = "voip-headset",
#ifdef DOCK_SUPPORT
    [SND_DEVICE_OUT_DOCK] = "dock",
    [SND_DEVICE_OUT_SPEAKER_AND_DOCK] = "speaker-and-dock",
#endif

    /* Capture sound devices */
    [SND_DEVICE_IN_HANDSET_MIC] = "handset-mic",
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = "handset-mic",
    [SND_DEVICE_IN_HANDSET_MIC_NS] = "handset-mic",
    [SND_DEVICE_IN_HANDSET_MIC_AEC_NS] = "handset-mic",
    [SND_DEVICE_IN_HANDSET_DMIC] = "dmic-endfire",
    [SND_DEVICE_IN_HANDSET_DMIC_AEC] = "dmic-endfire",
    [SND_DEVICE_IN_HANDSET_DMIC_NS] = "dmic-endfire",
    [SND_DEVICE_IN_HANDSET_DMIC_AEC_NS] = "dmic-endfire",
    [SND_DEVICE_IN_SPEAKER_MIC] = "speaker-mic",
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = "speaker-mic",
    [SND_DEVICE_IN_SPEAKER_MIC_NS] = "speaker-mic",
    [SND_DEVICE_IN_SPEAKER_MIC_AEC_NS] = "speaker-mic",
    [SND_DEVICE_IN_SPEAKER_DMIC] = "speaker-dmic-endfire",
    [SND_DEVICE_IN_SPEAKER_DMIC_AEC] = "speaker-dmic-endfire",
    [SND_DEVICE_IN_SPEAKER_DMIC_NS] = "speaker-dmic-endfire",
    [SND_DEVICE_IN_SPEAKER_DMIC_AEC_NS] = "speaker-dmic-endfire",
    [SND_DEVICE_IN_HEADSET_MIC] = "headset-mic",
    [SND_DEVICE_IN_HEADSET_MIC_FLUENCE] = "headset-mic",
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = "voice-speaker-mic",
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = "voice-headset-mic",
    [SND_DEVICE_IN_BT_SCO_MIC] = "bt-sco-mic",
    [SND_DEVICE_IN_BT_SCO_MIC_WB] = "bt-sco-mic-wb",
    [SND_DEVICE_IN_CAMCORDER_MIC] = "camcorder-mic",
    [SND_DEVICE_IN_VOICE_DMIC] = "voice-dmic-ef",
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC] = "voice-speaker-dmic-ef",
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = "voice-tty-full-headset-mic",
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = "voice-tty-vco-handset-mic",
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = "voice-tty-hco-headset-mic",
    [SND_DEVICE_IN_VOICE_REC_MIC] = "voice-rec-mic",
    [SND_DEVICE_IN_VOICE_REC_MIC_NS] = "voice-rec-mic",
    [SND_DEVICE_IN_VOICE_REC_DMIC_STEREO] = "voice-rec-dmic-ef",
    [SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE] = "voice-rec-dmic-ef",
    [SND_DEVICE_IN_VOIP_HANDSET_MIC] = "voip-handset-mic",
    [SND_DEVICE_IN_VOIP_SPEAKER_MIC] = "voip-speaker-mic",
    [SND_DEVICE_IN_VOIP_HEADSET_MIC] = "voip-headset-mic",
};

/* ACDB IDs (audio DSP path configuration IDs) for each sound device */
static int acdb_device_table[SND_DEVICE_MAX] = {
    [SND_DEVICE_NONE] = -1,
    [SND_DEVICE_OUT_HANDSET] = 0,
    [SND_DEVICE_OUT_SPEAKER] = 2,
    [SND_DEVICE_OUT_SPEAKER_REVERSE] = 2,
    [SND_DEVICE_OUT_HEADPHONES] = 4,
    [SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES] = 13,
    [SND_DEVICE_OUT_VOICE_HANDSET] = 0,
    [SND_DEVICE_OUT_VOICE_SPEAKER] = 2,
    [SND_DEVICE_OUT_VOICE_HEADPHONES] = 4,
    [SND_DEVICE_OUT_HDMI] = 15,
    [SND_DEVICE_OUT_SPEAKER_AND_HDMI] = 2,
    [SND_DEVICE_OUT_BT_SCO] = 17,
    [SND_DEVICE_OUT_BT_SCO_WB] = 17,
    [SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES] = 11,
    [SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES] = 11,
    [SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET] = 0,

    [SND_DEVICE_IN_HANDSET_MIC] = 1,
    [SND_DEVICE_IN_HANDSET_MIC_AEC] = 1,
    [SND_DEVICE_IN_HANDSET_MIC_NS] = 1,
    [SND_DEVICE_IN_HANDSET_MIC_AEC_NS] = 1,
    [SND_DEVICE_IN_HANDSET_DMIC] = 9,
    [SND_DEVICE_IN_HANDSET_DMIC_AEC] = 9,
    [SND_DEVICE_IN_HANDSET_DMIC_NS] = 9,
    [SND_DEVICE_IN_HANDSET_DMIC_AEC_NS] = 9,
    [SND_DEVICE_IN_SPEAKER_MIC] = 3,
    [SND_DEVICE_IN_SPEAKER_MIC_AEC] = 3,
    [SND_DEVICE_IN_SPEAKER_MIC_NS] = 3,
    [SND_DEVICE_IN_SPEAKER_MIC_AEC_NS] = 3,
    [SND_DEVICE_IN_SPEAKER_DMIC] = 10,
    [SND_DEVICE_IN_SPEAKER_DMIC_AEC] = 10,
    [SND_DEVICE_IN_SPEAKER_DMIC_NS] = 10,
    [SND_DEVICE_IN_SPEAKER_DMIC_AEC_NS] = 10,
    [SND_DEVICE_IN_HEADSET_MIC] = 5,
    [SND_DEVICE_IN_HEADSET_MIC_FLUENCE] = 5,
    [SND_DEVICE_IN_VOICE_SPEAKER_MIC] = 3,
    [SND_DEVICE_IN_VOICE_HEADSET_MIC] = 5,
    [SND_DEVICE_IN_BT_SCO_MIC] = 18,
    [SND_DEVICE_IN_BT_SCO_MIC_WB] = 18,
    [SND_DEVICE_IN_CAMCORDER_MIC] = 105,
    [SND_DEVICE_IN_VOICE_DMIC] = 9,
    [SND_DEVICE_IN_VOICE_SPEAKER_DMIC] = 10,
    [SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC] = 12,
    [SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC] = 1,
    [SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC] = 12,
    [SND_DEVICE_IN_VOICE_REC_MIC] = 3,
    [SND_DEVICE_IN_VOICE_REC_MIC_NS] = 3,
    [SND_DEVICE_IN_VOICE_REC_DMIC_STEREO] = 10,
    [SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE] = 10,
};

struct name_to_index {
    char name[100];
    unsigned int index;
};

#define TO_NAME_INDEX(X)   #X, X

/* Used to get index from parsed sting */
static struct name_to_index snd_device_name_index[SND_DEVICE_MAX] = {
    {TO_NAME_INDEX(SND_DEVICE_OUT_HANDSET)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_SPEAKER)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_SPEAKER_REVERSE)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_HEADPHONES)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_HANDSET)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_SPEAKER)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_HEADPHONES)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_HDMI)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_SPEAKER_AND_HDMI)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_BT_SCO)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_BT_SCO_WB)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOIP_HANDSET)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOIP_SPEAKER)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_VOIP_HEADPHONES)},
#ifdef DOCK_SUPPORT
    {TO_NAME_INDEX(SND_DEVICE_OUT_DOCK)},
    {TO_NAME_INDEX(SND_DEVICE_OUT_SPEAKER_AND_DOCK)},
#endif
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_MIC_AEC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_MIC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_MIC_AEC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_DMIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_DMIC_AEC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_DMIC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HANDSET_DMIC_AEC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_MIC_AEC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_MIC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_MIC_AEC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_DMIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_DMIC_AEC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_DMIC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_SPEAKER_DMIC_AEC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HEADSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_HEADSET_MIC_FLUENCE)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_SPEAKER_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_HEADSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_BT_SCO_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_BT_SCO_MIC_WB)},
    {TO_NAME_INDEX(SND_DEVICE_IN_CAMCORDER_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_DMIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_SPEAKER_DMIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_REC_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_REC_MIC_NS)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_REC_DMIC_STEREO)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOIP_HANDSET_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOIP_SPEAKER_MIC)},
    {TO_NAME_INDEX(SND_DEVICE_IN_VOIP_HEADSET_MIC)},
};

static char * backend_table[SND_DEVICE_MAX] = {0};

static struct name_to_index usecase_name_index[AUDIO_USECASE_MAX] = {
    {TO_NAME_INDEX(USECASE_AUDIO_PLAYBACK_DEEP_BUFFER)},
    {TO_NAME_INDEX(USECASE_AUDIO_PLAYBACK_LOW_LATENCY)},
    {TO_NAME_INDEX(USECASE_AUDIO_PLAYBACK_MULTI_CH)},
    {TO_NAME_INDEX(USECASE_AUDIO_RECORD)},
    {TO_NAME_INDEX(USECASE_AUDIO_RECORD_LOW_LATENCY)},
    {TO_NAME_INDEX(USECASE_VOICE_CALL)},
};

#define DEEP_BUFFER_PLATFORM_DELAY (29*1000LL)
#define LOW_LATENCY_PLATFORM_DELAY (13*1000LL)

#ifdef HWDEP_CAL_ENABLED
static int hw_util_open(int card_no)
{
    int fd = -1;
    char dev_name[256];

    snprintf(dev_name, sizeof(dev_name), "/dev/snd/hwC%uD%u",
                               card_no, WCD9XXX_CODEC_HWDEP_NODE);
    ALOGE("%s Opening device %s\n", __func__, dev_name);
    fd = open(dev_name, O_WRONLY);
    if (fd < 0) {
        ALOGE("%s: cannot open device '%s'\n", __func__, dev_name);
        return fd;
    }
    ALOGE("%s success", __func__);
    return fd;
}

struct param_data {
    int    use_case;
    int    acdb_id;
    int    get_size;
    int    buff_size;
    int    data_size;
    void   *buff;
};

static int send_codec_cal(acdb_loader_get_calibration_t acdb_loader_get_calibration, int fd)
{
    int ret = 0, type;

    for (type = WCD9XXX_ANC_CAL; type < WCD9XXX_MAX_CAL; type++) {
        struct wcdcal_ioctl_buffer codec_buffer;
        struct param_data calib;
        calib.get_size = 1;
        ret = acdb_loader_get_calibration(cal_name_info[type], sizeof(struct param_data),
                                                                 &calib);
        if (ret < 0) {
            ALOGE("%s get_calibration failed\n", __func__);
            return ret;
        }
        calib.get_size = 0;
        calib.buff = malloc(calib.buff_size);
        ret = acdb_loader_get_calibration(cal_name_info[type],
                              sizeof(struct param_data), &calib);
        if (ret < 0) {
            ALOGE("%s get_calibration failed\n", __func__);
            free(calib.buff);
            return ret;
        }
        codec_buffer.buffer = calib.buff;
        codec_buffer.size = calib.data_size;
        codec_buffer.cal_type = type;
        if (ioctl(fd, SNDRV_CTL_IOCTL_HWDEP_CAL_TYPE, &codec_buffer) < 0)
            ALOGE("Failed to call ioctl  for %s err=%d",
                                  cal_name_info[type], errno);
        ALOGE(" %s cal sent for %s", __func__, cal_name_info[type]);
        free(calib.buff);
    }
    return ret;
}

static void audio_hwdep_send_cal(struct platform_data *plat_data)
{
    int fd;

    fd = hw_util_open(plat_data->adev->snd_card);
    if (fd == -1) {
        ALOGE("%s error open\n", __func__);
        return;
    }
    acdb_loader_get_calibration = (acdb_loader_get_calibration_t)
          dlsym(plat_data->acdb_handle, "acdb_loader_get_calibration");

    if (acdb_loader_get_calibration == NULL) {
        ALOGE("%s: ERROR. dlsym Error:%s acdb_loader_get_calibration", __func__,
           dlerror());
        close(fd);
        return;
    }
    if (send_codec_cal(acdb_loader_get_calibration, fd) < 0)
        ALOGE("%s: Could not send anc cal", __FUNCTION__);
}
#endif

static void set_echo_reference(struct audio_device *adev, bool enable)
{
    if (enable)
        audio_route_apply_and_update_path(adev->audio_route, "echo-reference");
    else
        audio_route_reset_and_update_path(adev->audio_route, "echo-reference");

    ALOGV("Setting EC Reference: %d", enable);
}

static struct msm_data *open_msm_client()
{
    struct msm_data *msm = calloc(1, sizeof(struct msm_data));

    if (!msm) {
        ALOGE("failed to allocate msm_data mem");
        return NULL;
    }

    msm->msm_client = dlopen(LIB_MSM_CLIENT, RTLD_NOW);
    if (msm->msm_client == NULL) {
        ALOGE("%s: DLOPEN failed for %s", __func__, LIB_MSM_CLIENT);
        goto error;
    } else {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_MSM_CLIENT);

        msm->msm_set_voice_rx_vol = (msm_set_voice_rx_vol_t)dlsym(msm->msm_client,
                      "msm_set_voice_rx_vol");
        msm->msm_set_voice_tx_mute = (msm_set_voice_tx_mute_t)dlsym(msm->msm_client,
                      "msm_set_voice_tx_mute");
        msm->msm_start_voice = (msm_start_voice_t)dlsym(msm->msm_client,
                      "msm_start_voice");
        msm->msm_end_voice = (msm_end_voice_t)dlsym(msm->msm_client,
                      "msm_end_voice");
        msm->msm_mixer_open = (msm_mixer_open_t)dlsym(msm->msm_client,
                      "msm_mixer_open");
        msm->msm_mixer_close = (msm_mixer_close_t)dlsym(msm->msm_client,
                      "msm_mixer_close");
        msm->msm_reset_all_device = (msm_reset_all_device_t)dlsym(msm->msm_client,
                      "msm_reset_all_device");
#ifndef LEGACY_QCOM_VOICE
        msm->msm_get_voc_session = (msm_get_voc_session_t)dlsym(msm->msm_client,
                      "msm_get_voc_session");
        msm->msm_start_voice_ext = (msm_start_voice_ext_t)dlsym(msm->msm_client,
                      "msm_start_voice_ext");
        msm->msm_end_voice_ext = (msm_end_voice_ext_t)dlsym(msm->msm_client,
                      "msm_end_voice_ext");
        msm->msm_set_voice_tx_mute_ext = (msm_set_voice_tx_mute_ext_t)dlsym(msm->msm_client,
                      "msm_set_voice_tx_mute_ext");
        msm->msm_set_voice_rx_vol_ext = (msm_set_voice_rx_vol_ext_t)dlsym(msm->msm_client,
                      "msm_set_voice_rx_vol_ext");
#endif
        msm->msm_get_device_count = (msm_get_device_count_t)dlsym(msm->msm_client,
                      "msm_get_device_count");
        msm->msm_get_device_list = (msm_get_device_list_t)dlsym(msm->msm_client,
                      "msm_get_device_list");
        msm->msm_get_device = (msm_get_device_t)dlsym(msm->msm_client,
                      "msm_get_device");

        msm->voice_session_id = 0;

        if(msm->msm_mixer_open == NULL || msm->msm_mixer_open("/dev/snd/controlC0", 0) < 0)
          ALOGE("ERROR opening the device");

        //End any voice call if it exists. This is to ensure the next request
        //to voice call after a mediaserver crash or sub system restart
        //is not ignored by the voice driver.
        if (msm->msm_end_voice == NULL || msm->msm_end_voice() < 0)
          ALOGE("msm_end_voice() failed");

        if(msm->msm_reset_all_device == NULL || msm->msm_reset_all_device() < 0)
          ALOGE("msm_reset_all_device() failed");
    }
    return msm;

error:
    free(msm);
    msm = NULL;
    return msm;
}

void close_msm_client(struct msm_data *msm)
{
    if (msm != NULL) {
        if (msm->msm_mixer_close != NULL)
            msm->msm_mixer_close();

        //dlclose
        dlclose(LIB_MSM_CLIENT);

        free(msm);
        msm = NULL;
    }
}

void device_list_init(struct platform_data *plat_data)
{
    int rc, dev_cnt, i, index = 0;
    struct device_table *device_list = NULL;

    dev_cnt = plat_data->msm->msm_get_device_count();
    ALOGV("got device_count %d", dev_cnt);
    if (dev_cnt <= 0) {
        ALOGE("%s: NO devices registered", __func__);
        return;
    }

    device_list = calloc(1, sizeof(struct device_table));

    if (!device_list) {
        ALOGE("%s: failed to allocate device list", __func__);
        return;
    }

    device_list->name = plat_data->msm->msm_get_device_list();

    if (device_list->name == NULL) {
        ALOGE("%s: unable to load device list", __func__);
        free(device_list);
        device_list = NULL;
        return;
    }

    acdb_mapper_get_acdb_id_from_dev_name = (acdb_mapper_get_acdb_id_from_dev_name_t)
          dlsym(plat_data->acdb_handle, "acdb_mapper_get_acdb_id_from_dev_name");

    if (acdb_mapper_get_acdb_id_from_dev_name == NULL) {
        ALOGE("%s: ERROR. dlsym Error:%s acdb_mapper_get_acdb_id_from_dev_name", __func__,
           dlerror());
        free(device_list);
        device_list = NULL;
        return;
    }

    for(i = 0; i < MAX_DEVICE_COUNT; i++)
        device_list->dev_id[i] = -1;

    for(i = 0; i < dev_cnt; i++) {
        if(strcmp((char* )device_list->name[i],"handset_rx") == 0) {
            index = SND_DEVICE_OUT_HANDSET;
        }
#ifndef SAMSUNG_AUDIO
        else if(strcmp((char* )device_list->name[i],"handset_tx") == 0) {
            index = SND_DEVICE_IN_HANDSET_MIC;
        }
#endif
        else if((strcmp((char* )device_list->name[i],"speaker_stereo_rx") == 0) ||
                (strcmp((char* )device_list->name[i],"speaker_stereo_rx_playback") == 0) ||
                (strcmp((char* )device_list->name[i],"speaker_rx") == 0)) {
            index = SND_DEVICE_OUT_SPEAKER;
        }
        else if((strcmp((char* )device_list->name[i],"speaker_mono_tx") == 0) || (strcmp((char* )device_list->name[i],"speaker_tx") == 0)) {
            index = SND_DEVICE_IN_SPEAKER_MIC;
        }
        else if((strcmp((char* )device_list->name[i],"headset_stereo_rx") == 0) || (strcmp((char* )device_list->name[i],"headset_rx") == 0)) {
            index = SND_DEVICE_OUT_HEADPHONES;
        }
        else if((strcmp((char* )device_list->name[i],"headset_mono_tx") == 0) || (strcmp((char* )device_list->name[i],"headset_tx") == 0)) {
            index = SND_DEVICE_IN_HEADSET_MIC;
        }
/*
        else if(strcmp((char* )device_list->name[i],"fmradio_handset_rx") == 0) {
            index = DEVICE_FMRADIO_HANDSET_RX;
        }
        else if((strcmp((char* )device_list->name[i],"fmradio_headset_rx") == 0) || (strcmp((char* )device_list->name[i],"fm_radio_headset_rx") == 0)) {
            index = DEVICE_FMRADIO_HEADSET_RX;
        }
        else if((strcmp((char* )device_list->name[i],"fmradio_speaker_rx") == 0) || (strcmp((char* )device_list->name[i],"fm_radio_speaker_rx") == 0)) {
            index = DEVICE_FMRADIO_SPEAKER_RX;
        }
*/
        else if((strcmp((char* )device_list->name[i],"handset_dual_mic_endfire_tx") == 0) || (strcmp((char* )device_list->name[i],"dualmic_handset_ef_tx") == 0)) {
            if (plat_data->fluence_type == FLUENCE_DUAL_MIC) {
                 index = SND_DEVICE_IN_VOICE_DMIC;
            } else {
                 ALOGV("Endfire handset found but user request for %d\n", plat_data->fluence_type);
                 continue;
            }
        }
        else if((strcmp((char* )device_list->name[i],"speaker_dual_mic_endfire_tx") == 0)|| (strcmp((char* )device_list->name[i],"dualmic_speaker_ef_tx") == 0)) {
            if (plat_data->fluence_type == FLUENCE_DUAL_MIC) {
                 index = SND_DEVICE_IN_VOICE_SPEAKER_DMIC;
            } else {
                 ALOGV("Endfire speaker found but user request for %d\n", plat_data->fluence_type);
                 continue;
            }
        }
/*
        else if(strcmp((char* )device_list->name[i],"handset_dual_mic_broadside_tx") == 0) {
            if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                 index = DEVICE_DUALMIC_HANDSET_TX;
            } else {
                 ALOGV("Broadside handset found but user request for %d\n", fluence_mode);
                 continue;
            }
        }
        else if(strcmp((char* )device_list->name[i],"speaker_dual_mic_broadside_tx") == 0) {
            if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                 index = DEVICE_DUALMIC_SPEAKER_TX;
            } else {
                 ALOGV("Broadside speaker found but user request for %d\n", fluence_mode);
                 continue;
            }
        }
*/
        else if((strcmp((char* )device_list->name[i],"tty_headset_mono_rx") == 0) || (strcmp((char* )device_list->name[i],"tty_headset_rx") == 0)) {
            index = SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES;
        }
        else if((strcmp((char* )device_list->name[i],"tty_headset_mono_tx") == 0) || (strcmp((char* )device_list->name[i],"tty_headset_tx") == 0)) {
            index = SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC;
        }
        else if((strcmp((char* )device_list->name[i],"bt_sco_rx") == 0) || (strcmp((char* )device_list->name[i],"bt_sco_mono_rx") == 0)) {
            index = SND_DEVICE_OUT_BT_SCO;
        }
        else if((strcmp((char* )device_list->name[i],"bt_sco_tx") == 0) || (strcmp((char* )device_list->name[i],"bt_sco_mono_tx") == 0)) {
            index = SND_DEVICE_IN_BT_SCO_MIC;
        }
        else if((strcmp((char* )device_list->name[i],"headset_stereo_speaker_stereo_rx") == 0) ||
                (strcmp((char* )device_list->name[i],"headset_stereo_rx_playback") == 0) ||
                (strcmp((char* )device_list->name[i],"headset_speaker_stereo_rx") == 0) || (strcmp((char* )device_list->name[i],"speaker_headset_rx") == 0)) {
            index = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        }
/*
        else if((strcmp((char* )device_list->name[i],"fmradio_stereo_tx") == 0) || (strcmp((char* )device_list->name[i],"fm_radio_tx") == 0)) {
            index = DEVICE_FMRADIO_STEREO_TX;
        }
*/
        else if((strcmp((char* )device_list->name[i],"hdmi_stereo_rx") == 0) || (strcmp((char* )device_list->name[i],"hdmi_rx") == 0)) {
            index = SND_DEVICE_OUT_HDMI;
        }
/*
        else if(strcmp((char* )device_list->name[i],"fmradio_stereo_rx") == 0)
            index = DEVICE_FMRADIO_STEREO_RX;
*/
#ifdef SAMSUNG_AUDIO
        else if(strcmp((char* )device_list->name[i], "handset_voip_rx") == 0)
            index = SND_DEVICE_OUT_VOIP_HANDSET;
        else if(strcmp((char* )device_list->name[i], "handset_voip_tx") == 0)
            index = SND_DEVICE_IN_VOIP_HANDSET_MIC;
        else if(strcmp((char* )device_list->name[i], "speaker_voip_rx") == 0)
            index = SND_DEVICE_OUT_VOIP_SPEAKER;
        else if(strcmp((char* )device_list->name[i], "speaker_voip_tx") == 0)
            index = SND_DEVICE_IN_VOIP_SPEAKER_MIC;
        else if(strcmp((char* )device_list->name[i], "headset_voip_rx") == 0)
            index = SND_DEVICE_OUT_VOIP_HEADPHONES;
        else if(strcmp((char* )device_list->name[i], "headset_voip_tx") == 0)
            index = SND_DEVICE_IN_VOIP_HEADSET_MIC;
        else if(strcmp((char* )device_list->name[i], "handset_call_rx") == 0)
            index = SND_DEVICE_OUT_VOICE_HANDSET;
        else if(strcmp((char* )device_list->name[i], "handset_call_tx") == 0)
            index = SND_DEVICE_IN_HANDSET_MIC;
        else if(strcmp((char* )device_list->name[i], "speaker_call_rx") == 0)
            index = SND_DEVICE_OUT_VOICE_SPEAKER;
        else if(strcmp((char* )device_list->name[i], "speaker_call_tx") == 0)
            index = SND_DEVICE_IN_VOICE_SPEAKER_MIC;
        else if(strcmp((char* )device_list->name[i], "headset_call_rx") == 0)
            index = SND_DEVICE_OUT_VOICE_HEADPHONES;
        else if(strcmp((char* )device_list->name[i], "headset_call_tx") == 0)
            index = SND_DEVICE_IN_VOICE_HEADSET_MIC;
/*
        else if(strcmp((char* )device_list->name[i], "speaker_vr_tx") == 0)
            index = DEVICE_SPEAKER_VR_TX;
        else if(strcmp((char* )device_list->name[i], "headset_vr_tx") == 0)
            index = DEVICE_HEADSET_VR_TX;
*/
#endif
        else if((strcmp((char* )device_list->name[i], "camcoder_tx") == 0) ||
#ifdef SONY_AUDIO
                (strcmp((char* )device_list->name[i], "secondary_mic_tx") == 0))
#else
                (strcmp((char* )device_list->name[i], "camcorder_tx") == 0) ||
                (strcmp((char* )device_list->name[i], "handset_lgcam_tx") == 0))
#endif
            index = SND_DEVICE_IN_CAMCORDER_MIC;
        else {
            ALOGI("Not used device: %s", ( char* )device_list->name[i]);
            continue;
        }
        ALOGI("index = %d",index);

        device_list->dev_id[index] = plat_data->msm->msm_get_device((char* )device_list->name[i]);
        if(device_list->dev_id[index] >= 0) {
                ALOGI("Found device: %s:index = %d,dev_id: %d",( char* )device_list->name[i], index, device_list->dev_id[index]);
        }
        acdb_mapper_get_acdb_id_from_dev_name((char* )device_list->name[i], &device_list->acdb_id[index]);
        ALOGI("%s: acdb ID = %d for device %d", __func__, device_list->acdb_id[index], device_list->dev_id[index]);
    }

    plat_data->device_list = device_list;
}

static void set_platform_defaults(struct platform_data * my_data __unused)
{
    int32_t dev;
    for (dev = 0; dev < SND_DEVICE_MAX; dev++) {
        backend_table[dev] = NULL;
    }

    // TBD - do these go to the platform-info.xml file.
    // will help in avoiding strdups here
    backend_table[SND_DEVICE_IN_BT_SCO_MIC] = strdup("bt-sco");
    backend_table[SND_DEVICE_IN_BT_SCO_MIC_WB] = strdup("bt-sco-wb");
    backend_table[SND_DEVICE_OUT_BT_SCO] = strdup("bt-sco");
    backend_table[SND_DEVICE_OUT_BT_SCO_WB] = strdup("bt-sco-wb");
    backend_table[SND_DEVICE_OUT_HDMI] = strdup("hdmi");
    backend_table[SND_DEVICE_OUT_SPEAKER_AND_HDMI] = strdup("speaker-and-hdmi");
    backend_table[SND_DEVICE_OUT_VOICE_TX] = strdup("afe-proxy");
    backend_table[SND_DEVICE_IN_VOICE_RX] = strdup("afe-proxy");
    backend_table[SND_DEVICE_OUT_AFE_PROXY] = strdup("afe-proxy");
    backend_table[SND_DEVICE_OUT_USB_HEADSET] = strdup("usb-headphones");
    backend_table[SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET] =
        strdup("speaker-and-usb-headphones");
    backend_table[SND_DEVICE_IN_USB_HEADSET_MIC] = strdup("usb-headset-mic");
    backend_table[SND_DEVICE_IN_CAPTURE_FM] = strdup("capture-fm");
    backend_table[SND_DEVICE_OUT_TRANSMISSION_FM] = strdup("transmission-fm");
}

void *platform_init(struct audio_device *adev)
{
    char platform[PROPERTY_VALUE_MAX];
    char baseband[PROPERTY_VALUE_MAX];
    char baseband_arch[PROPERTY_VALUE_MAX];
    char value[PROPERTY_VALUE_MAX];
    struct platform_data *my_data = NULL;
    int retry_num = 0, snd_card_num = 0;
    const char *snd_card_name;

    my_data = calloc(1, sizeof(struct platform_data));

    if (!my_data) {
        ALOGE("failed to allocate platform data");
        return NULL;
    }

    while (snd_card_num < MAX_SND_CARD) {
        adev->mixer = mixer_open(snd_card_num);

        while (!adev->mixer && retry_num < RETRY_NUMBER) {
            usleep(RETRY_US);
            adev->mixer = mixer_open(snd_card_num);
            retry_num++;
        }

        if (!adev->mixer) {
            ALOGE("%s: Unable to open the mixer card: %d", __func__,
                   snd_card_num);
            retry_num = 0;
            snd_card_num++;
            continue;
        }

        snd_card_name = mixer_get_name(adev->mixer);
        ALOGD("%s: snd_card_name: %s", __func__, snd_card_name);

        my_data->hw_info = hw_info_init(snd_card_name);

        adev->audio_route = audio_route_init(snd_card_num,
                                         MIXER_XML_PATH);
        if (!adev->audio_route) {
            ALOGE("%s: Failed to init audio route controls, aborting.",
                   __func__);
            free(my_data);
            mixer_close(adev->mixer);
            return NULL;
        }
        adev->snd_card = snd_card_num;
        ALOGD("%s: Opened sound card:%d", __func__, snd_card_num);
        break;

        retry_num = 0;
        snd_card_num++;
        mixer_close(adev->mixer);
    }

    if (snd_card_num >= MAX_SND_CARD) {
        ALOGE("%s: Unable to find correct sound card, aborting.", __func__);
        free(my_data);
        return NULL;
    }

    //set max volume step for voice call
    property_get("ro.config.vc_call_vol_steps", value, TOSTRING(MAX_VOL_INDEX));
    my_data->max_vol_index = atoi(value);

    my_data->adev = adev;
    my_data->btsco_sample_rate = SAMPLE_RATE_8KHZ;
    my_data->fluence_in_spkr_mode = false;
    my_data->fluence_in_voice_call = false;
    my_data->fluence_in_voice_rec = false;
    my_data->fluence_in_audio_rec = false;
    my_data->fluence_type = FLUENCE_NONE;

    property_get("persist.audio.fluence.mode", my_data->fluence_cap, "");
    if (!strncmp("endfire", my_data->fluence_cap, sizeof("endfire"))) {
        my_data->fluence_type = FLUENCE_DUAL_MIC;
    } else {
        my_data->fluence_type = FLUENCE_NONE;
    }

    if (my_data->fluence_type != FLUENCE_NONE) {
        property_get("persist.audio.fluence.voicecall",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_voice_call = true;
        }

        property_get("persist.audio.fluence.voicerec",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_voice_rec = true;
        }

        property_get("persist.audio.fluence.audiorec",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_audio_rec = true;
        }

        property_get("persist.audio.fluence.speaker",value,"");
        if (!strncmp("true", value, sizeof("true"))) {
            my_data->fluence_in_spkr_mode = true;
        }
    }

    my_data->acdb_handle = dlopen(LIB_ACDB_LOADER, RTLD_NOW);
    if (my_data->acdb_handle == NULL) {
        ALOGE("%s: DLOPEN failed for %s", __func__, LIB_ACDB_LOADER);
    } else {
        ALOGV("%s: DLOPEN successful for %s", __func__, LIB_ACDB_LOADER);
        my_data->acdb_deallocate = (acdb_deallocate_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_deallocate_ACDB");
        if (!my_data->acdb_deallocate)
            ALOGE("%s: Could not find the symbol acdb_loader_deallocate_ACDB from %s",
                  __func__, LIB_ACDB_LOADER);

        my_data->acdb_send_audio_cal = (acdb_send_audio_cal_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_send_audio_cal");
        if (!my_data->acdb_send_audio_cal)
            ALOGE("%s: Could not find the symbol acdb_send_audio_cal from %s",
                  __func__, LIB_ACDB_LOADER);

        my_data->acdb_send_voice_cal = (acdb_send_voice_cal_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_send_voice_cal");
        if (!my_data->acdb_send_voice_cal)
            ALOGE("%s: Could not find the symbol acdb_loader_send_voice_cal from %s",
                  __func__, LIB_ACDB_LOADER);

        my_data->acdb_init = (acdb_init_t)dlsym(my_data->acdb_handle,
                                                    "acdb_loader_init_ACDB");
        if (my_data->acdb_init == NULL)
            ALOGE("%s: dlsym error %s for acdb_loader_init_ACDB", __func__, dlerror());
        else
            my_data->acdb_init();
    }

    set_platform_defaults(my_data);

    /* Initialize ACDB ID's */
    platform_info_init();

    /* If platform is msm8660, load MSM Client specific
     * symbols. Voice call is handled by MDM and apps processor talks to
     * MDM through MSM Client
     */
    property_get("ro.board.platform", platform, "");
    property_get("ro.baseband", baseband, "");
    property_get("ro.baseband.arch", baseband_arch, "");
    if (!strncmp("msm8660", platform, sizeof("msm8660"))) {
         my_data->msm = open_msm_client();
    }

    /* init device list */
    device_list_init(my_data);

    /* init usb */
    audio_extn_usb_init(adev);
    /* update sound cards appropriately */
    audio_extn_usb_set_proxy_sound_card(adev->snd_card);

    /* Read one time ssr property */
    audio_extn_ssr_update_enabled();
    audio_extn_spkr_prot_init(adev);
#ifdef HWDEP_CAL_ENABLED
    audio_hwdep_send_cal(my_data);
#endif
    return my_data;
}

void platform_deinit(void *platform)
{
    struct platform_data *my_data = (struct platform_data *)platform;

    hw_info_deinit(my_data->hw_info);
    close_msm_client(my_data->msm);

    if (my_data->acdb_deallocate != NULL)
        my_data->acdb_deallocate();
    dlclose(LIB_ACDB_LOADER);

    int32_t dev;
    for (dev = 0; dev < SND_DEVICE_MAX; dev++) {
        if (backend_table[dev]) {
            free(backend_table[dev]);
            backend_table[dev]= NULL;
        }
    }

    free(platform);
    /* deinit usb */
    audio_extn_usb_deinit();
}

const char *platform_get_snd_device_name(snd_device_t snd_device)
{
    if (snd_device >= SND_DEVICE_MIN && snd_device < SND_DEVICE_MAX)
        return device_table[snd_device];
    else
        return "";
}

int platform_get_snd_device_name_extn(void *platform, snd_device_t snd_device,
                                      char *device_name)
{
    struct platform_data *my_data = (struct platform_data *)platform;

    if (snd_device >= SND_DEVICE_MIN && snd_device < SND_DEVICE_MAX) {
        strlcpy(device_name, device_table[snd_device], DEVICE_NAME_MAX_SIZE);
        hw_info_append_hw_type(my_data->hw_info, snd_device, device_name);
    } else {
        strlcpy(device_name, "", DEVICE_NAME_MAX_SIZE);
        return -EINVAL;
    }

    return 0;
}

void platform_add_backend_name(char *mixer_path, snd_device_t snd_device)
{
    if ((snd_device < SND_DEVICE_MIN) || (snd_device >= SND_DEVICE_MAX)) {
        ALOGE("%s: Invalid snd_device = %d", __func__, snd_device);
        return;
    }

    const char * suffix = backend_table[snd_device];

    if (suffix != NULL) {
        strlcat(mixer_path, " ", MIXER_PATH_MAX_LENGTH);
        strlcat(mixer_path, suffix, MIXER_PATH_MAX_LENGTH);
    }
}

int platform_get_pcm_device_id(audio_usecase_t usecase, int device_type)
{
    int device_id;
    if (device_type == PCM_PLAYBACK)
        device_id = pcm_device_table[usecase][0];
    else
        device_id = pcm_device_table[usecase][1];
    return device_id;
}

static int find_index(struct name_to_index * table, int32_t len, const char * name)
{
    int ret = 0;
    int32_t i;

    if (table == NULL) {
        ALOGE("%s: table is NULL", __func__);
        ret = -ENODEV;
        goto done;
    }

    if (name == NULL) {
        ALOGE("null key");
        ret = -ENODEV;
        goto done;
    }

    for (i=0; i < len; i++) {
        const char* tn = table[i].name;
        size_t len = strlen(tn);
        if (strncmp(tn, name, len) == 0) {
            if (strlen(name) != len) {
                continue; // substring
            }
            ret = table[i].index;
            goto done;
        }
    }
    ALOGE("%s: Could not find index for name = %s",
            __func__, name);
    ret = -ENODEV;
done:
    return ret;
}

int platform_get_snd_device_index(char *device_name)
{
    return find_index(snd_device_name_index, SND_DEVICE_MAX, device_name);
}

int platform_get_usecase_index(const char *usecase_name)
{
    return find_index(usecase_name_index, AUDIO_USECASE_MAX, usecase_name);
}

int platform_set_snd_device_acdb_id(snd_device_t snd_device, unsigned int acdb_id)
{
    int ret = 0;

    if ((snd_device < SND_DEVICE_MIN) || (snd_device >= SND_DEVICE_MAX)) {
        ALOGE("%s: Invalid snd_device = %d",
            __func__, snd_device);
        ret = -EINVAL;
        goto done;
    }

    acdb_device_table[snd_device] = acdb_id;
done:
    return ret;
}

#ifdef FLUENCE_ENABLED
int platform_set_fluence_type(void *platform, char *value)
{
    int ret = 0;
    int fluence_type = FLUENCE_NONE;
    int fluence_flag = NONE_FLAG;
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;

    ALOGV("%s: fluence type:%d", __func__, my_data->fluence_type);

    /* only dual mic turn on and off is supported as of now through setparameters */
    if (!strncmp(AUDIO_PARAMETER_VALUE_DUALMIC,value, sizeof(AUDIO_PARAMETER_VALUE_DUALMIC))) {
        if (!strncmp("endfire", my_data->fluence_cap, sizeof("endfire"))) {
            ALOGV("fluence dualmic feature enabled \n");
            fluence_type = FLUENCE_DUAL_MIC;
            fluence_flag = DMIC_FLAG;
        } else {
            ALOGE("%s: Failed to set DUALMIC", __func__);
            ret = -1;
            goto done;
        }
    } else if (!strncmp(AUDIO_PARAMETER_KEY_NO_FLUENCE, value, sizeof(AUDIO_PARAMETER_KEY_NO_FLUENCE))) {
        ALOGV("fluence disabled");
        fluence_type = FLUENCE_NONE;
    } else {
        ALOGE("Invalid fluence value : %s",value);
        ret = -1;
        goto done;
    }

    if (fluence_type != my_data->fluence_type) {
        ALOGV("%s: Updating fluence_type to :%d", __func__, fluence_type);
        my_data->fluence_type = fluence_type;
        adev->acdb_settings = (adev->acdb_settings & FLUENCE_MODE_CLEAR) | fluence_flag;
    }
done:
    return ret;
}


int platform_get_fluence_type(void *platform, char *value, uint32_t len)
{
    int ret = 0;
    struct platform_data *my_data = (struct platform_data *)platform;

    if (my_data->fluence_type == FLUENCE_QUAD_MIC) {
        strlcpy(value, "quadmic", len);
    } else if (my_data->fluence_type == FLUENCE_DUAL_MIC) {
        strlcpy(value, "dualmic", len);
    } else if (my_data->fluence_type == FLUENCE_NONE) {
        strlcpy(value, "none", len);
    } else
        ret = -1;

    return ret;
}
#endif

int platform_send_audio_calibration(void *platform, snd_device_t snd_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int acdb_dev_id, acdb_dev_type;

    acdb_dev_id = acdb_device_table[snd_device];
    if (acdb_dev_id < 0) {
        ALOGE("%s: Could not find acdb id for device(%d)",
              __func__, snd_device);
        return -EINVAL;
    }
    if (my_data->acdb_send_audio_cal) {
        ALOGI("%s: sending audio calibration for snd_device(%d) acdb_id(%d)",
              __func__, snd_device, acdb_dev_id);
        if (snd_device >= SND_DEVICE_OUT_BEGIN &&
                snd_device < SND_DEVICE_OUT_END)
            acdb_dev_type = ACDB_DEV_TYPE_OUT;
        else
            acdb_dev_type = ACDB_DEV_TYPE_IN;
        my_data->acdb_send_audio_cal(acdb_dev_id, acdb_dev_type);
    }
    return 0;
}

int platform_switch_voice_call_device_pre(void *platform __unused)
{
    return 0;
}

int platform_switch_voice_call_device_post(void *platform,
                                           snd_device_t out_snd_device,
                                           snd_device_t in_snd_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    const char *mixer_ctl_name_rx = "voice-rx";
    const char *mixer_ctl_name_tx = "voice-tx";
    struct mixer_ctl *ctl;
    int acdb_rx_id, acdb_tx_id, dev_rx_id, dev_tx_id;
    int rc;

    if (my_data->acdb_send_voice_cal == NULL) {
        ALOGE("%s: dlsym error for acdb_send_voice_call", __func__);
    } else {

        acdb_rx_id = acdb_device_table[out_snd_device];
        acdb_tx_id = acdb_device_table[in_snd_device];

        if (acdb_rx_id > 0 && acdb_tx_id > 0)
            my_data->acdb_send_voice_cal(acdb_rx_id, acdb_tx_id);
        else
            ALOGE("%s: Incorrect ACDB IDs (rx: %d tx: %d)", __func__,
                  acdb_rx_id, acdb_tx_id);
    }

    if (out_snd_device == SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES)
        dev_rx_id = my_data->device_list->dev_id[SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES];
    else if (out_snd_device == SND_DEVICE_OUT_BT_SCO_WB)
        dev_rx_id = my_data->device_list->dev_id[SND_DEVICE_OUT_BT_SCO];
    else
        dev_rx_id = my_data->device_list->dev_id[out_snd_device];

    if (in_snd_device == SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC)
        dev_tx_id = my_data->device_list->dev_id[SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC];
    else if (in_snd_device == SND_DEVICE_IN_BT_SCO_MIC_WB)
        dev_tx_id = my_data->device_list->dev_id[SND_DEVICE_IN_BT_SCO_MIC];
    else
        dev_tx_id = my_data->device_list->dev_id[in_snd_device];

    ctl = mixer_get_ctl_by_name(my_data->adev->mixer, mixer_ctl_name_rx);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name_rx);
        return -EINVAL;
    }
    rc = mixer_ctl_set_value(ctl, 0, dev_rx_id);
    if (rc < 0)
        return rc;

    ctl = mixer_get_ctl_by_name(my_data->adev->mixer, mixer_ctl_name_tx);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name_tx);
        return -EINVAL;
    }
    rc = mixer_ctl_set_value(ctl, 0, dev_tx_id);
    if (rc < 0)
        return rc;

    return 0;
}

int platform_switch_voice_call_usecase_route_post(void *platform __unused,
                                                  snd_device_t out_snd_device __unused,
                                                  snd_device_t in_snd_device __unused)
{
    return 0;
}

int platform_start_voice_call(void *platform, uint32_t vsid __unused)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->msm != NULL) {
#ifdef LEGACY_QCOM_VOICE
        ret = my_data->msm->msm_start_voice();
#else
        my_data->msm->voice_session_id = my_data->msm->msm_get_voc_session(VOICE_SESSION_NAME);
        if (my_data->msm->voice_session_id <= 0) {
            ALOGE("voice session invalid");
            return 0;
        }

        ret = my_data->msm->msm_start_voice_ext(my_data->msm->voice_session_id);
#endif
        if (ret < 0) {
            ALOGE("%s: msm_start_voice error %d\n", __func__, ret);
        }
    }
    return ret;
}

int platform_stop_voice_call(void *platform, uint32_t vsid __unused)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->msm != NULL) {
#ifdef LEGACY_QCOM_VOICE
        ret = my_data->msm->msm_end_voice();
#else
        ret = my_data->msm->msm_end_voice_ext(my_data->msm->voice_session_id);
        my_data->msm->voice_session_id = 0;
#endif
        if (ret < 0) {
            ALOGE("%s: msm_stop_voice error %d\n", __func__, ret);
        }
    }
    return ret;
}

int platform_set_voice_volume(void *platform, int volume)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int vol_index = 0, ret = 0;

    // Voice volume levels are mapped to adsp volume levels as follows.
    // 100 -> 5, 80 -> 4, 60 -> 3, 40 -> 2, 20 -> 1  0 -> 0
    // But this values don't changed in kernel. So, below change is need.
    vol_index = (int)percent_to_index(volume, MIN_VOL_INDEX, my_data->max_vol_index);

    if (my_data->msm != NULL) {
#ifdef LEGACY_QCOM_VOICE
        ret = my_data->msm->msm_set_voice_rx_vol(vol_index);
#else
        ret = my_data->msm->msm_set_voice_rx_vol_ext(vol_index, my_data->msm->voice_session_id);
#endif
        if (ret < 0) {
            ALOGE("%s: msm_volume error %d", __func__, ret);
        }
    }
    return ret;
}

int platform_set_mic_mute(void *platform, bool state)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    int ret = 0;

    if (my_data->msm != NULL) {
#ifdef LEGACY_QCOM_VOICE
        ret = my_data->msm->msm_set_voice_tx_mute(state);
#else
        ret = my_data->msm->msm_set_voice_tx_mute_ext(state, my_data->msm->voice_session_id);
#endif

        if (ret < 0) {
            ALOGE("%s: msm_mic_mute error %d", __func__, ret);
        }
    }
    return ret;
}

int platform_set_device_mute(void *platform, bool state, char *dir)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    struct mixer_ctl *ctl;
    char *mixer_ctl_name = NULL;
    int ret = 0;

    if(dir == NULL) {
        ALOGE("%s: Invalid direction:%s", __func__, dir);
        return -EINVAL;
    }

    if (!strncmp("rx", dir, sizeof("rx"))) {
        mixer_ctl_name = "Voice Rx Device Mute";
    } else if (!strncmp("tx", dir, sizeof("tx"))) {
        mixer_ctl_name = "Voice Tx Device Mute";
    } else {
        return -EINVAL;
    }

    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }

    ALOGV("%s: Setting device mute state: %d, mixer ctrl:%s",
          __func__,state, mixer_ctl_name);
    mixer_ctl_set_value(ctl, 0, state);

    return ret;
}

#ifdef DOCK_SUPPORT
bool is_dock_connected()
{
    FILE *dock_node = NULL;
    char buf[32];
    bool connected = false;

    dock_node = fopen(DOCK_SWITCH, "r");
    if (dock_node) {
        fread(buf, sizeof(char), 32, dock_node);
        connected = atoi(buf) > 0;
        fclose(dock_node);
    }
    return connected;
}
#endif

#ifdef MOTOROLA_EMU_AUDIO
int emu_antipop_state = 0;

int set_emu_antipop(struct audio_device *adev, int emu_antipop)
{
    struct mixer_ctl *ctl;
    const char *mixer_ctl_name = "EMU Antipop";

    if (emu_antipop == emu_antipop_state) return 0;

    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ALOGV("Setting EMU Antipop to %d", emu_antipop);
    mixer_ctl_set_value(ctl, 0, emu_antipop);
    emu_antipop_state = emu_antipop;
    return 0;
}
#endif

snd_device_t platform_get_output_snd_device(void *platform, audio_devices_t devices)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    audio_mode_t mode = adev->mode;
    snd_device_t snd_device = SND_DEVICE_NONE;
#ifdef DOCK_SUPPORT
    bool dock_connected = is_dock_connected();
#ifdef MOTOROLA_EMU_AUDIO
    if (!dock_connected) set_emu_antipop(adev, 0);
#endif
#endif

    audio_channel_mask_t channel_mask = (adev->active_input == NULL) ?
                                AUDIO_CHANNEL_IN_MONO : adev->active_input->channel_mask;
    int channel_count = popcount(channel_mask);

    ALOGV("%s: enter: output devices(%#x)", __func__, devices);
    if (devices == AUDIO_DEVICE_NONE ||
        devices & AUDIO_DEVICE_BIT_IN) {
        ALOGV("%s: Invalid output devices (%#x)", __func__, devices);
        goto exit;
    }

    if (popcount(devices) == 2) {
        if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
                        AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_WIRED_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            if (audio_extn_get_anc_enabled())
                snd_device = SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET;
            else
                snd_device = SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES;
        } else if (devices == (AUDIO_DEVICE_OUT_AUX_DIGITAL |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_HDMI;
        } else if ((devices == (AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) ||
                    devices == (AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET |
                               AUDIO_DEVICE_OUT_SPEAKER)) {
#ifdef DOCK_SUPPORT
            if (dock_connected) {
                snd_device = SND_DEVICE_OUT_SPEAKER_AND_DOCK;
#ifdef MOTOROLA_EMU_AUDIO
                set_emu_antipop(adev, 1);
#endif
            } else
#endif
            snd_device = SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET;
        } else {
            ALOGE("%s: Invalid combo device(%#x)", __func__, devices);
            goto exit;
        }
        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (popcount(devices) != 1) {
        ALOGE("%s: Invalid output devices(%#x)", __func__, devices);
        goto exit;
    }

    if ((mode == AUDIO_MODE_IN_CALL) ||
        voice_extn_compress_voip_is_active(adev)) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
            devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if ((adev->voice.tty_mode != TTY_MODE_OFF) &&
                !voice_extn_compress_voip_is_active(adev)) {
                switch (adev->voice.tty_mode) {
                case TTY_MODE_FULL:
                    snd_device = SND_DEVICE_OUT_VOICE_TTY_FULL_HEADPHONES;
                    break;
                case TTY_MODE_VCO:
                    snd_device = SND_DEVICE_OUT_VOICE_TTY_VCO_HEADPHONES;
                    break;
                case TTY_MODE_HCO:
                    snd_device = SND_DEVICE_OUT_VOICE_TTY_HCO_HANDSET;
                    break;
                default:
                    ALOGE("%s: Invalid TTY mode (%#x)",
                          __func__, adev->voice.tty_mode);
                }
            } else if (audio_extn_get_anc_enabled()) {
                if (audio_extn_should_use_fb_anc())
                    snd_device = SND_DEVICE_OUT_VOICE_ANC_FB_HEADSET;
                else
                    snd_device = SND_DEVICE_OUT_VOICE_ANC_HEADSET;
            } else {
                if (voice_extn_compress_voip_is_active(adev) &&
                        voice_extn_dedicated_voip_device_prop_check()) {
                    snd_device = SND_DEVICE_OUT_VOIP_HEADPHONES;
                } else {
                    snd_device = SND_DEVICE_OUT_VOICE_HEADPHONES;
                }
            }
        } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
            if (my_data->btsco_sample_rate == SAMPLE_RATE_16KHZ)
                snd_device = SND_DEVICE_OUT_BT_SCO_WB;
            else
                snd_device = SND_DEVICE_OUT_BT_SCO;
        } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
            if (voice_extn_compress_voip_is_active(adev) &&
                    voice_extn_dedicated_voip_device_prop_check()) {
                snd_device = SND_DEVICE_OUT_VOIP_SPEAKER;
            } else {
                snd_device = SND_DEVICE_OUT_VOICE_SPEAKER;
            }
        } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                   devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
#ifdef DOCK_SUPPORT
            if (dock_connected) {
                snd_device = SND_DEVICE_OUT_DOCK;
#ifdef MOTOROLA_EMU_AUDIO
                set_emu_antipop(adev, 1);
#endif
            } else
#endif
            snd_device = SND_DEVICE_OUT_USB_HEADSET;
        } else if (devices & AUDIO_DEVICE_OUT_FM_TX) {
            snd_device = SND_DEVICE_OUT_TRANSMISSION_FM;
        } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
            if (audio_extn_should_use_handset_anc(channel_count))
                snd_device = SND_DEVICE_OUT_ANC_HANDSET;
            else if (voice_extn_compress_voip_is_active(adev) &&
                    voice_extn_dedicated_voip_device_prop_check())
                snd_device = SND_DEVICE_OUT_VOIP_HANDSET;
            else
                snd_device = SND_DEVICE_OUT_VOICE_HANDSET;
        } else if (devices & AUDIO_DEVICE_OUT_TELEPHONY_TX)
                snd_device = SND_DEVICE_OUT_VOICE_TX;

        if (snd_device != SND_DEVICE_NONE) {
            goto exit;
        }
    }

    if (devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
        devices & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
        if (devices & AUDIO_DEVICE_OUT_WIRED_HEADSET
            && audio_extn_get_anc_enabled()) {
            if (audio_extn_should_use_fb_anc())
                snd_device = SND_DEVICE_OUT_ANC_FB_HEADSET;
            else
                snd_device = SND_DEVICE_OUT_ANC_HEADSET;
        } else
            snd_device = SND_DEVICE_OUT_HEADPHONES;
    } else if (devices & AUDIO_DEVICE_OUT_SPEAKER) {
        if (adev->speaker_lr_swap)
            snd_device = SND_DEVICE_OUT_SPEAKER_REVERSE;
        else
            snd_device = SND_DEVICE_OUT_SPEAKER;
    } else if (devices & AUDIO_DEVICE_OUT_ALL_SCO) {
        if (my_data->btsco_sample_rate == SAMPLE_RATE_16KHZ)
            snd_device = SND_DEVICE_OUT_BT_SCO_WB;
        else
            snd_device = SND_DEVICE_OUT_BT_SCO;
    } else if (devices & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        snd_device = SND_DEVICE_OUT_HDMI ;
    } else if (devices & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
               devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
#ifdef DOCK_SUPPORT
        if (dock_connected) {
            snd_device = SND_DEVICE_OUT_DOCK;
#ifdef MOTOROLA_EMU_AUDIO
            set_emu_antipop(adev, 1);
#endif
        } else {
#endif
        ALOGD("%s: setting USB hadset channel capability(2) for Proxy", __func__);
        audio_extn_set_afe_proxy_channel_mixer(adev, 2);
        snd_device = SND_DEVICE_OUT_USB_HEADSET;
#ifdef DOCK_SUPPORT
        }
#endif
    } else if (devices & AUDIO_DEVICE_OUT_FM_TX) {
        snd_device = SND_DEVICE_OUT_TRANSMISSION_FM;
    } else if (devices & AUDIO_DEVICE_OUT_EARPIECE) {
        snd_device = SND_DEVICE_OUT_HANDSET;
#ifdef AFE_PROXY_ENABLED
    } else if (devices & AUDIO_DEVICE_OUT_PROXY) {
        channel_count = audio_extn_get_afe_proxy_channel_count();
        ALOGD("%s: setting sink capability(%d) for Proxy", __func__, channel_count);
        audio_extn_set_afe_proxy_channel_mixer(adev, channel_count);
        snd_device = SND_DEVICE_OUT_AFE_PROXY;
#endif
    } else {
        ALOGE("%s: Unknown device(s) %#x", __func__, devices);
    }
exit:
    ALOGV("%s: exit: snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

snd_device_t platform_get_input_snd_device(void *platform, audio_devices_t out_device)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    audio_source_t  source = (adev->active_input == NULL) ?
                                AUDIO_SOURCE_DEFAULT : adev->active_input->source;

    audio_mode_t    mode   = adev->mode;
    audio_devices_t in_device = ((adev->active_input == NULL) ?
                                    AUDIO_DEVICE_NONE : adev->active_input->device)
                                & ~AUDIO_DEVICE_BIT_IN;
    audio_channel_mask_t channel_mask = (adev->active_input == NULL) ?
                                AUDIO_CHANNEL_IN_MONO : adev->active_input->channel_mask;
    snd_device_t snd_device = SND_DEVICE_NONE;
    int channel_count = popcount(channel_mask);
#ifdef DOCK_SUPPORT
    bool dock_connected = is_dock_connected();
#endif

    ALOGV("%s: enter: out_device(%#x) in_device(%#x)",
          __func__, out_device, in_device);
    if ((out_device != AUDIO_DEVICE_NONE) && ((mode == AUDIO_MODE_IN_CALL) ||
        voice_extn_compress_voip_is_active(adev) || audio_extn_hfp_is_active(adev))) {
        if ((adev->voice.tty_mode != TTY_MODE_OFF) &&
            !voice_extn_compress_voip_is_active(adev)) {
            if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE ||
                out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
                switch (adev->voice.tty_mode) {
                case TTY_MODE_FULL:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_FULL_HEADSET_MIC;
                    break;
                case TTY_MODE_VCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_VCO_HANDSET_MIC;
                    break;
                case TTY_MODE_HCO:
                    snd_device = SND_DEVICE_IN_VOICE_TTY_HCO_HEADSET_MIC;
                    break;
                default:
                    ALOGE("%s: Invalid TTY mode (%#x)", __func__, adev->voice.tty_mode);
                }
                goto exit;
            }
        }
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE ||
            out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            if (out_device & AUDIO_DEVICE_OUT_EARPIECE &&
                audio_extn_should_use_handset_anc(channel_count)) {
                snd_device = SND_DEVICE_IN_AANC_HANDSET_MIC;
            } else if (my_data->fluence_type == FLUENCE_NONE ||
                my_data->fluence_in_voice_call == false) {
                if (voice_extn_compress_voip_is_active(adev) &&
                        voice_extn_dedicated_voip_device_prop_check())
                    snd_device = SND_DEVICE_IN_VOIP_HANDSET_MIC;
                else
                    snd_device = SND_DEVICE_IN_HANDSET_MIC;
                set_echo_reference(adev, true);
            } else {
                if (voice_extn_compress_voip_is_active(adev) &&
                        voice_extn_dedicated_voip_device_prop_check())
                    snd_device = SND_DEVICE_IN_VOIP_HANDSET_MIC;
                else
                    snd_device = SND_DEVICE_IN_VOICE_DMIC;
            }
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            if (voice_extn_compress_voip_is_active(adev) &&
                    voice_extn_dedicated_voip_device_prop_check())
                snd_device = SND_DEVICE_IN_VOIP_HEADSET_MIC;
            else
                snd_device = SND_DEVICE_IN_VOICE_HEADSET_MIC;
            set_echo_reference(adev, true);
        } else if (out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
            if (my_data->btsco_sample_rate == SAMPLE_RATE_16KHZ)
                snd_device = SND_DEVICE_IN_BT_SCO_MIC_WB;
            else
                snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            if (my_data->fluence_type != FLUENCE_NONE &&
                my_data->fluence_in_voice_call &&
                my_data->fluence_in_spkr_mode) {
                if(my_data->fluence_type & FLUENCE_QUAD_MIC) {
                    snd_device = SND_DEVICE_IN_VOICE_SPEAKER_QMIC;
                } else if (voice_extn_compress_voip_is_active(adev) &&
                        voice_extn_dedicated_voip_device_prop_check()) {
                    snd_device = SND_DEVICE_IN_VOIP_SPEAKER_MIC;
                } else {
                    snd_device = SND_DEVICE_IN_VOICE_SPEAKER_DMIC;
                }
            } else {
                if (voice_extn_compress_voip_is_active(adev) &&
                        voice_extn_dedicated_voip_device_prop_check())
                    snd_device = SND_DEVICE_IN_VOIP_SPEAKER_MIC;
                else
                    snd_device = SND_DEVICE_IN_VOICE_SPEAKER_MIC;
                set_echo_reference(adev, true);
            }
        } else if (out_device & AUDIO_DEVICE_OUT_TELEPHONY_TX)
            snd_device = SND_DEVICE_IN_VOICE_RX;
    } else if (source == AUDIO_SOURCE_CAMCORDER) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC ||
            in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_CAMCORDER_MIC;
        }
    } else if (source == AUDIO_SOURCE_VOICE_RECOGNITION) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            if (channel_count == 2) {
                snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_STEREO;
            } else if (adev->active_input->enable_ns)
                snd_device = SND_DEVICE_IN_VOICE_REC_MIC_NS;
            else if (my_data->fluence_type != FLUENCE_NONE &&
                     my_data->fluence_in_voice_rec) {
                snd_device = SND_DEVICE_IN_VOICE_REC_DMIC_FLUENCE;
            } else {
                snd_device = SND_DEVICE_IN_VOICE_REC_MIC;
            }
        }
    } else if (source == AUDIO_SOURCE_VOICE_COMMUNICATION ||
               (mode == AUDIO_MODE_IN_COMMUNICATION)) {
        if (out_device & AUDIO_DEVICE_OUT_SPEAKER)
            in_device = (AUDIO_DEVICE_IN_BACK_MIC & ~AUDIO_DEVICE_BIT_IN);
         if (adev->active_input) {
            if (voice_extn_dedicated_voip_device_prop_check()) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    snd_device = SND_DEVICE_IN_VOIP_SPEAKER_MIC;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    snd_device = SND_DEVICE_IN_VOIP_HANDSET_MIC;
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_VOIP_HEADSET_MIC;
                }
            } else if (adev->active_input->enable_aec &&
                    adev->active_input->enable_ns) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC &&
                       my_data->fluence_in_spkr_mode) {
                        snd_device = SND_DEVICE_IN_SPEAKER_DMIC_AEC_NS;
                    } else
                        snd_device = SND_DEVICE_IN_SPEAKER_MIC_AEC_NS;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC) {
                        snd_device = SND_DEVICE_IN_HANDSET_DMIC_AEC_NS;
                    } else
                        snd_device = SND_DEVICE_IN_HANDSET_MIC_AEC_NS;
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_HEADSET_MIC_FLUENCE;
                }
                set_echo_reference(adev, true);
            } else if (adev->active_input->enable_aec) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC) {
                        snd_device = SND_DEVICE_IN_SPEAKER_DMIC_AEC;
                    } else
                        snd_device = SND_DEVICE_IN_SPEAKER_MIC_AEC;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC) {
                        snd_device = SND_DEVICE_IN_HANDSET_DMIC_AEC;
                    } else
                        snd_device = SND_DEVICE_IN_HANDSET_MIC_AEC;
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_HEADSET_MIC_FLUENCE;
                }
                set_echo_reference(adev, true);
            } else if (adev->active_input->enable_ns) {
                if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC) {
                        snd_device = SND_DEVICE_IN_SPEAKER_DMIC_NS;
                    } else
                        snd_device = SND_DEVICE_IN_SPEAKER_MIC_NS;
                } else if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                    if (my_data->fluence_type & FLUENCE_DUAL_MIC) {
                        snd_device = SND_DEVICE_IN_HANDSET_DMIC_NS;
                    } else
                        snd_device = SND_DEVICE_IN_HANDSET_MIC_NS;
                } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                    snd_device = SND_DEVICE_IN_HEADSET_MIC_FLUENCE;
                }
                set_echo_reference(adev, false);
            } else
                set_echo_reference(adev, false);
        }
    } else if (source == AUDIO_SOURCE_MIC) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC &&
                channel_count == 1 ) {
            if(my_data->fluence_type & FLUENCE_DUAL_MIC &&
                    my_data->fluence_in_audio_rec) {
                snd_device = SND_DEVICE_IN_HANDSET_DMIC;
                set_echo_reference(adev, true);
            }
        }
    } else if (source == AUDIO_SOURCE_FM_TUNER) {
        snd_device = SND_DEVICE_IN_CAPTURE_FM;
    } else if (source == AUDIO_SOURCE_DEFAULT) {
        goto exit;
    }


    if (snd_device != SND_DEVICE_NONE) {
        goto exit;
    }

    if (in_device != AUDIO_DEVICE_NONE &&
            !(in_device & AUDIO_DEVICE_IN_VOICE_CALL) &&
            !(in_device & AUDIO_DEVICE_IN_COMMUNICATION)) {
        if (in_device & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            if (audio_extn_ssr_get_enabled() && channel_count == 6)
                snd_device = SND_DEVICE_IN_QUAD_MIC;
            else if (my_data->fluence_type & (FLUENCE_DUAL_MIC | FLUENCE_QUAD_MIC) &&
                    channel_count == 2)
                snd_device = SND_DEVICE_IN_HANDSET_STEREO_DMIC;
            else
                snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BACK_MIC) {
            snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            if (my_data->btsco_sample_rate == SAMPLE_RATE_16KHZ)
                snd_device = SND_DEVICE_IN_BT_SCO_MIC_WB;
            else
                snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET ||
                   in_device & AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET) {
#ifdef DOCK_SUPPORT
            if (dock_connected) snd_device = SND_DEVICE_IN_HANDSET_MIC;
            else
#endif
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
        } else if (in_device & AUDIO_DEVICE_IN_FM_TUNER) {
            snd_device = SND_DEVICE_IN_CAPTURE_FM;
        } else {
            ALOGE("%s: Unknown input device(s) %#x", __func__, in_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    } else {
        if (out_device & AUDIO_DEVICE_OUT_EARPIECE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            snd_device = SND_DEVICE_IN_HEADSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_SPEAKER) {
            if (channel_count == 2)
                snd_device = SND_DEVICE_IN_SPEAKER_STEREO_DMIC;
            else
                snd_device = SND_DEVICE_IN_SPEAKER_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET) {
            if (my_data->btsco_sample_rate == SAMPLE_RATE_16KHZ)
                snd_device = SND_DEVICE_IN_BT_SCO_MIC_WB;
            else
                snd_device = SND_DEVICE_IN_BT_SCO_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            snd_device = SND_DEVICE_IN_HDMI_MIC;
        } else if (out_device & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET ||
                   out_device & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) {
#ifdef DOCK_SUPPORT
            if (dock_connected) snd_device = SND_DEVICE_IN_HANDSET_MIC;
            else
#endif
            snd_device = SND_DEVICE_IN_USB_HEADSET_MIC;
        } else {
            ALOGE("%s: Unknown output device(s) %#x", __func__, out_device);
            ALOGW("%s: Using default handset-mic", __func__);
            snd_device = SND_DEVICE_IN_HANDSET_MIC;
        }
    }
exit:
    ALOGV("%s: exit: in_snd_device(%s)", __func__, device_table[snd_device]);
    return snd_device;
}

int platform_set_hdmi_channels(void *platform,  int channel_count)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    struct mixer_ctl *ctl;
    const char *channel_cnt_str = NULL;
    const char *mixer_ctl_name = "HDMI_RX Channels";
    switch (channel_count) {
    case 8:
        channel_cnt_str = "Eight"; break;
    case 7:
        channel_cnt_str = "Seven"; break;
    case 6:
        channel_cnt_str = "Six"; break;
    case 5:
        channel_cnt_str = "Five"; break;
    case 4:
        channel_cnt_str = "Four"; break;
    case 3:
        channel_cnt_str = "Three"; break;
    default:
        channel_cnt_str = "Two"; break;
    }
    ctl = mixer_get_ctl_by_name(adev->mixer, mixer_ctl_name);
    if (!ctl) {
        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, mixer_ctl_name);
        return -EINVAL;
    }
    ALOGV("HDMI channel count: %s", channel_cnt_str);
    mixer_ctl_set_enum_by_string(ctl, channel_cnt_str);
    return 0;
}

/* Legacy EDID channel retrieval */
#define MAX_EDID_BLOCKS             10
#define MAX_SHORT_AUDIO_DESC_CNT    30
#define MIN_AUDIO_DESC_LENGTH       3
#define MAX_CHANNELS_SUPPORTED      8

int legacy_edid_get_max_channels() {
    unsigned char channels[16];
    unsigned char* data = NULL;
    int i = 0;
    int count = 0;
    int channel_count = 0;
    int length = 0;
    int nCountDesc = 0;
    unsigned int sad[MAX_SHORT_AUDIO_DESC_CNT];

    const char* file = "/sys/class/graphics/fb1/audio_data_block";
    FILE* fpaudiocaps = fopen(file, "rb");
    if (fpaudiocaps) {
        ALOGV("%s: Opened audio_data_block successfully\n", __func__);
        fseek(fpaudiocaps, 0, SEEK_END);
        long size = ftell(fpaudiocaps);
        ALOGV("%s: audio_data_block size is %ld\n", __func__, size);
        data = (unsigned char*)malloc(size);
        if (data) {
            fseek(fpaudiocaps, 0, SEEK_SET);
            fread(data, 1, size, fpaudiocaps);
        }
        fclose(fpaudiocaps);
    } else {
        ALOGE("%s: Failed to open audio_data_block", __func__);
        return channel_count;
    }

    if (data) {
        memcpy(&count, data, sizeof(int));
        data += sizeof(int);
        ALOGV("%s: Audio Block Count is %d\n", __func__, count);
        memcpy(&length, data, sizeof(int));
        data += sizeof(int);
        ALOGV("%s: Total length is %d\n", __func__, length);

        for (i = 0; length >= MIN_AUDIO_DESC_LENGTH
                && count < MAX_SHORT_AUDIO_DESC_CNT; i++) {
            sad[i] =    (unsigned int)data[0]
                     + ((unsigned int)data[1] << 8)
                     + ((unsigned int)data[2] << 16);
            nCountDesc++;
            length -= MIN_AUDIO_DESC_LENGTH;
            data += MIN_AUDIO_DESC_LENGTH;
        }
        ALOGV("%s: Total # of audio descriptors is %d\n",
                __func__, nCountDesc);

        for (i = 0; i < nCountDesc; i++)
            channels[i] = (sad[i] & 0x7) + 1;

        free(data);
    }

    for (i = 0; i < nCountDesc && i < MAX_EDID_BLOCKS; i++) {
        if ((int)channels[i] > channel_count
                && (int)channels[i] <= MAX_CHANNELS_SUPPORTED)
            channel_count = (int)channels[i];
    }

    ALOGD("%s: Max channels reported by audio_data_block is: %d\n",
            __func__, channel_count);

    return channel_count;
}

int platform_edid_get_max_channels(void *platform)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    struct audio_device *adev = my_data->adev;
    char block[MAX_SAD_BLOCKS * SAD_BLOCK_SIZE];
    char *sad = block;
    int num_audio_blocks;
    int channel_count;
    int max_channels = 0;
    int i, ret, count;

    struct mixer_ctl *ctl;

    ctl = mixer_get_ctl_by_name(adev->mixer, AUDIO_DATA_BLOCK_MIXER_CTL);
    if (!ctl) {
        /* A-Family devices likely do not have HDMI EDID ctl,
         * attempt fall-back to legacy sysfs EDID retrieval.
         */
        max_channels = legacy_edid_get_max_channels();
        if (max_channels > 0)
            return max_channels;

        ALOGE("%s: Could not get ctl for mixer cmd - %s",
              __func__, AUDIO_DATA_BLOCK_MIXER_CTL);
        return 0;
    }

    mixer_ctl_update(ctl);

    count = mixer_ctl_get_num_values(ctl);

    /* Read SAD blocks, clamping the maximum size for safety */
    if (count > (int)sizeof(block))
        count = (int)sizeof(block);

    ret = mixer_ctl_get_array(ctl, block, count);
    if (ret != 0) {
        ALOGE("%s: mixer_ctl_get_array() failed to get EDID info", __func__);
        return 0;
    }

    /* Calculate the number of SAD blocks */
    num_audio_blocks = count / SAD_BLOCK_SIZE;

    for (i = 0; i < num_audio_blocks; i++) {
        /* Only consider LPCM blocks */
        if ((sad[0] >> 3) != EDID_FORMAT_LPCM) {
            sad += 3;
            continue;
        }

        channel_count = (sad[0] & 0x7) + 1;
        if (channel_count > max_channels)
            max_channels = channel_count;

        /* Advance to next block */
        sad += 3;
    }

    return max_channels;
}

static int platform_set_slowtalk(struct platform_data *my_data __unused, bool state __unused)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_set_parameters(void *platform, struct str_parms *parms)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    char *str;
    char value[256] = {0};
    int val;
    int ret = 0, err;
    char *kv_pairs = str_parms_to_str(parms);

    ALOGV_IF(kv_pairs != NULL, "%s: enter: %s", __func__, kv_pairs);

    err = str_parms_get_int(parms, AUDIO_PARAMETER_KEY_BTSCO, &val);
    if (err >= 0) {
        str_parms_del(parms, AUDIO_PARAMETER_KEY_BTSCO);
        my_data->btsco_sample_rate = val;
        if (val == SAMPLE_RATE_16KHZ) {
            audio_route_apply_path(my_data->adev->audio_route,
                                   "bt-sco-wb-samplerate");
            audio_route_update_mixer(my_data->adev->audio_route);
        }
    }

    err = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_SLOWTALK, value, sizeof(value));
    if (err >= 0) {
        bool state = false;
        if (!strncmp("true", value, sizeof("true"))) {
            state = true;
        }

        str_parms_del(parms, AUDIO_PARAMETER_KEY_SLOWTALK);
        ret = platform_set_slowtalk(my_data, state);
        if (ret)
            ALOGE("%s: Failed to set slow talk err: %d", __func__, ret);
    }

    ALOGV("%s: exit with code(%d)", __func__, ret);
    free(kv_pairs);
    return ret;
}

int platform_set_incall_recording_session_id(void *platform __unused,
                                             uint32_t session_id __unused, int rec_mode __unused)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_stop_incall_recording_usecase(void *platform __unused)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_start_incall_music_usecase(void *platform __unused)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

int platform_stop_incall_music_usecase(void *platform __unused)
{
    ALOGE("%s: Not implemented", __func__);
    return -ENOSYS;
}

void platform_get_parameters(void *platform,
                            struct str_parms *query,
                            struct str_parms *reply)
{
    struct platform_data *my_data = (struct platform_data *)platform;
    char *str = NULL;
    char value[256] = {0};
    int ret;
    char *kv_pairs = NULL;

    ret = str_parms_get_str(query, AUDIO_PARAMETER_KEY_SLOWTALK,
                            value, sizeof(value));
    if (ret >= 0) {
        str_parms_add_str(reply, AUDIO_PARAMETER_KEY_SLOWTALK,
                          my_data->slowtalk?"true":"false");
    }

    kv_pairs = str_parms_to_str(reply);
    ALOGV_IF(kv_pairs != NULL, "%s: exit: returns - %s", __func__, kv_pairs);
    free(kv_pairs);
}

/* Delay in Us */
int64_t platform_render_latency(audio_usecase_t usecase)
{
    switch (usecase) {
        case USECASE_AUDIO_PLAYBACK_DEEP_BUFFER:
            return DEEP_BUFFER_PLATFORM_DELAY;
        case USECASE_AUDIO_PLAYBACK_LOW_LATENCY:
            return LOW_LATENCY_PLATFORM_DELAY;
        default:
            return 0;
    }
}

int platform_update_usecase_from_source(int source, int usecase)
{
    ALOGV("%s: input source :%d", __func__, source);
    switch(source) {
        case AUDIO_SOURCE_VOICE_UPLINK:
            return USECASE_INCALL_REC_UPLINK;
        case AUDIO_SOURCE_VOICE_DOWNLINK:
            return USECASE_INCALL_REC_DOWNLINK;
        case AUDIO_SOURCE_VOICE_CALL:
            return USECASE_INCALL_REC_UPLINK_AND_DOWNLINK;
        default:
            return usecase;
    }
}

bool platform_listen_update_status(snd_device_t snd_device)
{
    if ((snd_device >= SND_DEVICE_IN_BEGIN) &&
        (snd_device < SND_DEVICE_IN_END) &&
        (snd_device != SND_DEVICE_IN_CAPTURE_FM) &&
        (snd_device != SND_DEVICE_IN_CAPTURE_VI_FEEDBACK))
        return true;
    else
        return false;
}

/* Read  offload buffer size from a property.
 * If value is not power of 2  round it to
 * power of 2.
 */
uint32_t platform_get_compress_offload_buffer_size(audio_offload_info_t* info)
{
    char value[PROPERTY_VALUE_MAX] = {0};
    uint32_t fragment_size = COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    if((property_get("audio.offload.buffer.size.kb", value, "")) &&
            atoi(value)) {
        fragment_size =  atoi(value) * 1024;
    }

    if (info != NULL && info->has_video && info->is_streaming) {
        fragment_size = COMPRESS_OFFLOAD_FRAGMENT_SIZE_FOR_AV_STREAMING;
        ALOGV("%s: offload fragment size reduced for AV streaming to %d",
               __func__, fragment_size);
    }

    fragment_size = ALIGN( fragment_size, 1024);

    if(fragment_size < MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MIN_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    else if(fragment_size > MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MAX_COMPRESS_OFFLOAD_FRAGMENT_SIZE;
    ALOGV("%s: fragment_size %d", __func__, fragment_size);
    return fragment_size;
}

uint32_t platform_get_pcm_offload_buffer_size(audio_offload_info_t* info)
{
    uint32_t fragment_size = MIN_PCM_OFFLOAD_FRAGMENT_SIZE;
    uint32_t bits_per_sample = 16;

#ifdef EXTN_OFFLOAD_ENABLED
    if (info->format == AUDIO_FORMAT_PCM_24_BIT_OFFLOAD) {
        bits_per_sample = 32;
    }
#endif

    if (!info->has_video) {
        fragment_size = MAX_PCM_OFFLOAD_FRAGMENT_SIZE;

    } else if (info->has_video && info->is_streaming) {
        fragment_size = (PCM_OFFLOAD_BUFFER_DURATION_FOR_AV_STREAMING
                                     * info->sample_rate
                                     * (bits_per_sample >> 3)
                                     * popcount(info->channel_mask))/1000;

    } else if (info->has_video) {
        fragment_size = (PCM_OFFLOAD_BUFFER_DURATION_FOR_AV
                                     * info->sample_rate
                                     * (bits_per_sample >> 3)
                                     * popcount(info->channel_mask))/1000;
    }

    char value[PROPERTY_VALUE_MAX] = {0};
    if((property_get("audio.offload.pcm.buffer.size", value, "")) &&
            atoi(value)) {
        fragment_size =  atoi(value) * 1024;
        ALOGV("Using buffer size from sys prop %d", fragment_size);
    }

    fragment_size = ALIGN( fragment_size, 1024);

    if(fragment_size < MIN_PCM_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MIN_PCM_OFFLOAD_FRAGMENT_SIZE;
    else if(fragment_size > MAX_PCM_OFFLOAD_FRAGMENT_SIZE)
        fragment_size = MAX_PCM_OFFLOAD_FRAGMENT_SIZE;

    ALOGV("%s: fragment_size %d", __func__, fragment_size);
    return fragment_size;
}

int platform_set_snd_device_backend(snd_device_t device, const char *backend)
{
    int ret = 0;

    if ((device < SND_DEVICE_MIN) || (device >= SND_DEVICE_MAX)) {
        ALOGE("%s: Invalid snd_device = %d",
            __func__, device);
        ret = -EINVAL;
        goto done;
    }

    if (backend_table[device]) {
        free(backend_table[device]);
    }
    backend_table[device] = strdup(backend);
done:
    return ret;
}

int platform_set_usecase_pcm_id(audio_usecase_t usecase, int32_t type, int32_t pcm_id)
{
    int ret = 0;
    if ((usecase <= USECASE_INVALID) || (usecase >= AUDIO_USECASE_MAX)) {
        ALOGE("%s: invalid usecase case idx %d", __func__, usecase);
        ret = -EINVAL;
        goto done;
    }

    if ((type != 0) && (type != 1)) {
        ALOGE("%s: invalid usecase type", __func__);
        ret = -EINVAL;
    }
    pcm_device_table[usecase][type] = pcm_id;
done:
    return ret;
}

int platform_set_snd_device_name(snd_device_t device, const char *name)
{
    int ret = 0;

    if ((device < SND_DEVICE_MIN) || (device >= SND_DEVICE_MAX)) {
        ALOGE("%s:: Invalid snd_device = %d", __func__, device);
        ret = -EINVAL;
        goto done;
    }

    device_table[device] = strdup(name);
done:
    return ret;
}