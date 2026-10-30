// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "sys/av_player.h"

extern "C" int sys_file_translate(const char* guest, char* out, size_t size);
extern "C" void runtime_memory_invalidate(uintptr_t address, uint64_t size);

struct SysAvMedia {
    SysAvFileCalls file{};
    uint64_t file_bytes = 0, position = 0;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    bool file_open = false;
    std::vector<unsigned> supported;
    int stream_index[2] = {-1, -1};
    AVCodecContext* codec[2] = {nullptr, nullptr};
    AVFrame* frame = nullptr;
    AVFrame* audio_frame = nullptr;
    SwsContext* scale = nullptr;
    SwrContext* resample = nullptr;
    AVFrame* nv12 = nullptr;
    int scale_width = 0, scale_height = 0, scale_format = -1;
};

namespace {

constexpr int IoBufferBytes = 64 * 1024;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int ReadFromGuest(void* opaque, uint8_t* buffer, int size) {
    auto* media = static_cast<SysAvMedia*>(opaque);
    if (media->position >= media->file_bytes) {
        return AVERROR_EOF;
    }
    size = int(std::min<uint64_t>(uint64_t(size), media->file_bytes - media->position));
    const int32_t got = media->file.read_offset(media->file.object, buffer, media->position, uint32_t(size));
    if (got <= 0) {
        return AVERROR_EOF;
    }
    media->position += uint64_t(got);
    return got;
}

int64_t SeekInGuest(void* opaque, int64_t offset, int whence) {
    auto* media = static_cast<SysAvMedia*>(opaque);
    if (whence & AVSEEK_SIZE) {
        return int64_t(media->file_bytes);
    }
    int64_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = int64_t(media->position);
        break;
    case SEEK_END:
        base = int64_t(media->file_bytes);
        break;
    default:
        return -1;
    }
    media->position = uint64_t(std::clamp<int64_t>(base + offset, 0, int64_t(media->file_bytes)));
    return int64_t(media->position);
}

uint64_t ToMillis(int64_t value, AVRational base) {
    if (value == AV_NOPTS_VALUE || value <= 0 || base.num <= 0 || base.den <= 0) {
        return 0;
    }
    const int64_t millis = av_rescale_q(value, base, AVRational{1, 1000});
    return millis > 0 ? uint64_t(millis) : 0;
}

uint64_t StreamMillis(const AVFormatContext* format, const AVStream* stream) {
    const uint64_t own = ToMillis(stream->duration, stream->time_base);
    return own ? own : ToMillis(format->duration, AVRational{1, AV_TIME_BASE});
}

uint64_t FrameMillis(const AVFrame* frame, AVRational base) {
    int64_t stamp = frame->best_effort_timestamp;
    if (stamp == AV_NOPTS_VALUE) {
        stamp = frame->pts;
    }
    if (stamp == AV_NOPTS_VALUE) {
        stamp = frame->pkt_dts;
    }
    if (stamp == AV_NOPTS_VALUE || stamp < 0 || base.den <= 0) {
        return 0;
    }
    const int64_t millis = av_rescale_q(stamp, base, AVRational{1, 1000});
    return millis > 0 ? uint64_t(millis) : 0;
}

SysAvMedia* Open(const char* path, const SysAvFileCalls* file) {
    auto* media = new SysAvMedia();
    media->frame = av_frame_alloc();
    media->audio_frame = av_frame_alloc();
    media->format = avformat_alloc_context();
    int opened;
    if (file->open) {
        media->file = *file;
        if (media->file.open(media->file.object, path) < 0) {
            avformat_free_context(media->format);
            av_frame_free(&media->frame);
            av_frame_free(&media->audio_frame);
            delete media;
            return nullptr;
        }
        media->file_open = true;
        media->file_bytes = media->file.size(media->file.object);
        auto* buffer = static_cast<uint8_t*>(av_malloc(IoBufferBytes));
        media->io = avio_alloc_context(buffer, IoBufferBytes, 0, media, ReadFromGuest, nullptr, SeekInGuest);
        media->format->pb = media->io;
        media->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        opened = avformat_open_input(&media->format, nullptr, nullptr, nullptr);
    } else {
        char host[1024];
        if (sys_file_translate(path, host, sizeof(host)) != 0) {
            avformat_free_context(media->format);
            av_frame_free(&media->frame);
            av_frame_free(&media->audio_frame);
            delete media;
            return nullptr;
        }
        opened = avformat_open_input(&media->format, host, nullptr, nullptr);
    }
    if (opened < 0) {
        if (media->io) {
            av_freep(&media->io->buffer);
            avio_context_free(&media->io);
        }
        if (media->file_open) {
            media->file.close(media->file.object);
        }
        av_frame_free(&media->frame);
        av_frame_free(&media->audio_frame);
        delete media;
        return nullptr;
    }
    return media;
}

void End(SysAvMedia* media) {
    avcodec_free_context(&media->codec[0]);
    avcodec_free_context(&media->codec[1]);
    sws_freeContext(media->scale);
    media->scale = nullptr;
    swr_free(&media->resample);
    av_frame_free(&media->nv12);
    media->scale_format = -1;
}

void Close(SysAvMedia* media) {
    End(media);
    avformat_close_input(&media->format);
    if (media->io) {
        av_freep(&media->io->buffer);
        avio_context_free(&media->io);
    }
    if (media->file_open) {
        media->file.close(media->file.object);
    }
    av_frame_free(&media->frame);
    av_frame_free(&media->audio_frame);
    delete media;
}

int FindStreams(SysAvMedia* media) {
    if (avformat_find_stream_info(media->format, nullptr) < 0) {
        return 0;
    }
    media->supported.clear();
    for (unsigned i = 0; i < media->format->nb_streams; ++i) {
        const AVCodecID id = media->format->streams[i]->codecpar->codec_id;
        if (id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_AAC) {
            media->supported.push_back(i);
        }
    }
    return !media->supported.empty();
}

int StreamCount(SysAvMedia* media) {
    return int(media->supported.size());
}

int StreamInfo(SysAvMedia* media, uint32_t index, SysAvStreamInfo* info) {
    if (index >= media->supported.size()) {
        return 0;
    }
    const AVStream* stream = media->format->streams[media->supported[index]];
    const AVCodecParameters* parameters = stream->codecpar;
    std::memset(info, 0, sizeof(*info));
    info->duration = StreamMillis(media->format, stream);
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    const auto copy_language = [&](char* target) {
        if (language) {
            std::memcpy(target, language->value, std::min<size_t>(std::strlen(language->value), 3));
        }
    };
    switch (parameters->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        info->type = SYS_AV_VIDEO;
        info->details.video.aspect_ratio = float(parameters->width) / float(parameters->height);
        info->details.video.width = AlignUp(uint32_t(parameters->width), 16);
        info->details.video.height = AlignUp(uint32_t(parameters->height), 16);
        copy_language(info->details.video.language);
        break;
    case AVMEDIA_TYPE_AUDIO:
        info->type = SYS_AV_AUDIO;
        info->details.audio.channel_count = uint16_t(parameters->ch_layout.nb_channels);
        info->details.audio.sample_rate = uint32_t(parameters->sample_rate);
        copy_language(info->details.audio.language);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        info->type = SYS_AV_TEXT;
        info->details.text.font_size = 12;
        info->details.text.text_size = 12;
        copy_language(info->details.text.language);
        break;
    default:
        info->type = SYS_AV_UNKNOWN;
        break;
    }
    return 1;
}

int EnableStream(SysAvMedia* media, uint32_t index) {
    if (index >= media->supported.size()) {
        return -1;
    }
    const int ffmpeg_index = int(media->supported[index]);
    switch (media->format->streams[ffmpeg_index]->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        media->stream_index[SYS_AV_VIDEO] = ffmpeg_index;
        return SYS_AV_VIDEO;
    case AVMEDIA_TYPE_AUDIO:
        media->stream_index[SYS_AV_AUDIO] = ffmpeg_index;
        return SYS_AV_AUDIO;
    default:
        return SYS_AV_TEXT;
    }
}

void SeekToStart(SysAvMedia* media) {
    for (const int index : media->stream_index) {
        if (index >= 0) {
            const AVStream* stream = media->format->streams[index];
            avformat_seek_file(media->format, index, 0, 0, stream->duration, 0);
        }
    }
}

int Begin(SysAvMedia* media, uint32_t* video_bytes) {
    SeekToStart(media);
    for (int kind = 0; kind < 2; ++kind) {
        const int index = media->stream_index[kind];
        if (index < 0) {
            continue;
        }
        const AVCodecParameters* parameters = media->format->streams[index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) {
            return 0;
        }
        media->codec[kind] = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(media->codec[kind], parameters) < 0 ||
            avcodec_open2(media->codec[kind], codec, nullptr) < 0) {
            return 0;
        }
        if (kind == SYS_AV_VIDEO) {
            const uint32_t pitch = AlignUp(uint32_t(media->codec[kind]->width), 64);
            const uint32_t height = AlignUp(uint32_t(media->codec[kind]->height), 16);
            *video_bytes = pitch * height * 3 / 2;
        }
    }
    return 1;
}

int ReadPacket(SysAvMedia* media, void** out, int* kind) {
    for (;;) {
        AVPacket* packet = av_packet_alloc();
        const int result = av_read_frame(media->format, packet);
        if (result < 0) {
            av_packet_free(&packet);
            return result == AVERROR_EOF ? 0 : -1;
        }
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (packet->stream_index == media->stream_index[candidate]) {
                *out = packet;
                *kind = candidate;
                return 1;
            }
        }
        av_packet_free(&packet);
    }
}

int Rewind(SysAvMedia* media) {
    avio_seek(media->format->pb, 0, SEEK_SET);
    SeekToStart(media);
    return 0;
}

void FreePacket(void* packet) {
    auto* typed = static_cast<AVPacket*>(packet);
    av_packet_free(&typed);
}

int SendPacket(SysAvMedia* media, int kind, void* packet) {
    const int result = avcodec_send_packet(media->codec[kind], static_cast<const AVPacket*>(packet));
    return result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF ? -1 : 0;
}

void CopyVideo(uint8_t* target, const uint8_t* const planes[2], const int pitches[2], uint32_t width, uint32_t height) {
    const uint32_t pitch = AlignUp(width, 64), rows = AlignUp(height, 16);
    uint8_t* luma = target;
    uint8_t* chroma = target + size_t(pitch) * rows;
    for (uint32_t y = 0; y < rows; ++y) {
        std::memcpy(luma + size_t(y) * pitch, planes[0] + size_t(std::min(y, height - 1)) * size_t(pitches[0]), width);
    }
    const uint32_t half = height / 2;
    for (uint32_t y = 0; y < rows / 2; ++y) {
        std::memcpy(chroma + size_t(y) * pitch, planes[1] + size_t(std::min(y, half - 1)) * size_t(pitches[1]), width);
    }
}

int ReceiveVideo(SysAvMedia* media, void* buffer, uint32_t size, SysAvFrame* out) {
    AVFrame* frame = media->frame;
    const uint32_t width = uint32_t(frame->width), height = uint32_t(frame->height);
    const uint32_t pitch = AlignUp(width, 64), rows = AlignUp(height, 16);
    if (size_t(pitch) * rows * 3 / 2 > size) {
        return SYS_AV_ERROR;
    }
    if (frame->format == AV_PIX_FMT_NV12) {
        const uint8_t* const planes[2] = {frame->data[0], frame->data[1]};
        CopyVideo(static_cast<uint8_t*>(buffer), planes, frame->linesize, width, height);
    } else {
        if (!media->scale || media->scale_width != frame->width || media->scale_height != frame->height ||
            media->scale_format != frame->format) {
            sws_freeContext(media->scale);
            av_frame_free(&media->nv12);
            media->scale = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                                          frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            media->nv12 = av_frame_alloc();
            media->nv12->format = AV_PIX_FMT_NV12;
            media->nv12->width = frame->width;
            media->nv12->height = frame->height;
            if (!media->scale || av_frame_get_buffer(media->nv12, 0) < 0) {
                return SYS_AV_ERROR;
            }
            media->scale_width = frame->width;
            media->scale_height = frame->height;
            media->scale_format = frame->format;
        }
        if (sws_scale(media->scale, frame->data, frame->linesize, 0, frame->height, media->nv12->data,
                      media->nv12->linesize) < 0) {
            return SYS_AV_ERROR;
        }
        const uint8_t* const planes[2] = {media->nv12->data[0], media->nv12->data[1]};
        CopyVideo(static_cast<uint8_t*>(buffer), planes, media->nv12->linesize, width, height);
    }
    runtime_memory_invalidate(reinterpret_cast<uintptr_t>(buffer), size_t(AlignUp(width, 16)) * rows * 3 / 2);
    const AVStream* stream = media->format->streams[media->stream_index[SYS_AV_VIDEO]];
    out->timestamp = FrameMillis(frame, stream->time_base);
    out->width = AlignUp(width, 16);
    out->height = rows;
    out->pitch = pitch;
    out->aspect_ratio = float(av_q2d(frame->sample_aspect_ratio));
    out->crop_left = uint32_t(frame->crop_left);
    out->crop_right = uint32_t(frame->crop_right) + (pitch - width);
    out->crop_top = uint32_t(frame->crop_top);
    out->crop_bottom = uint32_t(frame->crop_bottom) + (rows - height);
    return SYS_AV_FRAME;
}

int ReceiveAudio(SysAvMedia* media, void* buffer, uint32_t size, SysAvFrame* out) {
    const AVFrame* frame = media->audio_frame;
    const int channels = frame->ch_layout.nb_channels;
    const uint32_t bytes = uint32_t(channels) * uint32_t(frame->nb_samples) * sizeof(int16_t);
    if (bytes > size) {
        return SYS_AV_ERROR;
    }
    if (frame->format == AV_SAMPLE_FMT_S16) {
        std::memcpy(buffer, frame->data[0], bytes);
    } else {
        if (!media->resample) {
            AVChannelLayout layout = frame->ch_layout;
            if (swr_alloc_set_opts2(&media->resample, &layout, AV_SAMPLE_FMT_S16, frame->sample_rate, &layout,
                                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(media->resample) < 0) {
                return SYS_AV_ERROR;
            }
        }
        auto* target = static_cast<uint8_t*>(buffer);
        if (swr_convert(media->resample, &target, frame->nb_samples, frame->extended_data, frame->nb_samples) < 0) {
            return SYS_AV_ERROR;
        }
    }
    const AVStream* stream = media->format->streams[media->stream_index[SYS_AV_AUDIO]];
    out->timestamp = FrameMillis(frame, stream->time_base);
    out->channel_count = uint16_t(channels);
    out->sample_rate = uint32_t(frame->sample_rate);
    out->bytes = bytes;
    return SYS_AV_FRAME;
}

int ReceiveFrame(SysAvMedia* media, int kind, void* buffer, uint32_t size, SysAvFrame* out) {
    AVFrame* frame = kind == SYS_AV_VIDEO ? media->frame : media->audio_frame;
    const int result = avcodec_receive_frame(media->codec[kind], frame);
    if (result == AVERROR(EAGAIN)) {
        return SYS_AV_MORE;
    }
    if (result == AVERROR_EOF) {
        return SYS_AV_END;
    }
    if (result < 0) {
        return SYS_AV_ERROR;
    }
    const int status = kind == SYS_AV_VIDEO ? ReceiveVideo(media, buffer, size, out) : ReceiveAudio(media, buffer, size, out);
    av_frame_unref(frame);
    return status;
}

uint64_t Duration(SysAvMedia* media) {
    uint64_t longest = 0;
    for (const int index : media->stream_index) {
        if (index >= 0) {
            longest = std::max(longest, StreamMillis(media->format, media->format->streams[index]));
        }
    }
    if (!longest) {
        for (unsigned i = 0; i < media->format->nb_streams; ++i) {
            longest = std::max(longest, StreamMillis(media->format, media->format->streams[i]));
        }
    }
    return longest;
}

const SysAvDecoder decoder = {Open,    Close,       FindStreams, StreamCount, StreamInfo, EnableStream, Begin,
                              End,     ReadPacket,  Rewind,      FreePacket,  SendPacket, ReceiveFrame, Duration};

} // namespace

extern "C" void lantern_av_decode_start(void) {
    sys_av_player_set_decoder(&decoder);
}
