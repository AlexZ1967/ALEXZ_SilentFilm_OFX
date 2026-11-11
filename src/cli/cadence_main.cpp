#include "media_io.hpp"
#include "flow_sampler.hpp"
#include "silentfilm/cadence.hpp"

#ifdef SILENTFILM_CLI_CUDA
#include <cuda_runtime_api.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
namespace fs = std::filesystem;
using namespace silentfilm;
using namespace silentfilm::offline;

volatile std::sig_atomic_t interrupted = 0;
void signalHandler(int) { interrupted = 1; }

std::string trim(std::string s)
{
    const auto a = s.find_first_not_of(" \t\r");
    return a == std::string::npos ? std::string{} : s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

using Values = std::map<std::string, std::string>;
Values readSettings(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open request.ini");
    Values values;
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw std::runtime_error("Malformed request.ini line");
        if (!values.emplace(trim(line.substr(0, equals)), trim(line.substr(equals + 1))).second)
            throw std::runtime_error("Duplicate request.ini key");
    }
    return values;
}

void atomicValues(const fs::path& path, const Values& values)
{
    const auto temporary = fs::path(path.string() + ".tmp");
    std::ofstream out(temporary, std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    for (const auto& [key, value] : values) {
        auto safe = value;
        std::replace(safe.begin(), safe.end(), '\n', ' '); std::replace(safe.begin(), safe.end(), '\r', ' ');
        out << key << '=' << safe << '\n';
    }
    out.close(); fs::rename(temporary, path);
}

std::string number(double value)
{
    std::ostringstream out; out << std::setprecision(17) << value; return out.str();
}

class Job {
    std::chrono::steady_clock::time_point nextCheck_{};
    bool cancelled_ = false;
public:
    fs::path directory;
    Values status{{"schema", "1"}, {"state", "starting"}, {"frames_written", "0"}};
    explicit Job(fs::path path) : directory(std::move(path)) {}
    bool cancelled() {
        if (interrupted) cancelled_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextCheck_) {
            std::error_code error;
            cancelled_ = cancelled_ || fs::exists(directory / "cancel.request", error);
            nextCheck_ = now + std::chrono::milliseconds(100);
        }
        return cancelled_;
    }
    void update(const std::string& state, const std::string& message) {
        status["state"] = state; status["message"] = message;
        atomicValues(directory / "status.ini", status);
    }
};

struct Settings {
    std::string input, format = "prores", device = "cuda";
    std::string outputName;
    bool splitCamera = false;
    std::string blurMode = "simple";
    double outputFps = 25, expectedSourceFps = 0, sourceFps = 0;
    std::int64_t start = 0, length = 0;
    std::size_t cacheBytes = 1024ULL * 1024 * 1024, gpuCacheBytes = 512ULL * 1024 * 1024;
    int gpu = 0;
    Transfer inputTransfer = Transfer::SRGB;
    CadenceSettings camera;
};

Settings parse(Values values)
{
    Settings s;
    const auto get = [&](const std::string& key, const std::string& fallback) {
        const auto it = values.find(key);
        if (it == values.end()) return fallback;
        auto value = it->second; values.erase(it); return value;
    };
    const auto scalar = [&](const std::string& key, double fallback, double minimum, double maximum) {
        const auto text = get(key, number(fallback));
        std::size_t consumed = 0; const double v = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(v) || v < minimum || v > maximum)
            throw std::runtime_error("Invalid setting: " + key);
        return v;
    };
    const auto integer = [&](const std::string& key, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum) {
        const double v = scalar(key, static_cast<double>(fallback), static_cast<double>(minimum), static_cast<double>(maximum));
        if (v != std::floor(v)) throw std::runtime_error("Expected integer setting: " + key);
        return static_cast<std::int64_t>(v);
    };
    if (get("schema", "1") != "1") throw std::runtime_error("Unsupported request schema");
    const auto processing = get("processing", "full_camera");
    if (processing != "full_camera" && processing != "capture_rgb")
        throw std::runtime_error("Processing must be full_camera or capture_rgb");
    s.splitCamera = processing == "capture_rgb";
    if (s.splitCamera) for (const auto* key : {"focus_seed", "vignette", "edge_softness", "softness", "focus_drift", "edge_start", "edge_end"}) {
        if (values.find(key) != values.end())
            throw std::runtime_error(std::string("Optics parameter belongs to Camera Optics, not capture_rgb: ") + key);
    }
    s.blurMode = get("blur_mode", "simple");
    if (s.blurMode != "simple" && s.blurMode != "flow_integrated")
        throw std::runtime_error("Blur mode must be simple or flow_integrated");
    s.outputName = get("output_name", "");
    s.input = get("input", "");
    if (s.input.empty() || !fs::is_regular_file(s.input)) throw std::runtime_error("Input must be a regular local media file");
    s.format = get("format", "prores"); s.device = get("device", "cuda");
    if (s.format != "prores" && s.format != "tiff") throw std::runtime_error("Select prores or tiff output");
    if (s.device != "cuda" && s.device != "cpu") throw std::runtime_error("Select cuda or cpu backend");
    const auto transfer = get("input_transfer", "srgb");
    if (transfer == "linear") s.inputTransfer = Transfer::Linear;
    else if (transfer == "gamma24") s.inputTransfer = Transfer::Gamma24;
    else if (transfer != "srgb") throw std::runtime_error("Input transfer must be srgb, linear or gamma24");
    s.outputFps = scalar("output_fps", 25, 1, 120);
    for (const double ntsc : {24000.0 / 1001, 30000.0 / 1001, 48000.0 / 1001, 60000.0 / 1001, 120000.0 / 1001})
        if (std::abs(s.outputFps - ntsc) < 0.0005) { s.outputFps = ntsc; break; }
    s.expectedSourceFps = scalar("expected_source_fps", 0, 0, 1000);
    s.sourceFps = scalar("source_fps", 0, 0, 120);
    if (s.sourceFps > 0 && s.sourceFps < 1) throw std::runtime_error("source_fps must be 0 (auto) or 1..120");
    s.start = integer("start_frame", 0, 0, 1000000000); s.length = integer("frame_count", 0, 0, 1000000000);
    s.cacheBytes = static_cast<std::size_t>(integer("cache_mib", 1024, 32, 65536)) * 1024 * 1024;
    s.gpuCacheBytes = static_cast<std::size_t>(integer("gpu_cache_mib", 512, 32, 65536)) * 1024 * 1024;
    s.gpu = static_cast<int>(integer("gpu_index", 0, 0, 32));
    auto& c = s.camera;
    c.crank.minimumFps = static_cast<float>(scalar("fps_min", 16, 1, 60));
    c.crank.maximumFps = static_cast<float>(scalar("fps_max", 20, 1, 60));
    if (c.crank.minimumFps > c.crank.maximumFps) throw std::runtime_error("FPS Min must not exceed FPS Max");
    c.crank.rateVariation = static_cast<float>(scalar("variation", 0.7, 0, 1));
    c.crank.irregularity = static_cast<float>(scalar("irregularity", 0.15, 0, 1));
    c.crank.seed = static_cast<std::uint32_t>(integer("seed", 1925, 0, 2147483647));
    c.shutterFraction = scalar("shutter", 0.65, 0, 1.5); c.motionBlurStrength = scalar("motion_blur", 0.8, 0, 2);
    c.exposureVariation = scalar("exposure_link", 1, 0, 1); c.exposureSamples = static_cast<int>(integer("samples", 9, 1, maximumCameraExposureSamples));
    c.shake = static_cast<float>(scalar("shake", 0.25, 0, 1));
    c.focusSeed = static_cast<std::uint32_t>(integer("focus_seed", 1925, 0, 2147483647));
    c.optics.vignetteStrength = static_cast<float>(scalar("vignette", 0.12, 0, 1));
    c.optics.edgeSoftness = static_cast<float>(scalar("edge_softness", 0.15, 0, 1));
    c.optics.softness = static_cast<float>(scalar("softness", 0.1, 0, 1));
    c.optics.focusDrift = static_cast<float>(scalar("focus_drift", 0.03, 0, 0.5));
    c.optics.edgeStart = static_cast<float>(scalar("edge_start", 0.6, 0, 4));
    c.optics.edgeEnd = static_cast<float>(scalar("edge_end", 1, 0.01, 4));
    if (c.optics.edgeEnd <= c.optics.edgeStart) throw std::runtime_error("Edge End must exceed Edge Start");
    if (!values.empty()) throw std::runtime_error("Unknown request key: " + values.begin()->first);
    return s;
}

ImageView view(const std::vector<Pixel>& pixels, int width, int height)
{
    return {reinterpret_cast<unsigned char*>(const_cast<Pixel*>(pixels.data())),
        static_cast<std::ptrdiff_t>(width) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), 0, 0, width, height};
}

struct SampleIndices { std::int64_t left, right; float mix, phase, weight; };

/** Overlap CPU encoding with the next decode/GPU capture using a bounded, copy-free queue. */
class AsyncWriter {
    struct OutputFrame { std::vector<Pixel> pixels; std::int64_t index; };
    FrameWriter writer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<OutputFrame> pending_;
    std::vector<Pixel> reusable_;
    std::exception_ptr failure_;
    bool closing_ = false, stop_ = false;
    std::atomic<std::int64_t> written_{0};
    std::thread worker_;

    void work() noexcept {
        try {
            for (;;) {
                OutputFrame task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait(lock, [&] { return stop_ || closing_ || !pending_.empty(); });
                    if (stop_) return;
                    if (pending_.empty()) break;
                    task = std::move(pending_.front()); pending_.pop_front();
                    changed_.notify_all();
                }
                writer_.write(task.pixels, task.index); ++written_;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    if (reusable_.empty()) reusable_ = std::move(task.pixels);
                }
            }
            writer_.finish();
        } catch (...) {
            const std::lock_guard<std::mutex> lock(mutex_);
            failure_ = std::current_exception(); pending_.clear(); changed_.notify_all();
        }
    }
public:
    AsyncWriter(const fs::path& directory, const std::string& format, int width, int height, double fps, const std::string& outputName, PixelAspectRatio aspect)
        : writer_(directory, format, width, height, fps, outputName, aspect), worker_([this] { work(); }) {}
    ~AsyncWriter() {
        if (worker_.joinable()) {
            { const std::lock_guard<std::mutex> lock(mutex_); stop_ = true; pending_.clear(); }
            changed_.notify_all(); worker_.join();
        }
    }
    void write(std::vector<Pixel>& pixels, std::int64_t index, Job& job) {
        const auto size = pixels.size();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (pending_.size() >= 1 && !failure_) {
                changed_.wait_for(lock, std::chrono::milliseconds(100));
                if (job.cancelled()) throw std::runtime_error("Cancelled");
            }
            if (failure_) std::rethrow_exception(failure_);
            pending_.push_back({std::move(pixels), index});
            pixels.swap(reusable_);
        }
        changed_.notify_all();
        pixels.resize(size);
    }
    void finish() {
        { const std::lock_guard<std::mutex> lock(mutex_); closing_ = true; }
        changed_.notify_all(); worker_.join();
        if (failure_) std::rethrow_exception(failure_);
    }
    std::int64_t written() const { return written_.load(); }
    fs::path mediaPath() const { return writer_.mediaPath(); }
    std::string format() const { return writer_.format(); }
};

#ifdef SILENTFILM_CLI_CUDA
void requireCuda(cudaError_t status) { if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status)); }
class CudaRenderer {
    struct DeviceFrame { unsigned char* data = nullptr; std::int64_t used = 0; };
    cudaStream_t stream_ = nullptr;
    unsigned char* output_ = nullptr;
    std::map<std::int64_t, DeviceFrame> frames_;
    std::array<unsigned char*, maximumCameraExposureSamples> flowSamples_{};
    int width_, height_;
    std::size_t bytes_, budget_;
public:
    CudaRenderer(int width, int height, int gpu, std::size_t budget)
        : width_(width), height_(height), bytes_(static_cast<std::size_t>(width) * height * sizeof(Pixel)), budget_(budget) {
        requireCuda(cudaSetDevice(gpu));
        requireCuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
        const auto status = cudaMallocAsync(reinterpret_cast<void**>(&output_), bytes_, stream_);
        if (status != cudaSuccess) { cudaStreamDestroy(stream_); stream_ = nullptr; requireCuda(status); }
    }
    ~CudaRenderer() {
        if (!stream_) return;
        for (auto& [index, frame] : frames_) cudaFreeAsync(frame.data, stream_);
        for (auto* data : flowSamples_) if (data) cudaFreeAsync(data, stream_);
        if (output_) cudaFreeAsync(output_, stream_);
        cudaStreamSynchronize(stream_); cudaStreamDestroy(stream_);
    }
    void render(VideoReader& reader, const std::vector<SampleIndices>& indices, const CadenceSettings& c,
        const CameraOpticsParameters& optics, float gain, std::int64_t capture, std::vector<Pixel>& pixels,
        FlowSampler* flow) {
        struct UploadLifetime {
            cudaStream_t stream;
            std::vector<std::shared_ptr<const SourceFrame>> frames;
            ~UploadLifetime() { cudaStreamSynchronize(stream); }
        } lifetime{stream_, {}};
        auto& uploads = lifetime.frames;
        uploads.reserve(2 * indices.size());
        const auto image = [&](std::int64_t index) {
            auto it = frames_.find(index);
            if (it == frames_.end()) {
                auto host = reader.get(index);
                unsigned char* data = nullptr;
                requireCuda(cudaMallocAsync(reinterpret_cast<void**>(&data), bytes_, stream_));
                try { it = frames_.emplace(index, DeviceFrame{data, capture}).first; }
                catch (...) { cudaFreeAsync(data, stream_); throw; }
                uploads.push_back(std::move(host));
                requireCuda(cudaMemcpyAsync(data, uploads.back()->pixels.data(), bytes_, cudaMemcpyHostToDevice, stream_));
            }
            it->second.used = capture;
            return ImageView{it->second.data, static_cast<std::ptrdiff_t>(width_) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), 0, 0, width_, height_};
        };
        std::array<CameraExposureSample, maximumCameraExposureSamples> samples{};
        std::array<CameraSampleTransform, maximumCameraExposureSamples> transforms{};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto& p = indices[i];
            if (flow && p.mix > 0) {
                auto first = reader.get(p.left);
                auto second = reader.get(p.right);
                auto prepared = flow->sample(std::move(first), std::move(second), p.mix);
                if (!flowSamples_[i]) requireCuda(cudaMallocAsync(
                    reinterpret_cast<void**>(&flowSamples_[i]), bytes_, stream_));
                uploads.push_back(std::move(prepared));
                requireCuda(cudaMemcpyAsync(flowSamples_[i], uploads.back()->pixels.data(), bytes_, cudaMemcpyHostToDevice, stream_));
                samples[i] = {{flowSamples_[i], static_cast<std::ptrdiff_t>(width_) * static_cast<std::ptrdiff_t>(sizeof(Pixel)),
                    0, 0, width_, height_}, p.phase, {}, 0, p.weight};
            } else {
                samples[i] = {image(p.left), p.phase, p.mix > 0 ? image(p.right) : ImageView{}, p.mix, p.weight};
            }
            transforms[i] = cameraSampleTransform(p.phase, c.shake, c.crank.seed, width_, height_);
        }
        const ImageView output{output_, static_cast<std::ptrdiff_t>(width_) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), 0, 0, width_, height_};
        const auto status = renderCudaCamera(samples.data(), transforms.data(), static_cast<int>(indices.size()), output,
            {0, 0, width_, height_}, optics, gain, c.shake > 0, stream_, true);
        if (status.code) throw std::runtime_error(status.message ? status.message : "CUDA camera render failed");
        requireCuda(cudaMemcpyAsync(pixels.data(), output_, bytes_, cudaMemcpyDeviceToHost, stream_));
        // Это отдельный процесс: encoder на CPU должен получить уже законченный кадр.
        requireCuda(cudaStreamSynchronize(stream_));
        while (frames_.size() > 1 && frames_.size() * bytes_ > budget_) {
            const auto oldest = std::min_element(frames_.begin(), frames_.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            requireCuda(cudaFreeAsync(oldest->second.data, stream_)); frames_.erase(oldest);
        }
    }
};
#endif

void render(Job& job, const Settings& s)
{
    VideoReader reader(s.input, s.cacheBytes, s.inputTransfer, [&] { return job.cancelled(); });
    const double encodedFps = reader.fps();
    const double fps = s.sourceFps > 0 ? s.sourceFps : encodedFps;
    std::unique_ptr<FlowSampler> flow;
    if (s.blurMode == "flow_integrated") flow = std::make_unique<FlowSampler>(
        reader.width(), reader.height(), s.cacheBytes / 2, [&] { return job.cancelled(); });
    job.status["blur_mode"] = s.blurMode;
    job.status["flow_backend"] = flow ? "opencv_cpu_farneback_warp" : "none";
    if (s.expectedSourceFps > 0 && std::abs(encodedFps - s.expectedSourceFps) > 0.002)
        throw std::runtime_error("Media Pool FPS differs from encoded source FPS; render a CFR intermediate first");
    job.status["source_fps"] = number(fps); job.status["output_fps"] = number(s.outputFps);
    job.status["encoded_source_fps"] = number(encodedFps);
    job.status["width"] = std::to_string(reader.width()); job.status["height"] = std::to_string(reader.height());
    job.status["backend"] = s.device;
    job.status["target_fps_min"] = number(s.camera.crank.minimumFps);
    job.status["target_fps_max"] = number(s.camera.crank.maximumFps);
    job.status["fps_drift_strength"] = number(s.camera.crank.rateVariation);
    job.status["crank_irregularity"] = number(s.camera.crank.irregularity);
    job.status["seed"] = std::to_string(s.camera.crank.seed);
    job.status["shutter_fraction"] = number(s.camera.shutterFraction);
    job.status["motion_blur_strength"] = number(s.camera.motionBlurStrength);
    job.status["blur_samples"] = std::to_string(s.camera.exposureSamples);
    job.status["exposure_variation"] = number(s.camera.exposureVariation);
    job.status["camera_shake_strength"] = number(s.camera.shake);
    job.update("decoding", "Чтение исходника; результат создаётся без нормировки длительности");
    reader.locate(s.start);
    if (reader.eof() && reader.decodedCount() <= s.start) throw std::runtime_error("In point lies beyond source end");
    #ifdef SILENTFILM_CLI_CUDA
    std::unique_ptr<CudaRenderer> gpu;
    if (s.device == "cuda") gpu = std::make_unique<CudaRenderer>(reader.width(), reader.height(), s.gpu, s.gpuCacheBytes);
    #else
    if (s.device == "cuda") throw std::runtime_error("This helper was built without CUDA; select CPU or rebuild with CUDA");
    #endif
    const auto aspect = reader.pixelAspectRatio();
    job.status["pixel_aspect_ratio"] = std::to_string(aspect.numerator) + ":" + std::to_string(aspect.denominator);
    AsyncWriter writer(job.directory, s.format, reader.width(), reader.height(), s.outputFps, s.outputName, aspect);
    std::ofstream map(job.directory / "capture_map.csv"); map.exceptions(std::ios::failbit | std::ios::badbit);
    map << std::setprecision(17) << "output_frame,capture_start_seconds,capture_end_seconds,capture_fps,source_center_frame,exposure_frames,exposure_gain\n";
    HandCrankSchedule schedule;
    std::vector<Pixel> pixels(static_cast<std::size_t>(reader.width()) * reader.height());
    auto length = s.length;
    std::int64_t written = 0;
    const auto updateLength = [&] {
        if (!reader.eof()) return;
        const auto available = reader.decodedCount() - s.start;
        if (available <= 0) throw std::runtime_error("Source range contains no frames");
        if (s.length > 0 && s.length > available) throw std::runtime_error("Marked Out point exceeds decoded source length");
        length = s.length > 0 ? s.length : available;
    };
    const auto ended = [&](const CaptureInterval& interval) {
        return length > 0 && interval.start >= static_cast<double>(length) / fps - 1e-9;
    };
    for (std::int64_t capture = 0; capture < 10000000; ++capture) {
        if (job.cancelled()) throw std::runtime_error("Cancelled");
        const auto interval = schedule.captureAtIndex(capture, s.camera.crank);
        if (ended(interval)) break;
        const auto plan = planCaptureExposure(interval, fps, s.camera);
        // Сначала выясняем EOF, не конвертируя все декодированные кадры в float.
        for (int i = 0; i < plan.count; ++i) {
            const double position = std::clamp(plan.positions[i], 0.0,
                length > 0 ? static_cast<double>(length - 1) : std::numeric_limits<double>::max());
            const auto left = static_cast<std::int64_t>(std::floor(position));
            reader.locate(s.start + left);
            if (position - left > 1e-6) reader.locate(s.start + left + (length > 0 && left + 1 >= length ? 0 : 1));
            updateLength();
        }
        updateLength();
        if (ended(interval)) break;
        std::vector<SampleIndices> indices;
        for (int i = 0; i < plan.count; ++i) {
            const double position = std::clamp(plan.positions[i], 0.0,
                length > 0 ? static_cast<double>(length - 1) : std::numeric_limits<double>::max());
            const auto left = static_cast<std::int64_t>(std::floor(position));
            const auto right = length > 0 ? std::min(left + 1, length - 1) : left + 1;
            const float mix = right != left && position - left > 1e-6 ? static_cast<float>(position - left) : 0;
            const float phase = static_cast<float>(schedule.phaseAt((position + 0.5) / fps, s.camera.crank));
            indices.push_back({s.start + left, s.start + right, mix, phase, plan.weights[i]});
        }
        auto optics = s.camera.optics;
        optics.captureOnly = s.splitCamera;
        if (!s.splitCamera) optics.focusSignal = cameraFocusSignal(capture, s.camera.focusSeed);
        #ifdef SILENTFILM_CLI_CUDA
        if (gpu) gpu->render(reader, indices, s.camera, optics, plan.gain, capture, pixels, flow.get());
        else
        #endif
        {
            std::vector<std::shared_ptr<const SourceFrame>> frames;
            frames.reserve(2 * indices.size());
            std::array<CameraExposureSample, maximumCameraExposureSamples> samples{};
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const auto& p = indices[i];
                if (flow && p.mix > 0) {
                    auto first = reader.get(p.left);
                    auto second = reader.get(p.right);
                    frames.push_back(flow->sample(std::move(first), std::move(second), p.mix));
                    samples[i] = {view(frames.back()->pixels, reader.width(), reader.height()), p.phase, {}, 0, p.weight};
                    continue;
                }
                frames.push_back(reader.get(p.left));
                const auto left = view(frames.back()->pixels, reader.width(), reader.height());
                ImageView right{};
                if (p.mix > 0) { frames.push_back(reader.get(p.right)); right = view(frames.back()->pixels, reader.width(), reader.height()); }
                samples[i] = {left, p.phase, right, p.mix, p.weight};
            }
            if (!renderCameraCaptureCpu(samples.data(), static_cast<int>(indices.size()), view(pixels, reader.width(), reader.height()),
                {0, 0, reader.width(), reader.height()}, optics, plan.gain, s.camera.shake, s.camera.crank.seed,
                [](void* p) { return static_cast<Job*>(p)->cancelled(); }, &job)) throw std::runtime_error("Camera render cancelled or failed");
        }
        if (job.cancelled()) throw std::runtime_error("Cancelled");
        writer.write(pixels, written, job);
        map << written << ',' << interval.start << ',' << interval.end << ',' << interval.fps << ','
            << plan.sourceCenter + s.start << ',' << plan.exposureFrames << ',' << plan.gain << '\n';
        ++written;
        job.status["frames_written"] = std::to_string(writer.written());
        job.status["frames_submitted"] = std::to_string(written);
        const double durationHint = static_cast<double>(length > 0 ? length : std::max<std::int64_t>(0, reader.frameCountHint() - s.start)) / fps;
        job.status["source_duration_hint"] = number(durationHint);
        job.status["source_position"] = number(interval.end);
        job.update("rendering", "Пересчёт экспозиций и запись нового клипа");
        if (capture == 9999999) throw std::runtime_error("Capture count exceeds prototype limit");
    }
    if (s.length > 0 && reader.locate(s.start + s.length - 1) != s.start + s.length - 1)
        throw std::runtime_error("Marked source range is longer than the decoded input");
    updateLength();
    if (length <= 0 || written <= 0) throw std::runtime_error("Cannot determine completed source range");
    if (physicalCaptureCount(schedule, static_cast<double>(length) / fps, s.camera.crank) != written)
        throw std::runtime_error("Physical capture count disagrees with exported frame count");
    if (job.cancelled()) throw std::runtime_error("Cancelled");
    job.update("finishing", "Завершение файла");
    writer.finish(); map.close();
    if (job.cancelled()) throw std::runtime_error("Cancelled");
    if (writer.written() != written) throw std::runtime_error("Encoder did not finish all physical captures");
    job.status["frames_written"] = std::to_string(written);
    Values result = job.status;
    result["state"] = "complete"; result["input"] = s.input; result["format"] = writer.format();
    result["media_path"] = fs::absolute(writer.mediaPath()).string();
    result["frame_count"] = std::to_string(written); result["first_index"] = "0";
    result["last_index"] = std::to_string(written - 1); result["source_start_frame"] = std::to_string(s.start);
    result["source_frame_count"] = std::to_string(length); result["source_duration"] = number(static_cast<double>(length) / fps);
    result["output_duration"] = number(static_cast<double>(written) / s.outputFps);
    result["output_transfer"] = "srgb"; result["audio"] = "none";
    result["processing"] = s.splitCamera ? "capture_rgb" : "full_camera";
    result["clip_name"] = s.outputName.empty() ? "cadence" : s.outputName;
    result["message"] = "Готово: один выходной кадр на физическую экспозицию";
    atomicValues(job.directory / "result.ini", result);
    job.status = result; job.update("complete", result["message"]);
    std::cout << "Cadence complete: " << written << " frames, " << written / s.outputFps << " seconds at " << s.outputFps << " fps\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, signalHandler); std::signal(SIGTERM, signalHandler);
    if (argc == 2 && std::string(argv[1]) == "--capabilities") {
        std::cout << "{\"camera_parameter_schema\":2,\"max_blur_samples\":" << maximumCameraExposureSamples;
#ifdef SILENTFILM_CADENCE_OPENCV
        std::cout << ",\"optical_flow\":true,\"flow_backend\":\"opencv_cpu_farneback_warp\"}\n";
#else
        std::cout << ",\"optical_flow\":false,\"flow_backend\":\"none\"}\n";
#endif
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--inspect") {
        try {
            VideoReader reader(argv[2], 32ULL * 1024 * 1024, Transfer::SRGB, [] { return false; });
            reader.locate(0);
            const auto aspect = reader.pixelAspectRatio();
            std::cout << std::setprecision(17) << "{\"width\":" << reader.width() << ",\"height\":" << reader.height()
                << ",\"fps\":" << reader.fps() << ",\"has_alpha\":" << (reader.hasAlphaChannel() ? "true" : "false")
                << ",\"pixel_aspect_num\":" << aspect.numerator << ",\"pixel_aspect_den\":" << aspect.denominator << "}\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Inspect error: " << error.what() << '\n'; return 1;
        }
    }
    if (argc != 3 || std::string(argv[1]) != "--job") {
        std::cerr << "Usage: silentfilm_cadence --job /absolute/job/request.ini\n";
        return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 2;
    }
    const auto request = fs::absolute(argv[2]);
    Job job(request.parent_path());
    const auto lock = job.directory / "running.lock";
    bool ownedLock = false;
    try {
        if (fs::exists(job.directory / "result.ini")) throw std::runtime_error("Job is already complete; use a new output directory");
        if (!fs::create_directory(lock)) throw std::runtime_error("Job is already running; use a new job directory");
        ownedLock = true;
        job.update("starting", "Запуск нативного пересчёта");
        const auto settings = parse(readSettings(request));
        render(job, settings);
        fs::remove(lock); return 0;
    } catch (const std::exception& e) {
        std::cerr << "Cadence error: " << e.what() << '\n';
        if (ownedLock) {
            try { job.update(job.cancelled() ? "cancelled" : "error", e.what()); } catch (...) {}
            std::error_code ignored; fs::remove(lock, ignored);
        }
        return job.cancelled() ? 130 : 1;
    }
}
