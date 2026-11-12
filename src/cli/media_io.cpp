#include "media_io.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <climits>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace silentfilm::offline {
namespace {

void check(int code, const char* operation)
{
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

using AvFrame = std::shared_ptr<AVFrame>;
AvFrame frame() { auto* p = av_frame_alloc(); if (!p) throw std::bad_alloc(); return AvFrame(p, [](AVFrame* f) { av_frame_free(&f); }); }

std::uint16_t word(float value)
{
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite camera output");
    return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535));
}

void put16(std::ostream& out, std::uint16_t value)
{
    out.put(static_cast<char>(value & 255)); out.put(static_cast<char>(value >> 8));
}
void put32(std::ostream& out, std::uint32_t value) { put16(out, static_cast<std::uint16_t>(value)); put16(out, static_cast<std::uint16_t>(value >> 16)); }

void tiff(const std::filesystem::path& path, int width, int height, const std::vector<std::uint16_t>& data, PixelAspectRatio aspect)
{
    const auto bytes = static_cast<std::uint64_t>(width) * height * 8;
    constexpr std::uint16_t entries = 15;
    constexpr std::uint32_t bitsOffset = 8 + 2 + entries * 12 + 4;
    constexpr std::uint32_t xResolutionOffset = bitsOffset + 8;
    constexpr std::uint32_t yResolutionOffset = xResolutionOffset + 8;
    constexpr std::uint32_t dataOffset = yResolutionOffset + 8;
    if (bytes > std::numeric_limits<std::uint32_t>::max() - dataOffset) throw std::runtime_error("Frame exceeds classic TIFF size limit");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write("II", 2); put16(out, 42); put32(out, 8); put16(out, entries);
    const auto tag = [&](std::uint16_t id, std::uint16_t type, std::uint32_t count, std::uint32_t value) {
        put16(out, id); put16(out, type); put32(out, count); put32(out, value);
    };
    tag(256, 4, 1, width); tag(257, 4, 1, height); tag(258, 3, 4, bitsOffset);
    tag(259, 3, 1, 1); tag(262, 3, 1, 2); tag(273, 4, 1, dataOffset);
    tag(274, 3, 1, 1); tag(277, 3, 1, 4); tag(278, 4, 1, height);
    tag(279, 4, 1, static_cast<std::uint32_t>(bytes)); tag(282, 5, 1, xResolutionOffset);
    tag(283, 5, 1, yResolutionOffset); tag(284, 3, 1, 1); tag(296, 3, 1, 1); tag(338, 3, 1, 2);
    put32(out, 0);
    for (int c = 0; c < 4; ++c) put16(out, 16);
    put32(out, static_cast<std::uint32_t>(aspect.denominator)); put32(out, 1);
    put32(out, static_cast<std::uint32_t>(aspect.numerator)); put32(out, 1);
    // TIFF little-endian, независимо от порядка байтов машины.
    std::vector<char> packed(data.size() * 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        packed[2 * i] = static_cast<char>(data[i] & 255);
        packed[2 * i + 1] = static_cast<char>(data[i] >> 8);
    }
    out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
    out.close();
}

} // namespace

struct VideoReader::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    AvFrame decoded = frame();
    AVStream* stream = nullptr;
    int streamIndex = -1, width = 0, height = 0;
    double fps = 0;
    std::int64_t sourceStart = 0, sourceCount = 0, timelineCount = 0, timelineDecoded = 0;
    double timelineFps = 0;
    bool timelineEnded = false;
    std::int64_t nextIndex = 0, firstPts = AV_NOPTS_VALUE;
    bool draining = false, ended = false;
    bool decodedAlpha = false;
    AVRational sampleAspect{0, 1};
    Transfer inputTransfer = Transfer::SRGB;
    std::size_t budget = 0, rawBytes = 0;
    std::function<bool()> cancelled;
    struct Raw { std::int64_t index; AvFrame pixels; std::size_t bytes; };
    std::deque<Raw> raw;
    std::map<std::int64_t, std::weak_ptr<const SourceFrame>> converted;
    std::vector<std::uint16_t> rgba;
    int remote = -1;
    std::int64_t remoteCount = 0;
    FrameReadStats remoteStats;

    ~Impl() { if(remote>=0) ::close(remote); sws_freeContext(scaler); av_packet_free(&packet); avcodec_free_context(&codec); avformat_close_input(&format); }

    void transfer(void* data, std::size_t bytes, bool sending) {
        auto* position = static_cast<unsigned char*>(data);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while(bytes) {
            if(cancelled()) throw std::runtime_error("Cancelled");
            if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Resolve frame request timed out");
            pollfd descriptor{remote, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
            const int ready = ::poll(&descriptor,1,200);
            if(ready<0 && errno==EINTR) continue;
            if(ready<0) throw std::runtime_error("Resolve frame socket poll failed");
            if(!ready) continue;
            const auto amount = sending ? ::send(remote,position,bytes,MSG_NOSIGNAL) : ::recv(remote,position,bytes,0);
            if(amount<0 && errno==EINTR) continue;
            if(amount<=0) throw std::runtime_error("Resolve frame provider disconnected; see resolve_frames.log");
            position+=amount;bytes-=static_cast<std::size_t>(amount);
        }
    }

    void fetchRemote(std::int64_t index) {
        const auto requested=std::chrono::steady_clock::now();
        ++remoteStats.requests;
        unsigned char request[8];
        for(int i=0;i<8;++i)request[i]=static_cast<unsigned char>(static_cast<std::uint64_t>(index)>>(8*i));
        transfer(request,sizeof(request),true);
        unsigned char header[9];transfer(header,sizeof(header),false);
        std::uint64_t size=0;
        for(int i=0;i<8;++i)size|=static_cast<std::uint64_t>(header[i+1])<<(8*i);
        const auto limit=header[0] ? 8192ULL : static_cast<std::uint64_t>(width)*height*8+1024*1024;
        if(size==0 || size>limit || size>INT_MAX)throw std::runtime_error("Invalid Resolve frame payload size");
        std::vector<unsigned char> payload(static_cast<std::size_t>(size));
        transfer(payload.data(),payload.size(),false);
        const auto received=std::chrono::steady_clock::now();
        remoteStats.waitSeconds+=std::chrono::duration<double>(received-requested).count();
        if(header[0])throw std::runtime_error("Resolve frame error: "+std::string(payload.begin(),payload.end()));
        avcodec_flush_buffers(codec);
        av_packet_unref(packet);
        check(av_new_packet(packet,static_cast<int>(payload.size())),"Allocate PNG packet");
        std::memcpy(packet->data,payload.data(),payload.size());
        // Каждый ExportFrame — самостоятельное изображение. Без KEY PNG decoder
        // может применить поддержку delta P-frames и прибавить предыдущий кадр.
        packet->flags |= AV_PKT_FLAG_KEY;
        check(avcodec_send_packet(codec,packet),"Send Resolve PNG");
        check(avcodec_receive_frame(codec,decoded.get()),"Decode Resolve PNG");
        if(decoded->width!=width || decoded->height!=height)throw std::runtime_error("Resolve frame dimensions changed or differ from timeline");
        const auto* description=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));
        decodedAlpha=description && (description->flags&AV_PIX_FMT_FLAG_ALPHA);
        if(decoded->sample_aspect_ratio.num>0 && decoded->sample_aspect_ratio.den>0)sampleAspect=decoded->sample_aspect_ratio;
        auto* clone=av_frame_clone(decoded.get());if(!clone)throw std::bad_alloc();
        AvFrame owned(clone,[](AVFrame* p){av_frame_free(&p);});
        const int bytes=av_image_get_buffer_size(static_cast<AVPixelFormat>(decoded->format),width,height,1);
        check(bytes,"Measure Resolve PNG");
        raw.push_back({index,std::move(owned),static_cast<std::size_t>(bytes)});rawBytes+=bytes;
        while(raw.size()>1 && rawBytes>budget){rawBytes-=raw.front().bytes;raw.pop_front();}
        av_frame_unref(decoded.get());
        remoteStats.decodeSeconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-received).count();
    }

    void openRemote(const std::string& path) {
        std::ifstream input(path);
        std::string line;
        if(!std::getline(input,line) || line!="SILENTFILM_FRAME_SOURCE_V1")throw std::runtime_error("Invalid frame source metadata");
        std::map<std::string,std::string> values;
        while(std::getline(input,line)) {
            const auto split=line.find('=');
            if(split!=std::string::npos)values[line.substr(0,split)]=line.substr(split+1);
        }
        width=std::stoi(values.at("width"));height=std::stoi(values.at("height"));fps=std::stod(values.at("fps"));
        remoteCount=std::stoll(values.at("frame_count"));
        const auto name=values.at("socket_name"),token=values.at("token");
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(width<=0||height<=0||width>32768||height>32768||!std::isfinite(fps)||fps<=0||remoteCount<=0||remoteCount>1000000000
            ||name.empty()||name.size()+1>sizeof(address.sun_path)||token.size()!=64)
            throw std::runtime_error("Invalid Resolve frame source configuration");
        remote=::socket(AF_UNIX,SOCK_STREAM,0);
        if(remote<0)throw std::runtime_error("Cannot create Resolve frame socket");
        // Linux abstract socket avoids the 108-byte limit for project directory paths.
        std::memcpy(address.sun_path+1,name.data(),name.size());
        if(::connect(remote,reinterpret_cast<sockaddr*>(&address),offsetof(sockaddr_un,sun_path)+1+name.size())<0)
            throw std::runtime_error("Resolve frame provider is unavailable; start a new capture");
        transfer(const_cast<char*>(token.data()),token.size(),true);
        char acknowledgement[4];transfer(acknowledgement,4,false);
        if(std::string(acknowledgement,4)!="SF01")throw std::runtime_error("Resolve frame provider authentication failed");
        const auto* decoder=avcodec_find_decoder(AV_CODEC_ID_PNG);
        if(!decoder)throw std::runtime_error("FFmpeg PNG decoder unavailable");
        codec=avcodec_alloc_context3(decoder);if(!codec)throw std::bad_alloc();
        codec->thread_count=1;
        check(avcodec_open2(codec,decoder,nullptr),"Open Resolve PNG decoder");
        packet=av_packet_alloc();if(!packet)throw std::bad_alloc();
        rgba.resize(static_cast<std::size_t>(width)*height*4);
        fetchRemote(0);
    }

    void open(const std::string& path) {
        format = avformat_alloc_context();
        if (!format) throw std::bad_alloc();
        format->interrupt_callback.callback = [](void* context) -> int { return static_cast<Impl*>(context)->cancelled() ? 1 : 0; };
        format->interrupt_callback.opaque = this;
        check(avformat_open_input(&format, path.c_str(), nullptr, nullptr), "Open source");
        check(avformat_find_stream_info(format, nullptr), "Read source metadata");
        streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(streamIndex, "Find video stream"); stream = format->streams[streamIndex];
        const std::uint8_t* matrix = nullptr;
        std::size_t matrixSize = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 31, 102)
        if (const auto* side = av_packet_side_data_get(stream->codecpar->coded_side_data,
            stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
            matrix = side->data; matrixSize = side->size;
        }
#else
        matrix = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &matrixSize);
#endif
        if (matrix) {
            if (matrixSize < 9 * sizeof(std::int32_t)) throw std::runtime_error("Invalid source display matrix");
            const double angle = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix));
            if (!std::isfinite(angle) || std::abs(angle) > 1e-4)
                throw std::runtime_error("Source has display rotation metadata; export an oriented CFR intermediate from Resolve first");
        }
        const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("No native FFmpeg decoder for source; export an intermediate from Resolve");
        codec = avcodec_alloc_context3(decoder);
        if (!codec) throw std::bad_alloc();
        check(avcodec_parameters_to_context(codec, stream->codecpar), "Read codec parameters");
        codec->thread_count = static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
        check(avcodec_open2(codec, decoder, nullptr), "Open source decoder");
        width = codec->width; height = codec->height;
        const auto rate = av_guess_frame_rate(format, stream, nullptr);
        fps = av_q2d(rate);
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || !std::isfinite(fps) || fps <= 0 || fps > 1000)
            throw std::runtime_error("Unsupported source dimensions or missing constant frame rate");
        sampleAspect = av_guess_sample_aspect_ratio(format, stream, nullptr);
        if (sampleAspect.num <= 0 || sampleAspect.den <= 0) sampleAspect = codec->sample_aspect_ratio;
        packet = av_packet_alloc(); if (!packet) throw std::bad_alloc();
        rgba.resize(static_cast<std::size_t>(width) * height * 4);
    }

    bool advance() {
        for (;;) {
            if (cancelled()) throw std::runtime_error("Cancelled");
            const int result = avcodec_receive_frame(codec, decoded.get());
            if (result == AVERROR_EOF) { ended = true; return false; }
            if (result >= 0) {
                const auto* pixelFormat = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded->format));
                decodedAlpha = pixelFormat && (pixelFormat->flags & AV_PIX_FMT_FLAG_ALPHA);
                const AVRational frameAspect = av_guess_sample_aspect_ratio(format, stream, decoded.get());
                if (frameAspect.num > 0 && frameAspect.den > 0) {
                    if (nextIndex > 0 && sampleAspect.num > 0 && sampleAspect.den > 0 && av_cmp_q(sampleAspect, frameAspect) != 0)
                        throw std::runtime_error("Source pixel aspect ratio changes within the clip");
                    sampleAspect = frameAspect;
                }
                if (decoded->width != width || decoded->height != height) throw std::runtime_error("Source resolution changes within the clip");
                if (decoded->color_trc == AVCOL_TRC_SMPTE2084 || decoded->color_trc == AVCOL_TRC_ARIB_STD_B67)
                    throw std::runtime_error("HDR PQ/HLG is unsupported; prepare SDR Rec.709 input in Resolve");
                if (decoded->color_primaries != AVCOL_PRI_UNSPECIFIED && decoded->color_primaries != AVCOL_PRI_RESERVED0
                    && decoded->color_primaries != AVCOL_PRI_RESERVED && decoded->color_primaries != AVCOL_PRI_BT709)
                    throw std::runtime_error("Source primaries are not Rec.709; prepare an explicit Rec.709 intermediate");
                const auto pts = decoded->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    if (firstPts == AV_NOPTS_VALUE) firstPts = pts;
                    const double position = static_cast<double>(pts - firstPts) * av_q2d(stream->time_base) * fps;
                    if (std::abs(position - nextIndex) > 0.25)
                        throw std::runtime_error("Variable/missing source timestamps; export CFR input from Resolve first");
                }
                auto* clone = av_frame_clone(decoded.get()); if (!clone) throw std::bad_alloc();
                AvFrame owned(clone, [](AVFrame* f) { av_frame_free(&f); });
                const int bytes = av_image_get_buffer_size(static_cast<AVPixelFormat>(decoded->format), width, height, 1);
                check(bytes, "Measure source frame");
                raw.push_back({nextIndex++, std::move(owned), static_cast<std::size_t>(bytes)});
                rawBytes += bytes;
                while (raw.size() > 1 && rawBytes > budget) { rawBytes -= raw.front().bytes; raw.pop_front(); }
                av_frame_unref(decoded.get());
                return true;
            }
            if (result != AVERROR(EAGAIN)) check(result, "Decode source frame");
            if (draining) throw std::runtime_error("Decoder failed to finish draining");
            for (;;) {
                av_packet_unref(packet);
                const int read = av_read_frame(format, packet);
                if (read == AVERROR_EOF) { check(avcodec_send_packet(codec, nullptr), "Drain decoder"); draining = true; break; }
                check(read, "Read video packet");
                if (packet->stream_index == streamIndex) { check(avcodec_send_packet(codec, packet), "Send video packet"); break; }
            }
        }
    }

    std::int64_t locate(std::int64_t index) {
        if(remoteCount) {
            if(index>=remoteCount)ended=true;
            nextIndex=remoteCount;
            return std::clamp<std::int64_t>(index,0,remoteCount-1);
        }
        if (index < 0) index = 0;
        while (!ended && nextIndex <= index) advance();
        if (nextIndex == 0) throw std::runtime_error("Source contains no video frames");
        return std::min(index, nextIndex - 1);
    }

    std::int64_t sourceIndex(std::int64_t index) const {
        index = std::clamp<std::int64_t>(index, 0, timelineCount - 1);
        return sourceStart + std::min(sourceCount - 1, ((2 * index + 1) * sourceCount) / (2 * timelineCount));
    }

    std::int64_t locateTimeline(std::int64_t index) {
        if (index >= timelineCount) timelineEnded = true;
        index = std::clamp<std::int64_t>(index, 0, timelineCount - 1);
        const auto source = sourceIndex(index);
        if (locate(source) != source) throw std::runtime_error("Timeline In/Out exceeds decoded source length");
        timelineDecoded = std::max(timelineDecoded, index + 1);
        return index;
    }

    std::shared_ptr<const SourceFrame> get(std::int64_t index) {
        index = locate(index);
        const auto cached = converted.find(index);
        if (cached != converted.end()) if (auto existing = cached->second.lock()) return existing;
        if(remoteCount && std::none_of(raw.begin(),raw.end(),[index](const Raw& p){return p.index==index;}))fetchRemote(index);
        const auto it = std::find_if(raw.begin(), raw.end(), [index](const Raw& p) { return p.index == index; });
        if (it == raw.end()) throw std::runtime_error("Source frame fell outside the decode cache; increase cache_mib");
        const auto* input = it->pixels.get();
        scaler = sws_getCachedContext(scaler, width, height, static_cast<AVPixelFormat>(input->format), width, height,
            AV_PIX_FMT_RGBA64LE, SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error("Cannot convert decoded source to RGBA16");
        const auto* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(input->format));
        const bool rgb = description && (description->flags & AV_PIX_FMT_FLAG_RGB);
        const auto* coefficients = sws_getCoefficients(input->colorspace == AVCOL_SPC_SMPTE170M || input->colorspace == AVCOL_SPC_BT470BG
            ? SWS_CS_ITU601 : SWS_CS_ITU709);
        check(sws_setColorspaceDetails(scaler, coefficients, rgb || input->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
            coefficients, 1, 0, 1 << 16, 1 << 16), "Set decoder color matrix");
        std::uint8_t* planes[4]{reinterpret_cast<std::uint8_t*>(rgba.data()), nullptr, nullptr, nullptr};
        int strides[4]{width * 8, 0, 0, 0};
        if (sws_scale(scaler, input->data, input->linesize, 0, height, planes, strides) != height)
            throw std::runtime_error("Incomplete source color conversion");
        auto result = std::make_shared<SourceFrame>(); result->index = index;
        result->pixels.resize(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < result->pixels.size(); ++i) {
            const auto component = [&](int c) {
                const float v = rgba[4 * i + c] / 65535.0f;
                return inputTransfer == Transfer::SRGB ? v : film::encode(film::decode(v, inputTransfer), Transfer::SRGB);
            };
            result->pixels[i] = {component(0), component(1), component(2), rgba[4 * i + 3] / 65535.0f};
        }
        for (auto c = converted.begin(); c != converted.end();) {
            if (c->second.expired()) c = converted.erase(c); else ++c;
        }
        converted[index] = result;
        return result;
    }
};

VideoReader::VideoReader(const std::string& path, std::size_t bytes, Transfer transfer, std::function<bool()> cancelled, bool provider)
    : impl_(std::make_unique<Impl>()) {
    impl_->budget = bytes; impl_->inputTransfer = transfer; impl_->cancelled = std::move(cancelled);
    if(provider)impl_->openRemote(path);else impl_->open(path);
}
VideoReader::~VideoReader() = default;
int VideoReader::width() const { return impl_->width; }
int VideoReader::height() const { return impl_->height; }
double VideoReader::fps() const { return impl_->timelineCount ? impl_->timelineFps : impl_->fps; }
PixelAspectRatio VideoReader::pixelAspectRatio() const {
    const auto aspect = impl_->sampleAspect;
    return aspect.num > 0 && aspect.den > 0 ? PixelAspectRatio{aspect.num, aspect.den} : PixelAspectRatio{};
}
bool VideoReader::hasAlphaChannel() const { return impl_->decodedAlpha; }
FrameReadStats VideoReader::frameReadStats() const { return impl_->remoteStats; }
std::int64_t VideoReader::frameCountHint() const { return impl_->timelineCount ? impl_->timelineCount : (impl_->remoteCount ? impl_->remoteCount : impl_->stream->nb_frames); }
std::int64_t VideoReader::decodedCount() const { return impl_->timelineCount ? impl_->timelineDecoded : impl_->nextIndex; }
bool VideoReader::eof() const { return impl_->timelineCount ? impl_->timelineEnded : impl_->ended; }
std::int64_t VideoReader::locate(std::int64_t index) { return impl_->timelineCount ? impl_->locateTimeline(index) : impl_->locate(index); }
std::shared_ptr<const SourceFrame> VideoReader::get(std::int64_t index) {
    if (impl_->timelineCount) index = impl_->sourceIndex(impl_->locateTimeline(index));
    return impl_->get(index);
}
void VideoReader::setTimelineRange(std::int64_t start, std::int64_t count, std::int64_t frames, double fps) {
    if (impl_->nextIndex || start < 0 || count <= 0 || frames <= 0 || count > 1000000000 || frames > 1000000000
        || !std::isfinite(fps) || fps <= 0) throw std::runtime_error("Invalid native timeline source mapping");
    impl_->sourceStart = start; impl_->sourceCount = count; impl_->timelineCount = frames; impl_->timelineFps = fps;
}

struct FrameWriter::Impl {
    std::filesystem::path directory, path;
    std::string format;
    std::string movieName = "cadence", tiffPrefix = "frame_";
    int width = 0, height = 0;
    PixelAspectRatio aspect;
    bool finished = false;
    std::vector<std::uint16_t> rgba;
    AVFormatContext* container = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    AvFrame encoded = frame();
    ~Impl() {
        sws_freeContext(scaler); av_packet_free(&packet); avcodec_free_context(&codec);
        if (container) { if (container->pb) avio_closep(&container->pb); avformat_free_context(container); }
    }
    void packets() {
        for (;;) {
            const int status = avcodec_receive_packet(codec, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            check(status, "Encode ProRes packet");
            if (packet->duration <= 0) packet->duration = 1;
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base); packet->stream_index = stream->index;
            check(av_interleaved_write_frame(container, packet), "Write ProRes packet"); av_packet_unref(packet);
        }
    }
    void open(double fps, PixelAspectRatio aspect) {
        rgba.resize(static_cast<std::size_t>(width) * height * 4);
        if (format == "tiff") {
            if (!std::filesystem::create_directory(directory / "frames")) throw std::runtime_error("Output frames directory already exists");
            path = directory / "frames" / (tiffPrefix + "%08d.tif"); return;
        }
        if (format != "prores") throw std::runtime_error("Output format must be prores or tiff");
        path = directory / (movieName + ".mov");
        if (std::filesystem::exists(path)) throw std::runtime_error("Output movie already exists");
        check(avformat_alloc_output_context2(&container, nullptr, "mov", path.c_str()), "Create MOV output");
        const auto* encoder = avcodec_find_encoder_by_name("prores_ks");
        if (!encoder) throw std::runtime_error("FFmpeg prores_ks encoder is unavailable; select TIFF output");
        codec = avcodec_alloc_context3(encoder); if (!codec) throw std::bad_alloc();
        codec->width = width; codec->height = height; codec->pix_fmt = AV_PIX_FMT_YUVA444P10LE;
        codec->sample_aspect_ratio = AVRational{aspect.numerator, aspect.denominator};
        codec->framerate = av_d2q(fps, 1001000); codec->time_base = av_inv_q(codec->framerate);
        codec->color_primaries = AVCOL_PRI_BT709; codec->color_trc = AVCOL_TRC_IEC61966_2_1;
        codec->colorspace = AVCOL_SPC_BT709; codec->color_range = AVCOL_RANGE_MPEG;
        codec->thread_count = static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
        codec->flags |= AV_CODEC_FLAG_QSCALE; codec->global_quality = 5 * FF_QP2LAMBDA;
        if (container->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set_int(codec->priv_data, "profile", 4, 0), "Set ProRes 4444 profile");
        check(avcodec_open2(codec, encoder, nullptr), "Open ProRes encoder");
        stream = avformat_new_stream(container, nullptr); if (!stream) throw std::bad_alloc();
        stream->sample_aspect_ratio = codec->sample_aspect_ratio;
        check(avcodec_parameters_from_context(stream->codecpar, codec), "Set MOV video parameters");
        stream->time_base = codec->time_base; stream->avg_frame_rate = codec->framerate;
        check(avio_open(&container->pb, path.c_str(), AVIO_FLAG_WRITE), "Open MOV output");
        check(avformat_write_header(container, nullptr), "Write MOV header");
        encoded->format = codec->pix_fmt; encoded->width = width; encoded->height = height;
        encoded->sample_aspect_ratio = codec->sample_aspect_ratio;
        encoded->color_primaries = codec->color_primaries; encoded->color_trc = codec->color_trc;
        encoded->colorspace = codec->colorspace; encoded->color_range = codec->color_range;
        check(av_frame_get_buffer(encoded.get(), 32), "Allocate ProRes image");
        packet = av_packet_alloc(); if (!packet) throw std::bad_alloc();
        scaler = sws_getContext(width, height, AV_PIX_FMT_RGBA64LE, width, height, codec->pix_fmt,
            SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error("Cannot convert camera output to ProRes");
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        check(sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16), "Set ProRes color matrix");
    }
};

FrameWriter::FrameWriter(const std::filesystem::path& directory, const std::string& format, int width, int height, double fps,
    const std::string& outputName, PixelAspectRatio aspect)
    : impl_(std::make_unique<Impl>()) {
    if (!outputName.empty()) {
        if (outputName == "." || outputName == ".." || outputName.size() > 220
            || outputName.find_first_of("/\\%\r\n") != std::string::npos
            || std::any_of(outputName.begin(), outputName.end(), [](unsigned char value) { return value < 32 || value == 127; }))
            throw std::runtime_error("Invalid output basename");
        impl_->movieName = outputName;
        impl_->tiffPrefix = outputName + "_";
    }
    if (aspect.numerator <= 0 || aspect.denominator <= 0) throw std::runtime_error("Invalid pixel aspect ratio");
    impl_->aspect = aspect;
    impl_->directory = directory; impl_->format = format; impl_->width = width; impl_->height = height; impl_->open(fps, aspect);
}
FrameWriter::~FrameWriter() = default;
void FrameWriter::write(const std::vector<Pixel>& pixels, std::int64_t index)
{
    auto& p = *impl_;
    if (p.finished || pixels.size() != static_cast<std::size_t>(p.width) * p.height) throw std::runtime_error("Invalid output frame");
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        p.rgba[4 * i] = word(pixels[i].r); p.rgba[4 * i + 1] = word(pixels[i].g);
        p.rgba[4 * i + 2] = word(pixels[i].b); p.rgba[4 * i + 3] = word(pixels[i].a);
    }
    if (p.format == "tiff") {
        std::ostringstream name; name << p.tiffPrefix << std::setw(8) << std::setfill('0') << index << ".tif";
        tiff(p.directory / "frames" / name.str(), p.width, p.height, p.rgba, p.aspect); return;
    }
    check(av_frame_make_writable(p.encoded.get()), "Reuse ProRes image");
    const std::uint8_t* planes[4]{reinterpret_cast<const std::uint8_t*>(p.rgba.data()), nullptr, nullptr, nullptr};
    int strides[4]{p.width * 8, 0, 0, 0};
    if (sws_scale(p.scaler, planes, strides, 0, p.height, p.encoded->data, p.encoded->linesize) != p.height)
        throw std::runtime_error("Incomplete ProRes color conversion");
    p.encoded->pts = index; p.encoded->quality = p.codec->global_quality;
    check(avcodec_send_frame(p.codec, p.encoded.get()), "Send ProRes image"); p.packets();
}
void FrameWriter::finish()
{
    auto& p = *impl_;
    if (p.finished) return;
    if (p.format == "prores") { check(avcodec_send_frame(p.codec, nullptr), "Drain ProRes encoder"); p.packets(); check(av_write_trailer(p.container), "Finish MOV"); check(avio_closep(&p.container->pb), "Close MOV"); }
    p.finished = true;
}
std::filesystem::path FrameWriter::mediaPath() const { return impl_->path; }
std::string FrameWriter::format() const { return impl_->format; }

} // namespace silentfilm::offline
