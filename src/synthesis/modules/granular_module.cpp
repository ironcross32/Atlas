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

#include "granular_module.h"

namespace vital {

  GranularModule::GranularModule() : SynthModule(kNumInputs, kNumOutputs), on_(nullptr) {
    granular_ = new GranularSource();
    was_on_ = std::make_shared<bool>(true);
  }

  void GranularModule::init() {
    on_ = createBaseControl("granular_on");
    Value* keytrack = createBaseControl("granular_keytrack");
    Value* transpose_quantize = createBaseControl("granular_transpose_quantize");
    Value* transpose_quantize_key = createBaseControl("granular_transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl("granular_transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl("granular_transpose_quantize_mode");
    Value* mode = createBaseControl("granular_mode");
    Value* midi_density = createBaseControl("granular_midi_density");
    Value* grain_count = createBaseControl("granular_grain_count");
    Value* direction = createBaseControl("granular_direction");
    Output* start = createPolyModControl("granular_start");
    Output* end = createPolyModControl("granular_end");
    Output* density = createPolyModControl("granular_density");
    Output* grain_size = createPolyModControl("granular_grain_size");
    Output* speed = createPolyModControl("granular_speed");
    Output* position = createPolyModControl("granular_position");
    Output* position_mod = createPolyModControl("granular_position_mod");
    Output* position_mod_rate = createPolyModControl("granular_position_mod_rate");
    Output* random_position = createPolyModControl("granular_random_position");
    Output* random_volume = createPolyModControl("granular_random_volume");
    Output* random_pan = createPolyModControl("granular_random_pan");
    Output* random_pitch = createPolyModControl("granular_random_pitch");
    Output* interval = createPolyModControl("granular_interval");
    Output* interval_chance = createPolyModControl("granular_interval_chance");
    Output* low_cutoff = createPolyModControl("granular_low_cutoff", true, true);
    Output* high_cutoff = createPolyModControl("granular_high_cutoff", true, true);
    Output* transpose = createPolyModControl("granular_transpose");
    Output* tune = createPolyModControl("granular_tune");
    Output* level = createPolyModControl("granular_level", true, true);
    Output* pan = createPolyModControl("granular_pan");

    granular_->useInput(input(kReset), GranularSource::kReset);
    granular_->useInput(input(kVoiceEvent), GranularSource::kVoiceEvent);
    granular_->useInput(input(kMidi), GranularSource::kMidi);
    granular_->useInput(input(kNoteCount), GranularSource::kNoteCount);
    granular_->plug(keytrack, GranularSource::kKeytrack);
    granular_->plug(transpose, GranularSource::kTranspose);
    granular_->plug(transpose_quantize, GranularSource::kTransposeQuantize);
    granular_->plug(transpose_quantize_key, GranularSource::kTransposeQuantizeKey);
    granular_->plug(transpose_quantize_scale, GranularSource::kTransposeQuantizeScale);
    granular_->plug(transpose_quantize_mode, GranularSource::kTransposeQuantizeMode);
    granular_->plug(tune, GranularSource::kTune);
    granular_->plug(start, GranularSource::kStart);
    granular_->plug(end, GranularSource::kEnd);
    granular_->plug(mode, GranularSource::kMode);
    granular_->plug(midi_density, GranularSource::kMidiDensity);
    granular_->plug(grain_count, GranularSource::kGrainCount);
    granular_->plug(density, GranularSource::kDensity);
    granular_->plug(grain_size, GranularSource::kGrainSize);
    granular_->plug(speed, GranularSource::kSpeed);
    granular_->plug(position, GranularSource::kPosition);
    granular_->plug(position_mod, GranularSource::kPositionMod);
    granular_->plug(position_mod_rate, GranularSource::kPositionModRate);
    granular_->plug(random_position, GranularSource::kRandomPosition);
    granular_->plug(random_volume, GranularSource::kRandomVolume);
    granular_->plug(random_pan, GranularSource::kRandomPan);
    granular_->plug(random_pitch, GranularSource::kRandomPitch);
    granular_->plug(interval, GranularSource::kInterval);
    granular_->plug(interval_chance, GranularSource::kIntervalChance);
    granular_->plug(direction, GranularSource::kDirection);
    granular_->plug(low_cutoff, GranularSource::kLowCutoff);
    granular_->plug(high_cutoff, GranularSource::kHighCutoff);
    granular_->plug(level, GranularSource::kLevel);
    granular_->plug(pan, GranularSource::kPan);
    granular_->useOutput(output(kRaw), GranularSource::kRaw);
    granular_->useOutput(output(kLevelled), GranularSource::kLevelled);

    addProcessor(granular_);
    SynthModule::init();
  }

  void GranularModule::process(int num_samples) {
    bool on = on_->value();

    if (on)
      SynthModule::process(num_samples);
    else if (*was_on_) {
      output(kRaw)->clearBuffer();
      output(kLevelled)->clearBuffer();
      getPhaseOutput()->buffer[0] = 0.0f;
    }

    *was_on_ = on;
  }
} // namespace vital
