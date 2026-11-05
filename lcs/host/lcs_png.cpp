#include "lcs_png.hpp"

#include <fstream>
#include <iterator>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace lcs {

std::vector<std::uint8_t> decode_png_rgba(const std::filesystem::path &path, std::uint32_t &width,
                                          std::uint32_t &height) {
    width = height = 0u;
    std::vector<std::uint8_t> out;
    std::ifstream input(path, std::ios::binary);
    if (!input) return out;
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (file.size() < 8u || file.size() > 16u * 1024u * 1024u) return out;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) return out;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (context != nullptr && packet != nullptr && frame != nullptr && avcodec_open2(context, codec, nullptr) == 0) {
        // The packet's buffer needs FFmpeg's padding after the data.
        file.resize(file.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0u);
        packet->data = file.data();
        packet->size = static_cast<int>(file.size() - AV_INPUT_BUFFER_PADDING_SIZE);
        if (avcodec_send_packet(context, packet) == 0 && avcodec_receive_frame(context, frame) == 0 &&
            frame->width > 0 && frame->height > 0 && frame->width <= 4096 && frame->height <= 4096) {
            SwsContext *scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr,
                                                nullptr, nullptr);
            if (scaler != nullptr) {
                out.resize(static_cast<std::size_t>(frame->width) * frame->height * 4u);
                std::uint8_t *planes[1] = {out.data()};
                const int strides[1] = {frame->width * 4};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                sws_freeContext(scaler);
                width = static_cast<std::uint32_t>(frame->width);
                height = static_cast<std::uint32_t>(frame->height);
            }
        }
        packet->data = nullptr;
        packet->size = 0;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return out;
}

}  // namespace lcs
