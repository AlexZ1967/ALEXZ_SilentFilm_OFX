#include "flow_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

#ifdef SILENTFILM_CADENCE_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#endif

namespace silentfilm::offline {

#ifdef SILENTFILM_CADENCE_OPENCV
struct FlowSampler::Impl {
    struct Pair {
        cv::Mat forward, backward, confidenceFirst, confidenceSecond, difference;
        bool stationaryRgb = false;
        std::uint64_t used = 0;
    };
    int width, height;
    std::size_t capacity;
    std::function<bool()> cancelled;
    std::map<std::pair<std::int64_t, std::int64_t>, Pair> pairs;
    std::uint64_t clock = 0;

    void check() const {
        if (cancelled && cancelled()) throw std::runtime_error("Cancelled");
    }

    cv::Mat gray(const SourceFrame& frame) const {
        cv::Mat image(height, width, CV_8UC1);
        for (int y = 0; y < height; ++y) {
            check();
            auto* row = image.ptr<unsigned char>(y);
            for (int x = 0; x < width; ++x) {
                const auto& p = frame.pixels[static_cast<std::size_t>(y) * width + x];
                row[x] = static_cast<unsigned char>(std::clamp(
                    (p.r * 0.299f + p.g * 0.587f + p.b * 0.114f) * 255.0f, 0.0f, 255.0f));
            }
        }
        return image;
    }

    cv::Mat warp(const cv::Mat& image, const cv::Mat& flow, float amount) const {
        cv::Mat mapX(height, width, CV_32F), mapY(height, width, CV_32F), result;
        for (int y = 0; y < height; ++y) {
            check();
            const auto* f = flow.ptr<cv::Vec2f>(y);
            auto* xx = mapX.ptr<float>(y);
            auto* yy = mapY.ptr<float>(y);
            for (int x = 0; x < width; ++x) {
                xx[x] = x - f[x][0] * amount;
                yy[x] = y - f[x][1] * amount;
            }
        }
        cv::remap(image, result, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        return result;
    }

    cv::Mat confidence(const cv::Mat& flow, const cv::Mat& reverse) const {
        const auto opposite = warp(reverse, flow, -1.0f);
        cv::Mat result(height, width, CV_32F);
        for (int y = 0; y < height; ++y) {
            check();
            const auto* f = flow.ptr<cv::Vec2f>(y);
            const auto* r = opposite.ptr<cv::Vec2f>(y);
            auto* out = result.ptr<float>(y);
            for (int x = 0; x < width; ++x) {
                const float dx = f[x][0] + r[x][0], dy = f[x][1] + r[x][1];
                const float magnitude = f[x].dot(f[x]) + r[x].dot(r[x]);
                const float xx = x + f[x][0], yy = y + f[x][1];
                out[x] = xx >= 0 && xx <= width - 1 && yy >= 0 && yy <= height - 1
                    ? std::exp(-(dx * dx + dy * dy) / (0.5f + 0.01f * magnitude)) : 0.0f;
            }
        }
        return result;
    }

    Pair build(const SourceFrame& first, const SourceFrame& second) const {
        Pair pair;
        pair.stationaryRgb = true;
        // Поток оценивается по RGB: точное совпадение не требует деформации.
        // Alpha может отличаться и интерполируется отдельно, без epsilon-порога.
        for (int y = 0; y < height && pair.stationaryRgb; ++y) {
            check();
            for (int x = 0; x < width; ++x) {
                const auto i = static_cast<std::size_t>(y) * width + x;
                const auto& a = first.pixels[i]; const auto& b = second.pixels[i];
                if (a.r != b.r || a.g != b.g || a.b != b.b) {
                    pair.stationaryRgb = false;
                    break;
                }
            }
        }
        if (pair.stationaryRgb) return pair;
        auto a = gray(first), b = gray(second);
        const double scale = std::min(1.0, 640.0 / std::max(width, height));
        if (scale < 1) {
            const cv::Size size(std::max(8, static_cast<int>(std::nearbyint(width * scale))),
                std::max(8, static_cast<int>(std::nearbyint(height * scale))));
            cv::resize(a, a, size, 0, 0, cv::INTER_AREA);
            cv::resize(b, b, size, 0, 0, cv::INTER_AREA);
        }
        check();
        cv::calcOpticalFlowFarneback(a, b, pair.forward, 0.5, 3, 17, 3, 5, 1.1, 0);
        check();
        cv::calcOpticalFlowFarneback(b, a, pair.backward, 0.5, 3, 17, 3, 5, 1.1, 0);
        check();
        if (a.cols != width || a.rows != height) {
            for (auto* flow : {&pair.forward, &pair.backward}) {
                cv::resize(*flow, *flow, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
                for (int y = 0; y < height; ++y) {
                    check();
                    auto* row = flow->ptr<cv::Vec2f>(y);
                    for (int x = 0; x < width; ++x) {
                        row[x][0] *= static_cast<float>(width) / a.cols;
                        row[x][1] *= static_cast<float>(height) / a.rows;
                    }
                }
            }
        }
        pair.confidenceFirst = confidence(pair.forward, pair.backward);
        pair.confidenceSecond = confidence(pair.backward, pair.forward);
        pair.difference.create(height, width, CV_32F);
        for (int y = 0; y < height; ++y) {
            check();
            auto* row = pair.difference.ptr<float>(y);
            for (int x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y) * width + x;
                const auto& p = first.pixels[index]; const auto& q = second.pixels[index];
                row[x] = (std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b)) / 3.0f;
            }
        }
        return pair;
    }
};

FlowSampler::FlowSampler(int width, int height, std::size_t bytes, std::function<bool()> cancelled)
    : impl_(std::make_unique<Impl>())
{
    if (width <= 0 || height <= 0) throw std::runtime_error("Invalid optical-flow image dimensions");
    impl_->width = width; impl_->height = height; impl_->cancelled = std::move(cancelled);
    const auto pairBytes = static_cast<std::size_t>(width) * height * 7 * sizeof(float);
    impl_->capacity = std::clamp<std::size_t>(bytes / pairBytes, 1, 4);
}

std::shared_ptr<const SourceFrame> FlowSampler::sample(std::shared_ptr<const SourceFrame> first,
    std::shared_ptr<const SourceFrame> second, float mix)
{
    auto& s = *impl_;
    s.check();
    const auto count = static_cast<std::size_t>(s.width) * s.height;
    if (!first || !second || first->pixels.size() != count || second->pixels.size() != count
        || !std::isfinite(mix) || mix < 0 || mix > 1)
        throw std::runtime_error("Invalid optical-flow frame pair");
    if (mix <= 1e-6f || first->index == second->index) return first;
    if (mix == 1) return second;
    const auto key = std::make_pair(first->index, second->index);
    auto found = s.pairs.find(key);
    if (found == s.pairs.end()) {
        while (s.pairs.size() >= s.capacity) {
            auto oldest = std::min_element(s.pairs.begin(), s.pairs.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            s.pairs.erase(oldest);
        }
        found = s.pairs.emplace(key, s.build(*first, *second)).first;
    }
    auto& pair = found->second; pair.used = ++s.clock;
    if (pair.stationaryRgb) {
        auto result = std::make_shared<SourceFrame>();
        result->index = first->index; result->pixels.resize(count);
        for (int y = 0; y < s.height; ++y) {
            s.check();
            for (int x = 0; x < s.width; ++x) {
                const auto i = static_cast<std::size_t>(y) * s.width + x;
                result->pixels[i] = first->pixels[i];
                if (first->pixels[i].a != second->pixels[i].a)
                    result->pixels[i].a = first->pixels[i].a * (1.0f - mix) + second->pixels[i].a * mix;
            }
        }
        return result;
    }
    static_assert(sizeof(Pixel) == 4 * sizeof(float), "Flow requires tightly packed RGBA pixels");
    const cv::Mat a(s.height, s.width, CV_32FC4, const_cast<Pixel*>(first->pixels.data()));
    const cv::Mat b(s.height, s.width, CV_32FC4, const_cast<Pixel*>(second->pixels.data()));
    const auto warpedA = s.warp(a, pair.forward, mix);
    const auto warpedB = s.warp(b, pair.backward, 1.0f - mix);
    const auto confidenceA = s.warp(pair.confidenceFirst, pair.forward, mix);
    const auto confidenceB = s.warp(pair.confidenceSecond, pair.backward, 1.0f - mix);
    cv::Mat confidence(s.height, s.width, CV_32F);
    for (int y = 0; y < s.height; ++y) {
        s.check();
        auto* out = confidence.ptr<float>(y);
        const auto* wa = warpedA.ptr<cv::Vec4f>(y); const auto* wb = warpedB.ptr<cv::Vec4f>(y);
        for (int x = 0; x < s.width; ++x) {
            const float c = std::min({confidenceA.at<float>(y, x), confidenceB.at<float>(y, x),
                pair.confidenceFirst.at<float>(y, x), pair.confidenceSecond.at<float>(y, x)});
            const float disagreement = (std::abs(wa[x][0] - wb[x][0]) + std::abs(wa[x][1] - wb[x][1])
                + std::abs(wa[x][2] - wb[x][2])) / 3.0f;
            out[x] = c * std::exp(-std::max(0.0f, disagreement - pair.difference.at<float>(y, x)) / 0.03f);
        }
    }
    cv::GaussianBlur(confidence, confidence, cv::Size(5, 5), 0);
    auto result = std::make_shared<SourceFrame>();
    result->index = first->index; result->pixels.resize(count);
    cv::Mat output(s.height, s.width, CV_32FC4, result->pixels.data());
    for (int y = 0; y < s.height; ++y) {
        s.check();
        const auto* aa = a.ptr<cv::Vec4f>(y); const auto* bb = b.ptr<cv::Vec4f>(y);
        const auto* wa = warpedA.ptr<cv::Vec4f>(y); const auto* wb = warpedB.ptr<cv::Vec4f>(y);
        const auto* c = confidence.ptr<float>(y); auto* out = output.ptr<cv::Vec4f>(y);
        for (int x = 0; x < s.width; ++x)
            out[x] = (wa[x] * (1.0f - mix) + wb[x] * mix) * c[x]
                + (aa[x] * (1.0f - mix) + bb[x] * mix) * (1.0f - c[x]);
    }
    return result;
}
#else
struct FlowSampler::Impl {};
FlowSampler::FlowSampler(int, int, std::size_t, std::function<bool()>) {
    throw std::runtime_error("Optical flow requires OpenCV core/imgproc/video. Install libopencv-dev and reconfigure/rebuild Cadence, or select simple interpolation.");
}
std::shared_ptr<const SourceFrame> FlowSampler::sample(std::shared_ptr<const SourceFrame>,
    std::shared_ptr<const SourceFrame>, float) {
    throw std::runtime_error("Optical flow is unavailable in this build");
}
#endif
FlowSampler::~FlowSampler() = default;

} // namespace silentfilm::offline
