/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2015 - ROLI Ltd.

   Permission is granted to use this software under the terms of either:
   a) the GPL v2 (or any later version)
   b) the Affero GPL v3

   Details of these licenses can be found at: www.gnu.org/licenses

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

   ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.juce.com for more information.

  ==============================================================================
*/

#if JUCE_USE_FFMPEG

#include <unistd.h>
#include <sys/types.h>

namespace FFmpegNamespace {

extern "C" {
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
}

class customAVCOntext {
    InputStream *is;
    unsigned char *buffer;
    AVIOContext *ctx;

    customAVCOntext(customAVCOntext const &);
    customAVCOntext &operator=(customAVCOntext const &);

public:

    customAVCOntext(InputStream *_is)
            : is(_is),
              buffer(static_cast<unsigned char*>(av_malloc(4 * 1024))) {
        ctx = avio_alloc_context(buffer,
                                 4 * 1024, 0, this,
                                 &customAVCOntext::read, NULL,
                                 &customAVCOntext::seek);
    }

    ~customAVCOntext() {
        // av_free(ctx); // context is freed in FFmpegReader
        //av_free(buffer); // same as above, as documentation states
        // this buffer could be freed internally by libavcodec
    }

    static int read(void *opaque, unsigned char *buf, int buf_size) {
        customAVCOntext* h = static_cast<customAVCOntext*>(opaque);
        return h->is->read(buf, buf_size);
    }

    static int64_t seek(void *opaque, int64_t offset, int whence) {
        customAVCOntext* h = static_cast<customAVCOntext*>(opaque);

        if (whence == AVSEEK_SIZE)
            return h->is->getTotalLength();
        else if (whence == SEEK_CUR)
            return h->is->setPosition(h->is->getPosition() + offset);
        else if (whence == SEEK_END)
            return h->is->setPosition(h->is->getTotalLength() + offset);

        return h->is->setPosition(offset);
    }

    AVIOContext *getAVIO() {
        return ctx;
    }
};
}

//==============================================================================
static const char* const FFmpegFormatName = "FFmpeg file";

//==============================================================================
class FFmpegReader : public AudioFormatReader
{
public:
    FFmpegReader (InputStream* const inp)
        : AudioFormatReader (inp, FFmpegFormatName)
    {
        using namespace FFmpegNamespace;

        // register all ffmpeg formats (could be called many times)
        av_register_all();

        ffmpegContext = new customAVCOntext(inp);

        format = avformat_alloc_context();
        format->pb = ffmpegContext->getAVIO();

        // get format from file
        if (avformat_open_input(&format, "", NULL, NULL) == 0) {
            //std::cout << "open input ok" << std::endl;
            if (avformat_find_stream_info(format, NULL) >= 0) {
                //std::cout << "find stream ok" << std::endl;

                // find audio stream index
                audioIndex = -1;
                for (unsigned int i = 0; i < format->nb_streams; ++i) {
                    if (format->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                        audioIndex = i;
                        // // TODO: multiple streams?
                        // std::cout << "found audio stream" << std::endl;
                        break;
                    }
                }

                if (audioIndex != -1) {
                    stream = format->streams[audioIndex];
                    codec = stream->codec;

                    // find suitable codec
                    if (avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), NULL) >= 0) {
                        //std::cout << "found suitable codec for stream" << std::endl;
                        swr = swr_alloc();
                        av_opt_set_int(swr, "in_channel_count", codec->channels, 0);
                        av_opt_set_int(swr, "out_channel_count", 2, 0);
                        av_opt_set_int(swr, "in_channel_layout", codec->channel_layout, 0);
                        av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                        av_opt_set_int(swr, "in_sample_rate", codec->sample_rate, 0);
                        av_opt_set_int(swr, "out_sample_rate", codec->sample_rate, 0);
                        av_opt_set_sample_fmt(swr, "in_sample_fmt", codec->sample_fmt, 0);
                        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
                        swr_init(swr);

                        if (swr_is_initialized(swr)) {
                            //std::cout << "swr is initialized" << std::endl;

                            av_init_packet(&packet);
                            packet.size = 0;
                            frame = av_frame_alloc();

                            if (frame) {
                                lengthInSamples = (uint32)format->duration;// * codec->sample_rate / AV_TIME_BASE;
                                numChannels = 2;
                                bitsPerSample = 32;
                                sampleRate = codec->sample_rate;
                                usesFloatingPointData = true;

                                uint32 smp = (uint32)(av_q2d(format->streams[audioIndex]->time_base) *
                                                      format->streams[audioIndex]->duration *
                                                      codec->sample_rate);

                                // uint32 smp2 = (uint32)(av_q2d(format->streams[audioIndex]->time_base) *
                                //                        format->duration *
                                //                        codec->sample_rate);

                                lengthInSamples = smp;

                                // std::cout << "frame ok" << std::endl
                                //           << "samples: " << lengthInSamples << " or " << smp << " or " << smp2 << " or " << stream->nb_frames
                                //           << " channels: " << numChannels
                                //           << " format duration: " << format->duration
                                //           << " time base: " << AV_TIME_BASE
                                //           << " sample rate: " << sampleRate << std::endl;

                                buffer = new uint8_t*[2]; // alloc buffers for stereo planar data
                                buffer[0] = nullptr;
                                buffer[1] = nullptr;

                                bufferPosition = 0;
                                samplesInBuffer = 0;
                                totalBufferPosition = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    ~FFmpegReader()
    {
        //std::cout << "kill reader" << std::endl;
        using namespace FFmpegNamespace;
        av_freep(&buffer[0]);
        av_frame_free(&frame);
        swr_free(&swr);
        avcodec_close(codec);
        avformat_free_context(format);
        delete ffmpegContext;
    }

    //==============================================================================
    bool readSamples (int** destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      int64 startSampleInFile, int numSamples) override
    {
        using namespace FFmpegNamespace;

        // destSamples - here goes samples, 2d array destSamples[0] - left, destSamples[1] - right etc.
        // numDestChannels - how many channels (array first index there are)
        // startOffsetInDestBuffer - where to write in output buffer
        // startSampleInFile - where to read in file
        // numSamples - how many samples to read

        // std::cout << "------- read from: " << startSampleInFile <<
        //     " current ffmpeg sample " << totalBufferPosition << std::endl;

        bool seeking = totalBufferPosition != startSampleInFile;
        bool decodingDone = false;

        while (numSamples > 0 && !decodingDone) {
            // seeking
            if (seeking) {
                // std::cout << "seek" << std::endl;
                int64_t tm = av_rescale_q((int64_t)(startSampleInFile / sampleRate) * AV_TIME_BASE,
                                          AV_TIME_BASE_Q,
                                          stream->time_base);
                totalBufferPosition = startSampleInFile;
                samplesInBuffer = 0;
                bufferPosition = 0;
                av_packet_unref(&packet);
                int seekResult = av_seek_frame(format, audioIndex, tm, AVSEEK_FLAG_ANY);
                packet.size = 0;
                avcodec_flush_buffers(codec);
                seeking = false;

                if (seekResult < 0) {
                    decodingDone = true;
                    continue;
                }
            }

            // update state
            int samplesToCopy = jmin(numSamples, samplesInBuffer - bufferPosition);
            if (samplesToCopy > 0) {
                //std::cout << "copy " << samplesToCopy << " samples from buffer (" << (samplesInBuffer - bufferPosition) << " left)" << std::endl;

                numSamples -= samplesToCopy;

                // always stereo
                float *pleft = ((float*)(buffer[0])) + bufferPosition;
                float *pright = ((float*)(buffer[1])) + bufferPosition;
                float *outleft = ((float*)(destSamples[0])) + startOffsetInDestBuffer;
                float *outright = ((float*)(destSamples[1])) + startOffsetInDestBuffer;

                memcpy(outleft, pleft, samplesToCopy * sizeof(float));
                memcpy(outright, pright, samplesToCopy * sizeof(float));

                bufferPosition += samplesToCopy;
                totalBufferPosition += samplesToCopy;
                startOffsetInDestBuffer += samplesToCopy;
                samplesToCopy = 0;

                //std::cout << "done copy" << std::endl;
            }

            // buffer is empty now - decode some more
            if (numSamples > 0) {
                if (packet.size > 0) {
                    if (!decodeFrame()) {
                        decodingDone = true;
                        continue;
                    }
                }
                else {
                    if (av_read_frame(format, &packet) == 0) {
                        // std::cout << "~ read frame: dts: " << packet.dts <<
                        //     " pts: " << packet.pts <<
                        //     " duration: " << packet.duration << std::endl;
                        if (!decodeFrame()) {
                            decodingDone = true;
                            continue;
                        }
                    }
                    else {
                        //std::cout << "failed read frame " << std::endl;
                        decodingDone = true;
                        continue;
                    }
                }

                if (packet.size <= 0) {
                    av_packet_unref(&packet);
                }
            }
        }

        // release packet
        if (packet.size <= 0) {
            av_packet_unref(&packet);
        }

        // fill with silence
        if (numSamples > 0) {
            for (int i = numDestChannels; --i >= 0;)
                if (destSamples[i] != nullptr)
                    zeromem(destSamples[i] + startOffsetInDestBuffer, sizeof(int) * (size_t)numSamples);
        }

        return true;
    }

    bool decodeFrame() {
        using namespace FFmpegNamespace;
        int gotFrame = 0;
        int decodeSize = avcodec_decode_audio4(codec, frame, &gotFrame, &packet);
        if (decodeSize >= 0) {
            //std::cout << "~ decode audio... " << packet.size << " size stream ID " << packet.stream_index<< std::endl;

            if (gotFrame != 0) {
                //std::cout << "~ gotFrame " << frame->nb_samples << " decode size: " << decodeSize << std::endl;

                // allocate buffer for samples
                allocateBuffer();
                int frames = swr_convert(swr,
                                         buffer,
                                         frame->nb_samples,
                                         (const uint8_t**)frame->extended_data,
                                         frame->nb_samples);
                frames = frame->nb_samples;

                bufferPosition = 0;
                samplesInBuffer = frames;
                //std::cout << "read " << frames << " frames" << std::endl;
            }
            else {
                return false;
            }
        }
        else {
            return false;
        }

        packet.size -= decodeSize;
        packet.data += decodeSize;
        return true;
    }

    void allocateBuffer() {
        using namespace FFmpegNamespace;
        if (frame->nb_samples != bufferAllocatedSamples) {
            av_freep(&buffer[0]);
            av_samples_alloc(buffer, NULL, 2, frame->nb_samples, AV_SAMPLE_FMT_FLTP, 0);
            bufferAllocatedSamples = frame->nb_samples;
        }
    }

private:
    FFmpegNamespace::customAVCOntext *ffmpegContext;
    FFmpegNamespace::AVCodecContext *codec;
    FFmpegNamespace::AVFormatContext *format;
    FFmpegNamespace::AVStream *stream;
    FFmpegNamespace::SwrContext *swr;
    FFmpegNamespace::AVPacket packet;
    FFmpegNamespace::AVFrame *frame;
    int audioIndex;

    uint8_t **buffer;
    int bufferPosition = 0;
    int64 totalBufferPosition = 0;
    int samplesInBuffer = 0;
    int bufferAllocatedSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FFmpegReader)
};


//==============================================================================
FFmpegAudioFormat::FFmpegAudioFormat() :
        AudioFormat ("FFmpeg", {
                    ".ape",
                    ".avi",
                    ".caf",
                    ".flv",
                    ".m4v",
                    ".matroska",
                    ".webm",
                    ".mov",
                    ".mp4",
                    ".3gp",
                    ".3g2",
                    ".mj2",
                    ".mpc",
                    ".mpc8",
                    ".mpeg",
                    ".tta",
                    ".w64",
                    ".xmv",
                    ".xwma",
                    ".aac",
                    ".ac3",
                    ".aif",
                    ".aifc",
                    ".aiff",
                    ".amr",
                    ".au",
                    ".dts",
                    ".m4a",
                    ".mka",
                    ".mp1",
                    ".mp2",
                    ".mp3",
                    ".mpa",
                    ".ra",
                    ".snd",
                    ".spx",
                    ".wma"}) {}

FFmpegAudioFormat::~FFmpegAudioFormat()
{
}

Array<int> FFmpegAudioFormat::getPossibleSampleRates()
{
    const int rates[] = { 8000, 11025, 12000, 16000, 22050, 32000,
                          44100, 48000, 88200, 96000, 176400, 192000 };

    return Array<int> (rates, numElementsInArray (rates));
}

Array<int> FFmpegAudioFormat::getPossibleBitDepths()
{
    const int depths[] = { 16, 24, 32 };

    return Array<int> (depths, numElementsInArray (depths));
}

bool FFmpegAudioFormat::canDoStereo()    { return true; }
bool FFmpegAudioFormat::canDoMono()      { return true; }
bool FFmpegAudioFormat::isCompressed()   { return true; }

AudioFormatReader* FFmpegAudioFormat::createReaderFor (InputStream* in, const bool deleteStreamIfOpeningFails)
{
    ScopedPointer<FFmpegReader> r (new FFmpegReader (in));

    if (r->sampleRate > 0)
    {
        return r.release();
    }

    if (! deleteStreamIfOpeningFails)
        r->input = nullptr;

    return nullptr;
}

AudioFormatWriter* FFmpegAudioFormat::createWriterFor (OutputStream* out,
                                                       double sampleRate,
                                                       unsigned int numChannels,
                                                       int bitsPerSample,
                                                       const StringPairArray& metadataValues,
                                                       int qualityOptionIndex)
{
    jassertfalse; // not yet implemented!
    return nullptr;
}

StringArray FFmpegAudioFormat::getQualityOptions()
{
    static const char* options[] = { 0 };
    return StringArray (options);
}

#endif
