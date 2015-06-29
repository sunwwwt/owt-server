/*
 * Copyright 2015 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include "MediaRecorder.h"
#include <rtputils.h>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace mcu {

DEFINE_LOGGER(MediaRecorder, "mcu.media.MediaRecorder");

inline AVCodecID payloadType2VideoCodecID(int payloadType)
{
    switch (payloadType) {
        case VP8_90000_PT: return AV_CODEC_ID_VP8;
        case H264_90000_PT: return AV_CODEC_ID_H264;
        default: return AV_CODEC_ID_VP8;
    }
}

inline AVCodecID payloadType2AudioCodecID(int payloadType)
{
    switch (payloadType) {
        case PCMU_8000_PT: return AV_CODEC_ID_PCM_MULAW;
        case OPUS_48000_PT: return AV_CODEC_ID_OPUS;
        default: return AV_CODEC_ID_PCM_MULAW;
    }
}

MediaRecorder::MediaRecorder(const std::string& recordUrl, int snapshotInterval)
    : m_videoSource(nullptr)
    , m_audioSource(nullptr)
    , m_videoStream(nullptr)
    , m_audioStream(nullptr)
    , m_context(nullptr)
    , m_videoId(-1)
    , m_audioId(-1)
    , m_recordPath(recordUrl)
    , m_snapshotInterval(snapshotInterval)
{
    timeval time;
    gettimeofday(&time, nullptr);
    m_recordStartTime = (time.tv_sec * 1000) + (time.tv_usec / 1000);

    m_videoQueue.reset(new woogeen_base::MediaFrameQueue(m_recordStartTime));
    m_audioQueue.reset(new woogeen_base::MediaFrameQueue(m_recordStartTime));

    init();
    ELOG_DEBUG("created");
}

MediaRecorder::~MediaRecorder()
{
    if (m_muxing)
        close();
}

bool MediaRecorder::setMediaSource(woogeen_base::FrameDispatcher* videoSource, woogeen_base::FrameDispatcher* audioSource)
{
    if (m_videoSource && m_videoId != -1)
        m_videoSource->removeFrameConsumer(m_videoId);

    if (m_audioSource && m_audioId != -1)
        m_audioSource->removeFrameConsumer(m_audioId);

    m_videoSource = videoSource;
    m_audioSource = audioSource;

    // Start the recording of video and audio
    m_videoId = m_videoSource->addFrameConsumer(m_recordPath, VP8_90000_PT, this);
    m_audioId = m_audioSource->addFrameConsumer(m_recordPath, PCMU_8000_PT, this);

    return true;
}

void MediaRecorder::unsetMediaSource()
{
    if (m_videoSource && m_videoId != -1)
        m_videoSource->removeFrameConsumer(m_videoId);

    m_videoSource = nullptr;
    m_videoId = -1;

    if (m_audioSource && m_audioId != -1)
        m_audioSource->removeFrameConsumer(m_audioId);

    m_audioSource = nullptr;
    m_audioId = -1;
}

bool MediaRecorder::init()
{
    // FIXME: These should really only be called once per application run
    av_register_all();
    avcodec_register_all();
    av_log_set_level(AV_LOG_WARNING);

    m_context = avformat_alloc_context();
    if (m_context == NULL) {
        ELOG_ERROR("Error allocating memory for recording file IO context.");
        return false;
    }

    m_recordPath.copy(m_context->filename, sizeof(m_context->filename), 0);
    m_context->oformat = av_guess_format(NULL, m_context->filename, NULL);
    if (!m_context->oformat){
        ELOG_ERROR("Error guessing recording file format %s", m_context->filename);
        return false;
    }

    // File write thread
    m_muxing = true;
    m_thread = boost::thread(&MediaRecorder::recordLoop, this);

    return true;
}

void MediaRecorder::close()
{
    m_muxing = false;
    m_thread.join();

    if (m_audioStream != NULL && m_videoStream != NULL && m_context != NULL)
        av_write_trailer(m_context);

    if (m_videoStream && m_videoStream->codec != NULL)
        avcodec_close(m_videoStream->codec);

    if (m_audioStream && m_audioStream->codec != NULL)
        avcodec_close(m_audioStream->codec);

    if (m_context != NULL){
        if (!(m_context->oformat->flags & AVFMT_NOFILE))
            avio_close(m_context->pb);
        avformat_free_context(m_context);
        m_context = NULL;
    }
    ELOG_DEBUG("closed");
}

void MediaRecorder::onFrame(const woogeen_base::Frame& frame)
{
    if (m_status == woogeen_base::MediaMuxer::Context_ERROR)
        return;
    switch (frame.format) {
    case woogeen_base::FRAME_FORMAT_VP8:
        if (!m_videoStream) {
            addVideoStream(payloadType2VideoCodecID(VP8_90000_PT), frame.additionalInfo.video.width, frame.additionalInfo.video.height);
            ELOG_DEBUG("video stream added: %dx%d", frame.additionalInfo.video.width, frame.additionalInfo.video.height);
        }
        m_videoQueue->pushFrame(frame.payload, frame.length, frame.timeStamp);
        break;
    case woogeen_base::FRAME_FORMAT_PCMU:
        if (m_videoStream && !m_audioStream) { // make sure video stream is added first.
            addAudioStream(payloadType2AudioCodecID(PCMU_8000_PT), frame.additionalInfo.audio.channels, frame.additionalInfo.audio.sampleRate);
            ELOG_DEBUG("audio stream added: %d channel(s), %d Hz", frame.additionalInfo.audio.channels, frame.additionalInfo.audio.sampleRate);
        }
        m_audioQueue->pushFrame(frame.payload, frame.length, frame.timeStamp);
        break;
    default:
        break;
    }
}

void MediaRecorder::addAudioStream(enum AVCodecID codec_id, int nbChannels, int sampleRate)
{
    boost::lock_guard<boost::mutex> lock(m_contextMutex);
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        ELOG_ERROR("cannot add audio stream");
        m_status = woogeen_base::MediaMuxer::Context_ERROR;
        return;
    }
    AVCodecContext* c = stream->codec;
    c->codec_id       = codec_id;
    c->codec_type     = AVMEDIA_TYPE_AUDIO;
    c->channels       = nbChannels;
    c->channel_layout = av_get_default_channel_layout(nbChannels);
    c->sample_rate    = sampleRate;
    c->sample_fmt     = AV_SAMPLE_FMT_S16;
    stream->time_base = (AVRational){ 1, c->sample_rate };

    if (m_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    m_audioStream = stream;
}

void MediaRecorder::addVideoStream(enum AVCodecID codec_id, unsigned int width, unsigned int height)
{
    boost::lock_guard<boost::mutex> lock(m_contextMutex);
    m_context->oformat->video_codec = codec_id;
    AVStream* stream = avformat_new_stream(m_context, nullptr);
    if (!stream) {
        ELOG_ERROR("cannot add video stream");
        m_status = woogeen_base::MediaMuxer::Context_ERROR;
        return;
    }
    AVCodecContext* c = stream->codec;
    c->codec_id   = codec_id;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    c->width      = width;
    c->height     = height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
    * of which frame timestamps are represented. For fixed-fps content,
    * timebase should be 1/framerate and timestamp increments should be
    * identical to 1. */
    stream->time_base = (AVRational){ 1, 30 };
    c->time_base      = stream->time_base;
    c->pix_fmt        = AV_PIX_FMT_YUV420P;
    /* Some formats want stream headers to be separate. */
    if (m_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    m_context->oformat->flags |= AVFMT_VARIABLE_FPS;
    m_videoStream = stream;
}

void MediaRecorder::recordLoop()
{
    while (m_muxing) {
        switch (m_status) {
        case woogeen_base::MediaMuxer::Context_EMPTY:
            if (m_audioStream && m_videoStream) {
                if (!(m_context->oformat->flags & AVFMT_NOFILE)) {
                    if (avio_open(&m_context->pb, m_context->filename, AVIO_FLAG_WRITE) < 0) {
                        ELOG_ERROR("open output file failed");
                        m_status = woogeen_base::MediaMuxer::Context_ERROR;
                        return;
                    }
                }
                av_dump_format(m_context, 0, m_context->filename, 1);
                if (avformat_write_header(m_context, nullptr) < 0) {
                    m_status = woogeen_base::MediaMuxer::Context_ERROR;
                    return;
                }
                m_status = woogeen_base::MediaMuxer::Context_READY;
                ELOG_DEBUG("context ready");
            } else {
                usleep(1000);
                continue;
            }
            break;
        case woogeen_base::MediaMuxer::Context_READY:
            break;
        case woogeen_base::MediaMuxer::Context_ERROR:
        default:
            ELOG_ERROR("loop exit on error");
            return;
        }
        boost::shared_ptr<woogeen_base::EncodedFrame> mediaFrame;
        while (mediaFrame = m_audioQueue->popFrame())
            this->writeAudioFrame(*mediaFrame);

        while (mediaFrame = m_videoQueue->popFrame())
            this->writeVideoFrame(*mediaFrame);
    }
}

void MediaRecorder::writeVideoFrame(woogeen_base::EncodedFrame& encodedVideoFrame) {
    if (m_videoStream == NULL)
        // could not init our context yet.
        return;

    timeval time;
    gettimeofday(&time, nullptr);
    long long timestampToWrite = ((time.tv_sec * 1000) + (time.tv_usec / 1000)) - m_recordStartTime;
    timestampToWrite  = timestampToWrite / (1000 / m_videoStream->time_base.den);

    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = encodedVideoFrame.m_payloadData;
    avpkt.size = encodedVideoFrame.m_payloadSize;
    avpkt.pts = timestampToWrite;
    avpkt.stream_index = 0;
    av_write_frame(m_context, &avpkt);
    av_free_packet(&avpkt);
}

void MediaRecorder::writeAudioFrame(woogeen_base::EncodedFrame& encodedAudioFrame) {
    if (m_audioStream == NULL)
        // No audio stream has been initialized
        return;

    timeval time;
    gettimeofday(&time, nullptr);
    long long timestampToWrite = ((time.tv_sec * 1000) + (time.tv_usec / 1000)) - m_recordStartTime;
    timestampToWrite  = timestampToWrite / (1000 / m_videoStream->time_base.den);

    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = encodedAudioFrame.m_payloadData;
    avpkt.size = encodedAudioFrame.m_payloadSize;
    avpkt.pts = timestampToWrite;
    avpkt.stream_index = 1;
    av_write_frame(m_context, &avpkt);
    av_free_packet(&avpkt);
}

}/* namespace mcu */
