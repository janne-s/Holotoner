#include "core/dsp/ImageSpatializer.h"
#include "core/geometry/Motion.h"
#include "core/image/ShapeGenerator.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <emscripten/emscripten.h>
using namespace holotoner;
namespace {
ImageSpatializer engine;
constexpr int capacity = 2048, history = 8192;
float left[capacity]{}, right[capacity]{}, raw[history * 4]{},
    scope[4096 * 4]{}, metrics[4]{}, params[48]{}, corners[12]{},
    response[128 * 3]{}, visibleState[7]{};
unsigned char rgba[512 * 512 * 4]{};
ImageMask banks[2][32], compiled;
int bank = 0, frames = 0, frame = 0, at = 0;
double rate = 48000;
std::uint64_t epoch = 0;
CycleClock axes[7], textClock;
PlaneTransform visible;
PlaneTransform base() {
  return {params[0], params[1], params[2], params[3],
          params[4], params[5], params[6]};
}
} // namespace
extern "C" {
EMSCRIPTEN_KEEPALIVE void ht_init(double sr) {
  rate = sr;
  engine.prepare(sr);
}
EMSCRIPTEN_KEEPALIVE void *ht_buffer(int id) {
  switch (id) {
  case 0:
    return left;
  case 1:
    return right;
  case 2:
    return scope;
  case 3:
    return metrics;
  case 4:
    return params;
  case 5:
    return banks;
  case 6:
    return rgba;
  case 7:
    return &compiled;
  case 8:
    return corners;
  case 9:
    return response;
  case 10:
    return visibleState;
  default:
    return nullptr;
  }
}
EMSCRIPTEN_KEEPALIVE int ht_mask_size() { return sizeof(ImageMask); }
EMSCRIPTEN_KEEPALIVE void ht_commit(int which, int count) {
  bank = std::clamp(which, 0, 1);
  frames = std::clamp(count, 0, 32);
}
EMSCRIPTEN_KEEPALIVE void ht_reset() {
  engine.reset();
  epoch = 0;
  at = 0;
  std::fill_n(raw, history * 4, 0);
  for (auto &c : axes)
    c.reset();
  textClock.reset();
  visible = base();
}
EMSCRIPTEN_KEEPALIVE void ht_compile(int width, int height) {
  RgbaImage im;
  im.width = width;
  im.height = height;
  if (width < 1 || height < 1 || width > 512 || height > 512)
    return;
  im.pixels.assign(rgba, rgba + width * height * 4);
  ImageMaskSettings s;
  s.channel =
      static_cast<PixelChannel>(std::clamp(static_cast<int>(params[17]), 0, 5));
  s.gain = params[18];
  s.fill = params[19];
  s.normalize = params[20] > 0;
  s.invert = params[21] > 0;
  s.threshold = params[22];
  s.binary = params[23] > 0;
  compiled = buildImageMask(im, s);
}
EMSCRIPTEN_KEEPALIVE void ht_shape(int kind, int order) {
  auto im =
      generateShape(static_cast<ShapeKind>(std::clamp(
                        kind, 0, static_cast<int>(kShapeNames.size()) - 1)),
                    order);
  std::memcpy(rgba, im.pixels.data(), im.pixels.size());
}
EMSCRIPTEN_KEEPALIVE void ht_process(int count, int playing) {
  if (count < 1 || count > capacity)
    return;
  for (int i = 0; i < count; ++i) {
    int j = (at + i) % history;
    raw[j * 4] = left[i];
    raw[j * 4 + 1] = right[i];
  }
  ImageSpatializer::Parameters p;
  p.phaseProjection =
      params[7] > 0 ? PhaseProjection::Polar : PhaseProjection::Cartesian;
  p.imageAmount = params[8];
  p.edge = params[9];
  p.smooth = params[47];
  p.rest = params[10];
  p.wet = params[11];
  p.delta = params[12] > 0;
  p.interfere = params[13];
  p.echo = params[14];
  p.feedback = params[15];
  p.resonate = params[16];
  double bpm = std::clamp(static_cast<double>(params[24]), 20.0, 300.0);
  p.echoTime = params[25];
  int note = std::clamp(static_cast<int>(params[26]), 0, 23);
  if (note)
    p.echoTime = static_cast<float>(kSyncBeats[note] * 60 / bpm);
  for (int offset = 0; offset < count;) {
    int n = std::min(count - offset, 32 - static_cast<int>(epoch % 32));
    double partial = playing ? static_cast<double>(epoch % 32) : 0;
    double beat = textClock.beats - partial * bpm / (60 * rate);
    p.plane = base();
    for (int a = 0; a < 7; ++a) {
      double hz = params[29 + a * 2];
      int div = std::clamp(static_cast<int>(params[30 + a * 2]), 0, 23);
      if (hz > 0 || div > 0)
        p.plane = motionPlane(p.plane, {a + 1, params[27] > 0, params[28]},
                              axes[a].cycles(div, beat) -
                                  (div ? 0 : partial * hz / rate));
      axes[a].advance(n, rate, hz, bpm, playing);
    }
    frame = std::clamp(static_cast<int>(params[43]) - 1, 0,
                       std::max(0, frames - 1));
    int div = std::clamp(static_cast<int>(params[46]), 0, 23);
    if (params[44] > 0 && frames > 0) {
      double c =
          textClock.cycles(div, beat) - (div ? 0 : partial * params[45] / rate);
      if (std::abs(c - std::round(c)) < 1e-9)
        c = std::round(c);
      frame = static_cast<int>(std::floor(c)) % frames;
      if (frame < 0)
        frame += frames;
    }
    engine.setParameters(p);
    engine.process(left + offset, right + offset, n,
                   frames ? &banks[bank][frame] : nullptr);
    visible = engine.currentPlane();
    textClock.advance(n, rate, params[45], bpm, playing);
    epoch += n;
    offset += n;
  }
  for (int i = 0; i < count; ++i) {
    int j = (at + i) % history;
    raw[j * 4 + 2] = left[i];
    raw[j * 4 + 3] = right[i];
  }
  at = (at + count) % history;
}
EMSCRIPTEN_KEEPALIVE void ht_snapshot() {
  double ll = 0, rr = 0, lr = 0;
  float peak = 0;
  for (int i = 0; i < 4096; ++i) {
    int j = (at + history - 4096 + i) % history;
    auto in =
        stereoToScopePoint(raw[j * 4] * 5.011872f, raw[j * 4 + 1] * 5.011872f);
    auto out = stereoToScopePoint(raw[j * 4 + 2] * 5.011872f,
                                  raw[j * 4 + 3] * 5.011872f);
    scope[i * 4] = in.x;
    scope[i * 4 + 1] = in.y;
    scope[i * 4 + 2] = out.x;
    scope[i * 4 + 3] = out.y;
    double l = raw[j * 4 + 2], r = raw[j * 4 + 3];
    ll += l * l;
    rr += r * r;
    lr += l * r;
    peak =
        std::max(peak, static_cast<float>(std::max(std::abs(l), std::abs(r))));
  }
  metrics[0] =
      ll * rr > 1e-20 ? static_cast<float>(lr / std::sqrt(ll * rr)) : 0;
  metrics[1] = peak;
  metrics[2] = static_cast<float>(frame);
  metrics[3] = static_cast<float>(engine.takeSafetyResets());
}
EMSCRIPTEN_KEEPALIVE void ht_visual(int live) {
  auto transform = live ? visible : base();
  const float values[7]{transform.pitchDegrees, transform.yawDegrees,
                        transform.rollDegrees,  transform.positionX,
                        transform.positionY,    transform.scaleX,
                        transform.scaleY};
  std::copy_n(values, 7, visibleState);
  const auto selected = live ? frame
                             : std::clamp(static_cast<int>(params[43]) - 1, 0,
                                          std::max(0, frames - 1));
  const ImageMask &mask = frames ? banks[bank][selected] : compiled;
  transform.scaleX *= mask.aspectX;
  transform.scaleY *= mask.aspectY;
  auto plane = projectPlane(transform);
  for (int i = 0; i < 4; ++i) {
    corners[i * 3] = plane.corners[i].stereo;
    corners[i * 3 + 1] = plane.corners[i].frequency;
    corners[i * 3 + 2] = plane.corners[i].depth;
  }
  SpaceAmounts a{params[13], params[15], params[14], params[16], params[25]};
  int div = std::clamp(static_cast<int>(params[26]), 0, 23);
  if (div)
    a.echoTime = kSyncBeats[div] * 60 / std::max(20.0f, params[24]);
  auto weights = spaceRowWeights(mask);
  auto rings = resonators(plane, weights);
  for (int i = 0; i < 128; ++i) {
    double hz = 60 * std::pow(16000.0 / 60, i / 127.0);
    auto ear = interfereResponse(plane, a, hz);
    double level = std::abs(ear.left) *
                   (1 + kResonanceBoost * a.resonate * resonance(rings, hz));
    response[i * 3] =
        static_cast<float>(20 * std::log10(std::max(1e-6, level)));
    response[i * 3 + 1] =
        static_cast<float>(echoSeconds(plane, hz, a.echoTime));
    response[i * 3 + 2] = static_cast<float>(spaceRowWeight(weights, hz));
  }
}
}
