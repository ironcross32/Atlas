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
#include "granular_source.h"

namespace vital {
  class GranularModule : public SynthModule {
    public:
      enum {
        kReset,
        kVoiceEvent,
        kMidi,
        kNoteCount,
        kNumInputs
      };

      enum {
        kRaw,
        kLevelled,
        kNumOutputs
      };

      GranularModule();
      virtual ~GranularModule() { }

      void process(int num_samples) override;
      void init() override;
      Processor* clone() const override { return new GranularModule(*this); }

      Sample* getSample() { return granular_->getSample(); }
      force_inline Output* getPhaseOutput() const { return granular_->getPhaseOutput(); }

    protected:
      std::shared_ptr<bool> was_on_;
      GranularSource* granular_;
      Value* on_;

      JUCE_LEAK_DETECTOR(GranularModule)
  };
} // namespace vital
