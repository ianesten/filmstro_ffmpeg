/*
  ==============================================================================
  Copyright (c) 2017, Filmstro Ltd. - Daniel Walz
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:
  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
  3. Neither the name of the copyright holder nor the names of its contributors
     may be used to endorse or promote products derived from this software without
     specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
  OF THE POSSIBILITY OF SUCH DAMAGE.
  ==============================================================================
  \class        FFmpegVideoWriter
  \file         filmstro_ffmpeg_FFmpegVideoWriter.cpp
  \brief        A component to write a video stream with ffmpeg

  \author       Daniel Walz @ filmstro.com
  \date         September 19th 2016

  \description  Use this writer to write audio frames and video frames in a 
                video file using ffmpeg
  ==============================================================================
 */


#include "../JuceLibraryCode/JuceHeader.h"


FFmpegVideoWriter::FFmpegVideoWriter()
 :  writePosition   (0),
    formatContext   (nullptr),
    videoContext    (nullptr),
    audioContext    (nullptr),
    subtitleContext (nullptr),
    videoCodec      (AV_CODEC_ID_NONE),
    audioCodec      (AV_CODEC_ID_NONE),
    subtitleCodec   (AV_CODEC_ID_NONE),
    videoStreamIdx  (-1),
    audioStreamIdx  (-1),
    subtitleStreamIdx (-1),
    sampleRate      (48000),
    channelLayout   (AV_CH_LAYOUT_STEREO),
    videoWidth      (0),
    videoHeight     (0),
    pixelFormat     (AV_PIX_FMT_NONE),
    pixelAspect     (AVRational({1, 1})),
    audioFifo       (2, 8192)
{
    formatContext = nullptr;
    videoTimeBase = AV_TIME_BASE_Q;
    audioTimeBase = av_make_q (sampleRate, 1);
    subtitleTimeBase = AV_TIME_BASE_Q;
    av_register_all();
}

FFmpegVideoWriter::~FFmpegVideoWriter()
{
}

void FFmpegVideoWriter::setVideoCodec (const AVCodecID codec)
{
    videoCodec = codec;
}

void FFmpegVideoWriter::setAudioCodec (const AVCodecID codec)
{
    audioCodec = codec;
}

void FFmpegVideoWriter::setSubtitleCodec (const AVCodecID codec)
{
    subtitleCodec = codec;
}

void FFmpegVideoWriter::setSampleRate (const int newSampleRate)
{
    sampleRate = newSampleRate;
    audioTimeBase = av_make_q (newSampleRate, 1);
}

void FFmpegVideoWriter::setVideoSize (const int width, const int height)
{
    videoWidth = width;
    videoHeight = height;
}

void FFmpegVideoWriter::setPixelFormat (const AVPixelFormat format)
{
    pixelFormat = format;
}

void FFmpegVideoWriter::setPixelAspect (const int num, const int den)
{
    pixelAspect = av_make_q (num, den);
}

void FFmpegVideoWriter::setTimeBase (AVMediaType type, AVRational timebase)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:
            videoTimeBase = timebase;
            break;
        case AVMEDIA_TYPE_AUDIO:
            audioTimeBase = timebase;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            subtitleTimeBase = timebase;
            break;
        default:
            break;
    }
}

void FFmpegVideoWriter::copySettingsFromContext (const AVCodecContext* context)
{
    if (context) {
        if (context->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoCodec  = context->codec_id;
            videoWidth  = context->width;
            videoHeight = context->height;
            pixelFormat = context->pix_fmt;
            pixelAspect = context->sample_aspect_ratio;
            videoTimeBase = context->time_base;
        }
        else if (context->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioCodec  = context->codec_id;
            sampleRate  = context->sample_rate;
            channelLayout = context->channel_layout;
            // we don't copy audio, it's coming from JUCE so it's gonna be AV_SAMPLE_FMT_FLTP
        }
        else if (context->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            // FIXME: TODO
        }
    }
}

bool FFmpegVideoWriter::openMovieFile (const juce::File& outputFile)
{
    if (formatContext) {
        avformat_free_context (formatContext);
    }

    videoStreamIdx  = -1;
    audioStreamIdx  = -1;
    subtitleStreamIdx = -1;

    writePosition   = 0;

    /* allocate the output media context */
    avformat_alloc_output_context2 (&formatContext, NULL, NULL, outputFile.getFullPathName().toRawUTF8());
    if (!formatContext) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2 (&formatContext, NULL, "mpeg", outputFile.getFullPathName().toRawUTF8());
    }
    if (!formatContext)
        return false;

    if (videoCodec > AV_CODEC_ID_NONE) {
        AVStream* stream = avformat_new_stream (formatContext, NULL);
        if (!stream) {
            DBG ("Failed allocating video output stream\n");
        }
        else {
            videoStreamIdx = formatContext->nb_streams - 1;
            AVCodec* encoder = avcodec_find_encoder (videoCodec);
            if (encoder) {
                videoContext = avcodec_alloc_context3 (encoder);
                videoContext->time_base = videoTimeBase;
                videoContext->pix_fmt   = pixelFormat;
                videoContext->sample_aspect_ratio = pixelAspect;
                videoContext->codec_type = encoder->type;
                videoContext->codec_id  = videoCodec;
                videoContext->width     = videoWidth;
                videoContext->height    = videoHeight;
                avcodec_parameters_from_context (stream->codecpar, videoContext);

                AVDictionary* options = nullptr;

                if (encoder->id == AV_CODEC_ID_H264) {
                    av_dict_set (&options, "preset", "medium", 0);
                    av_dict_set (&options, "tune", "film", 0);
                }

                int ret = avcodec_open2 (videoContext, encoder, &options);
                if (ret < 0) {
                    DBG ("Cannot open video encoder");
                    videoStreamIdx = -1;
                    videoCodec = AV_CODEC_ID_NONE;
                }
                av_dict_free (&options);
            }
        }
    }

    if (audioCodec > AV_CODEC_ID_NONE) {
        AVStream* stream = avformat_new_stream (formatContext, NULL);
        if (!stream) {
            DBG ("Failed allocating audio output stream\n");
        }
        else {
            audioStreamIdx = formatContext->nb_streams - 1;
            AVCodec* encoder = avcodec_find_encoder (audioCodec);
            if (encoder) {
                audioContext = avcodec_alloc_context3 (encoder);
                audioContext->time_base = audioTimeBase;
                audioContext->sample_rate = sampleRate;
                audioContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
                audioContext->channel_layout = channelLayout;
                audioContext->channels = av_get_channel_layout_nb_channels (channelLayout);
                avcodec_parameters_from_context (stream->codecpar, audioContext);

                int ret = avcodec_open2 (audioContext, encoder, NULL);
                if (ret < 0) {
                    char codecName [256];
                    av_get_codec_tag_string (codecName, 256, audioCodec);
                    DBG ("Cannot open audio encoder for format: " + String::fromUTF8 (codecName));
                    audioStreamIdx = -1;
                    audioCodec = AV_CODEC_ID_NONE;
                }

            }
        }
    }

    //if (subtitleCodec > AV_CODEC_ID_NONE) {
    //    // FIXME: TODO
    //}

    av_dump_format (formatContext, 0, outputFile.getFullPathName().toRawUTF8(), 1);

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open (&formatContext->pb, outputFile.getFullPathName().toRawUTF8(), AVIO_FLAG_WRITE) < 0) {
            DBG ("Could not open output file '" + outputFile.getFullPathName() + "'");
            closeContexts();
            return false;
        }
    }

    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
        if (videoContext) videoContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (audioContext) audioContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (subtitleContext) subtitleContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (!videoContext && !audioContext && !subtitleContext) {
        DBG ("No stream provided to encode");
        closeContexts();
        return false;
    }

    if (!(formatContext->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&formatContext->pb, outputFile.getFullPathName().toRawUTF8(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", outputFile.getFullPathName().toRawUTF8());
            closeContexts();
            return false;
        }
    }

    int ret = avformat_write_header (formatContext, NULL);
    if (ret < 0) {
        DBG ("Error occurred when opening output file");
        closeContexts();
        return false;
    }

    return true;
}

void FFmpegVideoWriter::closeMovieFile ()
{
    finishWriting ();
}

void FFmpegVideoWriter::finishWriting ()
{
    //FIXME: flush all buffers

    if (formatContext)
        av_write_trailer (formatContext);

    closeContexts();
}

void FFmpegVideoWriter::closeContexts ()
{
    avformat_free_context (formatContext);
    videoStreamIdx  = -1;
    audioStreamIdx  = -1;
    subtitleStreamIdx = -1;
    videoContext = nullptr;
    audioContext = nullptr;
    subtitleContext = nullptr;
    avcodec_free_context (&videoContext);
    avcodec_free_context (&audioContext);
    avcodec_free_context (&subtitleContext);
    formatContext   = nullptr;
    writePosition   = 0;
}

void FFmpegVideoWriter::writeNextAudioBlock (juce::AudioSourceChannelInfo& info)
{
    audioFifo.addToFifo (*info.buffer, info.numSamples);
    writeAudioFrame (false);
}

void FFmpegVideoWriter::writeNextVideoFrame (const AVFrame* frame)
{
    if (formatContext &&
        isPositiveAndBelow (videoStreamIdx, static_cast<int> (formatContext->nb_streams))) {
        AVPacket packet;
        packet.data = nullptr;
        packet.size = 0;
        av_init_packet (&packet);

        packet.pts = frame->pts;
        DBG ("Start writing video frame");

        int got_output = 0;
        if (avcodec_encode_video2 (videoContext, &packet, frame, &got_output) >= 0) {
            if (got_output) {
                av_write_frame (formatContext, &packet);
            }
        }
        av_packet_unref (&packet);
    }
}

void FFmpegVideoWriter::writeNextVideoFrame (const juce::Image& image, const AVRational timestamp)
{
    // TODO: use scaler and add interface for image processing / e.g. branding
}

bool FFmpegVideoWriter::writeAudioFrame (const bool flush)
{
    if (formatContext &&
        isPositiveAndBelow (audioStreamIdx, static_cast<int> (formatContext->nb_streams)))
    {

        int numFrameSamples  = 1024;

        if (audioFifo.getNumReady() >= numFrameSamples || flush) {

            AVPacket packet;
            packet.data = nullptr;
            packet.size = 0;
            av_init_packet (&packet);
            packet.pts  = writePosition;
            const uint64_t channelLayout = AV_CH_LAYOUT_STEREO;
            const int numChannels = av_get_channel_layout_nb_channels (channelLayout);
            AVFrame* frame = av_frame_alloc ();
            frame->nb_samples   = numFrameSamples;
            frame->format       = AV_SAMPLE_FMT_FLTP;
            frame->channel_layout = channelLayout;
            frame->channels     = numChannels;
            frame->pts          = writePosition;
            DBG ("Start writing audio frame");
            int bufferSize = av_samples_get_buffer_size (nullptr, frame->channels, frame->nb_samples, AV_SAMPLE_FMT_FLTP, 0);
            float* samples = static_cast<float*> (av_malloc (bufferSize));

            avcodec_fill_audio_frame (frame,
                                      frame->channels,
                                      static_cast<enum AVSampleFormat> (frame->format),
                                      (const uint8_t*) (samples),
                                      bufferSize,
                                      0);

            float** sampleData = new float*[numChannels];
            for (int i=0; i < frame->channels; ++i) {
                sampleData[i] = samples + i * frame->nb_samples;
            }
            audioFifo.readFromFifo (sampleData, numFrameSamples);

            int got_output = 0;
            if (avcodec_encode_audio2 (audioContext, &packet, frame, &got_output) >= 0) {
                if (got_output) {
                    av_write_frame (formatContext, &packet);
                }
            }
            delete[] sampleData;
            //av_freep (buffer);
            av_packet_unref (&packet);
            
            writePosition += numFrameSamples;
        }
        
        return true;
    }
    return false;
}

void FFmpegVideoWriter::displayNewFrame (const AVFrame* frame)
{
    // simply forward it to ffmpeg
    writeNextVideoFrame (frame);
}


