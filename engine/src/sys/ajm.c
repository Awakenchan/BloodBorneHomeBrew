// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "host_sync.h"
#include "third_party/LibAtrac9/C/src/libatrac9.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    AJM_ERROR_CONTEXT = (int32_t)0x80930002,
    AJM_ERROR_INSTANCE = (int32_t)0x80930003,
    AJM_ERROR_BATCH = (int32_t)0x80930004,
    AJM_ERROR_PARAMETER = (int32_t)0x80930005,
    AJM_ERROR_RESOURCES = (int32_t)0x80930007,
    AJM_ERROR_REGISTERED = (int32_t)0x80930009,
    AJM_ERROR_NOT_REGISTERED = (int32_t)0x8093000A,
    AJM_ERROR_REVISION = (int32_t)0x8093000B,
    AJM_ERROR_MALFORMED = (int32_t)0x80930011,
    AJM_ERROR_CANCELLED = (int32_t)0x80930017,
};

enum {
    JOB_NOT_INITIALIZED = 0x1,
    JOB_INVALID_PARAMETER = 0x4,
    JOB_PARTIAL_INPUT = 0x8,
    JOB_NO_ROOM = 0x10,
    JOB_CODEC_ERROR = 0x40000000u,
    JOB_FATAL = 0x80000000u,
};

enum {
    CHUNK_JOB = 0,
    CHUNK_INPUT_RUN = 1,
    CHUNK_INPUT_CONTROL = 2,
    CHUNK_CONTROL_FLAGS = 3,
    CHUNK_RUN_FLAGS = 4,
    CHUNK_RETURN_ADDRESS = 6,
    CHUNK_INLINE = 7,
    CHUNK_OUTPUT_RUN = 17,
    CHUNK_OUTPUT_CONTROL = 18,
};

enum { PCM_S16 = 0, PCM_S32 = 1, PCM_FLOAT = 2 };

#define CODEC_ATRAC9 1
#define CODEC_COUNT 24
#define STATISTICS_INSTANCE 0x80000u
#define INSTANCE_MASK 0x3FFF
#define CONTEXT_COUNT 8
#define BATCH_COUNT 256
#define MAX_JOINED_INPUT (UINT64_C(64) << 20)
#define MAX_FRAME_SAMPLES (256 * 8)

#define FLAG(flags, bit) (((flags) >> (bit)) & 1)
#define RUN_CODEC_INFO 11
#define RUN_MULTIPLE_FRAMES 12
#define CONTROL_RESET 13
#define CONTROL_INITIALIZE 14
#define CONTROL_RESAMPLE 15
#define SIDEBAND_GAPLESS 45
#define SIDEBAND_FORMAT 46
#define SIDEBAND_STREAM 47

typedef struct {
    uint32_t word, size;
} Chunk;

typedef struct {
    uint32_t word, size;
    void *address;
} BufferChunk;

typedef struct {
    int32_t result, internal;
} SidebandResult;

typedef struct {
    int32_t input_consumed, output_written;
    uint64_t total_samples;
} SidebandStream;

typedef struct {
    uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved;
} SidebandFormat;

typedef struct {
    uint32_t total_samples;
    uint16_t skip_samples, skipped_samples;
} Gapless;

typedef struct {
    uint32_t superframe_size, frames_per_superframe, next_frame_size, frame_samples;
} CodecInfo;

typedef struct {
    int error;
    const void *job;
    uint32_t offset;
    const void *return_address;
} BatchError;

_Static_assert(sizeof(BufferChunk) == 16, "BufferChunk is 16 bytes");
_Static_assert(sizeof(SidebandFormat) == 24 && sizeof(Gapless) == 8 && sizeof(SidebandStream) == 16,
               "sideband layouts");

typedef struct {
    int used, format, gapless_loop, ready;
    uint32_t codec_flags;
    void *decoder;
    unsigned char config[4];
    Atrac9CodecInfo info;
    uint32_t superframe_left, frame_in_superframe;
    Gapless gapless_start, gapless;
    uint64_t total_samples;
} Instance;

typedef struct {
    int registered[CODEC_COUNT];
    Instance instances[INSTANCE_MASK + 1];
} Context;

typedef struct {
    int used, context, cancelled;
} Batch;

typedef struct {
    uint64_t flags;
    int has_flags, input_count, output_count;
    BufferChunk inputs[16], outputs[16], input_control, output_control;
} Job;

typedef struct {
    BufferChunk *chunks;
    int count, index;
    uint32_t offset;
} Output;

static HostRecursiveMutex lock = HOST_RECURSIVE_MUTEX_INIT;
static Context *contexts[CONTEXT_COUNT + 1];
static Batch batches[BATCH_COUNT];
static size_t jobs_run, frames_decoded, batches_run;

static int trace(void) {
    static int left = -1;
    if (left < 0) {
        const char *setting = getenv("BB_AUDIO_TRACE");
        left = setting && setting[0] == '1' ? 400 : 0;
    }
    return left > 0 && left--;
}

static uint32_t chunk_kind(uint32_t word) {
    return word & 0x3F;
}

static uint32_t chunk_value(uint32_t word) {
    return (word >> 6) & 0xFFFFF;
}

static uint32_t chunk_word(uint32_t kind, uint32_t value) {
    return (kind & 0x3F) | ((value & 0xFFFFF) << 6);
}

static uint32_t speaker_mask(int channels) {
    switch (channels) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 4: return 0x33;
    case 6: return 0x60F;
    case 8: return 0x63F;
    }
    return 0;
}

static int sample_bytes(int format) {
    return format == PCM_S16 ? 2 : 4;
}

static unsigned char *write_buffer_chunk(unsigned char *at, uint32_t kind, const void *address, uint64_t size) {
    BufferChunk c = {chunk_word(kind, 0), (uint32_t)size, (void *)address};
    memcpy(at, &c, sizeof(c));
    return at + sizeof(c);
}

static unsigned char *write_flags_chunk(unsigned char *at, uint32_t kind, uint64_t flags) {
    Chunk c = {chunk_word(kind, (uint32_t)(flags >> 32)), (uint32_t)flags};
    memcpy(at, &c, sizeof(c));
    return at + sizeof(c);
}

static void *close_job(unsigned char *start, unsigned char *end, uint32_t instance) {
    Chunk job = {chunk_word(CHUNK_JOB, instance), (uint32_t)(end - start - sizeof(Chunk))};
    memcpy(start, &job, sizeof(job));
    return end;
}

static SYS_ABI void *ajm_job_control(void *buffer, uint32_t instance, uint64_t flags, void *input, uint64_t input_size,
                                     void *output, uint64_t output_size, void *return_address) {
    unsigned char *at = (unsigned char *)buffer + sizeof(Chunk);
    if (return_address) at = write_buffer_chunk(at, CHUNK_RETURN_ADDRESS, return_address, 0);
    at = write_buffer_chunk(at, CHUNK_INPUT_CONTROL, input, input_size);
    flags &= instance == STATISTICS_INSTANCE ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    at = write_flags_chunk(at, CHUNK_CONTROL_FLAGS, flags);
    at = write_buffer_chunk(at, CHUNK_OUTPUT_CONTROL, output, output_size);
    return close_job(buffer, at, instance);
}

static SYS_ABI void *ajm_job_run(void *buffer, uint32_t instance, uint64_t flags, void *input, uint64_t input_size,
                                 void *output, uint64_t output_size, void *sideband, uint64_t sideband_size,
                                 void *return_address) {
    unsigned char *at = (unsigned char *)buffer + sizeof(Chunk);
    if (return_address) at = write_buffer_chunk(at, CHUNK_RETURN_ADDRESS, return_address, 0);
    at = write_buffer_chunk(at, CHUNK_INPUT_RUN, input, input_size);
    at = write_flags_chunk(at, CHUNK_RUN_FLAGS, flags & UINT64_C(0xE00000001FFF));
    at = write_buffer_chunk(at, CHUNK_OUTPUT_RUN, output, output_size);
    at = write_buffer_chunk(at, CHUNK_OUTPUT_CONTROL, sideband, sideband_size);
    return close_job(buffer, at, instance);
}

static void restart_decoder(Instance *in) {
    if (in->decoder) Atrac9ReleaseHandle(in->decoder);
    in->decoder = Atrac9GetHandle();
    in->ready = Atrac9InitDecoder(in->decoder, in->config) == 0;
    if (in->ready) Atrac9GetCodecInfo(in->decoder, &in->info);
    in->frame_in_superframe = 0;
    in->superframe_left = (uint32_t)in->info.superframeSize;
    if (trace())
        printf("Audio trace: ATRAC9 init %s: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, "
               "format %d, flags %#x\n",
               in->ready ? "ok" : "FAILED", in->info.channels, in->info.samplingRate, in->info.frameSamples,
               in->info.framesInSuperframe, in->info.superframeSize, in->format, in->codec_flags);
}

static void restart_gapless(Instance *in) {
    in->gapless.total_samples = in->gapless_start.total_samples;
    in->gapless.skip_samples = in->gapless_start.skip_samples;
    in->gapless.skipped_samples = 0;
}

static int gapless_finished(const Instance *in) {
    return in->gapless_start.total_samples && !in->gapless.total_samples;
}

static uint32_t next_frame_bytes(const Instance *in) {
    uint32_t frame = (uint32_t)in->info.frameSamples;
    uint32_t skip = in->gapless.skip_samples < frame ? in->gapless.skip_samples : frame;
    uint32_t samples = frame - skip;
    if (in->gapless_start.total_samples && in->gapless.total_samples < samples) samples = in->gapless.total_samples;
    return samples * (uint32_t)in->info.channels * (uint32_t)sample_bytes(in->format);
}

static uint64_t output_room(const Output *out) {
    uint64_t room = 0;
    for (int i = out->index; i < out->count; ++i) room += out->chunks[i].size - (i == out->index ? out->offset : 0);
    return room;
}

static void output_write(Output *out, const unsigned char *data, uint64_t size) {
    while (size && out->index < out->count) {
        BufferChunk *c = &out->chunks[out->index];
        uint64_t n = c->size - out->offset;
        if (n > size) n = size;
        memcpy((unsigned char *)c->address + out->offset, data, n);
        data += n;
        size -= n;
        out->offset += (uint32_t)n;
        if (out->offset == c->size) {
            ++out->index;
            out->offset = 0;
        }
    }
}

static void consume(Instance *in, const unsigned char **input, uint64_t *input_size, uint64_t bytes) {
    *input += bytes;
    *input_size -= bytes;
    in->superframe_left -= (uint32_t)bytes;
}

static void skip_rest_of_superframe(Instance *in, const unsigned char **input, uint64_t *input_size) {
    uint64_t skip = in->superframe_left < *input_size ? in->superframe_left : *input_size;
    *input += skip;
    *input_size -= skip;
    in->superframe_left = (uint32_t)in->info.superframeSize;
    in->frame_in_superframe = 0;
}

static void convert(const Instance *in, const int16_t *pcm, uint32_t skip, uint32_t samples, unsigned char *out) {
    int channels = in->info.channels, planar = (in->codec_flags >> 8) & 1;
    for (uint32_t s = 0; s < samples; ++s)
        for (int c = 0; c < channels; ++c) {
            int16_t v = pcm[(skip + s) * (uint32_t)channels + (uint32_t)c];
            size_t index = planar ? (size_t)c * samples + s : (size_t)s * (size_t)channels + (size_t)c;
            if (in->format == PCM_S16) {
                memcpy(out + index * 2, &v, 2);
            } else if (in->format == PCM_S32) {
                int32_t w = (int32_t)v << 16;
                memcpy(out + index * 4, &w, 4);
            } else {
                float f = (float)v / 32768.0f;
                memcpy(out + index * 4, &f, 4);
            }
        }
}

static uint32_t decode_frame(Instance *in, const unsigned char **input, uint64_t *input_size, Output *out,
                             uint32_t *written, int32_t *internal) {
    int channels = in->info.channels, frame = in->info.frameSamples, used = 0;
    int16_t pcm[MAX_FRAME_SAMPLES];
    if (frame * channels > MAX_FRAME_SAMPLES) return JOB_CODEC_ERROR | JOB_FATAL;
    int error = Atrac9Decode(in->decoder, *input, pcm, &used);
    if (error) {
        *internal = error;
        return JOB_CODEC_ERROR | JOB_FATAL;
    }
    consume(in, input, input_size, (uint64_t)used);
    ++frames_decoded;
    uint32_t skip = 0;
    if (in->gapless.skip_samples) {
        skip = in->gapless.skip_samples < (uint32_t)frame ? in->gapless.skip_samples : (uint32_t)frame;
        in->gapless.skip_samples -= (uint16_t)skip;
    }
    uint32_t samples = (uint32_t)frame - skip;
    if (in->gapless_start.total_samples && samples > in->gapless.total_samples) samples = in->gapless.total_samples;
    uint64_t room = output_room(out) / (uint64_t)(channels * sample_bytes(in->format));
    if (samples > room) samples = (uint32_t)room;
    unsigned char converted[MAX_FRAME_SAMPLES * 4];
    convert(in, pcm, skip, samples, converted);
    output_write(out, converted, (uint64_t)samples * (uint64_t)channels * (uint64_t)sample_bytes(in->format));
    *written = samples;
    in->gapless.skipped_samples += (uint16_t)((uint32_t)frame - samples);
    if (in->gapless_start.total_samples) in->gapless.total_samples -= samples;
    if (++in->frame_in_superframe % (uint32_t)in->info.framesInSuperframe == 0) {
        skip_rest_of_superframe(in, input, input_size);
    } else if (gapless_finished(in)) {
        while (in->frame_in_superframe % (uint32_t)in->info.framesInSuperframe && *input_size) {
            if (Atrac9Decode(in->decoder, *input, pcm, &used)) break;
            consume(in, input, input_size, (uint64_t)used);
            ++in->frame_in_superframe;
        }
        skip_rest_of_superframe(in, input, input_size);
    }
    return 0;
}

static void read_riff_header(Instance *in, const unsigned char **input, uint64_t *size) {
    const unsigned char *at = *input + 12, *end = *input + *size;
    while (at + 8 <= end) {
        uint32_t tag, length;
        memcpy(&tag, at, 4);
        memcpy(&length, at + 4, 4);
        at += 8;
        if (tag == 0x61746164) break;
        if (tag == 0x20746D66 && length >= 52 && at + 52 <= end) {
            memcpy(in->config, at + 44, 4);
            restart_decoder(in);
        } else if (tag == 0x74636166 && length >= 12 && at + 12 <= end) {
            uint32_t total, delay;
            memcpy(&total, at, 4);
            memcpy(&delay, at + 4, 4);
            in->gapless_start.total_samples = total;
            in->gapless_start.skip_samples = (uint16_t)delay;
            restart_gapless(in);
        }
        at += length;
    }
    *size -= (uint64_t)(at - *input);
    *input = at;
}

static int read_job(const unsigned char *at, uint32_t size, Job *job) {
    memset(job, 0, sizeof(*job));
    for (const unsigned char *end = at + size; at < end;) {
        uint32_t word;
        memcpy(&word, at, 4);
        switch (chunk_kind(word)) {
        case CHUNK_CONTROL_FLAGS:
        case CHUNK_RUN_FLAGS: {
            Chunk c;
            memcpy(&c, at, 8);
            job->flags = (uint64_t)chunk_value(c.word) << 32 | c.size;
            job->has_flags = 1;
            at += 8;
            break;
        }
        case CHUNK_INPUT_RUN:
            if (job->input_count == 16) return -1;
            memcpy(&job->inputs[job->input_count++], at, 16);
            at += 16;
            break;
        case CHUNK_OUTPUT_RUN:
            if (job->output_count == 16) return -1;
            memcpy(&job->outputs[job->output_count++], at, 16);
            at += 16;
            break;
        case CHUNK_INPUT_CONTROL:
            memcpy(&job->input_control, at, 16);
            at += 16;
            break;
        case CHUNK_OUTPUT_CONTROL:
            memcpy(&job->output_control, at, 16);
            at += 16;
            break;
        case CHUNK_RETURN_ADDRESS:
            at += 16;
            break;
        default:
            return -1;
        }
    }
    return job->has_flags ? 0 : -1;
}

static uint32_t apply_gapless_control(Instance *in, const Gapless *g) {
    uint32_t result = 0;
    uint32_t processed = in->gapless_start.total_samples - in->gapless.total_samples;
    if (g->total_samples || !g->skip_samples) {
        if (g->total_samples >= processed) {
            int64_t change = (int64_t)in->gapless_start.total_samples - g->total_samples;
            in->gapless_start.total_samples = g->total_samples;
            in->gapless.total_samples = (uint32_t)(in->gapless.total_samples - change);
        } else {
            result |= JOB_INVALID_PARAMETER;
        }
    }
    uint32_t skipped = (uint32_t)in->gapless_start.skip_samples - in->gapless.skip_samples;
    if (g->skip_samples || !g->total_samples) {
        if (g->skip_samples >= skipped) {
            int32_t change = (int32_t)in->gapless_start.skip_samples - g->skip_samples;
            in->gapless_start.skip_samples = g->skip_samples;
            in->gapless.skip_samples = (uint16_t)(in->gapless.skip_samples - change);
        } else {
            result |= JOB_INVALID_PARAMETER;
        }
    }
    return result;
}

static uint32_t apply_controls(Instance *in, const Job *job) {
    uint64_t f = job->flags;
    uint32_t result = 0;
    const unsigned char *at = job->input_control.size ? job->input_control.address : NULL;
    const unsigned char *end = at ? at + job->input_control.size : NULL;
    if (FLAG(f, CONTROL_RESET)) {
        in->total_samples = 0;
        restart_gapless(in);
        if (in->ready) restart_decoder(in);
    }
    if (at && FLAG(f, SIDEBAND_FORMAT) && at + 24 <= end) at += 24;
    if (at && FLAG(f, SIDEBAND_GAPLESS) && at + 8 <= end) {
        Gapless g;
        memcpy(&g, at, 8);
        at += 8;
        result |= apply_gapless_control(in, &g);
    }
    if (at && FLAG(f, CONTROL_RESAMPLE) && at + 8 <= end) at += 8;
    if (at && FLAG(f, CONTROL_INITIALIZE) && at + 8 <= end) {
        memcpy(in->config, at, 4);
        restart_decoder(in);
    }
    return result;
}

static void write_sidebands(const Instance *in, uint64_t f, unsigned char *at, const unsigned char *end,
                            const SidebandStream *stream, uint32_t frames) {
    if (at && FLAG(f, SIDEBAND_STREAM) && at + 16 <= end) {
        memcpy(at, stream, 16);
        at += 16;
    }
    if (at && FLAG(f, SIDEBAND_FORMAT) && at + 24 <= end) {
        SidebandFormat s = {(uint32_t)in->info.channels, speaker_mask(in->info.channels),
                            (uint32_t)in->info.samplingRate, (uint32_t)in->format, 0, 0};
        if (in->info.framesInSuperframe && in->info.frameSamples)
            s.bitrate = (uint32_t)((uint64_t)in->info.samplingRate * (uint64_t)in->info.superframeSize * 8 /
                                   ((uint64_t)in->info.framesInSuperframe * (uint64_t)in->info.frameSamples));
        memcpy(at, &s, 24);
        at += 24;
    }
    if (at && FLAG(f, SIDEBAND_GAPLESS) && at + 8 <= end) {
        memcpy(at, &in->gapless, 8);
        at += 8;
    }
    if (at && FLAG(f, RUN_MULTIPLE_FRAMES) && at + 8 <= end) {
        uint32_t counts[2] = {frames, 0};
        memcpy(at, counts, 8);
        at += 8;
    }
    if (at && FLAG(f, RUN_CODEC_INFO) && at + 16 <= end) {
        CodecInfo info = {(uint32_t)in->info.superframeSize, (uint32_t)in->info.framesInSuperframe, in->superframe_left,
                          (uint32_t)in->info.frameSamples};
        memcpy(at, &info, 16);
    }
}

static void run_job(Context *context, uint32_t id, Job *job) {
    ++jobs_run;
    unsigned char *side = job->output_control.size ? job->output_control.address : NULL;
    unsigned char *side_end = side ? side + job->output_control.size : NULL;
    SidebandResult *result = NULL;
    if (side && side + sizeof(SidebandResult) <= side_end) {
        result = (SidebandResult *)side;
        side += sizeof(*result);
        memset(result, 0, sizeof(*result));
    }
    if (id == STATISTICS_INSTANCE) {
        if (side) memset(side, 0, (size_t)(side_end - side));
        return;
    }
    Instance *in = &context->instances[id & INSTANCE_MASK];
    if (!(id & INSTANCE_MASK) || !in->used) {
        if (result) result->result = (int32_t)(JOB_FATAL | JOB_INVALID_PARAMETER);
        return;
    }
    uint64_t f = job->flags;
    uint32_t outcome = apply_controls(in, job);
    uint64_t input_size = 0;
    for (int i = 0; i < job->input_count; ++i) input_size += job->inputs[i].size;
    unsigned char *joined = NULL;
    const unsigned char *input = NULL;
    if (job->input_count == 1) {
        input = job->inputs[0].address;
    } else if (job->input_count > 1) {
        joined = input_size <= MAX_JOINED_INPUT ? malloc(input_size ? input_size : 1) : NULL;
        if (joined) {
            uint64_t at = 0;
            for (int i = 0; i < job->input_count; ++i) {
                memcpy(joined + at, job->inputs[i].address, job->inputs[i].size);
                at += job->inputs[i].size;
            }
        } else {
            input_size = 0;
            outcome |= JOB_INVALID_PARAMETER;
        }
        input = joined;
    }
    Output out = {job->outputs, job->output_count, 0, 0};
    uint64_t input_start = input_size, room_start = output_room(&out);
    uint32_t frames = 0;
    int32_t internal = 0;
    for (int more = input_size != 0; more;) {
        if (in->gapless_loop && gapless_finished(in)) {
            restart_gapless(in);
            in->total_samples = 0;
        }
        if ((in->codec_flags & 1) && input_size >= 4 && !memcmp(input, "RIFF", 4)) {
            read_riff_header(in, &input, &input_size);
            in->total_samples = 0;
        }
        if (!in->ready) {
            outcome |= JOB_NOT_INITIALIZED;
            break;
        }
        if (!gapless_finished(in) && output_room(&out) < next_frame_bytes(in)) outcome |= JOB_NO_ROOM;
        if (input_size < in->superframe_left) outcome |= JOB_PARTIAL_INPUT;
        if (outcome) break;
        uint32_t written = 0;
        uint32_t error = decode_frame(in, &input, &input_size, &out, &written, &internal);
        in->total_samples += written;
        ++frames;
        if (error) {
            outcome |= error;
            break;
        }
        more = (int)FLAG(f, RUN_MULTIPLE_FRAMES);
    }
    free(joined);
    if (outcome && trace())
        printf("Audio trace: Ajm job instance %u flags %#llx -> result %#x internal %d; input %llu/%llu used, output "
               "%llu/%llu written, frames %u\n",
               id, (unsigned long long)f, outcome, internal, (unsigned long long)(input_start - input_size),
               (unsigned long long)input_start, (unsigned long long)(room_start - output_room(&out)),
               (unsigned long long)room_start, frames);
    if (result) {
        result->result = (int32_t)outcome;
        result->internal = internal;
    }
    SidebandStream stream = {(int32_t)(input_start - input_size), (int32_t)(room_start - output_room(&out)),
                             in->total_samples};
    write_sidebands(in, f, side, side_end, &stream, frames);
}

static Context *find_context(uint32_t id) {
    return id && id <= CONTEXT_COUNT ? contexts[id] : NULL;
}

static SYS_ABI int32_t ajm_initialize(int64_t reserved, uint32_t *context) {
    if (!context || reserved) return AJM_ERROR_PARAMETER;
    host_recursive_lock(&lock);
    for (uint32_t i = 1; i <= CONTEXT_COUNT; ++i) {
        if (contexts[i]) continue;
        contexts[i] = calloc(1, sizeof(Context));
        host_recursive_unlock(&lock);
        if (!contexts[i]) return AJM_ERROR_RESOURCES;
        *context = i;
        puts("Runtime: Ajm context initialized (ATRAC9 via LibAtrac9)");
        return 0;
    }
    host_recursive_unlock(&lock);
    return AJM_ERROR_RESOURCES;
}

static SYS_ABI int32_t ajm_finalize(uint32_t id) {
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    if (context) {
        for (int i = 0; i <= INSTANCE_MASK; ++i)
            if (context->instances[i].decoder) Atrac9ReleaseHandle(context->instances[i].decoder);
        free(context);
        contexts[id] = NULL;
    }
    host_recursive_unlock(&lock);
    return context ? 0 : AJM_ERROR_CONTEXT;
}

static SYS_ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec >= CODEC_COUNT) return AJM_ERROR_PARAMETER;
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    int32_t result = !context ? AJM_ERROR_CONTEXT : context->registered[codec] ? AJM_ERROR_REGISTERED : 0;
    if (!result) context->registered[codec] = 1;
    host_recursive_unlock(&lock);
    return result;
}

static SYS_ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    int32_t result = !context                     ? AJM_ERROR_CONTEXT
                     : codec >= CODEC_COUNT       ? AJM_ERROR_PARAMETER
                     : !context->registered[codec] ? AJM_ERROR_NOT_REGISTERED
                                                   : 0;
    if (!result) context->registered[codec] = 0;
    host_recursive_unlock(&lock);
    return result;
}

static SYS_ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *instance) {
    if (!instance || codec >= CODEC_COUNT) return AJM_ERROR_PARAMETER;
    if (!(flags & 7)) return AJM_ERROR_REVISION;
    if (codec != CODEC_ATRAC9) {
        fprintf(stderr, "STOP: Ajm codec %u (0=MP3, 2=AAC) is not implemented\n", codec);
        exit(21);
    }
    if (trace()) printf("Audio trace: Ajm instance create codec %u flags %#llx\n", codec, (unsigned long long)flags);
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    int32_t result = !context ? AJM_ERROR_CONTEXT : !context->registered[codec] ? AJM_ERROR_NOT_REGISTERED : AJM_ERROR_RESOURCES;
    if (context && context->registered[codec]) {
        for (uint32_t i = 1; i <= INSTANCE_MASK; ++i) {
            Instance *in = &context->instances[i];
            if (in->used) continue;
            int format = (int)((flags >> 7) & 7);
            if (format > PCM_FLOAT) {
                memset(in, 0, sizeof(*in));
                result = AJM_ERROR_PARAMETER;
                break;
            }
            memset(in, 0, sizeof(*in));
            in->used = 1;
            in->format = format;
            in->gapless_loop = (int)((flags >> 10) & 1);
            in->codec_flags = (uint32_t)(flags >> 32);
            *instance = i | (codec << 14);
            result = 0;
            break;
        }
    }
    host_recursive_unlock(&lock);
    return result;
}

static SYS_ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    Instance *in = context ? &context->instances[instance & INSTANCE_MASK] : NULL;
    int32_t result = !context ? AJM_ERROR_CONTEXT : !(instance & INSTANCE_MASK) || !in->used ? AJM_ERROR_INSTANCE : 0;
    if (!result) {
        if (in->decoder) Atrac9ReleaseHandle(in->decoder);
        memset(in, 0, sizeof(*in));
    }
    host_recursive_unlock(&lock);
    return result;
}

static void report_malformed(BatchError *error, const unsigned char *job, const unsigned char *buffer) {
    if (!error) return;
    error->error = AJM_ERROR_MALFORMED;
    error->job = job;
    error->offset = (uint32_t)(job - buffer);
    error->return_address = NULL;
}

static SYS_ABI int32_t ajm_batch_start(uint32_t id, unsigned char *buffer, uint32_t size, int priority,
                                       BatchError *error, uint32_t *batch) {
    (void)priority;
    if (size & 7) return AJM_ERROR_MALFORMED;
    host_recursive_lock(&lock);
    Context *context = find_context(id);
    int slot = -1;
    for (int i = 0; context && i < BATCH_COUNT && slot < 0; ++i)
        if (!batches[i].used) slot = i;
    if (!context || slot < 0) {
        host_recursive_unlock(&lock);
        return !context ? AJM_ERROR_CONTEXT : AJM_ERROR_RESOURCES;
    }
    for (unsigned char *at = buffer, *end = buffer + size; at < end;) {
        Chunk chunk;
        memcpy(&chunk, at, 8);
        unsigned char *body = at + 8;
        at = body + chunk.size;
        if (chunk_kind(chunk.word) == CHUNK_INLINE) continue;
        Job job;
        if (chunk_kind(chunk.word) != CHUNK_JOB || at > end || read_job(body, chunk.size, &job)) {
            report_malformed(error, body - 8, buffer);
            host_recursive_unlock(&lock);
            return AJM_ERROR_MALFORMED;
        }
        run_job(context, chunk_value(chunk.word), &job);
    }
    batches[slot] = (Batch){1, (int)id, 0};
    ++batches_run;
    *batch = (uint32_t)slot + 1;
    host_recursive_unlock(&lock);
    return 0;
}

static int32_t check_batch(uint32_t id, uint32_t batch) {
    if (!find_context(id)) return AJM_ERROR_CONTEXT;
    if (!batch || batch > BATCH_COUNT || !batches[batch - 1].used) return AJM_ERROR_BATCH;
    return 0;
}

static SYS_ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *error) {
    (void)timeout;
    (void)error;
    host_recursive_lock(&lock);
    int32_t result = check_batch(id, batch);
    if (!result) {
        if (batches[batch - 1].cancelled) result = AJM_ERROR_CANCELLED;
        batches[batch - 1].used = 0;
    }
    host_recursive_unlock(&lock);
    return result;
}

static SYS_ABI int32_t ajm_batch_cancel(uint32_t id, uint32_t batch) {
    host_recursive_lock(&lock);
    int32_t result = check_batch(id, batch);
    host_recursive_unlock(&lock);
    return result;
}

static SYS_ABI int32_t ajm_batch_error_dump(void) {
    return 0;
}

void sys_ajm_report(void) {
    printf("Runtime: Ajm batches=%zu, jobs=%zu, ATRAC9 frames decoded=%zu\n", batches_run, jobs_run, frames_decoded);
}

static const SysExport exports[] = {
    {"NVDXiUesSbA", "sceAjmBatchCancel", ajm_batch_cancel},
    {"WfAiBW8Wcek", "sceAjmBatchErrorDump", ajm_batch_error_dump},
    {"dmDybN--Fn8", "sceAjmBatchJobControlBufferRa", ajm_job_control},
    {"ElslOCpOIns", "sceAjmBatchJobRunBufferRa", ajm_job_run},
    {"fFFkk0xfGWs", "sceAjmBatchStartBuffer", ajm_batch_start},
    {"-qLsfDAywIY", "sceAjmBatchWait", ajm_batch_wait},
    {"MHur6qCsUus", "sceAjmFinalize", ajm_finalize},
    {"dl+4eHSzUu4", "sceAjmInitialize", ajm_initialize},
    {"AxoDrINp4J8", "sceAjmInstanceCreate", ajm_instance_create},
    {"RbLbuKv8zho", "sceAjmInstanceDestroy", ajm_instance_destroy},
    {"Q3dyFuwGn64", "sceAjmModuleRegister", ajm_module_register},
    {"Wi7DtlLV+KI", "sceAjmModuleUnregister", ajm_module_unregister},
};

SYS_LIBRARY(sys_ajm, "libSceAjm", exports);
