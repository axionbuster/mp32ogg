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

void destroypkt(AVPacket* p)
{
  av_packet_free(&p);
}

void destroyfr(AVFrame* p)
{
  av_frame_free(&p);
}

// relay packet -> frame
//
// return true if we need to break early but there's no error
static bool relaypkts(AVFormatContext* ofctx, AVStream* ostr, AVCodecContext* oencctx, int& err)
{
  for (;;)
  {
    auto const opkt_{ av_packet_alloc() };
    if (!opkt_)
    {
      err = -26;
      return false;
    }
    fuptr<AVPacket, destroypkt> opkt{ opkt_ };

    if (auto const i{ avcodec_receive_packet(&*oencctx, &*opkt) }; i < 0)
      switch (i)
      {
      case AVERROR(EAGAIN): case AVERROR_EOF:
        return true;
      default:
      {
        err = -24;
        return false;
      }
      }

    // set stream index for output packet
    opkt->stream_index = ostr->index;

    // write packet to output file
    if (auto const i{ av_write_frame(&*ofctx, &*opkt) }; i < 0)
    {
      err = -25;
      return false;
    }

    av_packet_unref(&*opkt);
  }
}

extern "C" {
  // convert the mp3 file at inpath to the ogg file at outpath
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
    auto const UMAX = std::numeric_limits<unsigned>::max();
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

    // decode and encode
    auto infr_{ av_frame_alloc() };
    if (!infr_) return -18;
    fuptr<AVFrame, destroyfr> infr{ infr_ };
    auto oufr_{ av_frame_alloc() };
    if (!oufr_) return -19;
    fuptr<AVFrame, destroyfr> oufr{ oufr_ };

    // enter main loop
    for (;;) {
      // allocate a new packet
      auto const packet_{ av_packet_alloc() };
      if (!packet_)
        return -17;
      fuptr<AVPacket, destroypkt> packet{ packet_ };

      // read a frame into the packet
      if (auto const i{ av_read_frame(&*ifctx, &*packet) }; i < 0) {
        if (i == AVERROR_EOF)
          break;
        return -20;
      }

      // decode audio frame
      if (static_cast<unsigned>(packet->stream_index) != iaudiostr)
        continue;
      if (auto const i{ avcodec_send_packet(&*idecctx, &*packet) }; i < 0 && i != AVERROR(EAGAIN))
        return -21;

      // write
      for (;;)
      {
        // receive audio frame
        if (auto const i{ avcodec_receive_frame(&*idecctx, &*infr) }; i < 0)
          switch (i) {
          case AVERROR(EAGAIN): case AVERROR_EOF:
            // no frame available yet
            goto breakfor;
          default: return -22;
          }

        // TODO: resample as necessary

        // set timestamps for output frame
        auto istr{ ifctx->streams[iaudiostr] };
        oufr->pts = av_rescale_q(infr->pts, istr->time_base, ostr->time_base);

        // encode and write frame to output
        if (auto const i{ avcodec_send_frame(&*oencctx, &*oufr) }; i < 0 && i != AVERROR(EAGAIN))
          return -23;

        int err;
        if (relaypkts(&*ofctx, ostr, &*oencctx, err))
          continue;
        return err;
      }

    breakfor: {}
    }

    // flush encoders
    if (auto const i{ avcodec_send_frame(&*oencctx, {}) }; i < 0 && i != AVERROR_EOF)
      return -27;
    for (;;)
    {
      int err;
      if (relaypkts(&*ofctx, ostr, &*oencctx, err))
        continue;
      return err - 4; // -28, -29, -30
    }

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
