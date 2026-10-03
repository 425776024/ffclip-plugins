#include "TextEffectEllipticLayout.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace videocut::text::internal {
namespace {

constexpr double kPi = 3.1415926;

double Remap(double value, double begin, double end) {
  return std::clamp((value - begin) / (end - begin), 0.0, 1.0);
}

std::array<double, 2> EllipsePoint(double angle, double a, double b) {
  const double radians = angle * kPi / 180.0;
  return {a * std::sin(radians), b * std::cos(radians)};
}

double ChordSquared(double first, double last, double a, double b) {
  const auto p = EllipsePoint(first, a, b);
  const auto q = EllipsePoint(last, a, b);
  const double x = p[0] - q[0], y = p[1] - q[1];
  return x * x + y * y;
}

double NextAngle(double start, double end, double distance, double a, double b) {
  const double target = distance * distance;
  if (std::abs(ChordSquared(start, end, a, b) - target) <= 4.0)
    return end;
  double low = start, high = end, angle = end;
  // Signed brackets preserve the two traversal directions. The source stops
  // at a squared-distance error of 4 or a bracket width of one degree.
  for (unsigned iteration = 0; iteration < 32U; ++iteration) {
    angle = low + (high - low) * 0.5;
    const double width = high - low;
    const double error = ChordSquared(start, angle, a, b) - target;
    if (error > 0.0) high = angle;
    else low = angle;
    if (std::abs(error) <= 4.0 || std::abs(width) <= 1.0)
      break;
  }
  return angle;
}

double MiddleAngle(double distance, double a, double b) {
  const double initial = std::atan(distance / 2.0 / b) / 3.14 * 180.0;
  double previous = -100.0;
  for (double angle = initial; angle >= 1.0; angle -= 1.0) {
    const double error = ChordSquared(360.0 - angle, 360.0 + angle, a, b) -
                         distance * distance;
    if (std::abs(error) < 4.0 || (previous > 0.0 && error < 0.0))
      return angle;
    previous = error;
  }
  return initial;
}

bool BuildLayout(const TextEffectFrameInput &frame,
                 const std::array<double, 4> &parameters,
                 std::vector<std::array<double, 4>> &output) {
  const double fontSize = parameters[0], wordGap = parameters[1];
  const double tracking = wordGap * (wordGap < 0.0 ? 0.5 : 0.2);
  const double lineGap = parameters[2] * (wordGap < 0.0 ? 35.0 : 20.0);
  const bool vertical = parameters[3] == 1.0;
  if (!(fontSize > 0.0) || (parameters[3] != 0.0 && !vertical))
    return false;
  std::map<std::size_t, std::vector<std::size_t>> rows;
  double maximumSize = 0.0;
  output.reserve(frame.units.size());
  for (std::size_t i = 0; i < frame.units.size(); ++i) {
    const auto &unit = frame.units[i];
    output.push_back({unit.initialPositionX, unit.initialPositionY, 0.0, 0.0});
    if (unit.unicodeCodepoint == '\n') continue;
    rows[unit.row].push_back(i);
    if (unit.unicodeCodepoint != ' ')
      maximumSize = std::max({maximumSize, double(unit.rect.width),
                             double(unit.rect.height)});
  }
  if (rows.empty()) return true;
  const double radius = 170.0 + 400.0 * Remap(rows.begin()->second.size(), 10.0, 60.0)
                        + 500.0 * Remap(fontSize, 2.0, 100.0);
  double previousCapacity = 10.0, previousPerimeter = 0.0, previousScale = 4.0;
  std::map<std::size_t, double> rowScales;
  for (const auto &[row, indexes] : rows) {
    double a = radius + (maximumSize * 1.5 + lineGap + tracking) * row;
    double b = a * (0.7 + 0.14 * Remap(row + 1.0, 1.0, 10.0));
    const double perimeter = 2.0 * kPi * b + 4.0 * (a - b);
    if (!(a > 0.0) || !(b > 0.0) || !std::isfinite(perimeter)) return false;
    const double minimum = previousPerimeter == 0.0 ? 10.0 :
        std::floor(previousCapacity * perimeter / previousPerimeter);
    const double count = indexes.size(), capacity = std::max(count, minimum);
    double scale = count <= minimum
        ? std::max(1.35 - row * 0.05 - tracking, 0.0)
        : std::max(1.35 / std::pow(0.02 * (count - minimum) + 1.0, 2.0) - tracking, 0.0);
    if (scale > previousScale) scale = previousScale * 1.05;
    const double spaces = std::count_if(indexes.begin(), indexes.end(),
        [&](std::size_t i) { return frame.units[i].unicodeCodepoint == ' '; });
    const double correction = std::fmod(capacity, 2.0) == 0.0
        ? 0.95 + 0.02 * ((capacity - 10.0) / 32.0)
        : 0.95 + 0.02 * ((capacity - 11.0) / 33.0);
    const double denominator = capacity - spaces * (0.35 + 0.25 * ((capacity - 10.0) / 50.0));
    if (!(denominator > 0.0)) return false;
    const double distance = correction * perimeter / capacity * capacity / denominator;
    const double step = 360.0 / capacity;
    previousCapacity = capacity;
    previousPerimeter = perimeter;
    previousScale = scale;
    rowScales[row] = scale;
    if (vertical) std::swap(a, b);
    const std::size_t middle = (indexes.size() - 1U) / 2U;
    const bool even = indexes.size() % 2U == 0U;
    const bool middleSpace = frame.units[indexes[middle]].unicodeCodepoint == ' ';
    double middleAngle = even || middleSpace ? MiddleAngle(distance, a, b) : 0.0;
    if (!even && middleSpace) middleAngle *= -0.5;
    const double start = vertical ? 90.0 : 360.0;
    // Both halves use the same bounded chord solver and emission path.
    for (unsigned side = 0; side < 2U; ++side) {
      double angle = start + (side == 0U ? -middleAngle : middleAngle);
      const std::size_t length = side == 0U ? middle + 1U : indexes.size() - middle - 1U;
      for (std::size_t n = 0; n < length; ++n) {
        const auto index = indexes[side == 0U ? middle - n : middle + 1U + n];
        const auto codepoint = frame.units[index].unicodeCodepoint;
        const bool space = codepoint == ' ';
        if (space && n == 0U) angle += (side == 0U ? 0.5 : -0.5) * middleAngle;
        if (n > 0U || (side == 1U && !even))
          angle = NextAngle(angle, angle + (side == 0U ? -2.0 : 2.0) * step,
                            distance * (space ? 0.5 : 1.0), a, b);
        angle = side == 0U ? std::max(angle, vertical ? -90.0 : 180.0)
                           : std::min(angle, vertical ? 270.0 : 540.0);
        const auto position = EllipsePoint(angle, a, b);
        const bool upright = codepoint >= 0x4000U && codepoint <= 0xFFFFU;
        const double rotation = upright ? 0.0 : side == 0U
            ? std::abs(angle - 360.0) : std::abs(angle - 540.0) + 180.0;
        output[index] = {position[0], position[1], rotation, scale};
      }
    }
  }
  for (std::size_t i = 0; i < frame.units.size(); ++i)
    if (frame.units[i].unicodeCodepoint == '\n')
      if (const auto found = rowScales.find(frame.units[i].row); found != rowScales.end())
        output[i][3] = found->second;
  return std::all_of(output.begin(), output.end(), [](const auto &unit) {
    return std::all_of(unit.begin(), unit.end(), [](double v) { return std::isfinite(v); });
  });
}

} // namespace

bool EvaluateEllipticRowLayout(
    const TextEffectFrameInput &frame, const std::array<double, 4> &parameters,
    std::size_t unitIndex, std::size_t component,
    EllipticRowLayoutCache &cache, double &value, std::string &error) {
  if (component > 3U || unitIndex >= frame.units.size() ||
      !std::all_of(parameters.begin(), parameters.end(),
                   [](double v) { return std::isfinite(v); })) {
    error = "text elliptic row layout has invalid parameters";
    return false;
  }
  if (cache.parameters && *cache.parameters != parameters) {
    error = "text elliptic row layout parameters vary within a stage";
    return false;
  }
  if (!cache.parameters) {
    if (!BuildLayout(frame, parameters, cache.units)) {
      error = "text elliptic row layout has invalid geometry";
      return false;
    }
    cache.parameters = parameters;
  }
  value = cache.units[unitIndex][component];
  return true;
}

} // namespace videocut::text::internal
