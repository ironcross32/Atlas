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

#include "synth_module.h"
#include "oscillator_module.h"
#include "sample_module.h"
#include "granular_module.h"

namespace vital {
  class ProducersModule : public SynthModule {
    public:
      enum {
        kReset,
        kRetrigger,
        kVoiceEvent,
        kMidi,
        kZoneMidi,
        kVelocity,
        kActiveVoices,
        kNoteCount,
        kNumInputs
      };

      enum {
        kToFilter1,
        kToFilter2,
        kRawOut,
        kBus1Out,
        kBus2Out,
        kBus3Out,
        kDirectOut,
        kNumOutputs
      };

      static force_inline int getFirstModulationIndex(int index) {
        return index == 0 ? 1 : 0;
      }

      static force_inline int getSecondModulationIndex(int index) {
        return index == 1 ? 2 : (getFirstModulationIndex(index) + 1);
      }

      ProducersModule();
      virtual ~ProducersModule() { }

      void process(int num_samples) override;
      void init() override;
      virtual Processor* clone() const override { return new ProducersModule(*this); }

      Wavetable* getWavetable(int index) {
        return oscillators_[index]->getWavetable();
      }

      Sample* getSample() { return sampler_->getSample(); }
      Sample* getGranularSample() { return granular_->getSample(); }
      Output* samplePhaseOutput() { return sampler_->getPhaseOutput(); }
      Output* granularPhaseOutput() { return granular_->getPhaseOutput(); }
      void setFilter1On(const Value* on) { filter1_on_ = on; }
      void setFilter2On(const Value* on) { filter2_on_ = on; }

    protected:
      bool isFilter1On() { return filter1_on_ == nullptr || filter1_on_->value() != 0.0f; }
      bool isFilter2On() { return filter2_on_ == nullptr || filter2_on_->value() != 0.0f; }
      poly_float getZoneMask(Value* key_start, Value* key_end, Value* velocity_start, Value* velocity_end);
      void addZonedBuffer(poly_float* dest, const poly_float* source, poly_float zone_mask, int num_samples) const;
      OscillatorModule* oscillators_[kNumOscillators];
      Value* oscillator_destinations_[kNumOscillators];
      Value* oscillator_key_zone_start_[kNumOscillators];
      Value* oscillator_key_zone_end_[kNumOscillators];
      Value* oscillator_velocity_zone_start_[kNumOscillators];
      Value* oscillator_velocity_zone_end_[kNumOscillators];
      Value* sample_destination_;
      Value* sample_key_zone_start_;
      Value* sample_key_zone_end_;
      Value* sample_velocity_zone_start_;
      Value* sample_velocity_zone_end_;
      SampleModule* sampler_;
      Value* granular_destination_;
      Value* granular_key_zone_start_;
      Value* granular_key_zone_end_;
      Value* granular_velocity_zone_start_;
      Value* granular_velocity_zone_end_;
      GranularModule* granular_;

      const Value* filter1_on_;
      const Value* filter2_on_;

      JUCE_LEAK_DETECTOR(ProducersModule)
  };
} // namespace vital
