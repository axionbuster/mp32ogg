#include "pch.h"

#ifdef max
#undef max
#endif

template <auto fn>
struct deleter_from_fn {
  template <typename T>
  constexpr void operator()(T* arg) const {
    fn(arg);
  }
};

template <typename T, auto fn>
using fuptr = std::unique_ptr<T, deleter_from_fn<fn>>;

void destroyavfctx(AVFormatContext* p)
{
  avformat_free_context(p);
}

void destroyavcodec(AVCodecContext* p)
{
  avcodec_free_context(&p);
}

extern "C" {
  int cvtmp3toogg(char const* inpath, char const* outpath)
  {
    // open input file (mp3)
    AVFormatContext* ifctx_{};
    if (avformat_open_input(&ifctx_, inpath, {}, {}) < 0)
      return -1;
    fuptr<AVFormatContext, destroyavfctx> ifctx{ ifctx_ };
    if (avformat_find_stream_info(&*ifctx, {}) < 0)
      return -2;

    // find audio stream
    const auto UMAX = std::numeric_limits<unsigned>::max();
    unsigned iaudiostr{ UMAX };
    for (unsigned i{}; i < ifctx->nb_streams; i++)
      if (ifctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      {
        iaudiostr = i;
        break;
      }
    if (iaudiostr == UMAX)
      return -3;

    // find decoder for the stream
    auto idecpars{ ifctx->streams[iaudiostr]->codecpar };
    auto idec{ avcodec_find_decoder(idecpars->codec_id) };
    if (!idec)
      return -4;

    // get input codec context
    // and then copy parameters to icc; open codec
    auto idecctx_{ avcodec_alloc_context3(idec) };
    fuptr<AVCodecContext, destroyavcodec> idecctx{ idecctx_ };
    if (!idecctx)
      return -5;
    if (avcodec_parameters_to_context(&*idecctx, idecpars) < 0)
      return -6;
    if (avcodec_open2(&*idecctx, idec, {}) < 0)
      return -7;

    // open output file (OGG)
    AVFormatContext* ofctx_{};
    avformat_alloc_output_context2(&ofctx_, {}, "ogg", outpath);
    fuptr<AVFormatContext, destroyavfctx> ofctx{ ofctx_ };
    if (!ofctx)
      return -8;

    auto oenc{ avcodec_find_encoder(AV_CODEC_ID_VORBIS) };
    if (!oenc)
      return -9;

    auto ostr{ avformat_new_stream(&*ofctx, oenc) };
    if (!ostr)
      return -10;

    auto oencctx_{ avcodec_alloc_context3(oenc) };
    fuptr<AVCodecContext, destroyavcodec> oencctx{ oencctx_ };
    if (!oencctx)
      return -11;

    // copy settings & then open encoder
    // and then write header
    oencctx->sample_rate = idecctx->sample_rate;
    oencctx->channel_layout = idecctx->channel_layout; // FIXME: deprecated
    oencctx->channels = av_get_channel_layout_nb_channels(oencctx->channel_layout); // FIXME: deprecated
    oencctx->sample_fmt = AV_SAMPLE_FMT_FLTP; // important for Vorbis
    oencctx->bit_rate = idecctx->bit_rate;
    oencctx->time_base = { .num = 1, .den = oencctx->sample_rate };
    if (avcodec_open2(&*oencctx, oenc, {}))
      return -12;
    if (avcodec_parameters_from_context(ostr->codecpar, &*oencctx) < 0)
      return -13;
    if (!(ofctx->oformat->flags & AVFMT_NOFILE))
      if (avio_open(&ofctx->pb, outpath, AVIO_FLAG_WRITE) < 0)
        return -14;
    avformat_write_header(&*ofctx, {});

    // TODO: decode and reencode

    // write trailer; clean up
    if (av_write_trailer(&*ofctx) < 0)
      return -15;

    // free resources
    // (most of the work is done by the unique_ptr's anyway)
    if (!(ofctx->oformat->flags & AVFMT_NOFILE))
      if (avio_closep(&ofctx->pb) < 0)
        return -16;
    return 0;
  }
}
