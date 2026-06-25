/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "sample_source.h"

#include <array>

namespace vital {

  class GranularSource : public Processor {
    public:
      static constexpr int kMaxGrains = 64;
      static constexpr mono_float kMaxTranspose = 96.0f;
      static constexpr mono_float kMinTranspose = -96.0f;
      static constexpr mono_float kMaxAmplitude = 1.41421356237f;

      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kKeytrack,
        kLevel,
        kTranspose,
        kTransposeQuantize,
        kTransposeQuantizeKey,
        kTransposeQuantizeScale,
        kTransposeQuantizeMode,
        kTune,
        kStart,
        kEnd,
        kMode,
        kMidiDensity,
        kGrainCount,
        kDensity,
        kGrainSize,
        kSpeed,
        kPosition,
        kPositionMod,
        kPositionModRate,
        kRandomPosition,
        kRandomVolume,
        kRandomPan,
        kRandomPitch,
        kInterval,
        kIntervalChance,
        kDirection,
        kLowCutoff,
        kHighCutoff,
        kPan,
        kNoteCount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      GranularSource();

      void process(int num_samples) override;
      Processor* clone() const override { return new GranularSource(*this); }
      Sample* getSample() { return sample_.get(); }
      force_inline Output* getPhaseOutput() const { return phase_output_.get(); }

    private:
      struct Grain {
        bool active = false;
        double source_position = 0.0;
        double source_increment = 1.0;
        int age = 0;
        int length = 1;
        mono_float gain = 1.0f;
      };

      struct LaneState {
        double playhead = 0.0;
        double samples_until_next_grain = 0.0;
        double modulation_phase = 0.0;
        std::array<Grain, kMaxGrains> grains;

        void reset();
      };

      static poly_float onePoleCoefficient(poly_float midi_cutoff, int sample_rate);
      static force_inline poly_float processLowPass(poly_float input, poly_float& state, poly_float coefficient) {
        state += (input - state) * coefficient;
        return state;
      }

      static mono_float wrapPosition(mono_float value, mono_float length);
      mono_float readSampleAt(int lane, double position, double increment) const;
      void resetLane(int lane, mono_float start);
      void spawnGrain(int lane, mono_float start, mono_float end, mono_float pitch_ratio);
      poly_float snapTranspose(poly_float input_midi, poly_float transpose, int quantize);

      poly_float pan_amplitude_;
      int transpose_quantize_;
      poly_float last_quantized_transpose_;
      poly_float low_cut_state_;
      poly_float high_cut_state_;
      poly_float phase_;
      std::array<LaneState, poly_float::kSize> lanes_;
      std::shared_ptr<cr::Output> phase_output_;
      utils::RandomGenerator random_generator_;
      std::shared_ptr<Sample> sample_;

      JUCE_LEAK_DETECTOR(GranularSource)
  };
} // namespace vital
