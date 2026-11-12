#include "silentfilm/crank_exposure.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace silentfilm {

float CrankExposureMap::at(double frame, int offset) const
{
    if (!std::isfinite(frame)) throw std::runtime_error("Crank Exposure: неверное время кадра OFX");
    const double index = std::floor(frame + 0.5) + offset;
    if (index < 0 || index >= static_cast<double>(gains.size()))
        throw std::runtime_error("Crank Exposure: кадр вне карты (index=" + std::to_string(index)
            + ", count=" + std::to_string(gains.size()) + "); проверь Exposure Map и Frame Offset");
    return gains[static_cast<std::size_t>(index)];
}

void writeCrankExposureMap(const std::string& path, double fps, const std::vector<float>& gains, bool baked)
{
    if (!std::isfinite(fps) || fps < 1 || fps > 120 || gains.empty() || gains.size() > 10000000)
        throw std::runtime_error("Invalid Crank Exposure map timing");
    const auto temporary = path + ".tmp";
    std::ofstream out(temporary);
    out.imbue(std::locale::classic());
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << std::setprecision(17) << "SILENTFILM_CRANK_EXPOSURE 1\n" << (baked ? "bake" : "ofx")
        << ' ' << fps << ' ' << gains.size() << '\n';
    for (std::size_t i = 0; i < gains.size(); ++i) {
        if (!std::isfinite(gains[i]) || gains[i] <= 0 || gains[i] > 1000)
            throw std::runtime_error("Invalid Crank Exposure gain");
        out << i << ' ' << gains[i] << '\n';
    }
    out.close();
    std::filesystem::rename(temporary, path);
}

CrankExposureMap readCrankExposureMap(const std::string& path)
{
    if (path.empty()) throw std::runtime_error("Crank Exposure: выбери файл Exposure Map из результата Cadence");
    std::ifstream in(path);
    in.imbue(std::locale::classic());
    if (!in) throw std::runtime_error("Crank Exposure: файл Exposure Map отсутствует или недоступен");
    std::string signature, mode;
    int version = 0;
    std::int64_t count = 0;
    CrankExposureMap map;
    if (!(in >> signature >> version >> mode >> map.frameRate >> count) || signature != "SILENTFILM_CRANK_EXPOSURE"
        || version != 1 || (mode != "bake" && mode != "ofx") || !std::isfinite(map.frameRate)
        || map.frameRate < 1 || map.frameRate > 120 || count < 1 || count > 10000000)
        throw std::runtime_error("Crank Exposure: неверный формат карты");
    map.baked = mode == "bake";
    if (map.baked) throw std::runtime_error("Crank Exposure: экспозиция уже запечена; выбери результат Exposure via OFX");
    map.gains.resize(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        std::int64_t index = -1;
        float gain = 0;
        if (!(in >> index >> gain) || index != i || !std::isfinite(gain) || gain <= 0 || gain > 1000)
            throw std::runtime_error("Crank Exposure: повреждена таблица кадров/экспозиции");
        map.gains[static_cast<std::size_t>(i)] = gain;
    }
    std::string extra;
    if (in >> extra) throw std::runtime_error("Crank Exposure: лишние данные после таблицы кадров");
    return map;
}

std::shared_ptr<const CrankExposureMap> CrankExposureMapCache::load(const std::string& path)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!map_ || path_ != path) {
        auto value = std::make_shared<const CrankExposureMap>(readCrankExposureMap(path));
        path_ = path; map_ = std::move(value);
    }
    return map_;
}

void CrankExposureMapCache::clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    map_.reset(); path_.clear();
}

bool renderCrankExposureCpu(ImageView source, ImageView output, RenderWindow window, float gain, float blend,
    bool sourcePremultiplied, bool outputPremultiplied, AbortCallback abort, void* context)
{
    for (int y = std::max(window.y1, output.y1); y < std::min(window.y2, output.y2); ++y) {
        if (abort && abort(context)) return false;
        for (int x = std::max(window.x1, output.x1); x < std::min(window.x2, output.x2); ++x) {
            Pixel pixel{};
            if (x >= source.x1 && x < source.x2 && y >= source.y1 && y < source.y2) {
                std::memcpy(&pixel, source.data + (static_cast<std::ptrdiff_t>(y) - source.y1) * source.rowBytes
                    + (static_cast<std::ptrdiff_t>(x) - source.x1) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), sizeof(Pixel));
                pixel = crankExposurePixel(pixel, gain, blend, sourcePremultiplied, outputPremultiplied);
            }
            std::memcpy(output.data + (static_cast<std::ptrdiff_t>(y) - output.y1) * output.rowBytes
                + (static_cast<std::ptrdiff_t>(x) - output.x1) * static_cast<std::ptrdiff_t>(sizeof(Pixel)), &pixel, sizeof(Pixel));
        }
    }
    return true;
}
} // namespace silentfilm
