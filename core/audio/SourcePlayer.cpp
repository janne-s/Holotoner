#include "core/audio/SourcePlayer.h"

#include <algorithm>
#include <cmath>

namespace holotoner {

void SourcePlayer::prepare(double sampleRate)
{
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    restart();
}

void SourcePlayer::restart()
{
    counter_ = 0;
    filePosition_ = 0.0;
    file_ = nullptr;
}

void SourcePlayer::render(SourceKind kind, const SourceAudio* file, float* left, float* right, int samples)
{
    if (left == nullptr || right == nullptr || samples <= 0)
        return;

    switch (kind) {
    case SourceKind::Input:
        return;
    case SourceKind::Clicks: {
        const auto interval = std::max<std::int64_t>(
            1, static_cast<std::int64_t>(kClickIntervalSeconds * sampleRate_));
        for (int i = 0; i < samples; ++i, ++counter_) {
            const float value = counter_ % interval == 0 ? 1.0f : 0.0f;
            left[i] = value;
            right[i] = value;
        }
        return;
    }
    case SourceKind::File: {
        if (file == nullptr || file->left.size() < 2 || file->right.size() != file->left.size()
            || !std::isfinite(file->sampleRate) || file->sampleRate <= 0.0) {
            std::fill(left, left + samples, 0.0f);
            std::fill(right, right + samples, 0.0f);
            return;
        }
        if (file != file_) {
            file_ = file;
            filePosition_ = 0.0;
        }

        const double step = file->sampleRate / sampleRate_;
        const std::size_t size = file->left.size();
        const double length = static_cast<double>(size);
        const double fade = std::min(kLoopFadeSeconds * file->sampleRate, 0.25 * length);
        const auto at = [size](const std::vector<float>& channel, double position) {
            const auto first = static_cast<std::size_t>(position);
            const auto fraction = static_cast<float>(position - static_cast<double>(first));
            return channel[first] + fraction * (channel[(first + 1) % size] - channel[first]);
        };

        for (int i = 0; i < samples; ++i) {
            if (filePosition_ >= length)
                filePosition_ = fade + std::fmod(filePosition_ - length, length - fade);
            float valueLeft = at(file->left, filePosition_);
            float valueRight = at(file->right, filePosition_);
            if (fade >= 1.0 && filePosition_ >= length - fade) {
                const double intoFade = filePosition_ - (length - fade);
                const auto mix = static_cast<float>(intoFade / fade);
                valueLeft += mix * (at(file->left, intoFade) - valueLeft);
                valueRight += mix * (at(file->right, intoFade) - valueRight);
            }
            left[i] = valueLeft;
            right[i] = valueRight;
            filePosition_ += step;
        }
        return;
    }
    }
}

} // namespace holotoner
