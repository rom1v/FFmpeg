#include "avformat.h"
#include "libavutil/intreadwrite.h"

static int ff_kyber_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    unsigned char header[16];
    AV_WB64(header, pkt->pts);
    AV_WB32(header + 8, pkt->size);
    fprintf(stderr, "[pts=%ld size=%ld]\t%02x %02x %02x %02x "
                    "%02x %02x %02x %02x "
                    "%02x %02x %02x %02x\n",
                    (long) pkt->pts, (long) pkt->size,
                    header[0], header[1], header[2], header[3],
                    header[4], header[5], header[6], header[7],
                    header[8], header[9], header[10], header[11]);
    avio_write(s->pb, header, sizeof(header));
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

static int force_one_stream(AVFormatContext *s)
{
    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "%s files have exactly one stream\n",
               s->oformat->name);
        return AVERROR(EINVAL);
    }
    if (   s->oformat->audio_codec != AV_CODEC_ID_NONE
        && s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(s, AV_LOG_ERROR, "%s files have exactly one audio stream\n",
               s->oformat->name);
        return AVERROR(EINVAL);
    }
    if (   s->oformat->video_codec != AV_CODEC_ID_NONE
        && s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "%s files have exactly one video stream\n",
               s->oformat->name);
        return AVERROR(EINVAL);
    }
    return 0;
}

const AVOutputFormat ff_kyber_muxer = {
    .name              = "kyber",
    .long_name         = NULL_IF_CONFIG_SMALL("kyber video"),
    .extensions        = "kyber",
    .audio_codec       = AV_CODEC_ID_NONE,
    .video_codec       = AV_CODEC_ID_H264,
    .init              = force_one_stream,
    .write_packet      = ff_kyber_write_packet,
};
