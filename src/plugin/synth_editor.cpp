/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pylon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pylon.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "synth_editor.h"

#include "synth_plugin.h"
#include "value_bridge.h"
#include "sound_engine.h"
#include "load_save.h"
#include "line_generator.h"
#include "sample_source.h"
#include "synth_lfo.h"
#include "synth_strings.h"
#include "utils.h"
#include "formant_filter.h"
#include "wavetable.h"
#include "wavetable_group.h"
#include "wave_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <map>
#include <memory>

namespace {
  constexpr const char* kPresetSection = "Presets";
  constexpr const char* kSignalRoutingSection = "Signal routing";
  constexpr const char* kEffectsChainSection = "Chain and routing";
  constexpr const char* kAllBanks = "All banks";
  constexpr const char* kAllCategories = "All categories";

  void postPluginAnnouncement(const String&, AccessibilityHandler::AnnouncementPriority) { }

  struct MsegTimeDivision {
    const char* name;
    float quarter_notes;
  };

  const std::vector<MsegTimeDivision>& msegTimeDivisions() {
    static const std::vector<MsegTimeDivision> divisions {
      { "8 Bars", 32.0f }, { "4 Bars", 16.0f }, { "2 Bars", 8.0f }, { "1 Bar", 4.0f },
      { "1/2 Dotted", 3.0f }, { "1/2", 2.0f }, { "1/4 Dotted", 1.5f }, { "1/2 Triplet", 4.0f / 3.0f },
      { "1/4", 1.0f }, { "1/8 Dotted", 0.75f }, { "1/4 Triplet", 2.0f / 3.0f }, { "1/8", 0.5f },
      { "1/16 Dotted", 0.375f }, { "1/8 Triplet", 1.0f / 3.0f }, { "1/16", 0.25f },
      { "1/32 Dotted", 0.1875f }, { "1/16 Triplet", 1.0f / 6.0f }, { "1/32", 0.125f },
      { "1/64 Dotted", 0.09375f }, { "1/32 Triplet", 1.0f / 12.0f }, { "1/64", 0.0625f },
      { "1/128 Dotted", 0.046875f }, { "1/64 Triplet", 1.0f / 24.0f }, { "1/128", 0.03125f }
    };
    return divisions;
  }

  StringArray msegTimeDivisionChoices() {
    StringArray choices;
    for (const auto& division : msegTimeDivisions())
      choices.add(division.name);
    return choices;
  }

  String wavetableAudioLoadStyleName(int audio_load_style) {
    switch (audio_load_style) {
      case WavetableCreator::kVocoded:
        return "vocode";
      case WavetableCreator::kPitched:
        return "pitched";
      case WavetableCreator::kWavetableSplice:
      default:
        return "wavetable splice";
    }
  }

  WaveSource* editableWaveSourceFor(WavetableCreator* creator) {
    if (creator == nullptr || creator->numGroups() == 0)
      return nullptr;

    auto* group = creator->getGroup(0);
    if (group == nullptr || group->numComponents() == 0)
      return nullptr;

    return dynamic_cast<WaveSource*>(group->getComponent(0));
  }

  WaveSource* ensureEditableWaveSource(WavetableCreator* creator) {
    if (auto* existing = editableWaveSourceFor(creator))
      return existing;
    if (creator == nullptr || creator->getWavetable() == nullptr)
      return nullptr;

    auto* wavetable = creator->getWavetable();
    const auto* data = wavetable->getAllData();
    if (data == nullptr || data->num_frames <= 0)
      return nullptr;

    const std::string name = creator->getName();
    const std::string author = creator->getAuthor();
    auto* group = new WavetableGroup();
    auto* source = new WaveSource();
    const int frames = jlimit(1, vital::kNumOscillatorWaveFrames, data->num_frames);

    for (int frame = 0; frame < frames; ++frame) {
      source->insertNewKeyframe(frame);
      auto* wave_frame = source->getKeyframe(frame)->wave_frame();
      std::memcpy(wave_frame->time_domain, data->wave_data[frame],
                  sizeof(vital::mono_float) * vital::WaveFrame::kWaveformSize);
      wave_frame->index = frame;
      wave_frame->setFrequencyRatio(data->frequency_ratio);
      wave_frame->setSampleRate(data->sample_rate);
      wave_frame->toFrequencyDomain();
    }

    source->setInterpolationStyle(WavetableComponent::kLinear);
    group->addComponent(source);
    creator->clear();
    creator->setName(name);
    creator->setAuthor(author);
    creator->addGroup(group);
    creator->render();
    return source;
  }

  int closestVitalTempoIndex(float quarter_notes) {
    if (quarter_notes <= 0.0f)
      return 8;

    const float target = 1.0f / quarter_notes;
    int best = 8;
    float best_distance = std::numeric_limits<float>::max();
    for (int i = 1; i < vital::constants::kNumSyncedFrequencyRatios; ++i) {
      const float distance = std::abs(std::log2(jmax(0.000001f, vital::constants::kSyncedFrequencyRatios[i] / target)));
      if (distance < best_distance) {
        best_distance = distance;
        best = i;
      }
    }
    return best;
  }

  float engineValueFor(AudioProcessorParameter* parameter) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      return bridge->convertToEngineValue(bridge->getValue());
    return parameter ? parameter->getValue() : 0.0f;
  }

  int filterIndexForSection(const String& section_name) {
    if (section_name == "Filter 1")
      return 1;
    if (section_name == "Filter 2")
      return 2;
    if (section_name == "Filter")
      return 3;
    return 0;
  }

  String filterPrefixForSection(const String& section_name) {
    const int index = filterIndexForSection(section_name);
    if (index == 3)
      return "filter_fx_";
    return index > 0 ? "filter_" + String(index) + "_" : String();
  }

  String filterSuffixForParameter(const String& section_name, const String& parameter_id) {
    const String prefix = filterPrefixForSection(section_name);
    if (prefix.isEmpty() || !parameter_id.startsWith(prefix))
      return {};
    return parameter_id.fromFirstOccurrenceOf(prefix, false, false);
  }

  int numFilterStylesForModel(int model) {
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return 5;
      case vital::constants::kDiode:
      case vital::constants::kFormant:
      case vital::constants::kPhase:
        return 2;
      case vital::constants::kComb:
        return 6;
      default:
        return 1;
    }
  }

  String filterStyleNameForModel(int model, int style) {
    style = jlimit(0, jmax(0, numFilterStylesForModel(model) - 1), style);
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return strings::kFilterStyleNames[style];
      case vital::constants::kDiode:
        return strings::kDiodeStyleNames[style];
      case vital::constants::kFormant:
        if (style == vital::FormantFilter::kVocalTract)
          return "The Mouth";
        if (style == vital::FormantFilter::kAIUO)
          return "AIUO";
        return "AOIE";
      case vital::constants::kComb:
        return strings::kCombStyleNames[style];
      case vital::constants::kPhase:
        return style ? "Negative" : "Positive";
      default:
        return {};
    }
  }

  String filterBlendText(int model, double normalized_value) {
    const double engine_value = jlimit(0.0, 1.0, normalized_value) * 2.0;
    if (model == vital::constants::kComb) {
      if (engine_value <= 0.05)
        return "Low comb";
      if (engine_value >= 1.95)
        return "High comb";
      return "Low to high comb, " + String(engine_value, 2);
    }
    if (model == vital::constants::kPhase)
      return "Phaser blend, " + String(engine_value, 2);

    if (engine_value <= 0.05)
      return "Low pass";
    if (std::abs(engine_value - 1.0) <= 0.05)
      return "Band pass";
    if (engine_value >= 1.95)
      return "High pass";
    if (engine_value < 1.0)
      return "Between low pass and band pass, " + String(engine_value, 2);
    return "Between band pass and high pass, " + String(engine_value, 2);
  }

  String last_section_name;
  String last_preset_path;
  String last_preset_search;
  String last_preset_bank = kAllBanks;
  String last_preset_category = kAllCategories;
  bool last_preset_preview = false;

  force_inline int chunkNameToData(const char* chunk_name) {
    return static_cast<int>(ByteOrder::littleEndianInt(chunk_name));
  }

  String getWavetableDataString(InputStream& input_stream) {
    input_stream.setPosition(0);
    int first_chunk = input_stream.readInt();
    if (first_chunk != chunkNameToData("RIFF"))
      return "";

    int length = input_stream.readInt();
    int data_end = static_cast<int>(input_stream.getPosition()) + length;

    if (input_stream.readInt() != chunkNameToData("WAVE"))
      return "";

    while (!input_stream.isExhausted() && input_stream.getPosition() < data_end) {
      int chunk_label = input_stream.readInt();
      int chunk_length = input_stream.readInt();

      if (chunk_label == chunkNameToData("clm ")) {
        MemoryBlock memory_block;
        input_stream.readIntoMemoryBlock(memory_block, chunk_length);
        return memory_block.toString();
      }

      input_stream.setPosition(input_stream.getPosition() + chunk_length);
    }

    return "";
  }

  FileSource::FadeStyle getFadeStyleFromWavetableString(String data) {
    if (data.isEmpty())
      return FileSource::kFreqInterpolate;

    if (data.substring(0, 3) == "<!>") {
      StringArray tokens;
      tokens.addTokens(data.substring(3), " ", "");
      if (tokens.size() < 2 || tokens[1].isEmpty())
        return FileSource::kFreqInterpolate;

      char fade_character = tokens[1][0];
      if (fade_character == '0')
        return FileSource::kNoInterpolate;
      if (fade_character == '1')
        return FileSource::kTimeInterpolate;
    }

    return FileSource::kFreqInterpolate;
  }

  String getAuthorFromWavetableString(String data) {
    if (data.substring(0, 3) == "<!>") {
      int start = data.indexOf("[");
      int end = data.indexOf("]");
      if (start < end && start >= 0)
        return data.substring(start + 1, end);
    }

    return "";
  }

  int loadAudioFile(AudioSampleBuffer& destination, InputStream* audio_stream) {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();

    audio_stream->setPosition(0);
    std::unique_ptr<AudioFormatReader> format_reader(
        format_manager.createReaderFor(std::unique_ptr<InputStream>(audio_stream)));
    if (format_reader == nullptr)
      return 0;

    int num_samples = static_cast<int>(format_reader->lengthInSamples);
    destination.setSize(format_reader->numChannels, num_samples);
    format_reader->read(&destination, 0, num_samples, 0, true, true);
    return static_cast<int>(format_reader->sampleRate);
  }

  struct BrowserNode {
    std::map<String, std::unique_ptr<BrowserNode>> folders;
    Array<File> files;
  };

  String browserRootName(const File& root, const String& folder_name) {
    const File data_folder = LoadSave::getDataDirectory().getChildFile(folder_name);
    if (root == data_folder)
      return "Factory";

    const File user_folder = LoadSave::getUserDirectory().getChildFile(folder_name);
    if (root == user_folder)
      return "User";

    return root.getFileName().isNotEmpty() ? root.getFileName() : root.getFullPathName();
  }

  void addBrowserFile(BrowserNode& root, const File& base, const File& file) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(base), File::getSeparatorString(), "");
    if (parts.isEmpty())
      return;

    BrowserNode* node = &root;
    for (int i = 0; i < parts.size() - 1; ++i) {
      const String folder = parts[i];
      if (!node->folders.count(folder))
        node->folders[folder] = std::make_unique<BrowserNode>();
      node = node->folders[folder].get();
    }
    node->files.add(file);
  }

  void sortBrowserNode(BrowserNode& node) {
    node.files.sort();
    for (auto& folder : node.folders)
      sortBrowserNode(*folder.second);
  }

  bool addBrowserNodeToMenu(PopupMenu& menu, const BrowserNode& node,
                            const std::shared_ptr<std::vector<File>>& choices, int& item_id) {
    bool has_items = false;
    for (const auto& folder : node.folders) {
      PopupMenu sub_menu;
      if (addBrowserNodeToMenu(sub_menu, *folder.second, choices, item_id)) {
        menu.addSubMenu(folder.first, sub_menu, true);
        has_items = true;
      }
    }

    for (const auto& file : node.files) {
      choices->push_back(file);
      menu.addItem(item_id++, file.getFileNameWithoutExtension());
      has_items = true;
    }

    return has_items;
  }

  bool browserFileBelongsToRoot(const File& file, const File& root) {
    const String root_path = root.getFullPathName();
    const String file_path = file.getFullPathName();
    return file_path == root_path || file_path.startsWith(root_path + File::getSeparatorString());
  }

  void refreshBrowserFileCache(std::vector<File>& cache, const std::vector<File>& roots, const String& wildcard) {
    cache.clear();
    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      Array<File> files;
      root.findChildFiles(files, File::findFiles, true, wildcard);
      cache.reserve(cache.size() + static_cast<size_t>(files.size()));
      for (const auto& file : files)
        cache.push_back(file);
    }
  }

  std::vector<File> browserRootsWithAdditionalFolders(const String& folder_name,
                                                      const std::string& additional_folders_name) {
    auto roots = LoadSave::getDirectories(folder_name);
    for (const auto& path : LoadSave::getAdditionalFolders(additional_folders_name))
      roots.push_back(File(path));
    return roots;
  }

  File shiftedBrowserFile(const std::vector<File>& browser_files, const File& current_file, int shift) {
    if (browser_files.empty())
      return File();

    Array<File> sorted_files;
    for (const auto& file : browser_files)
      sorted_files.add(file);

    LoadSave::FileSorterAscending file_sorter;
    sorted_files.sort(file_sorter);

    const int index = sorted_files.indexOf(current_file);
    if (index < 0)
      return sorted_files[0];
    return sorted_files[(index + shift + sorted_files.size()) % sorted_files.size()];
  }

  PopupMenu createFileBrowserMenu(const std::vector<File>& roots, const String& folder_name,
                                  const std::vector<File>& browser_files,
                                  const std::shared_ptr<std::vector<File>>& choices) {
    PopupMenu menu;
    int item_id = 1;

    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      BrowserNode root_node;
      for (const auto& file : browser_files) {
        if (browserFileBelongsToRoot(file, root))
          addBrowserFile(root_node, root, file);
      }
      sortBrowserNode(root_node);

      PopupMenu root_menu;
      if (addBrowserNodeToMenu(root_menu, root_node, choices, item_id))
        menu.addSubMenu(browserRootName(root, folder_name), root_menu, true);
    }

    if (choices->empty())
      menu.addItem(1, "No files found", false);
    return menu;
  }

  int indexOfPathPart(const StringArray& parts, const String& name) {
    for (int i = 0; i < parts.size(); ++i) {
      if (parts[i] == name)
        return i;
    }
    return -1;
  }

  StringArray presetPathParts(const File& preset) {
    StringArray parts;
    parts.addTokens(preset.getRelativePathFrom(LoadSave::getDataDirectory()), File::getSeparatorString(), "");
    return parts;
  }

  String presetBankName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    if (preset_folder > 0)
      return parts[preset_folder - 1];
    return preset.getParentDirectory().getFileName();
  }

  String presetCategoryName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    if (preset_folder >= 0 && preset_folder + 2 < parts.size()) {
      StringArray category_parts;
      for (int i = preset_folder + 1; i < parts.size() - 1; ++i)
        category_parts.add(parts[i]);
      return category_parts.joinIntoString(" / ");
    }

    const String style = LoadSave::getStyleFromFile(preset);
    if (style.isNotEmpty())
      return style;

    return "Uncategorized";
  }

  bool isRoutingParameter(const String& id) {
    if (id == "effect_chain_order" || id == "post_effect_order" ||
        id == "filter_1_destination" || id == "filter_2_destination")
      return true;
    if (id.startsWith("effect_chain_slot_"))
      return true;
    if (id.startsWith("bus_") && id.endsWith("_destination"))
      return true;
    if (id.startsWith("bus_") && (id.endsWith("effect_chain_order") || id.endsWith("post_effect_order")))
      return true;
    if (id.startsWith("bus_") && id.contains("_effect_chain_slot_"))
      return true;
    if (id.startsWith("filter_") &&
        (id.endsWith("_osc1_input") || id.endsWith("_osc2_input") ||
         id.endsWith("_osc3_input") || id.endsWith("_osc4_input") || id.endsWith("_sample_input") ||
         id.endsWith("_filter_input")))
      return true;
    return false;
  }

  int sectionRank(const String& section) {
    static const std::array<const char*, 51> order = {{
      "Master and global",
      kPresetSection,
      "Voice and performance",
      kSignalRoutingSection,
      "Oscillator 1",
      "Oscillator 2",
      "Oscillator 3",
      "Oscillator 4",
      "Sample",
      "Granular",
      "Filter 1",
      "Filter 2",
      kEffectsChainSection,
      "Distortion",
      "Filter",
      "Chorus",
      "Flanger",
      "Phaser",
      "Delay",
      "Reverb",
      "Utility",
      "Phase Shift",
      "Equalizer",
      "Compressor",
      "Limiter",
      "Frequency Shifter",
      "Dimension Expander",
      "Envelope 1",
      "Envelope 2",
      "Envelope 3",
      "Envelope 4",
      "Envelope 5",
      "Envelope 6",
      "LFO 1",
      "LFO 2",
      "LFO 3",
      "LFO 4",
      "LFO 5",
      "LFO 6",
      "LFO 7",
      "LFO 8",
      "Random modulation",
      "Macros",
      "Modulation routing",
      "Zones - Oscillator 1",
      "Zones - Oscillator 2",
      "Zones - Oscillator 3",
      "Zones - Oscillator 4",
      "Zones - Sample",
      "Zones - Granular",
      "Other"
    }};

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (section == order[i])
        return i;
    }
    if (section.startsWith("Bus 1 - "))
      return 220 + sectionRank(section.fromFirstOccurrenceOf("Bus 1 - ", false, false));
    if (section.startsWith("Bus 2 - "))
      return 250 + sectionRank(section.fromFirstOccurrenceOf("Bus 2 - ", false, false));
    if (section.startsWith("Bus 3 - "))
      return 280 + sectionRank(section.fromFirstOccurrenceOf("Bus 3 - ", false, false));
    return static_cast<int>(order.size());
  }

  String groupForSection(const String& section) {
    if (section == kPresetSection)
      return "Presets";
    if (section.startsWith("Oscillator") || section == "Sample" || section == "Granular")
      return "Oscillators";
    if (section == "Voice and performance" || section == kSignalRoutingSection)
      return "Mixer and routing";
    if (section.startsWith("Filter "))
      return "Filters";
    if (section.startsWith("Bus 1 - "))
      return "Bus 1";
    if (section.startsWith("Bus 2 - "))
      return "Bus 2";
    if (section.startsWith("Bus 3 - "))
      return "Bus 3";
    if (section.startsWith("Envelope "))
      return "Envelopes";
    if (section.startsWith("LFO "))
      return "LFOs";
    if (section == "Random modulation")
      return "Random modulation";
    if (section == "Macros")
      return "Macros";
    if (section.startsWith("Zones - "))
      return "Zones";
    if (section == kEffectsChainSection || section == "Distortion" || section == "Filter" ||
        section == "Chorus" || section == "Flanger" || section == "Phaser" ||
        section == "Delay" || section == "Reverb" || section == "Utility" || section == "Phase Shift" ||
        section == "Equalizer" || section == "Compressor" || section == "Limiter" ||
        section == "Frequency Shifter" || section == "Dimension Expander")
      return "Effects";
    if (section == "Modulation routing")
      return "Modulation";
    return "Global";
  }

  int groupRank(const String& group) {
    static const std::array<const char*, 15> order = {{
      "Presets",
      "Oscillators",
      "Mixer and routing",
      "Filters",
      "Bus 1",
      "Bus 2",
      "Bus 3",
      "Envelopes",
      "LFOs",
      "Random modulation",
      "Macros",
      "Effects",
      "Modulation",
      "Zones",
      "Global"
    }};

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (group == order[i])
        return i;
    }
    return static_cast<int>(order.size());
  }

  int modulationSlotForParameterId(const String& id);
  int modulationParameterSuffixRank(const String& id);
  int macroIndexForControlId(const String& id);
  bool isMacroBipolarParameterId(const String& id);
  String macroBipolarParameterId(int macroIndex);

  int parameterRank(const String& id) {
    String rank_id = id;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (rank_id.startsWith(prefix)) {
        rank_id = rank_id.fromFirstOccurrenceOf(prefix, false, false);
        break;
      }
    }

    auto oscillator_suffix_rank = [](const String& suffix) {
      static const std::array<const char*, 35> oscillator_order = {{
        "on",
        "wave_frame",
        "transpose",
        "tune",
        "level",
        "pan",
        "destination",
        "unison_voices",
        "unison_detune",
        "unison_blend",
        "stack_style",
        "detune_power",
        "detune_range",
        "spectral_unison",
        "frame_spread",
        "stereo_spread",
        "phase",
        "random_phase",
        "midi_track",
        "smooth_interpolation",
        "distortion_type",
        "distortion_amount",
        "distortion_spread",
        "spectral_morph_type",
        "spectral_morph_amount",
        "spectral_morph_spread",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "view_2d"
      }};
      for (int i = 0; i < static_cast<int>(oscillator_order.size()); ++i) {
        if (suffix == oscillator_order[i])
          return 100 + i;
      }
      return 500;
    };

    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix))
        return osc * 1000 + oscillator_suffix_rank(id.fromFirstOccurrenceOf(prefix, false, false));
    }

    if (id.startsWith("sample_")) {
      const String suffix = id.fromFirstOccurrenceOf("sample_", false, false);
      static const std::array<const char*, 26> sample_order = {{
        "on",
        "destination",
        "playback_mode",
        "loop",
        "bounce",
        "random_phase",
        "keytrack",
        "start",
        "end",
        "loop_start",
        "loop_end",
        "loop_crossfade",
        "low_cutoff",
        "high_cutoff",
        "transpose",
        "tune",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "level",
        "pan"
      }};
      for (int i = 0; i < static_cast<int>(sample_order.size()); ++i) {
        if (suffix == sample_order[i])
          return 4500 + i;
      }
      if (suffix == "level")
        return 4525;
      if (suffix == "pan")
        return 4526;
      return 4590;
    }

    if (id.startsWith("granular_")) {
      const String suffix = id.fromFirstOccurrenceOf("granular_", false, false);
      static const std::array<const char*, 35> granular_order = {{
        "on",
        "destination",
        "mode",
        "keytrack",
        "grain_count",
        "density",
        "midi_density",
        "grain_size",
        "speed",
        "position",
        "position_mod",
        "position_mod_rate",
        "random_position",
        "random_volume",
        "random_pan",
        "random_pitch",
        "interval",
        "interval_chance",
        "direction",
        "start",
        "end",
        "low_cutoff",
        "high_cutoff",
        "transpose",
        "tune",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "level",
        "pan"
      }};
      for (int i = 0; i < static_cast<int>(granular_order.size()); ++i) {
        if (suffix == granular_order[i])
          return 4600 + i;
      }
      return 4690;
    }

    const int modulation_slot = modulationSlotForParameterId(id);
    if (modulation_slot > 0)
      return 200000 + modulation_slot * 10 + modulationParameterSuffixRank(id);

    const int macro_index = macroIndexForControlId(id);
    if (macro_index >= 0)
      return 10000 + macro_index;
    if (isMacroBipolarParameterId(id))
      return 11000 + id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue();

    static const std::array<const char*, 49> priority = {{
      "volume",
      "polyphony",
      "oversampling",
      "osc_1_on",
      "osc_1_destination",
      "osc_2_on",
      "osc_2_destination",
      "osc_3_on",
      "osc_3_destination",
      "osc_4_on",
      "osc_4_destination",
      "sample_on",
      "sample_destination",
      "granular_on",
      "granular_destination",
      "filter_1_on",
      "filter_1_filter_input",
      "filter_1_osc1_input",
      "filter_1_osc2_input",
      "filter_1_osc3_input",
      "filter_1_osc4_input",
      "filter_1_sample_input",
      "filter_2_on",
      "filter_2_filter_input",
      "filter_2_osc1_input",
      "filter_2_osc2_input",
      "filter_2_osc3_input",
      "filter_2_osc4_input",
      "filter_2_sample_input",
      "effect_chain_order",
      "post_effect_order",
      "destination",
      "distortion_on",
      "filter_fx_on",
      "chorus_on",
      "flanger_on",
      "phaser_on",
      "delay_on",
      "reverb_on",
      "utility_on",
      "phase_shift_on",
      "eq_on",
      "compressor_on",
      "limiter_on",
      "frequency_shifter_on",
      "dimension_expander_on",
      "filter_1_model",
      "filter_1_style",
      "filter_2_model"
    }};

    for (int i = 0; i < static_cast<int>(priority.size()); ++i) {
      if (rank_id == priority[i])
        return i;
    }
    return 1000;
  }

  String readableId(String id) {
    return id.replaceCharacter('_', ' ').toLowerCase().substring(0, 1).toUpperCase()
           + id.replaceCharacter('_', ' ').toLowerCase().substring(1);
  }

  String effectName(int effect) {
    if (isPositiveAndBelow(effect, vital::constants::kNumEffects))
      return readableId(strings::kEffectOrder[effect]);
    return {};
  }

  String effectNameForParameterPrefix(const String& prefix) {
    if (prefix == "filter_fx")
      return "Filter";
    return readableId(prefix);
  }

  String effectSectionForPrefixedId(const String& id, const String& section_prefix) {
    const String rest = id.fromFirstOccurrenceOf(section_prefix, false, false);
    if (rest == "destination")
      return "Chain and routing";
    if (rest == "effect_chain_order" || rest == "post_effect_order" || rest.startsWith("effect_chain_slot_"))
      return "Chain and routing";
    if (rest.startsWith("chorus_")) return "Chorus";
    if (rest.startsWith("compressor_")) return "Compressor";
    if (rest.startsWith("delay_")) return "Delay";
    if (rest.startsWith("dimension_expander_")) return "Dimension Expander";
    if (rest.startsWith("distortion_")) return "Distortion";
    if (rest.startsWith("eq_")) return "Equalizer";
    if (rest.startsWith("flanger_")) return "Flanger";
    if (rest.startsWith("filter_fx_")) return "Filter";
    if (rest.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (rest.startsWith("limiter_")) return "Limiter";
    if (rest.startsWith("phase_shift_")) return "Phase Shift";
    if (rest.startsWith("phaser_")) return "Phaser";
    if (rest.startsWith("reverb_")) return "Reverb";
    if (rest.startsWith("utility_")) return "Utility";
    return {};
  }

  String busEffectSectionForParameter(const String& id) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String effect_section = effectSectionForPrefixedId(id, prefix);
      if (effect_section.isNotEmpty())
        return "Bus " + String(bus) + " - " + effect_section;
    }
    return {};
  }

  bool isEffectChainSection(const String& section) {
    if (section == kEffectsChainSection)
      return true;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      if (section == "Bus " + String(bus) + " - Chain and routing")
        return true;
    }
    return false;
  }

  String effectChainPrefixForSection(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return "bus_" + String(bus) + "_";
    }
    return {};
  }

  String accessibleSectionTitle(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return section.fromFirstOccurrenceOf(prefix, false, false);
    }
    return section;
  }

  String accessibleParameterTitle(const String& section, String name) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String section_prefix = "Bus " + String(bus) + " - ";
      const String name_prefix = "Bus " + String(bus) + " ";
      if (section.startsWith(section_prefix) && name.startsWith(name_prefix))
        return name.fromFirstOccurrenceOf(name_prefix, false, false);
    }
    return name;
  }

  String sectionForParameter(const String& id) {
    const String bus_section = busEffectSectionForParameter(id);
    if (bus_section.isNotEmpty())
      return bus_section;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix) &&
          (id.endsWith("_key_zone_start") || id.endsWith("_key_zone_end") ||
           id.endsWith("_velocity_zone_start") || id.endsWith("_velocity_zone_end"))) {
        return "Zones - Oscillator " + String(osc);
      }
    }
    if (id == "sample_key_zone_start" || id == "sample_key_zone_end" ||
        id == "sample_velocity_zone_start" || id == "sample_velocity_zone_end")
      return "Zones - Sample";
    if (id == "granular_key_zone_start" || id == "granular_key_zone_end" ||
        id == "granular_velocity_zone_start" || id == "granular_velocity_zone_end")
      return "Zones - Granular";
    if (isRoutingParameter(id))
      return (id == "effect_chain_order" || id == "post_effect_order" || id.startsWith("effect_chain_slot_") ||
              id.endsWith("effect_chain_order") || id.endsWith("post_effect_order") ||
              id.contains("_effect_chain_slot_")) ?
             kEffectsChainSection : kSignalRoutingSection;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      if (id.startsWith("osc_" + String(osc)))
        return "Oscillator " + String(osc);
    }
    if (id.startsWith("sample_")) return "Sample";
    if (id.startsWith("granular_")) return "Granular";
    if (id.startsWith("filter_1")) return "Filter 1";
    if (id.startsWith("filter_2")) return "Filter 2";
    if (id.startsWith("env_1")) return "Envelope 1";
    if (id.startsWith("env_2")) return "Envelope 2";
    if (id.startsWith("env_3")) return "Envelope 3";
    if (id.startsWith("env_4")) return "Envelope 4";
    if (id.startsWith("env_5")) return "Envelope 5";
    if (id.startsWith("env_6")) return "Envelope 6";
    if (id.startsWith("lfo_")) {
      const String number = id.fromFirstOccurrenceOf("lfo_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int lfo = number.getIntValue();
      if (lfo > 0)
        return "LFO " + String(lfo);
      return "LFO 1";
    }
    if (id.startsWith("random_")) return "Random modulation";
    if (id.startsWith("modulation_")) return "Modulation routing";
    if (id.startsWith("chorus_")) return "Chorus";
    if (id.startsWith("compressor_")) return "Compressor";
    if (id.startsWith("delay_")) return "Delay";
    if (id.startsWith("dimension_expander_")) return "Dimension Expander";
    if (id.startsWith("distortion_")) return "Distortion";
    if (id.startsWith("eq_")) return "Equalizer";
    if (id.startsWith("flanger_")) return "Flanger";
    if (id.startsWith("filter_fx_")) return "Filter";
    if (id.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (id.startsWith("limiter_")) return "Limiter";
    if (id.startsWith("phase_shift_")) return "Phase Shift";
    if (id.startsWith("phaser_")) return "Phaser";
    if (id.startsWith("reverb_")) return "Reverb";
    if (id.startsWith("utility_")) return "Utility";
    if (id.startsWith("macro_")) return "Macros";
    if (id.contains("voice") || id.startsWith("polyphony") || id.startsWith("portamento") ||
        id.startsWith("legato")) return "Voice and performance";
    return "Master and global";
  }

  String modulationDestinationGroupForId(const String& id) {
    const String section = sectionForParameter(id);
    if (section == kSignalRoutingSection)
      return "Signal routing";
    if (section == kEffectsChainSection)
      return "Effects chain";
    return section;
  }

  String modulationDestinationLabelForId(const String& id) {
    if (vital::Parameters::isParameter(id.toStdString()))
      return vital::Parameters::getDisplayName(id.toStdString());
    return readableId(id);
  }

  String modulationSourceLabelForId(const String& id) {
    if (id == "mod_wheel") return "Mod wheel";
    if (id == "pitch_wheel") return "Pitch bend";
    if (id == "note_in_octave") return "Note in octave";
    if (id == "lift") return "Release velocity";
    if (id == "slide") return "MPE slide";
    if (id == "aftertouch") return "Aftertouch";
    if (id == "velocity") return "Velocity";
    if (id == "note") return "Note";
    if (id == "stereo") return "Stereo voice split";
    return readableId(id);
  }

  int modulationSlotForParameterId(const String& id) {
    if (!id.startsWith("modulation_"))
      return -1;

    const String slot = id.fromFirstOccurrenceOf("modulation_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    return slot.getIntValue();
  }

  int modulationParameterSuffixRank(const String& id) {
    static const std::array<const char*, 5> order = {{ "amount", "power", "bipolar", "stereo", "bypass" }};
    const String suffix = id.fromLastOccurrenceOf("_", false, false);
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
      if (suffix == order[i])
        return i;
    return static_cast<int>(order.size());
  }

  String numberWord(int number) {
    static const std::array<const char*, 20> words = {{
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen", "twenty"
    }};
    if (number >= 1 && number <= static_cast<int>(words.size()))
      return words[number - 1];
    return String(number);
  }

  bool isModulationMenuKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'm';
  }

  bool isMidiLearnKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'l';
  }

  bool isMidiClearKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'c';
  }

  int macroIndexForControlId(const String& id) {
    if (!id.startsWith("macro_control_"))
      return -1;

    const int macro = id.fromFirstOccurrenceOf("macro_control_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros) ? macro : -1;
  }

  bool isMacroBipolarParameterId(const String& id) {
    if (!id.startsWith("macro_bipolar_"))
      return false;

    const int macro = id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros);
  }

  String macroBipolarParameterId(int macroIndex) {
    return "macro_bipolar_" + String(macroIndex + 1);
  }

  int modulationSourceSortRank(const String& id) {
    const int macro = macroIndexForControlId(id);
    if (macro >= 0)
      return 4000 + macro;

    if (id.startsWith("lfo_"))
      return 1000 + id.fromFirstOccurrenceOf("lfo_", false, false).getIntValue();
    if (id.startsWith("env_"))
      return 2000 + id.fromFirstOccurrenceOf("env_", false, false).getIntValue();
    if (id.startsWith("random_"))
      return 3000 + id.fromFirstOccurrenceOf("random_", false, false).getIntValue();
    if (id == "velocity") return 5000;
    if (id == "aftertouch") return 5001;
    if (id == "mod_wheel") return 5002;
    if (id == "pitch_wheel") return 5003;
    if (id == "lift") return 5004;
    if (id == "slide") return 5005;
    if (id == "note") return 5006;
    if (id == "note_in_octave") return 5007;
    if (id == "stereo") return 5008;
    return 9000;
  }

  int modulationDestinationSortRank(const String& id) {
    return sectionRank(modulationDestinationGroupForId(id)) * 10000 + parameterRank(id);
  }

  String routingDestinationText(int destination) {
    switch (destination) {
      case vital::constants::kFilter1:
        return "Filter 1";
      case vital::constants::kFilter2:
        return "Filter 2";
      case vital::constants::kDualFilters:
        return "Filter 1 and 2";
      case vital::constants::kEffects:
        return "Main effects";
      case vital::constants::kBus1:
        return "Bus 1";
      case vital::constants::kBus2:
        return "Bus 2";
      case vital::constants::kDirectOut:
        return "Direct out";
      case vital::constants::kBus3:
        return "Bus 3";
      default:
        return "Main effects";
    }
  }

  String busRoutingDestinationText(int bus_index, int destination) {
    if (destination == vital::constants::kBusDestinationDirectOut)
      return "Direct out";
    if (destination == vital::constants::kBusDestinationMain)
      return "Main effects";
    if (destination >= vital::constants::kBusDestinationBus1 &&
        destination <= vital::constants::kBusDestinationBus3) {
      const int target_bus = destination - vital::constants::kBusDestinationBus1;
      if (target_bus > bus_index)
        return "Bus " + String(target_bus + 1);
    }
    return "Main effects";
  }

  String percentString(float value) {
    return String(roundToInt(jlimit(0.0f, 1.0f, value) * 100.0f)) + "%";
  }

  bool isTransposeQuantizeParameter(const String& id) {
    return id.endsWith("transpose_quantize");
  }

  struct QuantizeScaleChoice {
    const char* name;
    int mask;
  };

  const std::vector<QuantizeScaleChoice>& transposeQuantizeScales() {
    static const std::vector<QuantizeScaleChoice> scales {
      { "Off", 0 },
      { "Chromatic", (1 << 12) - 1 },
      { "Major", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Natural minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Harmonic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11) },
      { "Melodic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Major pentatonic", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9) },
      { "Minor pentatonic", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10) },
      { "Blues", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10) },
      { "Dorian", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Phrygian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Lydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Mixolydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Locrian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10) },
      { "Custom", -1 },
    };
    return scales;
  }

  int rotateTransposeMask(int mask, int key) {
    int result = 0;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((mask >> i) & 1)
        result |= 1 << ((i + key + vital::kNotesPerOctave) % vital::kNotesPerOctave);
    return result;
  }

  String transposeKeyName(int key) {
    static const std::array<const char*, 12> names = {{
      "C", "C sharp", "D", "E flat", "E", "F", "F sharp", "G", "A flat", "A", "B flat", "B"
    }};
    return names[jlimit(0, vital::kNotesPerOctave - 1, key)];
  }

  int scaleChoiceForMask(int pitch_mask, int& key) {
    const auto& scales = transposeQuantizeScales();
    if (pitch_mask == 0) {
      key = 0;
      return 0;
    }

    for (int scale = 1; scale < static_cast<int>(scales.size()) - 1; ++scale) {
      for (int candidate_key = 0; candidate_key < vital::kNotesPerOctave; ++candidate_key) {
        if (rotateTransposeMask(scales[scale].mask, candidate_key) == pitch_mask) {
          key = candidate_key;
          return scale;
        }
      }
    }

    key = 0;
    return static_cast<int>(scales.size()) - 1;
  }

  String transposeQuantizeDescription(int value) {
    constexpr int kPitchMask = (1 << vital::kNotesPerOctave) - 1;
    const int pitch_mask = value & kPitchMask;
    const bool global = vital::utils::isTransposeQuantizeGlobal(value);

    if (pitch_mask == 0)
      return "Off";
    if (pitch_mask == kPitchMask)
      return String(global ? "Global " : "Local ") + "chromatic";

    StringArray notes;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((pitch_mask >> i) & 1)
        notes.add(transposeKeyName(i));

    return String(global ? "Global scale, " : "Local scale, ") + notes.joinIntoString(", ");
  }

  int noteIndexForToken(String token) {
    token = token.trim().toLowerCase()
                 .replace("sharp", "#")
                 .replace("flat", "b")
                 .replaceCharacter(juce_wchar(0x266f), '#')
                 .replaceCharacter(juce_wchar(0x266d), 'b');
    if (token == "c") return 0;
    if (token == "c#" || token == "db") return 1;
    if (token == "d") return 2;
    if (token == "d#" || token == "eb") return 3;
    if (token == "e") return 4;
    if (token == "f") return 5;
    if (token == "f#" || token == "gb") return 6;
    if (token == "g") return 7;
    if (token == "g#" || token == "ab") return 8;
    if (token == "a") return 9;
    if (token == "a#" || token == "bb") return 10;
    if (token == "b") return 11;
    return -1;
  }

  int scaleMaskForName(String text) {
    text = text.toLowerCase().trim();
    if (text == "off" || text == "none")
      return 0;
    if (text == "minor")
      text = "natural minor";
    for (const auto& scale : transposeQuantizeScales())
      if (text == String(scale.name).toLowerCase())
        return scale.mask;
    return -1;
  }

  bool parseTransposeQuantizeText(String text, int current_value, int& result) {
    text = text.trim();
    if (text.isEmpty())
      return false;

    if (text.containsOnly("0123456789")) {
      result = jlimit(0, 8191, text.getIntValue());
      return true;
    }

    String lower = text.toLowerCase();
    bool global = vital::utils::isTransposeQuantizeGlobal(current_value);
    if (lower.contains("global")) {
      global = true;
      lower = lower.replace("global", "");
    }
    if (lower.contains("local")) {
      global = false;
      lower = lower.replace("local", "");
    }
    lower = lower.replace("snap", "").replace("scale", "").trim();

    int mask = scaleMaskForName(lower);
    if (mask < 0) {
      mask = 0;
      StringArray tokens;
      tokens.addTokens(lower.replace(",", " ").replace(";", " "), " \t\r\n", "");
      for (const auto& token : tokens) {
        const int note = noteIndexForToken(token);
        if (note >= 0)
          mask |= 1 << note;
      }
      if (mask == 0)
        return false;
    }

    result = mask | (global ? (1 << vital::kNotesPerOctave) : 0);
    return true;
  }

  float pointOutputValue(LineGenerator* generator, int index) {
    if (generator == nullptr || !isPositiveAndBelow(index, generator->getNumPoints()))
      return 0.0f;
    return 1.0f - generator->getPoint(index).second;
  }

  enum AccessibleLfoCurve {
    kCurveHold,
    kCurveLinear,
    kCurveEaseIn,
    kCurveEaseOut,
    kCurveSmooth,
    kCurveSCurve,
    kNumAccessibleLfoCurves
  };

  int curveIndexForPower(float power, bool smooth) {
    if (LineGenerator::isHoldPower(power))
      return kCurveHold;
    if (smooth)
      return std::abs(power) <= 0.5f ? kCurveSCurve : kCurveSmooth;
    if (power > 0.5f)
      return kCurveEaseIn;
    if (power < -0.5f)
      return kCurveEaseOut;
    return kCurveLinear;
  }

  float powerForCurveIndex(int index) {
    static const std::array<float, kNumAccessibleLfoCurves> powers = {{
      20.0f, 0.0f, 3.0f, -3.0f, 0.75f, 0.0f
    }};
    return powers[jlimit(0, static_cast<int>(powers.size()) - 1, index)];
  }

  bool smoothForCurveIndex(int index) {
    return index == kCurveSmooth || index == kCurveSCurve;
  }

  String curveNameForIndex(int index) {
    static const std::array<const char*, kNumAccessibleLfoCurves> names = {{
      "Hold", "Linear", "Ease In", "Ease Out", "Smooth", "S Curve"
    }};
    return names[jlimit(0, static_cast<int>(names.size()) - 1, index)];
  }

  class OrderedFocusTraverser : public ComponentTraverser {
    public:
      OrderedFocusTraverser(SynthEditor& editor, bool keyboardTraversal) :
          editor_(editor), keyboard_traversal_(keyboardTraversal) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        const auto& order = keyboard_traversal_ ? editor_.getKeyboardFocusOrder()
                                                : editor_.getAccessibleFocusOrder();
        for (auto* component : order) {
          if (component != nullptr && component->isShowing() && component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return withVisibleComponent(*std::next(found));
        if (direction < 0 && found != components.begin())
          return withVisibleComponent(*std::prev(found));
        return nullptr;
      }

      Component* withVisibleComponent(Component* component) {
        editor_.ensureComponentVisible(component);
        return component;
      }

      SynthEditor& editor_;
      bool keyboard_traversal_ = false;
  };

  class ChildListTraverser : public ComponentTraverser {
    public:
      explicit ChildListTraverser(const std::vector<Component*>& children) : children_(children) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        for (auto* component : children_) {
          if (component != nullptr && component->isShowing() && component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return *std::next(found);
        if (direction < 0 && found != components.begin())
          return *std::prev(found);
        return nullptr;
      }

      const std::vector<Component*>& children_;
  };

  class OffscreenSliderAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenSliderAccessibilityHandler(Slider& sliderToWrap) :
          AccessibilityHandler(sliderToWrap, AccessibilityRole::slider, AccessibilityActions{},
                               AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(sliderToWrap)}),
          slider_(sliderToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getHelp() const override { return slider_.getTooltip(); }

    private:
      class ValueInterface final : public AccessibilityValueInterface {
        public:
          explicit ValueInterface(Slider& sliderToWrap) :
              slider_(sliderToWrap), use_max_value_(sliderToWrap.isTwoValue()) { }

          bool isReadOnly() const override { return false; }

          double getCurrentValue() const override {
            return use_max_value_ ? slider_.getMaximum() : slider_.getValue();
          }

          void setValue(double newValue) override {
            Slider::ScopedDragNotification drag(slider_);
            if (use_max_value_)
              slider_.setMaxValue(newValue, sendNotificationSync);
            else
              slider_.setValue(newValue, sendNotificationSync);
          }

          String getCurrentValueAsString() const override {
            return slider_.getTextFromValue(getCurrentValue());
          }

          void setValueAsString(const String& newValue) override {
            setValue(slider_.getValueFromText(newValue));
          }

          AccessibleValueRange getRange() const override {
            return { { slider_.getMinimum(), slider_.getMaximum() }, slider_.getInterval() };
          }

        private:
          Slider& slider_;
          bool use_max_value_ = false;
      };

      Slider& slider_;
  };

  class OffscreenButtonAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenButtonAccessibilityHandler(Button& buttonToWrap, AccessibilityRole role) :
          AccessibilityHandler(buttonToWrap, role, buttonActions(buttonToWrap), buttonInterfaces(buttonToWrap)),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
        if (button_.isToggleable()) {
          state = state.withCheckable();
          if (button_.getToggleState())
            state = state.withChecked();
        }
        return state;
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getHelp() const override { return button_.getTooltip(); }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(Button& buttonToWrap) : button_(buttonToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override { return button_.getToggleState() ? "On" : "Off"; }
          void setValueAsString(const String&) override { }

        private:
          Button& button_;
      };

      static AccessibilityActions buttonActions(Button& button) {
        auto actions = AccessibilityActions().addAction(AccessibilityActionType::press,
                                                        [&button] { button.triggerClick(); });
        if (button.isToggleable()) {
          actions = actions.addAction(AccessibilityActionType::toggle, [&button] {
            if (button.isEnabled())
              button.setToggleState(!button.getToggleState(), sendNotification);
          });
        }
        return actions;
      }

      static AccessibilityHandler::Interfaces buttonInterfaces(Button& button) {
        if (button.isToggleable())
          return AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(button)};
        return {};
      }

      Button& button_;
  };

  class OffscreenTextButton : public TextButton {
    public:
      using TextButton::TextButton;

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::button);
      }
  };

  class OffscreenComboBox : public ComboBox {
    public:
      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        class Handler final : public AccessibilityHandler {
          public:
            explicit Handler(ComboBox& comboBoxToWrap) :
                AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                                     AccessibilityActions()
                                         .addAction(AccessibilityActionType::press,
                                                    [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                         .addAction(AccessibilityActionType::showMenu,
                                                    [&comboBoxToWrap] { comboBoxToWrap.showPopup(); }),
                                     AccessibilityHandler::Interfaces{
                                         std::make_unique<ValueInterface>(comboBoxToWrap) }),
                combo_box_(comboBoxToWrap) { }

            AccessibleState getCurrentState() const override {
              auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
              return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
            }

            String getHelp() const override { return combo_box_.getTooltip(); }

          private:
            class ValueInterface final : public AccessibilityTextValueInterface {
              public:
                explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

                bool isReadOnly() const override { return true; }
                String getCurrentValueAsString() const override { return combo_box_.getText(); }
                void setValueAsString(const String&) override { }

              private:
                ComboBox& combo_box_;
            };

            ComboBox& combo_box_;
        };

        return std::make_unique<Handler>(*this);
      }
  };

  class OffscreenSlider : public Slider {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void(Component&)> onTextEntryCommand;
      std::function<void()> onDefaultCommand;

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        if (key.getKeyCode() == KeyPress::returnKey) {
          if (onTextEntryCommand)
            onTextEntryCommand(*this);
          return true;
        }
        if (key.getKeyCode() == KeyPress::backspaceKey || key.getKeyCode() == KeyPress::deleteKey) {
          if (onDefaultCommand)
            onDefaultCommand();
          return true;
        }

        const bool increase = key.getKeyCode() == KeyPress::upKey || key.getKeyCode() == KeyPress::rightKey ||
                              key.getKeyCode() == KeyPress::pageUpKey;
        const bool decrease = key.getKeyCode() == KeyPress::downKey || key.getKeyCode() == KeyPress::leftKey ||
                              key.getKeyCode() == KeyPress::pageDownKey;
        if (!increase && !decrease)
          return Slider::keyPressed(key);

        const double range = getMaximum() - getMinimum();
        const double interval = getInterval() > 0.0 ? getInterval() : range / 100.0;
        const double multiplier = (key.getKeyCode() == KeyPress::pageUpKey ||
                                   key.getKeyCode() == KeyPress::pageDownKey) ? 10.0 : 1.0;
        const double delta = interval * multiplier * (increase ? 1.0 : -1.0);
        Slider::ScopedDragNotification drag(*this);
        setValue(jlimit(getMinimum(), getMaximum(), getValue() + delta), sendNotificationSync);
        if (auto* handler = getAccessibilityHandler())
          handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
        return true;
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenSliderAccessibilityHandler>(*this);
      }
  };

  class OffscreenToggleButton : public ToggleButton {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        return ToggleButton::keyPressed(key);
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::toggleButton);
      }
  };
}

std::unique_ptr<AccessibilityHandler> AccessibleTextButton::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(Button& buttonToWrap) :
          AccessibilityHandler(buttonToWrap, AccessibilityRole::button,
                               AccessibilityActions().addAction(AccessibilityActionType::press,
                                                                 [&buttonToWrap] { buttonToWrap.triggerClick(); })),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getHelp() const override { return button_.getTooltip(); }

    private:
      Button& button_;
  };

  return std::make_unique<Handler>(*this);
}

std::unique_ptr<AccessibilityHandler> AccessibleComboBox::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(ComboBox& comboBoxToWrap) :
          AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                               AccessibilityActions()
                                   .addAction(AccessibilityActionType::press,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                   .addAction(AccessibilityActionType::showMenu,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); }),
                               AccessibilityHandler::Interfaces{
                                   std::make_unique<ValueInterface>(comboBoxToWrap) }),
          combo_box_(comboBoxToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
        return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
      }

      String getHelp() const override { return combo_box_.getTooltip(); }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override { return combo_box_.getText(); }
          void setValueAsString(const String&) override { }

        private:
          ComboBox& combo_box_;
      };

      ComboBox& combo_box_;
  };

  return std::make_unique<Handler>(*this);
}

class AccessibleParameterRow : public Component {
  public:
    explicit AccessibleParameterRow(AudioProcessorParameter& parameter, const String& accessibleName = {},
                                    std::function<String(double)> textFromValue = {},
                                    double maxNormalizedValue = 1.0,
                                    double minNormalizedValue = 0.0) :
        parameter_(parameter), text_from_value_(std::move(textFromValue)),
        min_normalized_value_(jlimit(0.0, 1.0, minNormalizedValue)),
        max_normalized_value_(jlimit(min_normalized_value_, 1.0, maxNormalizedValue)) {
      const auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      const auto name = parameter_.getName(128);
      const bool random_rate = parameter_id.startsWith("random_") && parameter_id.endsWith("_frequency");
      const auto accessible_name = accessibleName.isNotEmpty() ? accessibleName : (random_rate ? String("Rate") : name);
      label_.setText(accessible_name, dontSendNotification);
      label_.setTitle(accessible_name + " label");
      label_.setColour(Label::textColourId, Colours::white);
      addAndMakeVisible(label_);

      if (parameter_.isBoolean()) {
        toggle_.setButtonText(accessible_name);
        toggle_.setTitle(accessible_name);
        toggle_.setDescription("Turn " + accessible_name + " on or off");
        toggle_.setHelpText("Press Space to toggle. Press Shift M for modulation, Shift L for MIDI learn, or Shift C to clear MIDI learn.");
        toggle_.setWantsKeyboardFocus(true);
        toggle_.onClick = [this] {
          parameter_.beginChangeGesture();
          parameter_.setValueNotifyingHost(toggle_.getToggleState() ? 1.0f : 0.0f);
          parameter_.endChangeGesture();
        };
        addAndMakeVisible(toggle_);
      }
      else {
        slider_.setSliderStyle(Slider::LinearHorizontal);
        slider_.setTextBoxStyle(Slider::TextBoxRight, false, 150, 28);
        const int steps = parameter_.getNumSteps();
        slider_.setRange(min_normalized_value_, max_normalized_value_,
                         steps > 1 && steps < 10000 ? 1.0 / (steps - 1) : 0.0);
        slider_.setTitle(accessible_name);
        slider_.setDescription(random_rate ? "Adjust free-running random modulation period"
                                           : "Adjust " + accessible_name);
        slider_.setHelpText(random_rate ? "In free mode this controls how long each random cycle lasts. In synced mode use Tempo."
                                        : "Use arrows for changes, Page Up and Page Down for larger changes, Enter to type a value, Backspace to reset to default, Shift M for modulation, Shift L for MIDI learn, and Shift C to clear MIDI learn");
        slider_.setWantsKeyboardFocus(true);
        slider_.textFromValueFunction = [this](double value) {
          if (text_from_value_)
            return text_from_value_(value);
          return parameter_.getText(static_cast<float>(value), 128);
        };
        slider_.valueFromTextFunction = [this](const String& text) {
          return static_cast<double>(parameter_.getValueForText(text));
        };
        slider_.onDragStart = [this] { parameter_.beginChangeGesture(); };
        slider_.onDragEnd = [this] { parameter_.endChangeGesture(); };
        slider_.onValueChange = [this] {
          if (!updating_)
            parameter_.setValueNotifyingHost(static_cast<float>(slider_.getValue()));
        };
        slider_.onDefaultCommand = [this] { resetToDefaultValue(); };
        addAndMakeVisible(slider_);
      }
      refresh();
    }

    void setModulationMenuCallback(std::function<void(const String&, Component&)> callback) {
      modulation_menu_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setMidiLearnCallback(std::function<void(const String&, Component&, bool)> callback) {
      midi_learn_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setValueEntryCallback(std::function<void(const String&, Component&)> callback) {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      slider_.onTextEntryCommand = [parameter_id, callback = std::move(callback)](Component& target) {
        if (parameter_id.isNotEmpty() && callback)
          callback(parameter_id, target);
      };
    }

    void setExtraCommandCallback(std::function<bool(const String&, const KeyPress&, Component&)> callback) {
      extra_command_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void refresh() {
      ScopedValueSetter<bool> guard(updating_, true);
      if (parameter_.isBoolean())
        toggle_.setToggleState(parameter_.getValue() >= 0.5f, dontSendNotification);
      else
        slider_.setValue(jlimit(min_normalized_value_, max_normalized_value_,
                                static_cast<double>(parameter_.getValue())), dontSendNotification);
    }

    Component* focusableControl() {
      return parameter_.isBoolean() ? static_cast<Component*>(&toggle_) : static_cast<Component*>(&slider_);
    }

    String parameterId() const {
      if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
        return bridge->getParameterId();
      return {};
    }

    void setControlFocusOrder(int order) {
      focusableControl()->setExplicitFocusOrder(order);
    }

    void resized() override {
      auto bounds = getLocalBounds().reduced(8, 4);
      label_.setBounds(bounds.removeFromLeft(jmin(240, bounds.getWidth() / 3)));
      if (parameter_.isBoolean())
        toggle_.setBounds(bounds);
      else
        slider_.setBounds(bounds);
    }

  private:
    void resetToDefaultValue() {
      const float default_value = jlimit(static_cast<float>(min_normalized_value_),
                                        static_cast<float>(max_normalized_value_),
                                        parameter_.getDefaultValue());
      parameter_.beginChangeGesture();
      parameter_.setValueNotifyingHost(default_value);
      parameter_.endChangeGesture();
      refresh();
      postPluginAnnouncement(parameter_.getName(128) + " reset to " +
                                             parameter_.getText(parameter_.getValue(), 128),
                                             AccessibilityHandler::AnnouncementPriority::high);
    }

    AudioProcessorParameter& parameter_;
    Label label_;
    OffscreenSlider slider_;
    OffscreenToggleButton toggle_;
    std::function<String(double)> text_from_value_;
    std::function<void(const String&, Component&)> modulation_menu_callback_;
    std::function<void(const String&, Component&, bool)> midi_learn_callback_;
    std::function<bool(const String&, const KeyPress&, Component&)> extra_command_callback_;
    double min_normalized_value_ = 0.0;
    double max_normalized_value_ = 1.0;
    bool updating_ = false;

    void updateAccessibleCommand() {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      auto command = [this, parameter_id](const KeyPress& key, Component& target) {
        if (parameter_id.isEmpty())
          return false;

        if (extra_command_callback_ && extra_command_callback_(parameter_id, key, target))
          return true;

        if (isModulationMenuKey(key) && modulation_menu_callback_) {
          modulation_menu_callback_(parameter_id, target);
          return true;
        }

        if (isMidiLearnKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, false);
          return true;
        }

        if (isMidiClearKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, true);
          return true;
        }

        return false;
      };

      slider_.onAccessibleCommand = command;
      toggle_.onAccessibleCommand = std::move(command);
    }
};

class AccessibleSectionHeader : public Component {
  public:
    AccessibleSectionHeader(const String& title, int groupRank, bool topLevel, const String& sectionKey = {}) :
        title_(title), top_level_(topLevel) {
      setTitle(title_);
      setDescription(top_level_ ? "Control group " + title_ : "Control section " + title_);
      setHelpText(top_level_ ? "Group heading" : "Section heading");
      setAccessible(true);
      setWantsKeyboardFocus(true);
      setFocusContainerType(Component::FocusContainerType::focusContainer);
      getProperties().set("ControlGroup", groupRank);
      if (sectionKey.isNotEmpty())
        getProperties().set("SectionName", sectionKey);
    }

    void paint(Graphics& g) override {
      const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
      g.setColour(top_level_ ? Colour(0xff28313d) : Colour(0xff20262f));
      g.fillRoundedRectangle(bounds, 5.0f);
      g.setColour(Colours::white);
      g.setFont(FontOptions(top_level_ ? 18.0f : 15.0f, top_level_ ? Font::bold : Font::plain));
      g.drawText(title_, getLocalBounds().reduced(top_level_ ? 12 : 22, 0),
                 Justification::centredLeft, true);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class HeaderAccessibilityHandler final : public AccessibilityHandler {
        public:
          explicit HeaderAccessibilityHandler(AccessibleSectionHeader& header) :
              AccessibilityHandler(header, AccessibilityRole::group) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }
      };
      return std::make_unique<HeaderAccessibilityHandler>(*this);
    }

    std::unique_ptr<ComponentTraverser> createFocusTraverser() override {
      return std::make_unique<ChildListTraverser>(child_focus_order_);
    }

    void addAccessibleChild(Component* component) {
      if (component != nullptr)
        child_focus_order_.push_back(component);
    }

    int headerHeight() const { return top_level_ ? 42 : 34; }

  private:
    String title_;
    bool top_level_ = false;
    std::vector<Component*> child_focus_order_;
};

SynthEditor::SynthEditor(SynthPlugin& synth) :
    AudioProcessorEditor(&synth), SynthGuiInterface(&synth, false), synth_(synth) {
  setTitle("Atlas");
  setDescription("Keyboard and VoiceOver accessible editor for Atlas");
  setHelpText("Use Group to choose the editor area, Element to choose the oscillator, filter, envelope, LFO, effect, or routing page, then Tab through visible controls. Press comma or period to move between elements.");
  setWantsKeyboardFocus(true);
  setFocusContainerType(Component::FocusContainerType::keyboardFocusContainer);

  title_.setText("Atlas", dontSendNotification);
  title_.setFont(FontOptions(24.0f, Font::bold));
  title_.setTitle("Atlas");
  title_.setColour(Label::textColourId, Colours::white);
  addAndMakeVisible(title_);

  instructions_.setText("Choose a group and element, then use Tab and arrow keys. Comma and period move between elements. Shift M summarizes modulation.",
                        dontSendNotification);
  instructions_.setTitle("Keyboard instructions");
  instructions_.setColour(Label::textColourId, Colours::white);
  addAndMakeVisible(instructions_);

  menu_button_.setTitle("Editor menu");
  menu_button_.setDescription("Open accessible navigation and routing commands");
  menu_button_.setHelpText("Press Enter to open a menu of sections and common routing commands");
  menu_button_.setWantsKeyboardFocus(true);
  menu_button_.onClick = [this] { showNavigationMenu(); };
  addAndMakeVisible(menu_button_);

  group_selector_.setTitle("Editor group");
  group_selector_.setDescription("Choose a top-level control group such as oscillators, filters, effects, LFOs, or modulation");
  group_selector_.setHelpText("Use Up and Down Arrow to choose a group. The Element picker changes to the controls inside this group.");
  group_selector_.setWantsKeyboardFocus(true);
  group_selector_.onChange = [this] {
    if (updating_navigation_)
      return;
    const String group = group_selector_.getText();
    populateElementSelectorForGroup(group);
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end() && found->second.size() > 0)
      selectSectionByName(found->second[0], true);
  };
  addAndMakeVisible(group_selector_);

  section_selector_.setTitle("Editor element");
  section_selector_.setDescription("Choose the specific element inside the selected group");
  section_selector_.setHelpText("Use Up and Down Arrow to choose an element, then press Tab to reach its visible controls");
  section_selector_.setWantsKeyboardFocus(true);
  section_selector_.onChange = [this] {
    if (updating_navigation_)
      return;
    const String group = group_selector_.getText();
    const int element_index = section_selector_.getSelectedItemIndex();
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end() && isPositiveAndBelow(element_index, found->second.size()))
      selectSectionByName(found->second[element_index], true);
  };
  addAndMakeVisible(section_selector_);

  preset_summary_.setTitle("Preset summary");
  preset_summary_.setDescription("Current preset and Atlas resources path");
  preset_summary_.setColour(Label::textColourId, Colours::white);
  preset_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(preset_summary_);

  preset_menu_.setTitle("Preset menu");
  preset_menu_.setDescription("Open preset actions including load, save, import preset, import bank, and refresh");
  preset_menu_.setHelpText("Press Enter to open preset file actions");
  preset_menu_.setWantsKeyboardFocus(true);
  preset_menu_.onClick = [this] { showPresetMenu(); };
  addChildComponent(preset_menu_);

  preset_bank_.setTitle("Preset bank");
  preset_bank_.setDescription("Filter presets by bank or sound pack");
  preset_bank_.setHelpText("Choose all banks or one installed bank");
  preset_bank_.setWantsKeyboardFocus(true);
  preset_bank_.onChange = [this] {
    if (updating_preset_list_)
      return;
    last_preset_bank = preset_bank_.getText();
    populatePresetFilters();
    filterPresetList();
  };
  addChildComponent(preset_bank_);

  preset_category_.setTitle("Preset category");
  preset_category_.setDescription("Filter presets by category when the bank provides one");
  preset_category_.setHelpText("Choose all categories or one category");
  preset_category_.setWantsKeyboardFocus(true);
  preset_category_.onChange = [this] {
    if (updating_preset_list_)
      return;
    last_preset_category = preset_category_.getText();
    filterPresetList();
  };
  addChildComponent(preset_category_);

  preset_search_.setTitle("Preset search");
  preset_search_.setDescription("Filter presets by name, folder, author, or style");
  preset_search_.setHelpText("Type text to narrow the preset list");
  preset_search_.setTextToShowWhenEmpty("Search presets", Colours::grey);
  preset_search_.setWantsKeyboardFocus(true);
  preset_search_.setText(last_preset_search, false);
  preset_search_.onTextChange = [this] {
    last_preset_search = preset_search_.getText();
    filterPresetList();
  };
  addChildComponent(preset_search_);

  preset_selector_.setTitle("Preset list");
  preset_selector_.setDescription("Factory and user presets from Atlas's resources folder. Press Enter to load.");
  preset_selector_.setHelpText("Choose a preset, then press Enter to load it. If preview is enabled, changing selection loads immediately.");
  preset_selector_.setWantsKeyboardFocus(true);
  preset_selector_.onReturnKey = [this] { loadSelectedPreset(); };
  preset_selector_.onChange = [this] {
    if (updating_preset_list_)
      return;
    const int index = preset_selector_.getSelectedItemIndex();
    if (isPositiveAndBelow(index, filtered_presets_.size()))
      last_preset_path = filtered_presets_[index].getFullPathName();
    if (preset_preview_.getToggleState() && isPositiveAndBelow(index, filtered_presets_.size()))
      loadPresetFile(filtered_presets_[index], true);
  };
  addChildComponent(preset_selector_);

  preset_preview_.setTitle("Preview selected preset");
  preset_preview_.setDescription("Automatically load presets as you move through the preset list");
  preset_preview_.setHelpText("Turn this on to audition presets while browsing. Leave it off to load only when pressing Enter.");
  preset_preview_.setWantsKeyboardFocus(true);
  preset_preview_.setToggleState(last_preset_preview, dontSendNotification);
  preset_preview_.onClick = [this] {
    last_preset_preview = preset_preview_.getToggleState();
    postPluginAnnouncement(last_preset_preview ? "Preset preview enabled" : "Preset preview disabled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
  };
  addChildComponent(preset_preview_);

  preset_name_editor_.setTitle("Preset name");
  preset_name_editor_.setDescription("Name to use when saving into the user preset folder");
  preset_name_editor_.setHelpText("Type a preset name, then press Save to user presets");
  preset_name_editor_.setTextToShowWhenEmpty("Preset name", Colours::grey);
  preset_name_editor_.setWantsKeyboardFocus(true);
  addChildComponent(preset_name_editor_);

  wavetable_name_.setTitle("Current wavetable");
  wavetable_name_.setDescription("Name of the wavetable loaded in the selected oscillator");
  wavetable_name_.setColour(Label::textColourId, Colours::white);
  addChildComponent(wavetable_name_);

  load_wavetable_.setTitle("Load wavetable");
  load_wavetable_.setDescription("Load an Atlas wavetable or audio file into the selected oscillator");
  load_wavetable_.setHelpText("Press Enter, then choose a wavetable, WAV, or FLAC file");
  load_wavetable_.setWantsKeyboardFocus(true);
  load_wavetable_.onClick = [this] { chooseWavetableFile(); };
  addChildComponent(load_wavetable_);

  reset_wavetable_.setTitle("Reset wavetable");
  reset_wavetable_.setDescription("Restore the selected oscillator to the default wavetable");
  reset_wavetable_.setHelpText("Press Enter to restore the default wavetable");
  reset_wavetable_.setWantsKeyboardFocus(true);
  reset_wavetable_.onClick = [this] {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_)) {
      synth_.pauseProcessing(true);
      creator->loadDefaultCreator();
      creator->render();
      synth_.pauseProcessing(false);
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
      postPluginAnnouncement("Default wavetable loaded in oscillator " +
                                               String(active_oscillator_ + 1),
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  };
  addChildComponent(reset_wavetable_);

  oscillator_summary_.setTitle("Oscillator summary");
  oscillator_summary_.setDescription("Musical oscillator controls for wavetable position and tuning");
  oscillator_summary_.setColour(Label::textColourId, Colours::white);
  oscillator_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(oscillator_summary_);

  oscillator_octave_.setTitle("Octave");
  oscillator_octave_.setDescription("Transpose the selected oscillator in octaves");
  oscillator_octave_.setHelpText("Choose an octave offset from minus four to plus four");
  oscillator_octave_.setWantsKeyboardFocus(true);
  for (int octave = -4; octave <= 4; ++octave)
    oscillator_octave_.addItem((octave > 0 ? "+" : "") + String(octave), octave + 5);
  oscillator_octave_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorTransposeFromDirectControls();
  };
  addChildComponent(oscillator_octave_);

  oscillator_semitone_.setTitle("Semitone");
  oscillator_semitone_.setDescription("Transpose the selected oscillator in semitones inside the octave");
  oscillator_semitone_.setHelpText("Choose a semitone offset from minus eleven to plus eleven");
  oscillator_semitone_.setWantsKeyboardFocus(true);
  for (int semitone = -11; semitone <= 11; ++semitone)
    oscillator_semitone_.addItem((semitone > 0 ? "+" : "") + String(semitone), semitone + 12);
  oscillator_semitone_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorTransposeFromDirectControls();
  };
  addChildComponent(oscillator_semitone_);

  oscillator_fine_tune_.setSliderStyle(Slider::LinearHorizontal);
  oscillator_fine_tune_.setTextBoxStyle(Slider::TextBoxRight, false, 96, 28);
  oscillator_fine_tune_.setRange(-100.0, 100.0, 1.0);
  oscillator_fine_tune_.setTitle("Fine tune");
  oscillator_fine_tune_.setDescription("Fine tune the selected oscillator in cents");
  oscillator_fine_tune_.setHelpText("Use Left and Right Arrow to adjust fine tune");
  oscillator_fine_tune_.setTextValueSuffix(" cents");
  oscillator_fine_tune_.setWantsKeyboardFocus(true);
  oscillator_fine_tune_.onValueChange = [this] {
    if (!updating_oscillator_controls_ && active_oscillator_ >= 0)
      setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_tune",
                              static_cast<float>(oscillator_fine_tune_.getValue() / 100.0));
  };
  addChildComponent(oscillator_fine_tune_);

  oscillator_wave_frame_.setSliderStyle(Slider::LinearHorizontal);
  oscillator_wave_frame_.setTextBoxStyle(Slider::TextBoxRight, false, 96, 28);
  oscillator_wave_frame_.setRange(0.0, vital::kNumOscillatorWaveFrames - 1, 1.0);
  oscillator_wave_frame_.setTitle("Wave frame");
  oscillator_wave_frame_.setDescription("Move through the selected oscillator wavetable frames");
  oscillator_wave_frame_.setHelpText("Use Left and Right Arrow to scan the wavetable position");
  oscillator_wave_frame_.setWantsKeyboardFocus(true);
  oscillator_wave_frame_.onValueChange = [this] {
    if (!updating_oscillator_controls_ && active_oscillator_ >= 0)
      setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_wave_frame",
                              static_cast<float>(oscillator_wave_frame_.getValue()));
  };
  addChildComponent(oscillator_wave_frame_);

  oscillator_scale_key_.setTitle("Transpose quantize key");
  oscillator_scale_key_.setDescription("Choose the key used by oscillator transpose quantize");
  oscillator_scale_key_.setHelpText("Choose the root note for transpose quantize");
  oscillator_scale_key_.setWantsKeyboardFocus(true);
  for (int key = 0; key < vital::kNotesPerOctave; ++key)
    oscillator_scale_key_.addItem(transposeKeyName(key), key + 1);
  oscillator_scale_key_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_key_);

  oscillator_scale_type_.setTitle("Transpose quantize scale");
  oscillator_scale_type_.setDescription("Choose the scale used by oscillator transpose quantize");
  oscillator_scale_type_.setHelpText("Choose Off, Chromatic, Major, Minor, Pentatonic, Blues, or a mode");
  oscillator_scale_type_.setWantsKeyboardFocus(true);
  const auto& scale_choices = transposeQuantizeScales();
  for (int i = 0; i < static_cast<int>(scale_choices.size()); ++i)
    oscillator_scale_type_.addItem(scale_choices[i].name, i + 1);
  oscillator_scale_type_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_type_);

  oscillator_scale_mode_.setTitle("Transpose quantize snap mode");
  oscillator_scale_mode_.setDescription("Choose whether transpose quantize snaps relative to played notes or to global pitch classes");
  oscillator_scale_mode_.setHelpText("Local snaps oscillator transpose relative to the played note. Global snaps the final pitch to the selected key and scale.");
  oscillator_scale_mode_.setWantsKeyboardFocus(true);
  oscillator_scale_mode_.addItem("Local", 1);
  oscillator_scale_mode_.addItem("Global", 2);
  oscillator_scale_mode_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_mode_);

  routing_summary_.setTitle("Routing summary");
  routing_summary_.setDescription("Summary of oscillator, sample, filter, and effects routing");
  routing_summary_.setColour(Label::textColourId, Colours::white);
  routing_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(routing_summary_);

  routing_mode_.setTitle("Filter routing mode");
  routing_mode_.setDescription("Choose how filter 1 and filter 2 are connected");
  routing_mode_.setHelpText("Choose parallel filters, filter 1 into filter 2, or filter 2 into filter 1");
  routing_mode_.setWantsKeyboardFocus(true);
  routing_mode_.addItemList({ "Parallel filters", "Filter 1 into filter 2", "Filter 2 into filter 1" }, 1);
  routing_mode_.onChange = [this] {
    if (!updating_routing_controls_)
      applyRoutingPreset(routing_mode_.getSelectedItemIndex());
  };
  addChildComponent(routing_mode_);

  routing_default_.setTitle("Default routing");
  routing_default_.setDescription("Set filters to parallel routing and restore the default oscillator and sample destinations");
  routing_default_.setHelpText("Press Enter to apply default routing");
  routing_default_.setWantsKeyboardFocus(true);
  routing_default_.onClick = [this] { applyRoutingPreset(3); };
  addChildComponent(routing_default_);

  routing_serial_forward_.setTitle("Serial filters 1 into 2");
  routing_serial_forward_.setDescription("Send filter 1 into filter 2");
  routing_serial_forward_.setHelpText("Press Enter to route filter 1 into filter 2");
  routing_serial_forward_.setWantsKeyboardFocus(true);
  routing_serial_forward_.onClick = [this] { applyRoutingPreset(1); };
  addChildComponent(routing_serial_forward_);

  routing_serial_backward_.setTitle("Serial filters 2 into 1");
  routing_serial_backward_.setDescription("Send filter 2 into filter 1");
  routing_serial_backward_.setHelpText("Press Enter to route filter 2 into filter 1");
  routing_serial_backward_.setWantsKeyboardFocus(true);
  routing_serial_backward_.onClick = [this] { applyRoutingPreset(2); };
  addChildComponent(routing_serial_backward_);

  effect_chain_summary_.setTitle("Effects chain order");
  effect_chain_summary_.setDescription("Current order of the effects chain");
  effect_chain_summary_.setColour(Label::textColourId, Colours::white);
  effect_chain_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(effect_chain_summary_);

  effect_chain_selector_.setTitle("Effect position");
  effect_chain_selector_.setDescription("Choose an effect in the current chain order");
  effect_chain_selector_.setHelpText("Choose an effect, then use the move buttons to reorder it in the chain");
  effect_chain_selector_.setWantsKeyboardFocus(true);
  addChildComponent(effect_chain_selector_);

  post_effect_order_selector_.setTitle("Post effect order");
  post_effect_order_selector_.setDescription("Choose the order of the two added post effects");
  post_effect_order_selector_.setHelpText("Choose Frequency Shifter then Limiter, or Limiter then Frequency Shifter");
  post_effect_order_selector_.setWantsKeyboardFocus(true);
  post_effect_order_selector_.addItem("Frequency Shifter then Limiter", 1);
  post_effect_order_selector_.addItem("Limiter then Frequency Shifter", 2);
  post_effect_order_selector_.onChange = [this] {
    const int selected = post_effect_order_selector_.getSelectedItemIndex();
    if (!isPositiveAndBelow(selected, 2))
      return;

    setParameterEngineValue(effectChainPrefixForSection(last_section_name) + "post_effect_order",
                            static_cast<float>(selected));
    refreshEffectChainControls();
    postPluginAnnouncement(post_effect_order_selector_.getText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  };
  addChildComponent(post_effect_order_selector_);

  effect_move_up_.setTitle("Move effect earlier");
  effect_move_up_.setDescription("Move the selected effect one position earlier in the effects chain");
  effect_move_up_.setHelpText("Press Enter to move the selected effect earlier");
  effect_move_up_.setWantsKeyboardFocus(true);
  effect_move_up_.onClick = [this] { moveSelectedEffect(-1); };
  addChildComponent(effect_move_up_);

  effect_move_down_.setTitle("Move effect later");
  effect_move_down_.setDescription("Move the selected effect one position later in the effects chain");
  effect_move_down_.setHelpText("Press Enter to move the selected effect later");
  effect_move_down_.setWantsKeyboardFocus(true);
  effect_move_down_.onClick = [this] { moveSelectedEffect(1); };
  addChildComponent(effect_move_down_);

  modulation_source_.setTitle("Modulation source");
  modulation_source_.setDescription("Choose an envelope, LFO, macro, MIDI source, or other modulation source");
  modulation_source_.setHelpText("Use Up and Down Arrow to choose a source");
  modulation_source_.setWantsKeyboardFocus(true);
  addChildComponent(modulation_source_);

  modulation_destination_group_.setTitle("Destination group");
  modulation_destination_group_.setDescription("Choose a category of modulation destinations");
  modulation_destination_group_.setHelpText("Choose a group first, then choose a destination inside that group");
  modulation_destination_group_.setWantsKeyboardFocus(true);
  modulation_destination_group_.onChange = [this] {
    if (!updating_modulation_destinations_)
      updateModulationDestinationList();
  };
  addChildComponent(modulation_destination_group_);

  modulation_destination_.setTitle("Modulation destination");
  modulation_destination_.setDescription("Choose the synthesizer parameter to modulate inside the selected group");
  modulation_destination_.setHelpText("Choose a destination. Change Destination group to shorten this list.");
  modulation_destination_.setWantsKeyboardFocus(true);
  addChildComponent(modulation_destination_);

  for (const auto& source : synth_.getEngine()->getModulationSources())
    modulation_source_ids_.add(source.first);
  std::vector<String> sorted_sources;
  for (const auto& id : modulation_source_ids_)
    sorted_sources.push_back(id);
  std::stable_sort(sorted_sources.begin(), sorted_sources.end(), [](const String& a, const String& b) {
    const int rank_a = modulationSourceSortRank(a);
    const int rank_b = modulationSourceSortRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return modulationSourceLabelForId(a).compareNatural(modulationSourceLabelForId(b)) < 0;
  });
  modulation_source_ids_.clear();
  for (const auto& id : sorted_sources)
    modulation_source_ids_.add(id);
  for (const auto& id : modulation_source_ids_) {
    const int macro_index = macroIndexForControlId(id);
    const String label = macro_index >= 0 ? synth_.getMacroName(macro_index)
                                          : modulationSourceLabelForId(id);
    modulation_source_.addItem(label, modulation_source_.getNumItems() + 1);
  }

  populateModulationDestinations();

  add_modulation_.setTitle("Connect modulation");
  add_modulation_.setDescription("Connect the selected source to the selected destination");
  add_modulation_.setHelpText("Choose a source and destination, then press Enter");
  add_modulation_.setWantsKeyboardFocus(true);
  add_modulation_.onClick = [this] {
    const int source = modulation_source_.getSelectedItemIndex();
    const int destination = modulation_destination_.getSelectedItemIndex();
    if (!isPositiveAndBelow(source, modulation_source_ids_.size()) ||
        !isPositiveAndBelow(destination, modulation_destination_ids_.size())) {
      postPluginAnnouncement("Choose both a modulation source and destination",
                                               AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    connectModulationAndPromptForAmount(modulation_source_ids_[source], modulation_destination_ids_[destination],
                                        add_modulation_);
  };
  addChildComponent(add_modulation_);

  remove_modulation_.setTitle("Remove selected modulation");
  remove_modulation_.setDescription("Disconnect the selected modulation route");
  remove_modulation_.setHelpText("Select a route in the list, then press Enter");
  remove_modulation_.setWantsKeyboardFocus(true);
  remove_modulation_.onClick = [this] {
    const int row = modulation_list_.getSelectedRow();
    if (isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size()))) {
      synth_.disconnectModulation(modulation_routes_[row]);
      refreshModulationRoutes();
      postPluginAnnouncement("Modulation removed",
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  };
  addChildComponent(remove_modulation_);

  modulation_list_.setTitle("Connected modulations");
  modulation_list_.setDescription("List of modulation source and destination routes");
  modulation_list_.setHelpText("Use Up and Down Arrow to select a route. Its amount and options appear below.");
  modulation_list_.setWantsKeyboardFocus(true);
  modulation_list_.setRowHeight(30);
  addChildComponent(modulation_list_);

  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription("Type the maximum modulation amount from minus one to one");
  modulation_amount_prompt_.setColour(Label::textColourId, Colours::white);
  modulation_amount_prompt_.setJustificationType(Justification::centredLeft);
  addChildComponent(modulation_amount_prompt_);

  modulation_amount_editor_.setTitle("Maximum modulation amount");
  modulation_amount_editor_.setDescription("Maximum modulation amount from minus one to one");
  modulation_amount_editor_.setHelpText("Type a value from minus one to one, then press Return or OK");
  modulation_amount_editor_.setWantsKeyboardFocus(true);
  modulation_amount_editor_.onReturnKey = [this] { applyInlineTextPrompt(); };
  addChildComponent(modulation_amount_editor_);

  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_ok_.setWantsKeyboardFocus(true);
  modulation_amount_ok_.onClick = [this] { applyInlineTextPrompt(); };
  addChildComponent(modulation_amount_ok_);

  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_cancel_.setWantsKeyboardFocus(true);
  modulation_amount_cancel_.onClick = [this] { cancelInlineTextPrompt(); };
  addChildComponent(modulation_amount_cancel_);

  lfo_mseg_summary_.setTitle("Accessible LFO MSEG summary");
  lfo_mseg_summary_.setDescription("Current LFO shape and selected point");
  lfo_mseg_summary_.setColour(Label::textColourId, Colours::white);
  lfo_mseg_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(lfo_mseg_summary_);

  lfo_mseg_lfo_.setTitle("LFO to edit");
  lfo_mseg_lfo_.setDescription("Choose which Vital LFO shape to edit as an accessible MSEG");
  lfo_mseg_lfo_.setHelpText("Choose an LFO, then edit its points below");
  lfo_mseg_lfo_.setWantsKeyboardFocus(true);
  for (int i = 0; i < vital::kNumLfos; ++i)
    lfo_mseg_lfo_.addItem("LFO " + String(i + 1), i + 1);
  lfo_mseg_lfo_.setSelectedItemIndex(0, dontSendNotification);
  lfo_mseg_lfo_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, lfo_mseg_lfo_.getSelectedItemIndex());
    selectSectionByName("LFO " + String(active_lfo_index_ + 1), true);
    refreshLfoMsegControls();
    postPluginAnnouncement("Editing LFO " + String(active_lfo_index_ + 1),
                                           AccessibilityHandler::AnnouncementPriority::medium);
  };
  addChildComponent(lfo_mseg_lfo_);

  lfo_mseg_mode_.setTitle("Mode");
  lfo_mseg_mode_.setDescription("LFO editor mode");
  lfo_mseg_mode_.setHelpText("Loop edits the normal looping Vital LFO. One shot and retrigger are editor labels for compatibility with ModWarp shortcuts.");
  lfo_mseg_mode_.setWantsKeyboardFocus(true);
  lfo_mseg_mode_.addItemList({ "Loop", "One shot", "Retrigger per MIDI note" }, 1);
  lfo_mseg_mode_.setSelectedItemIndex(0, dontSendNotification);
  lfo_mseg_mode_.onChange = [this] {
    if (!updating_lfo_mseg_controls_)
      applyLfoMode();
  };
  addChildComponent(lfo_mseg_mode_);

  lfo_mseg_cycle_.setTitle("Cycle Length");
  lfo_mseg_cycle_.setDescription("Length of the complete LFO shape");
  lfo_mseg_cycle_.setHelpText("Semicolon and apostrophe change cycle length from the keyboard editor");
  lfo_mseg_cycle_.setWantsKeyboardFocus(true);
  lfo_mseg_cycle_.addItemList(msegTimeDivisionChoices(), 1);
  lfo_mseg_cycle_.setSelectedItemIndex(lfo_cycle_index_, dontSendNotification);
  lfo_mseg_cycle_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    lfo_cycle_index_ = jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1,
                              lfo_mseg_cycle_.getSelectedItemIndex());
    applyLfoCycleLength();
    updateLfoMsegSummary();
  };
  addChildComponent(lfo_mseg_cycle_);

  lfo_mseg_grid_.setTitle("Editor Grid");
  lfo_mseg_grid_.setDescription("Keyboard movement size for the LFO editor");
  lfo_mseg_grid_.setHelpText("Minus makes the grid coarser. Equals makes it finer.");
  lfo_mseg_grid_.setWantsKeyboardFocus(true);
  lfo_mseg_grid_.addItemList(msegTimeDivisionChoices(), 1);
  lfo_mseg_grid_.setSelectedItemIndex(lfo_grid_index_, dontSendNotification);
  lfo_mseg_grid_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    lfo_grid_index_ = jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1,
                             lfo_mseg_grid_.getSelectedItemIndex());
    updateLfoMsegSummary();
    postPluginAnnouncement("Grid " + lfo_mseg_grid_.getText() + ". " + lfoMsegStatusText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  };
  addChildComponent(lfo_mseg_grid_);

  lfo_mseg_shape_.setTitle("LFO shape preset");
  lfo_mseg_shape_.setDescription("Choose a standard LFO shape or MSEG starting point");
  lfo_mseg_shape_.setHelpText("Choose a shape, then press Apply shape");
  lfo_mseg_shape_.setWantsKeyboardFocus(true);
  lfo_mseg_shape_.addItemList({ "Custom", "Sine", "Triangle", "Square", "Saw up", "Saw down",
                                "Pulse", "Ramp hold", "Step climb", "Step fall",
                                "Flat 0", "Flat 50", "Flat 100" }, 1);
  lfo_mseg_shape_.setSelectedItemIndex(0, dontSendNotification);
  addChildComponent(lfo_mseg_shape_);

  lfo_mseg_apply_shape_.setTitle("Apply LFO shape");
  lfo_mseg_apply_shape_.setDescription("Replace the selected LFO shape with the selected preset");
  lfo_mseg_apply_shape_.setHelpText("Press Enter to apply the selected shape preset");
  lfo_mseg_apply_shape_.setWantsKeyboardFocus(true);
  lfo_mseg_apply_shape_.onClick = [this] { applyLfoShapePreset(); };
  addChildComponent(lfo_mseg_apply_shape_);

  lfo_mseg_point_.setTitle("LFO point");
  lfo_mseg_point_.setDescription("Choose a point in the selected LFO MSEG");
  lfo_mseg_point_.setHelpText("Choose a point, then use the edit buttons to move it or change its value and curve");
  lfo_mseg_point_.setWantsKeyboardFocus(true);
  lfo_mseg_point_.onChange = [this] {
    if (!updating_lfo_mseg_controls_)
      updateLfoMsegSummary();
  };
  addChildComponent(lfo_mseg_point_);

  auto configure_lfo_button = [this](TextButton& button, const String& title, const String& description) {
    button.setTitle(title);
    button.setDescription(description);
    button.setHelpText("Press Enter to " + description.toLowerCase());
    button.setWantsKeyboardFocus(true);
    addChildComponent(button);
  };
  configure_lfo_button(lfo_mseg_add_point_, "Add MSEG point", "Add a point in the largest gap of the current LFO shape");
  configure_lfo_button(lfo_mseg_delete_point_, "Delete MSEG point", "Delete the selected LFO point");
  configure_lfo_button(lfo_mseg_time_down_, "Point earlier", "Move the selected point earlier in time");
  configure_lfo_button(lfo_mseg_time_up_, "Point later", "Move the selected point later in time");
  configure_lfo_button(lfo_mseg_value_down_, "Value down", "Lower the selected point value");
  configure_lfo_button(lfo_mseg_value_up_, "Value up", "Raise the selected point value");
  lfo_mseg_add_point_.onClick = [this] { addLfoPoint(); };
  lfo_mseg_delete_point_.onClick = [this] { deleteLfoPoint(); };
  lfo_mseg_time_down_.onClick = [this] { moveLfoPointTime(-lfoGridAmount()); };
  lfo_mseg_time_up_.onClick = [this] { moveLfoPointTime(lfoGridAmount()); };
  lfo_mseg_value_down_.onClick = [this] { moveLfoPointValue(-lfoGridAmount()); };
  lfo_mseg_value_up_.onClick = [this] { moveLfoPointValue(lfoGridAmount()); };

  lfo_mseg_curve_.setTitle("Segment curve");
  lfo_mseg_curve_.setDescription("Choose the curve after the selected point");
  lfo_mseg_curve_.setHelpText("This changes how the selected point moves into the next point");
  lfo_mseg_curve_.setWantsKeyboardFocus(true);
  for (int i = 0; i < kNumAccessibleLfoCurves; ++i)
    lfo_mseg_curve_.addItem(curveNameForIndex(i), i + 1);
  lfo_mseg_curve_.setSelectedItemIndex(kCurveLinear, dontSendNotification);
  lfo_mseg_curve_.onChange = [this] {
    if (!updating_lfo_mseg_controls_)
      setLfoPointCurveFromCombo();
  };
  addChildComponent(lfo_mseg_curve_);

  lfo_mseg_smooth_.setTitle("Smooth LFO MSEG");
  lfo_mseg_smooth_.setDescription("Use smooth curves between LFO points");
  lfo_mseg_smooth_.setHelpText("Toggle smoothing for the whole selected LFO shape");
  lfo_mseg_smooth_.setWantsKeyboardFocus(true);
  lfo_mseg_smooth_.onClick = [this] { setLfoSmoothFromToggle(); };
  addChildComponent(lfo_mseg_smooth_);

  lfo_mseg_keyboard_.onKeyPressed = [this](const KeyPress& key) { return handleLfoMsegShortcut(key); };
  lfo_mseg_keyboard_.getStatusText = [this] { return lfoMsegStatusText(); };
  addChildComponent(lfo_mseg_keyboard_);

  viewport_.setTitle("Section controls");
  viewport_.setDescription("Controls for the selected synthesizer section");
  viewport_.setViewedComponent(&rows_container_, false);
  addAndMakeVisible(viewport_);

  buildSections();
  refreshPresetList();
  group_selector_.setVisible(!show_all_sections_);
  section_selector_.setVisible(!show_all_sections_);
  String initial_section = last_section_name;
  if (section_names_.indexOf(initial_section) < 0 && section_names_.size() > 0)
    initial_section = section_names_[0];
  if (show_all_sections_) {
    last_section_name = initial_section;
    showAllSections(false);
  }
  else {
    selectSectionByName(initial_section, false);
  }
  rebuildFocusOrder();
  setResizable(true, true);
  setResizeLimits(680, 480, 1400, 1000);
  setSize(900, 700);
  startTimerHz(20);
}

void SynthEditor::paint(Graphics& graphics) {
  graphics.fillAll(Colour(0xff17191d));
}

void SynthEditor::resized() {
  auto bounds = getLocalBounds().reduced(16);
  title_.setBounds(bounds.removeFromTop(40));
  instructions_.setBounds(bounds.removeFromTop(34));
  auto navigation = bounds.removeFromTop(38);
  menu_button_.setBounds(navigation.removeFromLeft(120).reduced(3));
  if (group_selector_.isVisible())
    group_selector_.setBounds(navigation.removeFromLeft(jmin(260, navigation.getWidth() / 2)).reduced(3));
  if (section_selector_.isVisible())
    section_selector_.setBounds(navigation.removeFromLeft(jmin(360, navigation.getWidth())).reduced(3));
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    bounds.removeFromTop(8);
    modulation_amount_prompt_.setBounds(bounds.removeFromTop(34));
    auto amount_row = bounds.removeFromTop(38);
    modulation_amount_editor_.setBounds(amount_row.removeFromLeft(jmax(180, amount_row.getWidth() - 220)).reduced(3));
    modulation_amount_ok_.setBounds(amount_row.removeFromLeft(100).reduced(3));
    modulation_amount_cancel_.setBounds(amount_row.removeFromLeft(120).reduced(3));
  }
  if (preset_controls_visible_ && !show_all_sections_) {
    bounds.removeFromTop(8);
    preset_summary_.setBounds(bounds.removeFromTop(54));
    auto menu_row = bounds.removeFromTop(38);
    preset_menu_.setBounds(menu_row.removeFromLeft(150).reduced(3));
    preset_bank_.setBounds(menu_row.removeFromLeft(menu_row.getWidth() / 2).reduced(3));
    preset_category_.setBounds(menu_row.reduced(3));
    auto search_row = bounds.removeFromTop(38);
    preset_search_.setBounds(search_row.removeFromLeft(search_row.getWidth() / 2).reduced(3));
    preset_selector_.setBounds(search_row.reduced(3));
    auto save_row = bounds.removeFromTop(38);
    preset_preview_.setBounds(save_row.removeFromLeft(220).reduced(3));
    preset_name_editor_.setBounds(save_row.reduced(3));
  }
  if (wavetable_name_.isVisible()) {
    bounds.removeFromTop(8);
    auto wavetable_bounds = bounds.removeFromTop(38);
    wavetable_name_.setBounds(wavetable_bounds.removeFromLeft(jmax(220, wavetable_bounds.getWidth() - 340)));
    load_wavetable_.setBounds(wavetable_bounds.removeFromLeft(165).reduced(3));
    reset_wavetable_.setBounds(wavetable_bounds.removeFromLeft(165).reduced(3));
    auto direct_summary = bounds.removeFromTop(38);
    oscillator_summary_.setBounds(direct_summary.reduced(3));
    auto direct_controls = bounds.removeFromTop(38);
    oscillator_octave_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 4).reduced(3));
    oscillator_semitone_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 3).reduced(3));
    oscillator_fine_tune_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 2).reduced(3));
    oscillator_wave_frame_.setBounds(direct_controls.reduced(3));
    auto scale_controls = bounds.removeFromTop(38);
    oscillator_scale_key_.setBounds(scale_controls.removeFromLeft(scale_controls.getWidth() / 3).reduced(3));
    oscillator_scale_type_.setBounds(scale_controls.removeFromLeft(scale_controls.getWidth() / 2).reduced(3));
    oscillator_scale_mode_.setBounds(scale_controls.reduced(3));
  }
  if (routing_controls_visible_) {
    bounds.removeFromTop(8);
    if (routing_mode_.getParentComponent() == this) {
      routing_summary_.setBounds(bounds.removeFromTop(54));
      routing_mode_.setBounds(bounds.removeFromTop(38).reduced(3));
      auto buttons = bounds.removeFromTop(38);
      routing_default_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 3).reduced(3));
      routing_serial_forward_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(3));
      routing_serial_backward_.setBounds(buttons.reduced(3));
    }
  }
  if (effect_chain_controls_visible_) {
    bounds.removeFromTop(8);
    if (effect_chain_selector_.getParentComponent() == this) {
      effect_chain_summary_.setBounds(bounds.removeFromTop(54));
      auto controls = bounds.removeFromTop(38);
      effect_chain_selector_.setBounds(controls.removeFromLeft(jmax(220, controls.getWidth() - 360)).reduced(3));
      effect_move_up_.setBounds(controls.removeFromLeft(180).reduced(3));
      effect_move_down_.setBounds(controls.removeFromLeft(180).reduced(3));
    }
  }
  if (modulation_controls_visible_) {
    bounds.removeFromTop(8);
    auto selectors = bounds.removeFromTop(38);
    modulation_source_.setBounds(selectors.removeFromLeft(selectors.getWidth() / 3).reduced(3));
    modulation_destination_group_.setBounds(selectors.removeFromLeft(selectors.getWidth() / 2).reduced(3));
    modulation_destination_.setBounds(selectors.reduced(3));
    auto buttons = bounds.removeFromTop(38);
    add_modulation_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(3));
    remove_modulation_.setBounds(buttons.reduced(3));
    modulation_list_.setBounds(bounds.removeFromTop(jmin(180, bounds.getHeight() / 3)).reduced(3));
  }
  if (lfo_mseg_controls_visible_) {
    bounds.removeFromTop(8);
    auto editor_area = bounds.removeFromTop(jmin(330, jmax(230, bounds.getHeight() / 2)));
    auto controls = editor_area.removeFromLeft(jmin(270, editor_area.getWidth() / 3)).reduced(0, 2);
    editor_area.removeFromLeft(14);
    lfo_mseg_keyboard_.setBounds(editor_area.reduced(3));

    const int row_height = 31;
    const int gap = 7;
    auto placeControl = [&](Component& component) {
      component.setBounds(controls.removeFromTop(row_height).reduced(3));
      controls.removeFromTop(gap);
    };
    placeControl(lfo_mseg_lfo_);
    placeControl(lfo_mseg_mode_);
    placeControl(lfo_mseg_cycle_);
    placeControl(lfo_mseg_grid_);
    placeControl(lfo_mseg_shape_);
    placeControl(lfo_mseg_apply_shape_);
    placeControl(lfo_mseg_point_);
    placeControl(lfo_mseg_curve_);
    placeControl(lfo_mseg_smooth_);

    lfo_mseg_summary_.setBounds(bounds.removeFromTop(44));
    auto edit_row = bounds.removeFromTop(38);
    lfo_mseg_add_point_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 6).reduced(3));
    lfo_mseg_delete_point_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 5).reduced(3));
    lfo_mseg_time_down_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 4).reduced(3));
    lfo_mseg_time_up_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 3).reduced(3));
    lfo_mseg_value_down_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 2).reduced(3));
    lfo_mseg_value_up_.setBounds(edit_row.reduced(3));
  }
  bounds.removeFromTop(10);
  viewport_.setBounds(bounds);
  if (!show_all_sections_)
    rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()),
                            static_cast<int>(rows_.size()) * 48);
}

void SynthEditor::buildSections() {
  sections_.clear();
  section_names_.clear();
  group_sections_.clear();
  group_names_.clear();
  section_selector_.clear(dontSendNotification);
  group_selector_.clear(dontSendNotification);

  sections_[kPresetSection];
  for (auto* parameter : synth_.getParameters()) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter)) {
      parameters_by_id_[bridge->getParameterId()] = bridge;
      sections_[sectionForParameter(bridge->getParameterId())].push_back(parameter);
    }
  }

  for (auto& section : sections_) {
    std::stable_sort(section.second.begin(), section.second.end(), [](auto* a, auto* b) {
      auto* bridge_a = dynamic_cast<ValueBridge*>(a);
      auto* bridge_b = dynamic_cast<ValueBridge*>(b);
      if (bridge_a == nullptr || bridge_b == nullptr)
        return a->getName(128) < b->getName(128);
      const int rank_a = parameterRank(bridge_a->getParameterId());
      const int rank_b = parameterRank(bridge_b->getParameterId());
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return bridge_a->getParameterId() < bridge_b->getParameterId();
    });
  }

  std::vector<String> names;
  for (const auto& section : sections_)
    names.push_back(section.first);
  std::stable_sort(names.begin(), names.end(), [](const String& a, const String& b) {
    const int rank_a = sectionRank(a);
    const int rank_b = sectionRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    section_names_.add(name);
  buildGroups();
}

void SynthEditor::buildGroups() {
  group_sections_.clear();
  group_names_.clear();

  for (const auto& section : section_names_)
    group_sections_[groupForSection(section)].add(section);

  std::vector<String> names;
  for (const auto& group : group_sections_)
    names.push_back(group.first);
  std::stable_sort(names.begin(), names.end(), [](const String& a, const String& b) {
    const int rank_a = groupRank(a);
    const int rank_b = groupRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    group_names_.add(name);

  populateGroupSelector();
}

void SynthEditor::populateGroupSelector(const String& preferredGroup) {
  ScopedValueSetter<bool> guard(updating_navigation_, true);
  group_selector_.clear(dontSendNotification);
  group_selector_.addItemList(group_names_, 1);
  int group_index = group_names_.indexOf(preferredGroup);
  if (group_index < 0 && group_names_.size() > 0)
    group_index = 0;
  if (group_index >= 0)
    group_selector_.setSelectedItemIndex(group_index, dontSendNotification);
}

void SynthEditor::populateElementSelectorForGroup(const String& group, const String& preferredSection) {
  ScopedValueSetter<bool> guard(updating_navigation_, true);
  section_selector_.clear(dontSendNotification);
  const auto found = group_sections_.find(group);
  if (found == group_sections_.end())
    return;

  for (int i = 0; i < found->second.size(); ++i)
    section_selector_.addItem(accessibleSectionTitle(found->second[i]), i + 1);
  int section_index = found->second.indexOf(preferredSection);
  if (section_index < 0 && found->second.size() > 0)
    section_index = 0;
  if (section_index >= 0)
    section_selector_.setSelectedItemIndex(section_index, dontSendNotification);
}

void SynthEditor::selectSectionByName(const String& section, bool announce) {
  const int section_index = section_names_.indexOf(section);
  if (section_index < 0)
    return;

  if (show_all_sections_) {
    last_section_name = section;
    if (row_focus_order_.empty())
      showAllSections(false);
    Component* target = nullptr;
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getProperties()["SectionName"].toString() == section) {
        target = component;
        break;
      }
    }
    if (target == nullptr) {
      const String title = accessibleSectionTitle(section);
      for (auto* component : row_focus_order_) {
        if (component != nullptr && component->getTitle() == title) {
          target = component;
          break;
        }
      }
    }
    if (target == nullptr) {
      const String group = groupForSection(section);
      for (auto* component : row_focus_order_) {
        if (component != nullptr && component->getTitle() == group) {
          target = component;
          break;
        }
      }
    }
    if (target != nullptr) {
      ensureComponentVisible(target);
      target->grabKeyboardFocus();
    }
    if (announce)
      postPluginAnnouncement(accessibleSectionTitle(section),
                                             AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const String group = groupForSection(section);
  {
    ScopedValueSetter<bool> guard(updating_navigation_, true);
    const int group_index = group_names_.indexOf(group);
    if (group_index >= 0 && group_selector_.getSelectedItemIndex() != group_index)
      group_selector_.setSelectedItemIndex(group_index, dontSendNotification);

    section_selector_.clear(dontSendNotification);
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end()) {
      for (int i = 0; i < found->second.size(); ++i)
        section_selector_.addItem(accessibleSectionTitle(found->second[i]), i + 1);
      const int element_index = found->second.indexOf(section);
      if (element_index >= 0)
        section_selector_.setSelectedItemIndex(element_index, dontSendNotification);
    }
  }

  showSection(section_index, announce);
}

std::unique_ptr<AccessibleParameterRow> SynthEditor::createAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                                  const String& sectionName) const {
  auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
  if (bridge == nullptr)
    return std::make_unique<AccessibleParameterRow>(parameter);

  const String suffix = filterSuffixForParameter(sectionName, bridge->getParameterId());
  if (isTransposeQuantizeParameter(bridge->getParameterId())) {
    return std::make_unique<AccessibleParameterRow>(parameter, "Transpose quantize",
                                                    [bridge](double value) {
                                                      const int quantize = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return transposeQuantizeDescription(quantize);
                                                    });
  }

  const String parameter_id = bridge->getParameterId();
  const int macro_index = macroIndexForControlId(parameter_id);
  if (macro_index >= 0) {
    const String macro_name = synth_.getMacroName(macro_index);
    return std::make_unique<AccessibleParameterRow>(parameter, macro_name,
                                                    [this, macro_index, bridge](double value) {
                                                      if (isMacroBipolar(macro_index)) {
                                                        const int percent = roundToInt((value * 2.0 - 1.0) * 100.0);
                                                        return String(percent) + "%";
                                                      }
                                                      return bridge->getText(static_cast<float>(value), 128);
                                                    });
  }

  if ((parameter_id.startsWith("osc_") && parameter_id.endsWith("_destination")) ||
      parameter_id == "sample_destination" || parameter_id == "granular_destination") {
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return routingDestinationText(destination);
                                                    },
                                                    max_normalized);
  }

  if (parameter_id.startsWith("bus_") && parameter_id.endsWith("_destination")) {
    const String number = parameter_id.fromFirstOccurrenceOf("bus_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    const int bus_index = jlimit(0, vital::kNumBuses - 1, number.getIntValue() - 1);
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge, bus_index](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return busRoutingDestinationText(bus_index, destination);
                                                    });
  }

  if (parameter_id == "filter_1_destination" || parameter_id == "filter_2_destination") {
    const double min_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kEffects));
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return routingDestinationText(destination);
                                                    },
                                                    max_normalized, min_normalized);
  }

  if (suffix.isEmpty())
    return std::make_unique<AccessibleParameterRow>(parameter,
                                                    accessibleParameterTitle(sectionName, parameter.getName(128)));

  const String prefix = filterPrefixForSection(sectionName);
  int model = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));

  if (suffix == "model")
    return std::make_unique<AccessibleParameterRow>(parameter, "Filter model");

  if (suffix == "style") {
    const int max_style = jmax(0, numFilterStylesForModel(model) - 1);
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(max_style));
    return std::make_unique<AccessibleParameterRow>(parameter, "Filter style",
                                                    [bridge, model](double value) {
                                                      const int style = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return filterStyleNameForModel(model, style);
                                                    },
                                                    max_normalized);
  }

  if (suffix == "blend") {
    const String name = model == vital::constants::kComb ? "Comb blend"
                                                        : "Filter blend, low pass to high pass";
    return std::make_unique<AccessibleParameterRow>(parameter, name,
                                                    [model](double value) { return filterBlendText(model, value); });
  }

  return std::make_unique<AccessibleParameterRow>(parameter,
                                                  accessibleParameterTitle(sectionName, parameter.getName(128)));
}

bool SynthEditor::shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const {
  auto* bridge = dynamic_cast<ValueBridge*>(parameter);
  if (bridge == nullptr)
    return true;

  if (isMacroBipolarParameterId(bridge->getParameterId()))
    return false;

  const String suffix = filterSuffixForParameter(sectionName, bridge->getParameterId());
  if (suffix.isEmpty())
    return true;

  const String prefix = filterPrefixForSection(sectionName);
  int model = 0;
  int style = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
  if (auto* style_bridge = parameterBridge(prefix + "style"))
    style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));
  if (model == vital::constants::kFormant)
    style = jlimit(0, vital::FormantFilter::kNumFormantStyles - 1, style);

  const bool formant = model == vital::constants::kFormant;
  const bool vocal_tract = formant && style == vital::FormantFilter::kVocalTract;
  const bool comb = model == vital::constants::kComb;

  if (suffix == "cutoff" || suffix == "resonance" || suffix == "keytrack")
    return !formant;
  if (suffix == "drive")
    return !formant && !comb;
  if (suffix == "blend")
    return !formant || vocal_tract;
  if (suffix == "blend_transpose")
    return comb;
  if (suffix == "formant_x" || suffix == "formant_y" ||
      suffix == "formant_resonance" || suffix == "formant_spread")
    return formant;
  if (suffix == "formant_transpose")
    return formant && !vocal_tract;

  return true;
}

String SynthEditor::focusedParameterId() const {
  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl()->hasKeyboardFocus(true))
      return row->parameterId();
  }
  return {};
}

String SynthEditor::focusedAccessibleTitle() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return {};

  const String title = focused->getTitle();
  if (title.isNotEmpty())
    return title;
  return focused->getName();
}

Component* SynthEditor::persistentFocusedComponent() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return nullptr;

  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl() == focused)
      return nullptr;
  }
  return focused;
}

void SynthEditor::restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                           const String& accessibleTitle) {
  Component* target = nullptr;
  if (parameterId.isNotEmpty()) {
    for (const auto& row : rows_) {
      if (row != nullptr && row->parameterId() == parameterId) {
        target = row->focusableControl();
        break;
      }
    }
  }

  if (target == nullptr && persistentComponent != nullptr && persistentComponent->isShowing())
    target = persistentComponent;

  if (target == nullptr && accessibleTitle.isNotEmpty()) {
    for (auto* component : focus_order_) {
      if (component == nullptr || !component->isShowing())
        continue;
      if (component->getTitle() == accessibleTitle || component->getName() == accessibleTitle) {
        target = component;
        break;
      }
    }
  }

  if (target == nullptr)
    return;

  Component::SafePointer<Component> safe_target(target);
  MessageManager::callAsync([this, safe_target] {
    if (safe_target != nullptr && safe_target->isShowing()) {
      ensureComponentVisible(safe_target.getComponent());
      safe_target->grabKeyboardFocus();
    }
  });
}

bool SynthEditor::refreshFilterRowsIfNeeded() {
  bool changed = false;
  for (int filter = 0; filter < 3; ++filter) {
    const String prefix = filter == 2 ? String("filter_fx_") : "filter_" + String(filter + 1) + "_";
    int model = -1;
    int style = -1;
    if (auto* model_bridge = parameterBridge(prefix + "model"))
      model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
    if (auto* style_bridge = parameterBridge(prefix + "style"))
      style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));

    if (last_filter_models_[filter] != model || last_filter_styles_[filter] != style) {
      last_filter_models_[filter] = model;
      last_filter_styles_[filter] = style;
      changed = true;
    }
  }

  if (!changed)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  if (show_all_sections_)
    showAllSections(false);
  else if (last_section_name == "Filter 1" || last_section_name == "Filter 2" || last_section_name == "Filter")
    selectSectionByName(last_section_name, false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);

  return true;
}

void SynthEditor::showSection(int index, bool announce) {
  if (!isPositiveAndBelow(index, section_names_.size()))
    return;

  section_headers_.clear();
  row_focus_order_.clear();
  top_level_accessibility_order_.clear();
  rows_.clear();
  rows_container_.removeAllChildren();
  const auto section_name = section_names_[index];
  last_section_name = section_name;
  setPresetControlsVisible(section_name == kPresetSection);
  configureWavetableActions(section_name);
  setRoutingControlsVisible(section_name == kSignalRoutingSection);
  setEffectChainControlsVisible(isEffectChainSection(section_name));
  setModulationControlsVisible(section_name == "Modulation routing");
  if (section_name.startsWith("LFO ")) {
    active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1);
    lfo_cursor_phase_ = 0.0f;
  }
  setLfoMsegControlsVisible(section_name.startsWith("LFO "));
  int y = 0;
  int focus_order = 10;

  auto addActionButton = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->onClick = std::move(action);
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    y += 44;
  };

  auto addWavetableEditorControls = [&](int oscillator) {
    const int frames = wavetableFrameCount(oscillator);

    auto summary = std::make_unique<Label>();
    auto* summary_ptr = summary.get();
    summary_ptr->setTitle("Wavetable editor");
    summary_ptr->setDescription(wavetableEditorSummary(oscillator));
    summary_ptr->setText(wavetableEditorSummary(oscillator), dontSendNotification);
    summary_ptr->setColour(Label::textColourId, Colours::white);
    summary_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(summary_ptr);
    section_headers_.push_back(std::move(summary));
    y += 44;

    auto scope = std::make_unique<OffscreenComboBox>();
    auto* scope_ptr = scope.get();
    scope_ptr->setTitle("Edit scope");
    scope_ptr->setDescription("Choose whether wavetable edits affect the selected frame or all frames");
    scope_ptr->setHelpText("Choose current frame for precise edits, or all frames for global harmonic changes");
    scope_ptr->addItem("Current frame", 1);
    scope_ptr->addItem("All frames", 2);
    scope_ptr->setSelectedId(1, dontSendNotification);
    scope_ptr->setWantsKeyboardFocus(true);
    scope_ptr->setExplicitFocusOrder(focus_order++);
    scope_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(scope_ptr);
    row_focus_order_.push_back(scope_ptr);
    section_headers_.push_back(std::move(scope));
    y += 44;

    auto addEditorSlider = [&](const String& title, const String& description, double minimum, double maximum,
                               double value, double interval, const String& suffix) {
      auto slider = std::make_unique<OffscreenSlider>();
      auto* slider_ptr = slider.get();
      slider_ptr->setTitle(title);
      slider_ptr->setName(title);
      slider_ptr->setDescription(description);
      slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes");
      slider_ptr->setRange(minimum, maximum, interval);
      slider_ptr->setValue(value, dontSendNotification);
      slider_ptr->setWantsKeyboardFocus(true);
      slider_ptr->setExplicitFocusOrder(focus_order++);
      slider_ptr->textFromValueFunction = [suffix](double slider_value) {
        return String(slider_value, 0) + suffix;
      };
      slider_ptr->onTextEntryCommand = [this, slider_ptr, title](Component& target) {
        promptForCustomValue(title, slider_ptr->getTextFromValue(slider_ptr->getValue()), target,
                             [slider_ptr](const String& text) {
                               Slider::ScopedDragNotification drag(*slider_ptr);
                               slider_ptr->setValue(slider_ptr->getValueFromText(text), sendNotificationSync);
                             });
      };
      slider_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 42);
      rows_container_.addAndMakeVisible(slider_ptr);
      row_focus_order_.push_back(slider_ptr);
      section_headers_.push_back(std::move(slider));
      y += 48;
      return slider_ptr;
    };

    auto* frame_slider = addEditorSlider("Frame", "Selected wavetable frame", 1.0, frames, 1.0, 1.0, "");
    auto* harmonic_slider = addEditorSlider("Harmonic", "Selected harmonic number", 1.0,
                                            vital::WaveFrame::kNumRealComplex - 2, 1.0, 1.0, "");
    auto* level_slider = addEditorSlider("Harmonic level", "Harmonic level as percent", 0.0, 400.0, 100.0, 1.0, "%");
    auto* phase_slider = addEditorSlider("Harmonic phase", "Harmonic phase in degrees", -180.0, 180.0, 0.0, 1.0, " degrees");

    auto selected_frame = [frame_slider] {
      return jmax(0, roundToInt(frame_slider->getValue()) - 1);
    };
    auto all_frames = [scope_ptr] {
      return scope_ptr->getSelectedId() == 2;
    };
    auto selected_harmonic = [harmonic_slider] {
      return roundToInt(harmonic_slider->getValue());
    };

    addActionButton("Set harmonic", "Set or add the selected harmonic at the chosen level and phase",
                    [this, oscillator, selected_frame, all_frames, selected_harmonic, level_slider, phase_slider] {
                      applyWavetableHarmonicEdit(oscillator, selected_frame(), all_frames(), selected_harmonic(),
                                                 static_cast<float>(level_slider->getValue()),
                                                 static_cast<float>(phase_slider->getValue()));
                    });
    addActionButton("Clear harmonic", "Clear the selected harmonic",
                    [this, oscillator, selected_frame, all_frames, selected_harmonic] {
                      clearWavetableHarmonic(oscillator, selected_frame(), all_frames(), selected_harmonic());
                    });
    addActionButton("Remove fundamental", "Remove harmonic 1",
                    [this, oscillator, selected_frame, all_frames] {
                      removeWavetableFundamental(oscillator, selected_frame(), all_frames());
                    });
    addActionButton("Normalize wavetable", "Normalize the current frame or all frames",
                    [this, oscillator, selected_frame, all_frames] {
                      normalizeWavetableFrame(oscillator, selected_frame(), all_frames());
                    });
  };

  if (section_name.startsWith("Oscillator ")) {
    const int oscillator = section_name.fromFirstOccurrenceOf("Oscillator ", false, false).getIntValue() - 1;
    if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
      addActionButton("Load wavetable", "Load a wavetable into " + section_name, [this, oscillator] {
        active_oscillator_ = oscillator;
        chooseWavetableFile();
      });
      addActionButton("Import audio as wavetable splice",
                      "Import an audio file into " + section_name + " using wavetable splice",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kWavetableSplice);
                      });
      addActionButton("Import audio as vocode",
                      "Import an audio file into " + section_name + " using vocode",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kVocoded);
                      });
      addActionButton("Import audio as pitched wavetable",
                      "Import an audio file into " + section_name + " using pitched mode",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kPitched);
                      });
      addActionButton("Browse wavetable folders", "Browse wavetable folders for " + section_name,
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        showWavetableBrowserMenu(oscillator, rows_container_);
                      });
      addActionButton("Previous wavetable", "Load the previous wavetable into " + section_name,
                      [this, oscillator] { loadShiftedWavetable(oscillator, -1); });
      addActionButton("Next wavetable", "Load the next wavetable into " + section_name,
                      [this, oscillator] { loadShiftedWavetable(oscillator, 1); });
      if (wavetable_editor_visible_[oscillator]) {
        addActionButton("Hide wavetable editor", "Hide the wavetable editor controls",
                        [this, oscillator] { setWavetableEditorVisible(oscillator, false); });
        addWavetableEditorControls(oscillator);
      }
      else {
        addActionButton("Show wavetable editor", "Show the wavetable editor controls",
                        [this, oscillator] { setWavetableEditorVisible(oscillator, true); });
      }
    }
  }
  else if (section_name == "Sample") {
    addActionButton("Load sample", "Load an audio sample", [this] { chooseSampleFile(); });
    addActionButton("Browse sample folders", "Browse sample folders",
                    [this] { showSampleBrowserMenu(rows_container_); });
    addActionButton("Previous sample", "Load the previous sample", [this] { loadShiftedSample(-1); });
    addActionButton("Next sample", "Load the next sample", [this] { loadShiftedSample(1); });
  }
  else if (section_name == "Granular") {
    addActionButton("Load sample", "Load an audio sample for Granular",
                    [this] { chooseSampleFile(true); });
    addActionButton("Browse sample folders", "Browse sample folders for Granular",
                    [this] { showSampleBrowserMenu(rows_container_, true); });
    addActionButton("Previous sample", "Load the previous sample into Granular",
                    [this] { loadShiftedSample(-1, true); });
    addActionButton("Next sample", "Load the next sample into Granular",
                    [this] { loadShiftedSample(1, true); });
  }

  if (routing_controls_visible_) {
    refreshRoutingControls();
    routing_summary_.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 54);
    rows_container_.addAndMakeVisible(routing_summary_);
    y += 60;

    auto addRoutingControl = [&](Component& component) {
      component.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
      rows_container_.addAndMakeVisible(component);
      y += 44;
    };

    addRoutingControl(routing_mode_);
    addRoutingControl(routing_default_);
    addRoutingControl(routing_serial_forward_);
    addRoutingControl(routing_serial_backward_);
  }

  for (auto* parameter : sections_[section_name]) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      if (bridge->getParameterId().endsWith("effect_chain_order") ||
          bridge->getParameterId().endsWith("post_effect_order") ||
          bridge->getParameterId().contains("_effect_chain_slot_") ||
          bridge->getParameterId().startsWith("effect_chain_slot_"))
        continue;
    if (!shouldShowParameterInSection(section_name, parameter))
      continue;

    auto row = createAccessibleParameterRow(*parameter, section_name);
    row->setModulationMenuCallback([this](const String& destinationId, Component& target) {
      showModulationSourceMenuForParameter(destinationId, target);
    });
    row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
      if (clear) {
        synth_.clearMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
      else {
        synth_.armMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
    });
    row->setValueEntryCallback([this](const String& parameterId, Component& target) {
      promptForParameterValue(parameterId, target);
    });
    row->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
      return handleMacroShortcut(parameterId, key, target);
    });
    row->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 48);
    row->setExplicitFocusOrder(focus_order++);
    row->setControlFocusOrder(focus_order++);
    rows_container_.addAndMakeVisible(row.get());
    rows_.push_back(std::move(row));
    y += 48;
  }

  if (effect_chain_controls_visible_) {
    refreshEffectChainControls();
    effect_chain_summary_.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 54);
    rows_container_.addAndMakeVisible(effect_chain_summary_);
    y += 60;

    auto addEffectControl = [&](Component& component) {
      component.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
      rows_container_.addAndMakeVisible(component);
      y += 44;
    };

    addEffectControl(effect_chain_selector_);
    addEffectControl(effect_move_up_);
    addEffectControl(effect_move_down_);
  }

  if (routing_controls_visible_)
    refreshRoutingControls();
  if (preset_controls_visible_)
    updatePresetSummary();
  if (modulation_controls_visible_) {
    refreshModulationRoutes();
    showSelectedModulationParameters();
  }
  if (lfo_mseg_controls_visible_) {
    refreshLfoMsegControls();
  }
  rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), y);
  viewport_.setViewPosition(0, 0);
  rebuildFocusOrder();

  if (announce)
    postPluginAnnouncement(accessibleSectionTitle(section_name) + ", " + String(rows_.size()) + " controls",
                                           AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showAllSections(bool announce) {
  section_headers_.clear();
  rows_.clear();
  row_focus_order_.clear();
  top_level_accessibility_order_.clear();
  rows_container_.removeAllChildren();

  setPresetControlsVisible(true);
  configureWavetableActions({});
  setRoutingControlsVisible(false);
  setEffectChainControlsVisible(false);
  setModulationControlsVisible(false);
  setLfoMsegControlsVisible(false);

  int y = 0;
  const int width = jmax(760, viewport_.getMaximumVisibleWidth());
  int focus_order = 10;

  for (const auto& group : group_names_) {
    auto group_component = std::make_unique<AccessibleSectionHeader>(group, groupRank(group), true);
    auto* group_ptr = group_component.get();
    group_ptr->setExplicitFocusOrder(focus_order++);
    rows_container_.addAndMakeVisible(group_ptr);
    row_focus_order_.push_back(group_ptr);
    top_level_accessibility_order_.push_back(group_ptr);
    section_headers_.push_back(std::move(group_component));

    const auto found = group_sections_.find(group);
    if (found == group_sections_.end())
      continue;

    int group_y = group_ptr->headerHeight();
    if (group == kPresetSection) {
      preset_summary_.setBounds(16, group_y, jmax(620, width - 32), 54);
      group_ptr->addAndMakeVisible(preset_summary_);
      group_y += 60;

      auto addPresetControl = [&](Component& component) {
        component.setBounds(16, group_y, jmax(620, width - 32), 38);
        group_ptr->addAndMakeVisible(component);
        group_ptr->addAccessibleChild(&component);
        row_focus_order_.push_back(&component);
        group_y += 44;
      };

      addPresetControl(preset_menu_);
      addPresetControl(preset_bank_);
      addPresetControl(preset_category_);
      addPresetControl(preset_search_);
      addPresetControl(preset_selector_);
      addPresetControl(preset_preview_);
      addPresetControl(preset_name_editor_);
    }

    for (const auto& section_name : found->second) {
      const bool flatten_section = found->second.size() == 1;
      AccessibleSectionHeader* section_ptr = group_ptr;
      int section_y = flatten_section ? group_y : 0;

      if (!flatten_section) {
        auto section_component = std::make_unique<AccessibleSectionHeader>(accessibleSectionTitle(section_name),
                                                                           groupRank(group), false, section_name);
        section_ptr = section_component.get();
        section_ptr->setExplicitFocusOrder(focus_order++);
        group_ptr->addAndMakeVisible(section_ptr);
        group_ptr->addAccessibleChild(section_ptr);
        row_focus_order_.push_back(section_ptr);
        section_headers_.push_back(std::move(section_component));
        section_y = section_ptr->headerHeight();
      }

      if (section_name == "Modulation routing") {
        const int slot_width = jmax(620, width - (flatten_section ? 32 : 34));
        for (int slot = 1; slot <= vital::kMaxModulationConnections; ++slot) {
          const String title = modulationSlotTitle(slot);

          auto slot_component = std::make_unique<AccessibleSectionHeader>(title, groupRank(group), false);
          auto* slot_ptr = slot_component.get();
          slot_ptr->setExplicitFocusOrder(focus_order++);
          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_ptr->headerHeight());
          section_ptr->addAndMakeVisible(slot_ptr);
          section_ptr->addAccessibleChild(slot_ptr);
          row_focus_order_.push_back(slot_ptr);
          section_headers_.push_back(std::move(slot_component));

          int slot_y = slot_ptr->headerHeight();
          const String prefix = "modulation_" + String(slot) + "_";
          for (auto* parameter : sections_[section_name]) {
            auto* bridge = dynamic_cast<ValueBridge*>(parameter);
            if (bridge == nullptr || !bridge->getParameterId().startsWith(prefix))
              continue;

            auto row = std::make_unique<AccessibleParameterRow>(*parameter);
            row->setModulationMenuCallback([this](const String& destinationId, Component& target) {
              showModulationSourceMenuForParameter(destinationId, target);
            });
            row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
              if (clear) {
                synth_.clearMidiLearn(parameterId.toStdString());
                postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                       AccessibilityHandler::AnnouncementPriority::high);
              }
              else {
                synth_.armMidiLearn(parameterId.toStdString());
                postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                                       AccessibilityHandler::AnnouncementPriority::high);
              }
            });
            row->setValueEntryCallback([this](const String& parameterId, Component& target) {
              promptForParameterValue(parameterId, target);
            });
            row->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
              return handleMacroShortcut(parameterId, key, target);
            });
            row->setExplicitFocusOrder(focus_order++);
            row->setControlFocusOrder(focus_order++);
            row->setBounds(10, slot_y, jmax(600, slot_width - 20), 48);
            slot_ptr->addAndMakeVisible(row.get());
            slot_ptr->addAccessibleChild(row->focusableControl());
            row_focus_order_.push_back(row->focusableControl());
            rows_.push_back(std::move(row));
            slot_y += 48;
          }

          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_y);
          section_y += slot_y + 8;
        }

        if (flatten_section)
          group_y = section_y + 8;
        else {
          section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
          group_y += section_y + 8;
        }
        continue;
      }

      auto addActionButton = [&](const String& title, const String& description, std::function<void()> action) {
        auto button = std::make_unique<OffscreenTextButton>(title);
        auto* button_ptr = button.get();
        button_ptr->setTitle(title);
        button_ptr->setDescription(description);
        button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
        button_ptr->setWantsKeyboardFocus(true);
        button_ptr->onClick = std::move(action);
        button_ptr->setExplicitFocusOrder(focus_order++);
        button_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
        section_ptr->addAndMakeVisible(button_ptr);
        section_ptr->addAccessibleChild(button_ptr);
        row_focus_order_.push_back(button_ptr);
        section_headers_.push_back(std::move(button));
        section_y += 44;
      };

      auto addWavetableEditorControls = [&](int oscillator) {
        const int frames = wavetableFrameCount(oscillator);
        const int control_x = flatten_section ? 16 : 10;
        const int control_width = jmax(620, width - (flatten_section ? 32 : 34));

        auto summary = std::make_unique<Label>();
        auto* summary_ptr = summary.get();
        summary_ptr->setTitle("Wavetable editor");
        summary_ptr->setDescription(wavetableEditorSummary(oscillator));
        summary_ptr->setText(wavetableEditorSummary(oscillator), dontSendNotification);
        summary_ptr->setColour(Label::textColourId, Colours::white);
        summary_ptr->setBounds(control_x, section_y, control_width, 38);
        section_ptr->addAndMakeVisible(summary_ptr);
        section_headers_.push_back(std::move(summary));
        section_y += 44;

        auto scope = std::make_unique<OffscreenComboBox>();
        auto* scope_ptr = scope.get();
        scope_ptr->setTitle("Edit scope");
        scope_ptr->setDescription("Choose whether wavetable edits affect the selected frame or all frames");
        scope_ptr->setHelpText("Choose current frame for precise edits, or all frames for global harmonic changes");
        scope_ptr->addItem("Current frame", 1);
        scope_ptr->addItem("All frames", 2);
        scope_ptr->setSelectedId(1, dontSendNotification);
        scope_ptr->setWantsKeyboardFocus(true);
        scope_ptr->setExplicitFocusOrder(focus_order++);
        scope_ptr->setBounds(control_x, section_y, control_width, 38);
        section_ptr->addAndMakeVisible(scope_ptr);
        section_ptr->addAccessibleChild(scope_ptr);
        row_focus_order_.push_back(scope_ptr);
        section_headers_.push_back(std::move(scope));
        section_y += 44;

        auto addEditorSlider = [&](const String& title, const String& description, double minimum, double maximum,
                                   double value, double interval, const String& suffix) {
          auto slider = std::make_unique<OffscreenSlider>();
          auto* slider_ptr = slider.get();
          slider_ptr->setTitle(title);
          slider_ptr->setName(title);
          slider_ptr->setDescription(description);
          slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes");
          slider_ptr->setRange(minimum, maximum, interval);
          slider_ptr->setValue(value, dontSendNotification);
          slider_ptr->setWantsKeyboardFocus(true);
          slider_ptr->setExplicitFocusOrder(focus_order++);
          slider_ptr->textFromValueFunction = [suffix](double slider_value) {
            return String(slider_value, 0) + suffix;
          };
          slider_ptr->onTextEntryCommand = [this, slider_ptr, title](Component& target) {
            promptForCustomValue(title, slider_ptr->getTextFromValue(slider_ptr->getValue()), target,
                                 [slider_ptr](const String& text) {
                                   Slider::ScopedDragNotification drag(*slider_ptr);
                                   slider_ptr->setValue(slider_ptr->getValueFromText(text), sendNotificationSync);
                                 });
          };
          slider_ptr->setBounds(control_x, section_y, control_width, 42);
          section_ptr->addAndMakeVisible(slider_ptr);
          section_ptr->addAccessibleChild(slider_ptr);
          row_focus_order_.push_back(slider_ptr);
          section_headers_.push_back(std::move(slider));
          section_y += 48;
          return slider_ptr;
        };

        auto* frame_slider = addEditorSlider("Frame", "Selected wavetable frame", 1.0, frames, 1.0, 1.0, "");
        auto* harmonic_slider = addEditorSlider("Harmonic", "Selected harmonic number", 1.0,
                                                vital::WaveFrame::kNumRealComplex - 2, 1.0, 1.0, "");
        auto* level_slider = addEditorSlider("Harmonic level", "Harmonic level as percent", 0.0, 400.0, 100.0, 1.0, "%");
        auto* phase_slider = addEditorSlider("Harmonic phase", "Harmonic phase in degrees", -180.0, 180.0, 0.0, 1.0, " degrees");

        auto selected_frame = [frame_slider] {
          return jmax(0, roundToInt(frame_slider->getValue()) - 1);
        };
        auto all_frames = [scope_ptr] {
          return scope_ptr->getSelectedId() == 2;
        };
        auto selected_harmonic = [harmonic_slider] {
          return roundToInt(harmonic_slider->getValue());
        };

        addActionButton("Set harmonic", "Set or add the selected harmonic at the chosen level and phase",
                        [this, oscillator, selected_frame, all_frames, selected_harmonic, level_slider, phase_slider] {
                          applyWavetableHarmonicEdit(oscillator, selected_frame(), all_frames(), selected_harmonic(),
                                                     static_cast<float>(level_slider->getValue()),
                                                     static_cast<float>(phase_slider->getValue()));
                        });
        addActionButton("Clear harmonic", "Clear the selected harmonic",
                        [this, oscillator, selected_frame, all_frames, selected_harmonic] {
                          clearWavetableHarmonic(oscillator, selected_frame(), all_frames(), selected_harmonic());
                        });
        addActionButton("Remove fundamental", "Remove harmonic 1",
                        [this, oscillator, selected_frame, all_frames] {
                          removeWavetableFundamental(oscillator, selected_frame(), all_frames());
                        });
        addActionButton("Normalize wavetable", "Normalize the current frame or all frames",
                        [this, oscillator, selected_frame, all_frames] {
                          normalizeWavetableFrame(oscillator, selected_frame(), all_frames());
                        });
      };

      if (section_name.startsWith("Oscillator ")) {
        const int oscillator = section_name.fromFirstOccurrenceOf("Oscillator ", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
          addActionButton("Load wavetable", "Load a wavetable into " + section_name, [this, oscillator] {
            active_oscillator_ = oscillator;
            chooseWavetableFile();
          });
          addActionButton("Import audio as wavetable splice",
                          "Import an audio file into " + section_name + " using wavetable splice",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kWavetableSplice);
                          });
          addActionButton("Import audio as vocode",
                          "Import an audio file into " + section_name + " using vocode",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kVocoded);
                          });
          addActionButton("Import audio as pitched wavetable",
                          "Import an audio file into " + section_name + " using pitched mode",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kPitched);
                          });
          addActionButton("Browse wavetable folders", "Browse wavetable folders for " + section_name,
                          [this, oscillator, section_ptr] {
                            active_oscillator_ = oscillator;
                            showWavetableBrowserMenu(oscillator, *section_ptr);
                          });
          addActionButton("Previous wavetable", "Load the previous wavetable into " + section_name,
                          [this, oscillator] { loadShiftedWavetable(oscillator, -1); });
          addActionButton("Next wavetable", "Load the next wavetable into " + section_name,
                          [this, oscillator] { loadShiftedWavetable(oscillator, 1); });
          if (wavetable_editor_visible_[oscillator]) {
            addActionButton("Hide wavetable editor", "Hide the wavetable editor controls",
                            [this, oscillator] { setWavetableEditorVisible(oscillator, false); });
            addWavetableEditorControls(oscillator);
          }
          else {
            addActionButton("Show wavetable editor", "Show the wavetable editor controls",
                            [this, oscillator] { setWavetableEditorVisible(oscillator, true); });
          }
        }
      }
      else if (section_name == "Sample") {
        addActionButton("Load sample", "Load an audio sample", [this] { chooseSampleFile(); });
        addActionButton("Browse sample folders", "Browse sample folders",
                        [this, section_ptr] { showSampleBrowserMenu(*section_ptr); });
        addActionButton("Previous sample", "Load the previous sample", [this] { loadShiftedSample(-1); });
        addActionButton("Next sample", "Load the next sample", [this] { loadShiftedSample(1); });
      }
      else if (section_name == "Granular") {
        addActionButton("Load sample", "Load an audio sample for Granular",
                        [this] { chooseSampleFile(true); });
        addActionButton("Browse sample folders", "Browse sample folders for Granular",
                        [this, section_ptr] { showSampleBrowserMenu(*section_ptr, true); });
        addActionButton("Previous sample", "Load the previous sample into Granular",
                        [this] { loadShiftedSample(-1, true); });
        addActionButton("Next sample", "Load the next sample into Granular",
                        [this] { loadShiftedSample(1, true); });
      }
      else if (section_name == kSignalRoutingSection) {
        refreshRoutingControls();
        routing_summary_.setVisible(true);
        routing_mode_.setVisible(true);
        routing_default_.setVisible(true);
        routing_serial_forward_.setVisible(true);
        routing_serial_backward_.setVisible(true);

        routing_summary_.setBounds(flatten_section ? 16 : 10, section_y,
                                   jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(routing_summary_);
        section_y += 60;

        auto addRoutingControl = [&](Component& component) {
          component.setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(component);
          section_ptr->addAccessibleChild(&component);
          row_focus_order_.push_back(&component);
          section_y += 44;
        };

        addRoutingControl(routing_mode_);
        addRoutingControl(routing_default_);
        addRoutingControl(routing_serial_forward_);
        addRoutingControl(routing_serial_backward_);
      }
      else if (section_name.startsWith("LFO ")) {
        const int lfo = section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(lfo, vital::kNumLfos)) {
          auto shape = std::make_unique<OffscreenComboBox>();
          auto* shape_ptr = shape.get();
          shape_ptr->setTitle(section_name + " shape preset");
          shape_ptr->setDescription("Choose a standard shape for " + section_name);
          shape_ptr->setHelpText("Choose a shape, then use the apply shape button");
          shape_ptr->setWantsKeyboardFocus(true);
          shape_ptr->addItemList({ "Custom", "Sine", "Triangle", "Square", "Saw up", "Saw down",
                                   "Pulse", "Ramp hold", "Step climb", "Step fall",
                                   "Flat 0", "Flat 50", "Flat 100" }, 1);
          shape_ptr->setSelectedItemIndex(0, dontSendNotification);
          shape_ptr->setExplicitFocusOrder(focus_order++);
          shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(shape_ptr);
          section_ptr->addAccessibleChild(shape_ptr);
          row_focus_order_.push_back(shape_ptr);
          section_headers_.push_back(std::move(shape));
          section_y += 44;

          auto apply_shape = std::make_unique<OffscreenTextButton>("Apply shape");
          auto* apply_shape_ptr = apply_shape.get();
          apply_shape_ptr->setTitle(section_name + " apply shape");
          apply_shape_ptr->setDescription("Replace " + section_name + " with the selected shape preset");
          apply_shape_ptr->setHelpText("Press Enter to apply the selected shape to " + section_name);
          apply_shape_ptr->setWantsKeyboardFocus(true);
          apply_shape_ptr->onClick = [this, lfo, shape_ptr] {
            active_lfo_index_ = lfo;
            lfo_mseg_shape_.setSelectedItemIndex(shape_ptr->getSelectedItemIndex(), dontSendNotification);
            applyLfoShapePreset();
          };
          apply_shape_ptr->setExplicitFocusOrder(focus_order++);
          apply_shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                     jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(apply_shape_ptr);
          section_ptr->addAccessibleChild(apply_shape_ptr);
          row_focus_order_.push_back(apply_shape_ptr);
          section_headers_.push_back(std::move(apply_shape));
          section_y += 44;

          auto keyboard = std::make_unique<LfoMsegKeyboardComponent>();
          auto* keyboard_ptr = keyboard.get();
          keyboard_ptr->setTitle(section_name + " accessible editor");
          keyboard_ptr->setDescription("Keyboard editor for " + section_name + " points, values, and curves");
          keyboard_ptr->onKeyPressed = [this, lfo](const KeyPress& key) {
            active_lfo_index_ = lfo;
            refreshLfoMsegControls();
            return handleLfoMsegShortcut(key);
          };
          keyboard_ptr->getStatusText = [this, lfo] { return lfoMsegStatusTextFor(lfo); };
          keyboard_ptr->setExplicitFocusOrder(focus_order++);
          keyboard_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                  jmax(620, width - (flatten_section ? 32 : 34)), 74);
          section_ptr->addAndMakeVisible(keyboard_ptr);
          section_ptr->addAccessibleChild(keyboard_ptr);
          row_focus_order_.push_back(keyboard_ptr);
          section_headers_.push_back(std::move(keyboard));
          section_y += 82;
        }
      }

      for (auto* parameter : sections_[section_name]) {
        if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
          if (bridge->getParameterId().endsWith("effect_chain_order") ||
              bridge->getParameterId().endsWith("post_effect_order") ||
              bridge->getParameterId().contains("_effect_chain_slot_") ||
              bridge->getParameterId().startsWith("effect_chain_slot_"))
            continue;
        if (!shouldShowParameterInSection(section_name, parameter))
          continue;

        auto row = createAccessibleParameterRow(*parameter, section_name);
        row->setModulationMenuCallback([this](const String& destinationId, Component& target) {
          showModulationSourceMenuForParameter(destinationId, target);
        });
        row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
          if (clear) {
            synth_.clearMidiLearn(parameterId.toStdString());
            postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                   AccessibilityHandler::AnnouncementPriority::high);
          }
          else {
            synth_.armMidiLearn(parameterId.toStdString());
            postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                                   AccessibilityHandler::AnnouncementPriority::high);
          }
        });
        row->setValueEntryCallback([this](const String& parameterId, Component& target) {
          promptForParameterValue(parameterId, target);
        });
        row->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
          return handleMacroShortcut(parameterId, key, target);
        });
        row->setExplicitFocusOrder(focus_order++);
        row->setControlFocusOrder(focus_order++);
        row->setBounds(flatten_section ? 16 : 10, section_y, jmax(620, width - (flatten_section ? 32 : 34)), 48);
        section_ptr->addAndMakeVisible(row.get());
        section_ptr->addAccessibleChild(row->focusableControl());
        row_focus_order_.push_back(row->focusableControl());
        rows_.push_back(std::move(row));
        section_y += 48;
      }

      if (isEffectChainSection(section_name)) {
        auto summary = std::make_unique<Label>();
        auto* summary_ptr = summary.get();
        summary_ptr->setTitle("Effects chain order");
        summary_ptr->setDescription("Current order of the effects chain");
        summary_ptr->setColour(Label::textColourId, Colours::white);
        summary_ptr->setJustificationType(Justification::centredLeft);
        summary_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(summary_ptr);
        section_ptr->addAccessibleChild(summary_ptr);
        section_headers_.push_back(std::move(summary));
        section_y += 60;

        auto addEffectControl = [&](Component& component) {
          component.setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(component);
          section_ptr->addAccessibleChild(&component);
          row_focus_order_.push_back(&component);
          section_y += 44;
        };

        auto selector = std::make_unique<AccessibleComboBox>();
        auto* selector_ptr = selector.get();
        selector_ptr->setTitle("Effect position");
        selector_ptr->setDescription("Choose an effect in the current chain order");
        selector_ptr->setHelpText("Choose an effect, then use the move buttons to reorder it in the chain");
        selector_ptr->setWantsKeyboardFocus(true);
        selector_ptr->setExplicitFocusOrder(focus_order++);
        populateEffectChainSelector(*selector_ptr, summary_ptr, section_name);
        addEffectControl(*selector_ptr);
        section_headers_.push_back(std::move(selector));

        auto moveLocalEffect = [this, section_name, selector_ptr, summary_ptr](int direction) {
          const int selected = selector_ptr->getSelectedItemIndex();
          const int next = selected + direction;
          if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
              !isPositiveAndBelow(next, vital::constants::kNumEffects))
            return;

          int order[vital::constants::kNumEffects];
          readEffectChainOrder(section_name, order);
          std::swap(order[selected], order[next]);
          const String moved = effectName(order[next]);
          writeEffectChainOrder(section_name, order);
          populateEffectChainSelector(*selector_ptr, summary_ptr, section_name);
          selector_ptr->setSelectedItemIndex(next, dontSendNotification);
          postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        };

        auto move_up = std::make_unique<AccessibleTextButton>("Move effect earlier");
        auto* move_up_ptr = move_up.get();
        move_up_ptr->setTitle("Move effect earlier");
        move_up_ptr->setDescription("Move the selected effect one position earlier in the effects chain");
        move_up_ptr->setHelpText("Press Enter to move the selected effect earlier");
        move_up_ptr->setWantsKeyboardFocus(true);
        move_up_ptr->setExplicitFocusOrder(focus_order++);
        move_up_ptr->onClick = [moveLocalEffect] { moveLocalEffect(-1); };
        addEffectControl(*move_up_ptr);
        section_headers_.push_back(std::move(move_up));

        auto move_down = std::make_unique<AccessibleTextButton>("Move effect later");
        auto* move_down_ptr = move_down.get();
        move_down_ptr->setTitle("Move effect later");
        move_down_ptr->setDescription("Move the selected effect one position later in the effects chain");
        move_down_ptr->setHelpText("Press Enter to move the selected effect later");
        move_down_ptr->setWantsKeyboardFocus(true);
        move_down_ptr->setExplicitFocusOrder(focus_order++);
        move_down_ptr->onClick = [moveLocalEffect] { moveLocalEffect(1); };
        addEffectControl(*move_down_ptr);
        section_headers_.push_back(std::move(move_down));
      }

      if (flatten_section)
        group_y = section_y + 8;
      else {
        section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
        group_y += section_y + 8;
      }
    }
    group_ptr->setBounds(0, y, width, group_y + 8);
    y += group_y + 12;
  }

  rows_container_.setSize(width, y);
  viewport_.setViewPosition(0, 0);
  rebuildFocusOrder();

  if (announce)
    postPluginAnnouncement("All controls visible, grouped like Surge, " +
                                             String(rows_.size()) + " controls",
                                             AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showNavigationMenu() {
  PopupMenu menu;
  menu.addSectionHeader("Groups and elements");
  for (const auto& group : group_names_) {
    PopupMenu group_menu;
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end()) {
      for (int i = 0; i < found->second.size(); ++i) {
        const String section = found->second[i];
        const int section_index = section_names_.indexOf(section);
        if (section_index >= 0)
          group_menu.addItem(1000 + section_index, accessibleSectionTitle(section), true, section == last_section_name);
      }
    }
    menu.addSubMenu(group, group_menu, true);
  }

  menu.addSeparator();
  menu.addSectionHeader("Presets");
  menu.addItem(30, "Go to presets");
  menu.addItem(31, "Load selected preset");
  menu.addItem(32, "Refresh preset list");
  menu.addItem(33, "Open preset file");
  menu.addItem(34, "Import bank");
  menu.addItem(35, "Save to user presets");
  menu.addItem(36, "Save preset as");

  menu.addSeparator();
  menu.addSectionHeader("Routing");
  menu.addItem(10, "Go to signal routing");
  menu.addItem(11, "Apply default routing");
  menu.addItem(12, "Route filters serial, 1 into 2");
  menu.addItem(13, "Route filters serial, 2 into 1");

  menu.addSeparator();
  menu.addSectionHeader("Effects");
  menu.addItem(20, "Go to effects chain");

  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(menu_button_),
                     [this](int result) {
    if (result >= 1000) {
      if (isPositiveAndBelow(result - 1000, section_names_.size()))
        selectSectionByName(section_names_[result - 1000], true);
      return;
    }
    if (result == 10) {
      selectSectionByName(kSignalRoutingSection, true);
      return;
    }
    if (result == 11) {
      applyRoutingPreset(3);
      return;
    }
    if (result == 12) {
      applyRoutingPreset(1);
      return;
    }
    if (result == 13) {
      applyRoutingPreset(2);
      return;
    }
    if (result == 20)
      selectSectionByName(kEffectsChainSection, true);
    if (result == 30)
      selectSectionByName(kPresetSection, true);
    if (result == 31)
      loadSelectedPreset();
    if (result == 32)
      refreshPresetList();
    if (result == 33)
      choosePresetFile();
    if (result == 34)
      chooseBankFile();
    if (result == 35)
      savePresetToUserDirectory();
    if (result == 36)
      savePresetAs();
  });
}

void SynthEditor::rebuildFocusOrder() {
  focus_order_.clear();
  accessibility_focus_order_.clear();
  focus_order_.push_back(&menu_button_);
  accessibility_focus_order_.push_back(&menu_button_);
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    focus_order_.push_back(&modulation_amount_editor_);
    focus_order_.push_back(&modulation_amount_ok_);
    focus_order_.push_back(&modulation_amount_cancel_);
    accessibility_focus_order_.push_back(&modulation_amount_editor_);
    accessibility_focus_order_.push_back(&modulation_amount_ok_);
    accessibility_focus_order_.push_back(&modulation_amount_cancel_);
  }
  if (show_all_sections_) {
    for (auto* component : row_focus_order_)
      focus_order_.push_back(component);
    for (auto* component : top_level_accessibility_order_)
      accessibility_focus_order_.push_back(component);
    return;
  }

  focus_order_.push_back(&group_selector_);
  focus_order_.push_back(&section_selector_);
  if (preset_controls_visible_) {
    focus_order_.push_back(&preset_menu_);
    focus_order_.push_back(&preset_bank_);
    focus_order_.push_back(&preset_category_);
    focus_order_.push_back(&preset_search_);
    focus_order_.push_back(&preset_selector_);
    focus_order_.push_back(&preset_preview_);
    focus_order_.push_back(&preset_name_editor_);
  }
  if (wavetable_name_.isVisible()) {
    focus_order_.push_back(&load_wavetable_);
    focus_order_.push_back(&reset_wavetable_);
    focus_order_.push_back(&oscillator_octave_);
    focus_order_.push_back(&oscillator_semitone_);
    focus_order_.push_back(&oscillator_fine_tune_);
    focus_order_.push_back(&oscillator_wave_frame_);
    focus_order_.push_back(&oscillator_scale_key_);
    focus_order_.push_back(&oscillator_scale_type_);
    focus_order_.push_back(&oscillator_scale_mode_);
  }
  if (routing_controls_visible_) {
    focus_order_.push_back(&routing_mode_);
    focus_order_.push_back(&routing_default_);
    focus_order_.push_back(&routing_serial_forward_);
    focus_order_.push_back(&routing_serial_backward_);
  }
  if (effect_chain_controls_visible_) {
    focus_order_.push_back(&effect_chain_selector_);
    focus_order_.push_back(&effect_move_up_);
    focus_order_.push_back(&effect_move_down_);
  }
  if (modulation_controls_visible_) {
    focus_order_.push_back(&modulation_source_);
    focus_order_.push_back(&modulation_destination_group_);
    focus_order_.push_back(&modulation_destination_);
    focus_order_.push_back(&add_modulation_);
    focus_order_.push_back(&remove_modulation_);
    focus_order_.push_back(&modulation_list_);
  }
  if (lfo_mseg_controls_visible_) {
    focus_order_.push_back(&lfo_mseg_lfo_);
    focus_order_.push_back(&lfo_mseg_mode_);
    focus_order_.push_back(&lfo_mseg_cycle_);
    focus_order_.push_back(&lfo_mseg_grid_);
    focus_order_.push_back(&lfo_mseg_shape_);
    focus_order_.push_back(&lfo_mseg_apply_shape_);
    focus_order_.push_back(&lfo_mseg_keyboard_);
    focus_order_.push_back(&lfo_mseg_point_);
    focus_order_.push_back(&lfo_mseg_add_point_);
    focus_order_.push_back(&lfo_mseg_delete_point_);
    focus_order_.push_back(&lfo_mseg_time_down_);
    focus_order_.push_back(&lfo_mseg_time_up_);
    focus_order_.push_back(&lfo_mseg_value_down_);
    focus_order_.push_back(&lfo_mseg_value_up_);
    focus_order_.push_back(&lfo_mseg_curve_);
    focus_order_.push_back(&lfo_mseg_smooth_);
  }
  for (auto* component : row_focus_order_)
    focus_order_.push_back(component);
  for (auto& row : rows_)
    focus_order_.push_back(row->focusableControl());

  accessibility_focus_order_ = focus_order_;
}

std::unique_ptr<ComponentTraverser> SynthEditor::createFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, false);
}

std::unique_ptr<ComponentTraverser> SynthEditor::createKeyboardFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, true);
}

void SynthEditor::ensureComponentVisible(Component* component) {
  if (component == nullptr)
    return;

  Component* parent = component;
  bool inside_rows = false;
  while (parent != nullptr) {
    if (parent == &rows_container_) {
      inside_rows = true;
      break;
    }
    parent = parent->getParentComponent();
  }
  if (!inside_rows)
    return;

  const auto area = rows_container_.getLocalArea(component, component->getLocalBounds()).expanded(0, 12);
  const int current_y = viewport_.getViewPositionY();
  const int visible_height = viewport_.getViewHeight();
  int target_y = current_y;
  if (area.getY() < current_y)
    target_y = area.getY();
  else if (area.getBottom() > current_y + visible_height)
    target_y = area.getBottom() - visible_height;
  target_y = jlimit(0, jmax(0, rows_container_.getHeight() - visible_height), target_y);
  if (target_y != current_y)
    viewport_.setViewPosition(0, target_y);
}

String SynthEditor::modulationDestinationsForSource(const String& sourceId) const {
  StringArray destinations;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name != sourceId.toStdString() || route->destination_name.empty())
      continue;

    const int index = synth_.getConnectionIndex(route->source_name, route->destination_name);
    float amount = 0.0f;
    if (index >= 0) {
      if (auto* amount_bridge = parameterBridge("modulation_" + String(index + 1) + "_amount"))
        amount = amount_bridge->convertToEngineValue(amount_bridge->getValue());
    }

    const String destination = vital::Parameters::isParameter(route->destination_name)
        ? vital::Parameters::getDisplayName(route->destination_name)
        : readableId(route->destination_name);
    const String amount_text = amount == 0.0f ? String("0")
        : String(amount > 0.0f ? "+" : "") + String(amount, 2);
    destinations.add(destination + " " + amount_text);
  }
  return destinations.joinIntoString(", ");
}

String SynthEditor::modulationSlotTitle(int slot) const {
  String title = "Modulation " + numberWord(slot);
  auto* connection = synth_.getModulationBank().atIndex(slot - 1);
  if (connection == nullptr)
    return title;

  if (!connection->source_name.empty()) {
    const String source = readableId(connection->source_name);
    const String destinations = modulationDestinationsForSource(connection->source_name);
    return destinations.isEmpty() ? title + ", " + source
                                  : title + ", " + source + " (" + destinations + ")";
  }

  if (!connection->destination_name.empty())
    title += " (" + modulationDestinationLabelForId(connection->destination_name) + ")";
  return title;
}

void SynthEditor::announceModulationSummary() {
  std::map<String, StringArray> destinations_by_source;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name.empty() || route->destination_name.empty())
      continue;

    const int index = synth_.getConnectionIndex(route->source_name, route->destination_name);
    float amount = 0.0f;
    if (index >= 0) {
      if (auto* amount_bridge = parameterBridge("modulation_" + String(index + 1) + "_amount"))
        amount = amount_bridge->convertToEngineValue(amount_bridge->getValue());
    }

    const String destination = vital::Parameters::isParameter(route->destination_name)
        ? vital::Parameters::getDisplayName(route->destination_name)
        : readableId(route->destination_name);
    const String amount_text = amount == 0.0f ? String("0")
        : String(amount > 0.0f ? "+" : "") + String(amount, 2);
    destinations_by_source[modulationSourceLabelForId(route->source_name)].add(destination + " " + amount_text);
  }

  if (destinations_by_source.empty()) {
    postPluginAnnouncement("No modulations in this patch",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  StringArray summaries;
  int total_destinations = 0;
  for (const auto& [source, destinations] : destinations_by_source) {
    total_destinations += destinations.size();
    if (summaries.size() < 12)
      summaries.add(source + " to " + destinations.joinIntoString(", "));
  }

  String message = String(destinations_by_source.size()) + " modulation source" +
                   (destinations_by_source.size() == 1 ? ", " : "s, ") +
                   String(total_destinations) + " destination" +
                   (total_destinations == 1 ? ": " : "s: ") +
                   summaries.joinIntoString("; ");
  if (static_cast<int>(destinations_by_source.size()) > summaries.size())
    message += "; plus " + String(static_cast<int>(destinations_by_source.size()) - summaries.size()) +
               " more sources";
  postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setPresetControlsVisible(bool visible) {
  preset_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(preset_summary_);
    addChildComponent(preset_menu_);
    addChildComponent(preset_bank_);
    addChildComponent(preset_category_);
    addChildComponent(preset_search_);
    addChildComponent(preset_selector_);
    addChildComponent(preset_preview_);
    addChildComponent(preset_name_editor_);
  }
  preset_summary_.setVisible(visible);
  preset_menu_.setVisible(visible);
  preset_bank_.setVisible(visible);
  preset_category_.setVisible(visible);
  preset_search_.setVisible(visible);
  preset_selector_.setVisible(visible);
  preset_preview_.setVisible(visible);
  preset_name_editor_.setVisible(visible);
  if (visible)
    updatePresetSummary();
  resized();
}

void SynthEditor::refreshPresetList() {
  all_presets_.clear();
  LoadSave::getAllPresets(all_presets_);
  LoadSave::FileSorterAscending sorter;
  all_presets_.sort(sorter);
  populatePresetFilters();
  filterPresetList();
  updatePresetSummary();
  postPluginAnnouncement("Preset list refreshed, " + String(all_presets_.size()) + " presets found",
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::populatePresetFilters() {
  ScopedValueSetter<bool> guard(updating_preset_list_, true);

  preset_banks_.clear();
  preset_banks_.add(kAllBanks);
  for (const auto& preset : all_presets_) {
    const String bank = presetBankName(preset);
    if (bank.isNotEmpty() && !preset_banks_.contains(bank))
      preset_banks_.add(bank);
  }
  preset_banks_.sort(true);
  preset_banks_.removeString(kAllBanks);
  preset_banks_.insert(0, kAllBanks);

  preset_bank_.clear(dontSendNotification);
  preset_bank_.addItemList(preset_banks_, 1);
  int bank_index = preset_banks_.indexOf(last_preset_bank);
  if (bank_index < 0)
    bank_index = 0;
  preset_bank_.setSelectedItemIndex(bank_index, dontSendNotification);
  last_preset_bank = preset_bank_.getText();

  preset_categories_.clear();
  preset_categories_.add(kAllCategories);
  for (const auto& preset : all_presets_) {
    if (last_preset_bank != kAllBanks && presetBankName(preset) != last_preset_bank)
      continue;
    const String category = presetCategoryName(preset);
    if (category.isNotEmpty() && !preset_categories_.contains(category))
      preset_categories_.add(category);
  }
  preset_categories_.sort(true);
  preset_categories_.removeString(kAllCategories);
  preset_categories_.insert(0, kAllCategories);

  preset_category_.clear(dontSendNotification);
  preset_category_.addItemList(preset_categories_, 1);
  int category_index = preset_categories_.indexOf(last_preset_category);
  if (category_index < 0)
    category_index = 0;
  preset_category_.setSelectedItemIndex(category_index, dontSendNotification);
  last_preset_category = preset_category_.getText();
}

void SynthEditor::filterPresetList() {
  ScopedValueSetter<bool> guard(updating_preset_list_, true);
  filtered_presets_.clear();
  const String filter = preset_search_.getText().trim().toLowerCase();
  const File data_directory = LoadSave::getDataDirectory();
  const String bank_filter = preset_bank_.getText().isEmpty() ? kAllBanks : preset_bank_.getText();
  const String category_filter = preset_category_.getText().isEmpty() ? kAllCategories : preset_category_.getText();

  for (const auto& preset : all_presets_) {
    const String bank = presetBankName(preset);
    const String category = presetCategoryName(preset);
    if (bank_filter != kAllBanks && bank != bank_filter)
      continue;
    if (category_filter != kAllCategories && category != category_filter)
      continue;

    const String searchable = (preset.getFileNameWithoutExtension() + " " +
                               bank + " " + category + " " +
                               preset.getRelativePathFrom(data_directory) + " " +
                               LoadSave::getAuthorFromFile(preset) + " " +
                               LoadSave::getStyleFromFile(preset)).toLowerCase();
    if (filter.isEmpty() || searchable.contains(filter))
      filtered_presets_.add(preset);
  }

  const String previous_path = last_preset_path.isNotEmpty() ? last_preset_path
                             : synth_.getActiveFile().existsAsFile() ? synth_.getActiveFile().getFullPathName()
                             : String();
  preset_selector_.clear(dontSendNotification);
  for (int i = 0; i < filtered_presets_.size(); ++i) {
    const auto& preset = filtered_presets_.getReference(i);
    String label = preset.getFileNameWithoutExtension() + " — " + presetBankName(preset);
    const String category = presetCategoryName(preset);
    if (category.isNotEmpty() && category != "Uncategorized")
      label += " — " + category;
    preset_selector_.addItem(label, i + 1);
  }
  if (!filtered_presets_.isEmpty()) {
    int selected = 0;
    for (int i = 0; i < filtered_presets_.size(); ++i) {
      if (filtered_presets_[i].getFullPathName() == previous_path) {
        selected = i;
        break;
      }
    }
    preset_selector_.setSelectedItemIndex(selected, dontSendNotification);
    last_preset_path = filtered_presets_[selected].getFullPathName();
  }
  updatePresetSummary();
}

void SynthEditor::updatePresetSummary() {
  const String current = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  if (preset_name_editor_.getText().trim().isEmpty() && current != "Untitled")
    preset_name_editor_.setText(current, false);

  const String summary = "Current preset: " + current +
                         ". Atlas resources path: " + LoadSave::getDataDirectory().getFullPathName() +
                         ". " + String(all_presets_.size()) + " presets found, " +
                         String(filtered_presets_.size()) + " shown. Bank: " +
                         (preset_bank_.getText().isEmpty() ? kAllBanks : preset_bank_.getText()) +
                         ". Category: " +
                         (preset_category_.getText().isEmpty() ? kAllCategories : preset_category_.getText()) +
                         ". Preview is " + (preset_preview_.getToggleState() ? "on." : "off.");
  preset_summary_.setText(summary, dontSendNotification);
  preset_summary_.setDescription(summary);
  if (auto* handler = preset_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::showPresetMenu() {
  PopupMenu menu;
  menu.addSectionHeader("Preset actions");
  menu.addItem(1, "Load selected preset");
  menu.addItem(2, "Refresh preset list");
  menu.addSeparator();
  menu.addItem(3, "Open preset file...");
  menu.addItem(4, "Import bank...");
  menu.addSeparator();
  menu.addItem(5, "Save to user presets");
  menu.addItem(6, "Save preset as...");
  menu.addSeparator();
  menu.addItem(7, preset_preview_.getToggleState() ? "Turn preview off" : "Turn preview on");

  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(preset_menu_),
                     [this](int result) {
    if (result == 1)
      loadSelectedPreset();
    else if (result == 2)
      refreshPresetList();
    else if (result == 3)
      choosePresetFile();
    else if (result == 4)
      chooseBankFile();
    else if (result == 5)
      savePresetToUserDirectory();
    else if (result == 6)
      savePresetAs();
    else if (result == 7) {
      preset_preview_.setToggleState(!preset_preview_.getToggleState(), sendNotificationSync);
    }
  });
}

void SynthEditor::selectPresetFile(const File& file) {
  if (!file.existsAsFile())
    return;
  last_preset_path = file.getFullPathName();
  String bank = presetBankName(file);
  if (preset_banks_.contains(bank)) {
    last_preset_bank = bank;
    populatePresetFilters();
  }
  String category = presetCategoryName(file);
  if (preset_categories_.contains(category)) {
    last_preset_category = category;
    preset_category_.setSelectedItemIndex(preset_categories_.indexOf(category), dontSendNotification);
  }
  filterPresetList();
}

void SynthEditor::loadSelectedPreset() {
  const int index = preset_selector_.getSelectedItemIndex();
  if (!isPositiveAndBelow(index, filtered_presets_.size())) {
    postPluginAnnouncement("Choose a preset first", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  loadPresetFile(filtered_presets_[index]);
}

void SynthEditor::choosePresetFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Open Atlas preset", LoadSave::getDataDirectory(),
                                                  vital::kPresetExtensionsList);
  preset_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadPresetFile(file);
    preset_chooser_.reset();
  });
}

void SynthEditor::loadPresetFile(const File& file, bool preview) {
  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  std::string error;
  if (!synth_.loadFromFile(file, error)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load preset", error);
    postPluginAnnouncement("Unable to load preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path = file.getFullPathName();
  synth_.updateHostDisplay();
  updateFullGui();
  selectPresetFile(file);
  updatePresetSummary();
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
  postPluginAnnouncement((preview ? "Previewing preset " : "Loaded preset ") +
                                           file.getFileNameWithoutExtension(),
                                         preview ? AccessibilityHandler::AnnouncementPriority::medium
                                                 : AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::chooseBankFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Import Atlas bank", LoadSave::getDataDirectory(),
                                                  vital::kBankExtensionsList);
  preset_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      importBankFile(file);
    preset_chooser_.reset();
  });
}

void SynthEditor::importBankFile(const File& file) {
  FileInputStream input_stream(file);
  if (!input_stream.openedOk()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                          "The bank file could not be opened.");
    postPluginAnnouncement("Unable to open bank file", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  File data_directory = LoadSave::getDataDirectory();
  data_directory.createDirectory();
  if (!LoadSave::hasDataDirectory())
    LoadSave::saveDataDirectory(data_directory);

  ZipFile import_zip(input_stream);
  const Result unzip_result = import_zip.uncompressTo(data_directory);
  if (unzip_result.wasOk()) {
    LoadSave::markPackInstalled(file.getFileNameWithoutExtension().toStdString());
    refreshPresetList();
    postPluginAnnouncement("Imported bank " + file.getFileNameWithoutExtension() +
                                             ". Preset list refreshed.",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  LoadSave::writeErrorLog("Unzipping bank failed: " + unzip_result.getErrorMessage());
  NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                        unzip_result.getErrorMessage());
  postPluginAnnouncement("Unable to import bank", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::savePresetToUserDirectory() {
  String preset_name = preset_name_editor_.getText().trim();
  if (preset_name.isEmpty())
    preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  preset_name = preset_name.removeCharacters("\\/:*?\"<>|");
  if (preset_name.isEmpty())
    preset_name = "Untitled";

  savePresetFile(LoadSave::getUserPresetDirectory().getChildFile(preset_name));
}

void SynthEditor::savePresetAs() {
  String preset_name = preset_name_editor_.getText().trim();
  if (preset_name.isEmpty())
    preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  preset_chooser_ = std::make_unique<FileChooser>("Save Atlas preset",
                                                  LoadSave::getUserPresetDirectory().getChildFile(preset_name),
                                                  String("*.") + vital::kPresetExtension);
  preset_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                   FileBrowserComponent::warnAboutOverwriting,
                               [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file != File())
      savePresetFile(file);
    preset_chooser_.reset();
  });
}

void SynthEditor::savePresetFile(const File& file) {
  if (!synth_.saveToFile(file)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save preset",
                                          "The preset could not be written to disk.");
    postPluginAnnouncement("Unable to save preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path = synth_.getActiveFile().getFullPathName();
  refreshPresetList();
  selectPresetFile(synth_.getActiveFile());
  updatePresetSummary();
  postPluginAnnouncement("Saved preset " + file.withFileExtension(vital::kPresetExtension).getFileNameWithoutExtension(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::populateModulationDestinations() {
  modulation_destination_all_ids_.clear();
  modulation_destination_groups_.clear();
  modulation_destination_groups_.add("All destinations");

  for (const auto& destination : synth_.getEngine()->getMonoModulationDestinations()) {
    const String id = destination.first;
    if (!vital::Parameters::isParameter(destination.first) || id.startsWith("modulation_") ||
        isMacroBipolarParameterId(id))
      continue;

    modulation_destination_all_ids_.add(id);
    const String group = modulationDestinationGroupForId(id);
    if (group.isNotEmpty() && !modulation_destination_groups_.contains(group))
      modulation_destination_groups_.add(group);
  }

  std::vector<String> sorted_ids;
  for (const auto& id : modulation_destination_all_ids_)
    sorted_ids.push_back(id);
  std::stable_sort(sorted_ids.begin(), sorted_ids.end(), [](const String& a, const String& b) {
    const int rank_a = modulationDestinationSortRank(a);
    const int rank_b = modulationDestinationSortRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return modulationDestinationLabelForId(a).compareNatural(modulationDestinationLabelForId(b)) < 0;
  });
  modulation_destination_all_ids_.clear();
  for (const auto& id : sorted_ids)
    modulation_destination_all_ids_.add(id);

  modulation_destination_groups_.sort(true);
  modulation_destination_groups_.removeString("All destinations");
  modulation_destination_groups_.insert(0, "All destinations");

  ScopedValueSetter<bool> guard(updating_modulation_destinations_, true);
  modulation_destination_group_.clear(dontSendNotification);
  modulation_destination_group_.addItemList(modulation_destination_groups_, 1);
  modulation_destination_group_.setSelectedItemIndex(0, dontSendNotification);
  updateModulationDestinationList();
}

void SynthEditor::updateModulationDestinationList() {
  ScopedValueSetter<bool> guard(updating_modulation_destinations_, true);
  const String selected_group = modulation_destination_group_.getText().isEmpty()
      ? String("All destinations") : modulation_destination_group_.getText();
  const String previous_id = isPositiveAndBelow(modulation_destination_.getSelectedItemIndex(),
                                                modulation_destination_ids_.size())
      ? modulation_destination_ids_[modulation_destination_.getSelectedItemIndex()]
      : String();

  modulation_destination_ids_.clear();
  modulation_destination_.clear(dontSendNotification);
  for (const auto& id : modulation_destination_all_ids_) {
    const String group = modulationDestinationGroupForId(id);
    if (selected_group != "All destinations" && group != selected_group)
      continue;

    modulation_destination_ids_.add(id);
    modulation_destination_.addItem(modulationDestinationLabelForId(id), modulation_destination_.getNumItems() + 1);
  }

  int selected = previous_id.isNotEmpty() ? modulation_destination_ids_.indexOf(previous_id) : 0;
  if (selected < 0)
    selected = 0;
  if (!modulation_destination_ids_.isEmpty())
    modulation_destination_.setSelectedItemIndex(selected, dontSendNotification);

  modulation_destination_.setDescription(selected_group + " destinations, " +
                                         String(modulation_destination_ids_.size()) + " choices");
  if (auto* handler = modulation_destination_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

LineGenerator* SynthEditor::lfoGeneratorForIndex(int lfoIndex) const {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return nullptr;
  return synth_.getLfoSource(lfoIndex);
}

LineGenerator* SynthEditor::activeLfoGenerator() const {
  return lfoGeneratorForIndex(active_lfo_index_);
}

int SynthEditor::selectedLfoPointIndex() const {
  return lfo_mseg_point_.getSelectedItemIndex();
}

float SynthEditor::lfoCycleQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int cycle = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  return jmax(0.03125f, divisions[cycle].quarter_notes);
}

float SynthEditor::lfoGridQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int grid = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_grid_index_);
  return jmax(0.03125f, divisions[grid].quarter_notes);
}

float SynthEditor::lfoGridAmount() const {
  return jlimit(0.001f, 1.0f, lfoGridQuarterNotes() / lfoCycleQuarterNotes());
}

float SynthEditor::lfoPhaseToQuarterNotes(float phase) const {
  return jmax(0.0f, phase) * lfoCycleQuarterNotes();
}

float SynthEditor::lfoQuarterNotesToPhase(float quarterNotes) const {
  return jlimit(0.0f, 1.0f, quarterNotes / lfoCycleQuarterNotes());
}

float SynthEditor::snapLfoPhaseToGrid(float phase) const {
  const float grid = lfoGridQuarterNotes();
  const float quarter_notes = lfoPhaseToQuarterNotes(jlimit(0.0f, 1.0f, phase));
  const float snapped = std::round(quarter_notes / grid) * grid;
  return lfoQuarterNotesToPhase(snapped);
}

String SynthEditor::lfoPointDescription(int pointIndex) const {
  return lfoPointDescriptionFor(active_lfo_index_, pointIndex);
}

String SynthEditor::lfoPointDescriptionFor(int lfoIndex, int pointIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr || !isPositiveAndBelow(pointIndex, generator->getNumPoints()))
    return {};

  const auto point = generator->getPoint(pointIndex);
  const float power = pointIndex < generator->getNumPoints() - 1
      ? generator->getPower(pointIndex)
      : generator->getPower(jmax(0, pointIndex - 1));
  return lfoTimeDescription(point.first) +
         ", value " + percentString(1.0f - point.second) +
         ", curve " + curveNameForIndex(curveIndexForPower(power, generator->smooth()));
}

String SynthEditor::lfoTimeDescription(float phase) const {
  const float quarter_note = lfoPhaseToQuarterNotes(phase);
  const int bar = static_cast<int>(std::floor(quarter_note / 4.0f)) + 1;
  const float within_bar = quarter_note - static_cast<float>(bar - 1) * 4.0f;
  const int beat = static_cast<int>(std::floor(within_bar)) + 1;
  const int percent = jlimit(0, 99, roundToInt((within_bar - std::floor(within_bar)) * 100.0f));
  return "Bar " + String(bar) + " beat " + String(beat) + " " + String(percent) + "%";
}

String SynthEditor::lfoMsegStatusText() const {
  return lfoMsegStatusTextFor(active_lfo_index_);
}

String SynthEditor::lfoMsegStatusTextFor(int lfoIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr)
    return "LFO editor unavailable";
  const int selected = lfoIndex == active_lfo_index_ ? selectedLfoPointIndex() : 0;
  if (isPositiveAndBelow(selected, generator->getNumPoints()))
    return lfoPointDescriptionFor(lfoIndex, selected) + ", grid " + String(msegTimeDivisions()[lfo_grid_index_].name);
  return lfoTimeDescription(lfo_cursor_phase_) + ", value " +
         percentString(generator->valueAtPhase(lfo_cursor_phase_)) + ", no point, grid " +
         String(msegTimeDivisions()[lfo_grid_index_].name);
}

void SynthEditor::applyLfoMode() {
  if (active_lfo_index_ < 0)
    return;

  const int mode = jlimit(0, 2, lfo_mseg_mode_.getSelectedItemIndex());
  const int vital_mode = mode == 0 ? vital::SynthLfo::kSync
                       : mode == 1 ? vital::SynthLfo::kEnvelope
                                   : vital::SynthLfo::kTrigger;
  setParameterEngineValue("lfo_" + String(active_lfo_index_ + 1) + "_sync_type",
                          static_cast<float>(vital_mode));
  postPluginAnnouncement("LFO " + String(active_lfo_index_ + 1) +
                                         " mode " + lfo_mseg_mode_.getText(),
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::applyLfoCycleLength() {
  if (active_lfo_index_ < 0)
    return;

  const auto& divisions = msegTimeDivisions();
  const int index = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  const int tempo_index = closestVitalTempoIndex(divisions[index].quarter_notes);
  const String prefix = "lfo_" + String(active_lfo_index_ + 1) + "_";
  setParameterEngineValue(prefix + "sync", vital::SynthLfo::kTempo);
  setParameterEngineValue(prefix + "tempo", static_cast<float>(tempo_index));
  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  postPluginAnnouncement("Cycle length " + String(divisions[index].name) + ". " + lfoMsegStatusText(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setLfoMsegControlsVisible(bool visible) {
  lfo_mseg_controls_visible_ = visible;
  lfo_mseg_summary_.setVisible(visible);
  lfo_mseg_lfo_.setVisible(visible);
  lfo_mseg_mode_.setVisible(visible);
  lfo_mseg_cycle_.setVisible(visible);
  lfo_mseg_grid_.setVisible(visible);
  lfo_mseg_shape_.setVisible(visible);
  lfo_mseg_apply_shape_.setVisible(visible);
  lfo_mseg_point_.setVisible(visible);
  lfo_mseg_add_point_.setVisible(visible);
  lfo_mseg_delete_point_.setVisible(visible);
  lfo_mseg_time_down_.setVisible(visible);
  lfo_mseg_time_up_.setVisible(visible);
  lfo_mseg_value_down_.setVisible(visible);
  lfo_mseg_value_up_.setVisible(visible);
  lfo_mseg_curve_.setVisible(visible);
  lfo_mseg_smooth_.setVisible(visible);
  lfo_mseg_keyboard_.setVisible(visible);
  if (visible)
    refreshLfoMsegControls();
  resized();
}

void SynthEditor::refreshLfoMsegControls() {
  ScopedValueSetter<bool> guard(updating_lfo_mseg_controls_, true);
  active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, active_lfo_index_);
  lfo_mseg_lfo_.setSelectedItemIndex(active_lfo_index_, dontSendNotification);
  if (auto* mode_bridge = parameterBridge("lfo_" + String(active_lfo_index_ + 1) + "_sync_type")) {
    const int vital_mode = roundToInt(mode_bridge->convertToEngineValue(mode_bridge->getValue()));
    const int editor_mode = vital_mode == vital::SynthLfo::kEnvelope ? 1
                          : vital_mode == vital::SynthLfo::kTrigger ? 2 : 0;
    lfo_mseg_mode_.setSelectedItemIndex(editor_mode, dontSendNotification);
  }
  lfo_mseg_cycle_.setSelectedItemIndex(jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1, lfo_cycle_index_),
                                       dontSendNotification);
  lfo_mseg_grid_.setSelectedItemIndex(jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1, lfo_grid_index_),
                                      dontSendNotification);
  if (auto* generator = activeLfoGenerator()) {
    lfo_mseg_smooth_.setToggleState(generator->smooth(), dontSendNotification);
    updateLfoPointSelector();
  }
  updateLfoMsegSummary();
}

void SynthEditor::updateLfoPointSelector() {
  auto* generator = activeLfoGenerator();
  const int previous = selectedLfoPointIndex();
  lfo_mseg_point_.clear(dontSendNotification);
  if (generator == nullptr)
    return;

  for (int i = 0; i < generator->getNumPoints(); ++i)
    lfo_mseg_point_.addItem(lfoPointDescription(i), i + 1);

  if (generator->getNumPoints() > 0)
    lfo_mseg_point_.setSelectedItemIndex(jlimit(0, generator->getNumPoints() - 1, previous), dontSendNotification);
}

void SynthEditor::updateLfoMsegSummary() {
  auto* generator = activeLfoGenerator();
  String summary = "Accessible MSEG editor unavailable";
  if (generator) {
    const int point_index = selectedLfoPointIndex();
    summary = "Editing LFO " + String(active_lfo_index_ + 1) +
              ", " + String(generator->getNumPoints()) + " points";
    if (isPositiveAndBelow(point_index, generator->getNumPoints()))
      summary += ". " + lfoPointDescription(point_index);
    summary += generator->smooth() ? ". Smooth curves on." : ". Smooth curves off.";

    ScopedValueSetter<bool> guard(updating_lfo_mseg_controls_, true);
    const int curve_point = isPositiveAndBelow(point_index, generator->getNumPoints() - 1)
        ? point_index : jmax(0, point_index - 1);
    lfo_mseg_curve_.setSelectedItemIndex(curveIndexForPower(generator->getPower(curve_point), generator->smooth()),
                                         dontSendNotification);
    lfo_mseg_smooth_.setToggleState(generator->smooth(), dontSendNotification);
  }

  lfo_mseg_summary_.setText(summary, dontSendNotification);
  lfo_mseg_summary_.setDescription(summary);
  if (auto* handler = lfo_mseg_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  lfo_mseg_keyboard_.repaint();
}

void SynthEditor::moveLfoCursor(float delta) {
  lfo_cursor_phase_ = snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, lfo_cursor_phase_ + delta));
  auto* generator = activeLfoGenerator();
  if (generator) {
    int found = -1;
    for (int i = 0; i < generator->getNumPoints(); ++i) {
      if (std::abs(generator->getPoint(i).first - lfo_cursor_phase_) <= 0.0005f) {
        found = i;
        break;
      }
    }
    if (found >= 0)
      lfo_mseg_point_.setSelectedItemIndex(found, dontSendNotification);
  }
  updateLfoMsegSummary();
  postPluginAnnouncement(lfoMsegStatusText(), AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::moveToLfoPoint(int direction) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0)
    return;
  const int next = jlimit(0, generator->getNumPoints() - 1, selectedLfoPointIndex() + direction);
  lfo_mseg_point_.setSelectedItemIndex(next, sendNotificationSync);
  lfo_cursor_phase_ = generator->getPoint(next).first;
  postPluginAnnouncement(lfoMsegStatusText(), AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::stepLfoShapePreset(int direction) {
  const int count = jmax(1, lfo_mseg_shape_.getNumItems());
  const int next = jlimit(0, count - 1, lfo_mseg_shape_.getSelectedItemIndex() + direction);
  lfo_mseg_shape_.setSelectedItemIndex(next, dontSendNotification);
  postPluginAnnouncement("Shape preset " + lfo_mseg_shape_.getText(),
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::stepLfoCycleLength(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_cycle_index_ = jlimit(0, count - 1, lfo_cycle_index_ + direction);
  lfo_mseg_cycle_.setSelectedItemIndex(lfo_cycle_index_, dontSendNotification);
  applyLfoCycleLength();
  updateLfoMsegSummary();
}

void SynthEditor::stepLfoGrid(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_grid_index_ = jlimit(0, count - 1, lfo_grid_index_ + direction);
  lfo_mseg_grid_.setSelectedItemIndex(lfo_grid_index_, dontSendNotification);
  updateLfoMsegSummary();
  postPluginAnnouncement("Grid " + lfo_mseg_grid_.getText() + ". " + lfoMsegStatusText(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  synth_.pauseProcessing(true);
  generator->setNumPoints(2);
  generator->setPoint(0, { 0.0f, 0.5f });
  generator->setPoint(1, { 1.0f, 0.5f });
  generator->setPower(0, 0.0f);
  generator->setPower(1, 0.0f);
  generator->setSmooth(false);
  generator->render();
  synth_.pauseProcessing(false);
  lfo_cursor_phase_ = 0.0f;
  refreshLfoMsegControls();
  postPluginAnnouncement("Cleared LFO " + String(active_lfo_index_ + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

bool SynthEditor::handleLfoMsegShortcut(const KeyPress& key) {
  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = key.getModifiers().isShiftDown();
  const bool command = key.getModifiers().isCommandDown();

  if (command && (key == KeyPress::upKey || key == KeyPress::downKey)) {
    lfo_mseg_curve_.setSelectedItemIndex((lfo_mseg_curve_.getSelectedItemIndex() + (key == KeyPress::upKey ? 1 : -1) +
                                          lfo_mseg_curve_.getNumItems()) % jmax(1, lfo_mseg_curve_.getNumItems()),
                                         sendNotificationSync);
    return true;
  }
  if (shift && (key == KeyPress::leftKey || key == KeyPress::rightKey)) {
    moveLfoPointTime(key == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (key == KeyPress::leftKey || key == KeyPress::rightKey) {
    moveLfoCursor(key == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (key == KeyPress::upKey || key == KeyPress::downKey) {
    moveLfoPointValue(key == KeyPress::upKey ? (shift ? 0.05f : 0.01f) : (shift ? -0.05f : -0.01f));
    return true;
  }
  if (key.getKeyCode() == KeyPress::backspaceKey) {
    clearLfoShape();
    return true;
  }
  if (shift && character == 'a') {
    applyLfoShapePreset();
    return true;
  }
  if (character == 'a') {
    addLfoPoint();
    return true;
  }
  if (character == 'e' || key.getKeyCode() == KeyPress::deleteKey) {
    deleteLfoPoint();
    return true;
  }
  if (character == '[' || character == '{') {
    moveLfoPointValue(shift ? -0.05f : -0.01f);
    return true;
  }
  if (character == ']' || character == '}') {
    moveLfoPointValue(shift ? 0.05f : 0.01f);
    return true;
  }
  if (character == 'i' || character == 'k') {
    const int direction = character == 'i' ? 1 : -1;
    lfo_mseg_curve_.setSelectedItemIndex((lfo_mseg_curve_.getSelectedItemIndex() + direction +
                                          lfo_mseg_curve_.getNumItems()) % jmax(1, lfo_mseg_curve_.getNumItems()),
                                         sendNotificationSync);
    return true;
  }
  if (character == 'j' || character == 'l') {
    moveLfoPointTime(character == 'l' ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (character == ',') {
    moveToLfoPoint(-1);
    return true;
  }
  if (character == '.') {
    moveToLfoPoint(1);
    return true;
  }
  if (character == 'g' || character == 'h') {
    stepLfoShapePreset(character == 'h' ? 1 : -1);
    return true;
  }
  if (character == ';' || character == '\'') {
    stepLfoCycleLength(character == '\'' ? 1 : -1);
    return true;
  }
  if (character == '-' || character == '_') {
    stepLfoGrid(-1);
    return true;
  }
  if (character == '=' || character == '+') {
    stepLfoGrid(1);
    return true;
  }
  if (character == 'c') {
    copied_lfo_points_.clear();
    if (auto* generator = activeLfoGenerator()) {
      const int index = selectedLfoPointIndex();
      if (isPositiveAndBelow(index, generator->getNumPoints())) {
        copied_lfo_points_.push_back(generator->getPoint(index));
        postPluginAnnouncement("Copied 1 point",
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
    }
    return true;
  }
  if (character == 'v') {
    if (!copied_lfo_points_.empty()) {
      lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
      if (auto* generator = activeLfoGenerator()) {
        if (generator->getNumPoints() >= LineGenerator::kMaxPoints) {
          postPluginAnnouncement("No more LFO points can be added",
                                                 AccessibilityHandler::AnnouncementPriority::high);
          return true;
        }
        int insert_index = generator->getNumPoints();
        for (int i = 0; i < generator->getNumPoints(); ++i) {
          if (std::abs(generator->getPoint(i).first - lfo_cursor_phase_) <= 0.0005f) {
            insert_index = i;
            break;
          }
          if (generator->getPoint(i).first > lfo_cursor_phase_) {
            insert_index = i;
            break;
          }
        }

        const auto copied = copied_lfo_points_.front();
        synth_.pauseProcessing(true);
        if (insert_index < generator->getNumPoints() &&
            std::abs(generator->getPoint(insert_index).first - lfo_cursor_phase_) <= 0.0005f) {
          generator->setPoint(insert_index, { lfo_cursor_phase_, copied.second });
        }
        else {
          generator->addPoint(insert_index, { lfo_cursor_phase_, copied.second });
        }
        generator->render();
        synth_.pauseProcessing(false);
        refreshLfoMsegControls();
        lfo_mseg_point_.setSelectedItemIndex(jlimit(0, generator->getNumPoints() - 1, insert_index),
                                             dontSendNotification);
        updateLfoMsegSummary();
        if (isPositiveAndBelow(insert_index, generator->getNumPoints())) {
          postPluginAnnouncement("Pasted 1 point at " + lfoTimeDescription(lfo_cursor_phase_),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        }
      }
    }
    else {
      postPluginAnnouncement("No points copied", AccessibilityHandler::AnnouncementPriority::high);
    }
    return true;
  }
  return false;
}

void SynthEditor::applyLfoShapePreset() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  synth_.pauseProcessing(true);
  const int shape = lfo_mseg_shape_.getSelectedItemIndex();
  if (shape == 0) {
    synth_.pauseProcessing(false);
    postPluginAnnouncement("Custom shape selected. No change applied.",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }
  if (shape == 1)
    generator->initSin();
  else if (shape == 2)
    generator->initTriangle();
  else if (shape == 3)
    generator->initSquare();
  else if (shape == 4)
    generator->initSawUp();
  else if (shape == 5)
    generator->initSawDown();
  else {
    auto setPoints = [generator](std::initializer_list<std::pair<float, float>> points) {
      generator->setNumPoints(static_cast<int>(points.size()));
      int index = 0;
      for (const auto& point : points) {
        generator->setPoint(index, { point.first, 1.0f - point.second });
        generator->setPower(index, 0.0f);
        index++;
      }
      generator->setSmooth(false);
      generator->render();
    };

    if (shape == 6) {
      setPoints({ { 0.0f, 1.0f }, { 0.25f, 0.0f }, { 1.0f, 1.0f } });
    }
    else if (shape == 7) {
      setPoints({ { 0.0f, 0.0f }, { 0.25f, 0.3f }, { 0.5f, 0.7f }, { 0.75f, 1.0f }, { 1.0f, 0.0f } });
    }
    else if (shape == 8) {
      setPoints({ { 0.0f, 0.0f }, { 0.125f, 0.125f }, { 0.25f, 0.25f }, { 0.375f, 0.375f },
                  { 0.5f, 0.5f }, { 0.625f, 0.625f }, { 0.75f, 0.75f }, { 0.875f, 0.875f }, { 1.0f, 1.0f } });
    }
    else if (shape == 9) {
      setPoints({ { 0.0f, 1.0f }, { 0.125f, 0.875f }, { 0.25f, 0.75f }, { 0.375f, 0.625f },
                  { 0.5f, 0.5f }, { 0.625f, 0.375f }, { 0.75f, 0.25f }, { 0.875f, 0.125f }, { 1.0f, 0.0f } });
    }
    else if (shape == 10) {
      setPoints({ { 0.0f, 0.0f }, { 1.0f, 0.0f } });
    }
    else if (shape == 11) {
      setPoints({ { 0.0f, 0.5f }, { 1.0f, 0.5f } });
    }
    else {
      setPoints({ { 0.0f, 1.0f }, { 1.0f, 1.0f } });
    }
  }
  synth_.pauseProcessing(false);

  refreshLfoMsegControls();
  postPluginAnnouncement("Applied " + lfo_mseg_shape_.getText() + " to LFO " +
                                         String(active_lfo_index_ + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::addLfoPoint() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() >= LineGenerator::kMaxPoints)
    return;

  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  int insert_index = generator->getNumPoints();
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const float point_time = generator->getPoint(i).first;
    if (std::abs(point_time - lfo_cursor_phase_) <= 0.0005f) {
      lfo_mseg_point_.setSelectedItemIndex(i, sendNotificationSync);
      postPluginAnnouncement("Point already exists, " + lfoMsegStatusText(),
                                             AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    if (point_time > lfo_cursor_phase_) {
      insert_index = i;
      break;
    }
  }

  const float output_value = generator->valueAtPhase(lfo_cursor_phase_);
  synth_.pauseProcessing(true);
  generator->addPoint(insert_index, { lfo_cursor_phase_, 1.0f - output_value });
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  lfo_mseg_point_.setSelectedItemIndex(insert_index, dontSendNotification);
  updateLfoMsegSummary();
  postPluginAnnouncement("Added point, " + lfoPointDescription(insert_index),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::deleteLfoPoint() {
  auto* generator = activeLfoGenerator();
  const int index = selectedLfoPointIndex();
  if (generator == nullptr || generator->getNumPoints() <= 2 ||
      !isPositiveAndBelow(index, generator->getNumPoints())) {
    postPluginAnnouncement("LFO needs at least two points",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  const String deleted = lfoPointDescription(index);
  synth_.pauseProcessing(true);
  generator->removePoint(index);
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  postPluginAnnouncement("Deleted point, " + deleted, AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::moveLfoPointTime(float delta) {
  auto* generator = activeLfoGenerator();
  const int index = selectedLfoPointIndex();
  if (generator == nullptr || !isPositiveAndBelow(index, generator->getNumPoints()))
    return;
  if (index == 0 || index == generator->getNumPoints() - 1) {
    postPluginAnnouncement("Start and end point times are fixed",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  const auto point = generator->getPoint(index);
  const float min_time = generator->getPoint(index - 1).first + 0.001f;
  const float max_time = generator->getPoint(index + 1).first - 0.001f;
  const float new_time = snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, point.first + delta));
  synth_.pauseProcessing(true);
  generator->setPoint(index, { jlimit(min_time, max_time, new_time), point.second });
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  lfo_mseg_point_.setSelectedItemIndex(index, dontSendNotification);
  updateLfoMsegSummary();
  postPluginAnnouncement("Moved point, " + lfoPointDescription(index),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::moveLfoPointValue(float delta) {
  auto* generator = activeLfoGenerator();
  const int index = selectedLfoPointIndex();
  if (generator == nullptr || !isPositiveAndBelow(index, generator->getNumPoints()))
    return;

  const auto point = generator->getPoint(index);
  const float output_value = jlimit(0.0f, 1.0f, pointOutputValue(generator, index) + delta);
  synth_.pauseProcessing(true);
  generator->setPoint(index, { point.first, 1.0f - output_value });
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  lfo_mseg_point_.setSelectedItemIndex(index, dontSendNotification);
  updateLfoMsegSummary();
  postPluginAnnouncement("Value " + percentString(output_value) + ", " + lfoPointDescription(index),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setLfoPointCurveFromCombo() {
  auto* generator = activeLfoGenerator();
  const int point_index = selectedLfoPointIndex();
  if (generator == nullptr || generator->getNumPoints() < 2)
    return;
  const int curve_point = isPositiveAndBelow(point_index, generator->getNumPoints() - 1)
      ? point_index : generator->getNumPoints() - 2;
  const int curve_index = lfo_mseg_curve_.getSelectedItemIndex();
  synth_.pauseProcessing(true);
  generator->setSmooth(smoothForCurveIndex(curve_index));
  generator->setPower(curve_point, powerForCurveIndex(curve_index));
  generator->render();
  synth_.pauseProcessing(false);
  updateLfoMsegSummary();
  postPluginAnnouncement("Curve " + lfo_mseg_curve_.getText() + ", " +
                                         lfoPointDescription(curve_point),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::setLfoSmoothFromToggle() {
  if (auto* generator = activeLfoGenerator()) {
    synth_.pauseProcessing(true);
    generator->setSmooth(lfo_mseg_smooth_.getToggleState());
    synth_.pauseProcessing(false);
    updateLfoMsegSummary();
    postPluginAnnouncement(generator->smooth() ? "Smooth MSEG on" : "Smooth MSEG off",
                                           AccessibilityHandler::AnnouncementPriority::medium);
  }
}

void SynthEditor::setModulationControlsVisible(bool visible) {
  modulation_controls_visible_ = visible;
  modulation_source_.setVisible(visible);
  modulation_destination_group_.setVisible(visible);
  modulation_destination_.setVisible(visible);
  add_modulation_.setVisible(visible);
  remove_modulation_.setVisible(visible);
  modulation_list_.setVisible(visible);
  resized();
}

void SynthEditor::setRoutingControlsVisible(bool visible) {
  routing_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(routing_summary_);
    addChildComponent(routing_mode_);
    addChildComponent(routing_default_);
    addChildComponent(routing_serial_forward_);
    addChildComponent(routing_serial_backward_);
  }
  routing_summary_.setVisible(visible);
  routing_mode_.setVisible(visible);
  routing_default_.setVisible(visible);
  routing_serial_forward_.setVisible(visible);
  routing_serial_backward_.setVisible(visible);
  if (visible)
    refreshRoutingControls();
  resized();
}

void SynthEditor::setEffectChainControlsVisible(bool visible) {
  effect_chain_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(effect_chain_summary_);
    addChildComponent(effect_chain_selector_);
    addChildComponent(effect_move_up_);
    addChildComponent(effect_move_down_);
  }
  effect_chain_summary_.setVisible(visible);
  effect_chain_selector_.setVisible(visible);
  post_effect_order_selector_.setVisible(false);
  effect_move_up_.setVisible(visible);
  effect_move_down_.setVisible(visible);
  if (visible)
    refreshEffectChainControls();
  resized();
}

ValueBridge* SynthEditor::parameterBridge(const String& id) const {
  const auto found = parameters_by_id_.find(id);
  return found == parameters_by_id_.end() ? nullptr : found->second;
}

void SynthEditor::setParameterEngineValue(const String& id, float engineValue) {
  if (auto* bridge = parameterBridge(id)) {
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bridge->convertToPluginValue(engineValue));
    bridge->endChangeGesture();
  }
}

void SynthEditor::updateRoutingSummary() {
  auto textFor = [this](const String& id) {
    if (auto* bridge = parameterBridge(id))
      return bridge->getText(bridge->getValue(), 128);
    return String("Unavailable");
  };

  String summary;
  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    if (summary.isNotEmpty())
      summary += "; ";
    summary += "Oscillator " + String(osc) + " to " + textFor("osc_" + String(osc) + "_destination");
  }
  summary +=
      "; Sample to " + textFor("sample_destination") +
      "; Granular to " + textFor("granular_destination") +
      "; Filter 1 to " + textFor("filter_1_destination") +
      "; Filter 2 to " + textFor("filter_2_destination") +
      ". Filter link: " +
      ((parameterBridge("filter_2_filter_input") && parameterBridge("filter_2_filter_input")->getValue() >= 0.5f)
           ? "filter 1 into filter 2"
           : (parameterBridge("filter_1_filter_input") && parameterBridge("filter_1_filter_input")->getValue() >= 0.5f)
                 ? "filter 2 into filter 1"
                 : "parallel filters");
  const bool changed = routing_summary_.getText() != summary;
  routing_summary_.setText(summary, dontSendNotification);
  routing_summary_.setDescription(summary);
  if (changed) {
    if (auto* handler = routing_summary_.getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshRoutingControls() {
  updateRoutingSummary();

  int mode = 0;
  if (parameterBridge("filter_2_filter_input") && parameterBridge("filter_2_filter_input")->getValue() >= 0.5f)
    mode = 1;
  else if (parameterBridge("filter_1_filter_input") && parameterBridge("filter_1_filter_input")->getValue() >= 0.5f)
    mode = 2;

  ScopedValueSetter<bool> guard(updating_routing_controls_, true);
  routing_mode_.setSelectedItemIndex(mode, dontSendNotification);
}

void SynthEditor::applyRoutingPreset(int preset) {
  if (preset == 0) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Filters routed in parallel", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 1) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 1.0f);
    postPluginAnnouncement("Filter 1 routed into filter 2", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 2) {
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    setParameterEngineValue("filter_1_filter_input", 1.0f);
    postPluginAnnouncement("Filter 2 routed into filter 1", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 3) {
    setParameterEngineValue("osc_1_destination", vital::constants::kFilter1);
    setParameterEngineValue("osc_2_destination", vital::constants::kFilter2);
    setParameterEngineValue("osc_3_destination", vital::constants::kEffects);
    setParameterEngineValue("osc_4_destination", vital::constants::kEffects);
    setParameterEngineValue("sample_destination", vital::constants::kEffects);
    setParameterEngineValue("granular_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_2_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Default routing applied", AccessibilityHandler::AnnouncementPriority::high);
  }
  refreshRoutingControls();
  timerCallback();
}

void SynthEditor::readEffectChainOrder(const String& sectionName, int* order) const {
  const String prefix = effectChainPrefixForSection(sectionName);
  bool slots_are_default = true;
  bool slots_are_valid = true;
  bool seen[vital::constants::kNumEffects] = {};

  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String slot_id = prefix + "effect_chain_slot_" + String(i + 1);
    auto* slot = parameterBridge(slot_id);
    if (slot == nullptr) {
      slots_are_valid = false;
      break;
    }

    const int effect = jlimit(0, vital::constants::kNumEffects - 1,
                              static_cast<int>(std::round(slot->convertToEngineValue(slot->getValue()))));
    order[i] = effect;
    slots_are_default = slots_are_default && effect == i;
    if (seen[effect])
      slots_are_valid = false;
    seen[effect] = true;
  }

  if (slots_are_valid && !slots_are_default)
    return;

  for (int i = 0; i < vital::constants::kNumEffects; ++i)
    order[i] = i;

  if (auto* bridge = parameterBridge(prefix + "effect_chain_order")) {
    const float max_order = vital::utils::factorial(vital::constants::kNumReorderableEffects) - 1;
    const float chain_order = jlimit(0.0f, max_order, std::round(bridge->convertToEngineValue(bridge->getValue())));
    vital::utils::decodeFloatToOrder(order, chain_order, vital::constants::kNumReorderableEffects);
  }

  if (auto* post_bridge = parameterBridge(prefix + "post_effect_order")) {
    if (post_bridge->convertToEngineValue(post_bridge->getValue()) >= 0.5f)
      std::swap(order[vital::constants::kFrequencyShifter], order[vital::constants::kLimiter]);
  }
}

void SynthEditor::writeEffectChainOrder(const String& sectionName, const int* order) {
  const String prefix = effectChainPrefixForSection(sectionName);
  for (int i = 0; i < vital::constants::kNumEffects; ++i)
    setParameterEngineValue(prefix + "effect_chain_slot_" + String(i + 1), static_cast<float>(order[i]));
}

void SynthEditor::populateEffectChainSelector(ComboBox& selector, Label* summary, const String& sectionName) const {
  int order[vital::constants::kNumEffects];
  readEffectChainOrder(sectionName, order);

  const int previous = selector.getSelectedItemIndex();
  selector.clear(dontSendNotification);
  String summary_text = "Current effects order: ";
  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String name = effectName(order[i]);
    selector.addItem(String(i + 1) + ". " + name, i + 1);
    summary_text += (i == 0 ? "" : ", ") + name;
  }

  selector.setSelectedItemIndex(jlimit(0, vital::constants::kNumEffects - 1, previous),
                                dontSendNotification);
  if (summary != nullptr) {
    summary->setText(summary_text, dontSendNotification);
    summary->setDescription(summary_text);
    if (auto* handler = summary->getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshEffectChainControls() {
  populateEffectChainSelector(effect_chain_selector_, &effect_chain_summary_, last_section_name);
}

void SynthEditor::moveSelectedEffect(int direction) {
  const int selected = effect_chain_selector_.getSelectedItemIndex();
  const int next = selected + direction;
  if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
      !isPositiveAndBelow(next, vital::constants::kNumEffects))
    return;

  int order[vital::constants::kNumEffects];
  readEffectChainOrder(last_section_name, order);
  std::swap(order[selected], order[next]);
  const String moved = effectName(order[next]);
  writeEffectChainOrder(last_section_name, order);
  refreshEffectChainControls();
  effect_chain_selector_.setSelectedItemIndex(next, dontSendNotification);
  postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::connectModulationAndPromptForAmount(const String& sourceId, const String& destinationId,
                                                      Component& target) {
  if (sourceId.isEmpty() || destinationId.isEmpty())
    return;

  const bool created = synth_.connectModulation(sourceId.toStdString(), destinationId.toStdString());
  const int macro_index = macroIndexForControlId(sourceId);
  if (macro_index >= 0) {
    const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar",
                              isMacroBipolar(macro_index) ? 1.0f : 0.0f);
  }
  refreshModulationRoutes();
  for (int row = 0; row < static_cast<int>(modulation_routes_.size()); ++row) {
    const auto* route = modulation_routes_[row];
    if (route != nullptr && route->source_name == sourceId.toStdString() &&
        route->destination_name == destinationId.toStdString()) {
      modulation_list_.selectRow(row);
      break;
    }
  }

  promptForInitialModulationAmount(sourceId, destinationId, created, target);
}

void SynthEditor::promptForInitialModulationAmount(const String& sourceId, const String& destinationId,
                                                   bool created, Component& target) {
  ignoreUnused(target);
  parameter_value_prompt_visible_ = false;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  pending_parameter_value_focus_ = nullptr;
  pending_modulation_source_ = sourceId;
  pending_modulation_destination_ = destinationId;
  pending_modulation_created_ = created;
  modulation_amount_prompt_visible_ = true;
  modulation_amount_prompt_.setText("Maximum amount for " + modulationSourceLabelForId(sourceId) + " to " +
                                    modulationDestinationLabelForId(destinationId),
                                    dontSendNotification);
  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription("Type the maximum modulation amount from minus one to one");
  modulation_amount_editor_.setTitle("Maximum modulation amount");
  modulation_amount_editor_.setDescription("Maximum modulation amount from minus one to one");
  modulation_amount_editor_.setHelpText("Type a value from minus one to one, then press Return or OK");
  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText("1.0", dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type maximum modulation amount",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForParameterValue(const String& parameterId, Component& target) {
  auto* bridge = parameterBridge(parameterId);
  if (bridge == nullptr)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_ = parameterId;
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  pending_parameter_value_focus_ = &target;

  const String name = bridge->getName(128);
  String value = bridge->getText(bridge->getValue(), 128);
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index >= 0 && isMacroBipolar(macro_index))
    value = String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%";
  modulation_amount_prompt_.setText("Set " + name + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + name);
  modulation_amount_prompt_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setTitle(name + " value");
  modulation_amount_editor_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setHelpText("Type a value and press Return. Units like percent, ms, seconds, dB, and named options are accepted.");
  if (isTransposeQuantizeParameter(parameterId)) {
    modulation_amount_editor_.setDescription("Type a scale name such as major or minor, custom notes such as C D E F G A B, or add global for global snap");
    modulation_amount_editor_.setHelpText("Examples: major, minor, global major, C D E F G A B, C D E flat F G A flat B flat, chromatic, or off.");
  }
  modulation_amount_ok_.setTitle("Set " + name);
  modulation_amount_ok_.setDescription("Apply the typed value for " + name);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + name);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type value for " + name,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForMacroName(int macroIndex, Component& target) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  Component::SafePointer<Component> target_pointer(&target);
  promptForCustomValue("Macro " + String(macroIndex + 1) + " name", synth_.getMacroName(macroIndex), target,
                       [this, macroIndex, target_pointer](const String& text) {
                         String name = text.trim();
                         if (name.isEmpty())
                           name = "MACRO " + String(macroIndex + 1);

                         synth_.setMacroName(macroIndex, name);
                         if (target_pointer != nullptr) {
                           target_pointer->setTitle(name);
                           target_pointer->setDescription("Adjust " + name);
                         }
                       });
}

void SynthEditor::promptForCustomValue(const String& title, const String& value, Component& target,
                                       std::function<void(const String&)> apply) {
  if (!apply)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_ = title;
  pending_custom_value_apply_ = std::move(apply);
  pending_parameter_value_focus_ = &target;

  modulation_amount_prompt_.setText("Set " + title + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + title);
  modulation_amount_prompt_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setTitle(title + " value");
  modulation_amount_editor_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setHelpText("Type a value and press Return or OK");
  modulation_amount_ok_.setTitle("Set " + title);
  modulation_amount_ok_.setDescription("Apply the typed value for " + title);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + title);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type value for " + title,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::applyInlineTextPrompt() {
  if (parameter_value_prompt_visible_)
    applyParameterValue();
  else if (modulation_amount_prompt_visible_)
    applyInitialModulationAmount();
}

void SynthEditor::applyInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const float amount = jlimit(-1.0f, 1.0f, static_cast<float>(modulation_amount_editor_.getText().getDoubleValue()));
  const int connection = synth_.getConnectionIndex(source_id.toStdString(), destination_id.toStdString());
  if (connection >= 0) {
    setParameterEngineValue("modulation_" + String(connection + 1) + "_amount", amount);
    refreshModulationRoutes();
    showSelectedModulationParameters();
  }

  modulation_amount_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  rebuildFocusOrder();
  resized();

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                                         modulationDestinationLabelForId(destination_id) +
                                         ", amount " + String(amount, 2),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::applyParameterValue() {
  const String parameter_id = pending_parameter_value_id_;
  const String custom_title = pending_custom_value_title_;
  if (parameter_id.isEmpty() && pending_custom_value_apply_) {
    auto apply = std::move(pending_custom_value_apply_);
    apply(modulation_amount_editor_.getText());

    parameter_value_prompt_visible_ = false;
    modulation_amount_prompt_.setVisible(false);
    modulation_amount_editor_.setVisible(false);
    modulation_amount_ok_.setVisible(false);
    modulation_amount_cancel_.setVisible(false);
    pending_custom_value_title_.clear();
    rebuildFocusOrder();
    resized();

    if (pending_parameter_value_focus_ != nullptr)
      pending_parameter_value_focus_->grabKeyboardFocus();
    pending_parameter_value_focus_ = nullptr;

    postPluginAnnouncement(custom_title + " " + modulation_amount_editor_.getText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  auto* bridge = parameterBridge(parameter_id);
  if (bridge != nullptr) {
    float value = bridge->getValueForText(modulation_amount_editor_.getText());
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0) {
      if (isMacroBipolar(macro_index)) {
        const String text = modulation_amount_editor_.getText().trim();
        float percent = text.getFloatValue();
        if (text.contains("%") || std::abs(percent) > 1.0f)
          percent *= 0.01f;
        value = jlimit(0.0f, 1.0f, (jlimit(-1.0f, 1.0f, percent) + 1.0f) * 0.5f);
      }
      else if (bridge->convertToEngineValue(value) < 0.0f) {
        value = bridge->convertToPluginValue(0.0f);
      }
    }
    if (isTransposeQuantizeParameter(parameter_id)) {
      const int current_value = static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));
      int quantize_value = current_value;
      if (parseTransposeQuantizeText(modulation_amount_editor_.getText(), current_value, quantize_value)) {
        value = bridge->convertToPluginValue(static_cast<float>(quantize_value));
        if (parameter_id.startsWith("osc_")) {
          const String prefix = parameter_id.upToLastOccurrenceOf("transpose_quantize", false, false) +
                                "transpose_quantize_";
          if (auto* scale_bridge = parameterBridge(prefix + "scale")) {
            scale_bridge->beginChangeGesture();
            scale_bridge->setValueNotifyingHost(scale_bridge->convertToPluginValue(14.0f));
            scale_bridge->endChangeGesture();
          }
        }
      }
    }
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(value);
    bridge->endChangeGesture();
  }

  parameter_value_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  rebuildFocusOrder();
  resized();

  if (pending_parameter_value_focus_ != nullptr)
    pending_parameter_value_focus_->grabKeyboardFocus();
  pending_parameter_value_focus_ = nullptr;

  if (bridge != nullptr) {
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0 && isMacroBipolar(macro_index)) {
      postPluginAnnouncement(synth_.getMacroName(macro_index) + " " +
                                             String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%",
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
    else {
      postPluginAnnouncement(bridge->getName(128) + " " + bridge->getText(bridge->getValue(), 128),
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
  }
}

bool SynthEditor::isMacroBipolar(int macroIndex) const {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return false;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex)))
    return bridge->getValue() >= 0.5f;
  return false;
}

void SynthEditor::setMacroBipolar(int macroIndex, bool bipolar) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex))) {
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bipolar ? 1.0f : 0.0f);
    bridge->endChangeGesture();
  }

  const String source_id = "macro_control_" + String(macroIndex + 1);
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name != source_id.toStdString() || route->destination_name.empty())
      continue;

    const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar", bipolar ? 1.0f : 0.0f);
  }
  refreshModulationRoutes();
}

bool SynthEditor::handleMacroShortcut(const String& parameterId, const KeyPress& key, Component& target) {
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index < 0)
    return false;

  if (key.getModifiers().isShiftDown() && key.getKeyCode() == KeyPress::returnKey) {
    promptForMacroName(macro_index, target);
    return true;
  }

  const bool is_b_key = CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'b' ||
                        key.getKeyCode() == 'b' || key.getKeyCode() == 'B';
  if (key.getModifiers().isAnyModifierKeyDown() || !is_b_key) {
    return false;
  }

  const bool bipolar = !isMacroBipolar(macro_index);
  setMacroBipolar(macro_index, bipolar);

  if (auto* bridge = parameterBridge(parameterId)) {
    if (auto* slider = dynamic_cast<Slider*>(&target)) {
      const double interval = slider->getInterval();
      slider->setRange(0.0, 1.0, interval);
      slider->setValue(jlimit(0.0, 1.0, static_cast<double>(bridge->getValue())), dontSendNotification);
      if (auto* handler = slider->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
  }

  postPluginAnnouncement(synth_.getMacroName(macro_index) +
                                         (bipolar ? " bipolar, minus 100 to 100"
                                                  : " unipolar, 0 to 100"),
                                         AccessibilityHandler::AnnouncementPriority::high);
  return true;
}

void SynthEditor::cancelInlineTextPrompt() {
  if (parameter_value_prompt_visible_) {
    parameter_value_prompt_visible_ = false;
    modulation_amount_prompt_.setVisible(false);
    modulation_amount_editor_.setVisible(false);
    modulation_amount_ok_.setVisible(false);
    modulation_amount_cancel_.setVisible(false);
    pending_parameter_value_id_.clear();
    pending_custom_value_title_.clear();
    pending_custom_value_apply_ = nullptr;
    rebuildFocusOrder();
    resized();
    if (pending_parameter_value_focus_ != nullptr)
      pending_parameter_value_focus_->grabKeyboardFocus();
    pending_parameter_value_focus_ = nullptr;
    postPluginAnnouncement("Value entry cancelled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  if (modulation_amount_prompt_visible_)
    cancelInitialModulationAmount();
}

void SynthEditor::cancelInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const bool created = pending_modulation_created_;
  modulation_amount_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  rebuildFocusOrder();
  resized();

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                                         modulationDestinationLabelForId(destination_id) +
                                         (created ? "" : ", already assigned"),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showModulationSourceMenuForParameter(const String& destinationId, Component& target) {
  if (destinationId.isEmpty())
    return;

  struct SourceChoice {
    String id;
    String label;
  };

  auto addSource = [](std::map<String, std::vector<SourceChoice>>& groups,
                      const String& group, const String& id) {
    groups[group].push_back({ id, readableId(id) });
  };

  std::map<String, std::vector<SourceChoice>> groups;
  for (const auto& id : modulation_source_ids_) {
    if (id.startsWith("lfo_"))
      addSource(groups, "LFOs", id);
    else if (id.startsWith("env_"))
      addSource(groups, "Envelopes", id);
    else if (id.startsWith("random_"))
      addSource(groups, "Random", id);
    else if (id.startsWith("macro_"))
      addSource(groups, "Macros", id);
    else if (id == "aftertouch" || id == "velocity" || id == "lift" || id == "mod_wheel" ||
             id == "pitch_wheel" || id == "slide" || id == "note" || id == "note_in_octave" ||
             id == "stereo")
      addSource(groups, "MIDI and expression", id);
  }

  PopupMenu menu;
  menu.addSectionHeader("Assign modulation to " + modulationDestinationLabelForId(destinationId));
  std::vector<String> choice_ids;
  int item_id = 1;
  for (const auto& group_name : StringArray{ "LFOs", "Envelopes", "Random", "Macros",
                                             "MIDI and expression" }) {
    const auto found = groups.find(group_name);
    if (found == groups.end() || found->second.empty())
      continue;

    auto sources = found->second;
    std::stable_sort(sources.begin(), sources.end(), [](const SourceChoice& a, const SourceChoice& b) {
      const int rank_a = modulationSourceSortRank(a.id);
      const int rank_b = modulationSourceSortRank(b.id);
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return a.label.compareNatural(b.label) < 0;
    });

    PopupMenu source_menu;
    for (const auto& source : sources) {
      choice_ids.push_back(source.id);
      const int macro_index = macroIndexForControlId(source.id);
      const String label = macro_index >= 0 ? synth_.getMacroName(macro_index)
                                            : modulationSourceLabelForId(source.id);
      source_menu.addItem(item_id++, label);
    }
    menu.addSubMenu(group_name, source_menu, true);
  }

  if (choice_ids.empty()) {
    menu.addItem(1, "No modulation sources available", false);
  }

  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                     [this, destinationId, choice_ids](int result) {
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choice_ids.size())))
      return;

    connectModulationAndPromptForAmount(choice_ids[static_cast<size_t>(result - 1)], destinationId,
                                        *this);
  });
}

int SynthEditor::getNumRows() { return static_cast<int>(modulation_routes_.size()); }

String SynthEditor::getNameForRow(int row) {
  if (!isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size())))
    return {};
  const auto* route = modulation_routes_[row];
  const int index = synth_.getConnectionIndex(route->source_name, route->destination_name);
  const String destination = vital::Parameters::isParameter(route->destination_name)
      ? vital::Parameters::getDisplayName(route->destination_name) : readableId(route->destination_name);
  return "Modulation " + numberWord(index + 1) + ", " +
         modulationSourceLabelForId(route->source_name) + " to " + destination;
}

void SynthEditor::paintListBoxItem(int row, Graphics& graphics, int width, int height, bool selected) {
  if (selected)
    graphics.fillAll(Colour(0xff315f8f));
  graphics.setColour(Colours::white);
  graphics.drawText(getNameForRow(row), 8, 0, width - 16, height, Justification::centredLeft);
}

void SynthEditor::selectedRowsChanged(int) { showSelectedModulationParameters(); }

void SynthEditor::refreshModulationRoutes() {
  modulation_routes_.clear();
  for (auto* route : synth_.getModulationConnections()) {
    if (!route->source_name.empty() && !route->destination_name.empty())
      modulation_routes_.push_back(route);
  }
  std::stable_sort(modulation_routes_.begin(), modulation_routes_.end(), [this](auto* a, auto* b) {
    return synth_.getConnectionIndex(a->source_name, a->destination_name) <
           synth_.getConnectionIndex(b->source_name, b->destination_name);
  });
  modulation_list_.updateContent();
  if (!modulation_routes_.empty() && modulation_list_.getSelectedRow() < 0)
    modulation_list_.selectRow(0);
  if (modulation_routes_.empty())
    showSelectedModulationParameters();
}

void SynthEditor::showSelectedModulationParameters() {
  if (!modulation_controls_visible_)
    return;
  rows_.clear();
  rows_container_.removeAllChildren();
  const int row = modulation_list_.getSelectedRow();
  if (!isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size()))) {
    rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), 0);
    return;
  }
  const auto* route = modulation_routes_[row];
  const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
  if (connection < 0)
    return;
  const String prefix = "modulation_" + String(connection + 1) + "_";
  int y = 0;
  for (auto* parameter : sections_["Modulation routing"]) {
    const auto* bridge = dynamic_cast<ValueBridge*>(parameter);
    if (bridge == nullptr || !bridge->getParameterId().startsWith(prefix))
      continue;
    auto control = std::make_unique<AccessibleParameterRow>(*parameter);
    control->setModulationMenuCallback([this](const String& destinationId, Component& target) {
      showModulationSourceMenuForParameter(destinationId, target);
    });
    control->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
      if (clear) {
        synth_.clearMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
      else {
        synth_.armMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
    });
    control->setValueEntryCallback([this](const String& parameterId, Component& target) {
      promptForParameterValue(parameterId, target);
    });
    control->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
      return handleMacroShortcut(parameterId, key, target);
    });
    control->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 48);
    rows_container_.addAndMakeVisible(control.get());
    rows_.push_back(std::move(control));
    y += 48;
  }
  rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), y);
  viewport_.setViewPosition(0, 0);
}

void SynthEditor::configureWavetableActions(const String& section_name) {
  active_oscillator_ = -1;
  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    if (section_name == "Oscillator " + String(osc)) {
      active_oscillator_ = osc - 1;
      break;
    }
  }
  const bool visible = active_oscillator_ >= 0;
  wavetable_name_.setVisible(visible);
  load_wavetable_.setVisible(visible);
  reset_wavetable_.setVisible(visible);
  oscillator_summary_.setVisible(visible);
  oscillator_octave_.setVisible(visible);
  oscillator_semitone_.setVisible(visible);
  oscillator_fine_tune_.setVisible(visible);
  oscillator_wave_frame_.setVisible(visible);
  oscillator_scale_key_.setVisible(visible);
  oscillator_scale_type_.setVisible(visible);
  oscillator_scale_mode_.setVisible(visible);
  if (visible) {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_))
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
    refreshOscillatorDirectControls();
  }
  resized();
}

void SynthEditor::refreshOscillatorDirectControls() {
  if (active_oscillator_ < 0)
    return;

  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_";
  auto engineValue = [this, &prefix](const String& suffix) {
    if (auto* bridge = parameterBridge(prefix + suffix))
      return bridge->convertToEngineValue(bridge->getValue());
    return 0.0f;
  };

  const int transpose = roundToInt(engineValue("transpose"));
  int octave = transpose / 12;
  int semitone = transpose - octave * 12;
  octave = jlimit(-4, 4, octave);
  semitone = jlimit(-11, 11, semitone);
  const float fine = engineValue("tune") * 100.0f;
  const float wave_frame = engineValue("wave_frame");

  ScopedValueSetter<bool> guard(updating_oscillator_controls_, true);
  oscillator_octave_.setSelectedId(octave + 5, dontSendNotification);
  oscillator_semitone_.setSelectedId(semitone + 12, dontSendNotification);
  oscillator_fine_tune_.setValue(fine, dontSendNotification);
  oscillator_wave_frame_.setValue(wave_frame, dontSendNotification);
  refreshOscillatorScaleControls();

  const String summary = "Oscillator " + String(active_oscillator_ + 1) +
                         ": octave " + String(octave) +
                         ", semitone " + String(semitone) +
                         ", fine tune " + String(roundToInt(fine)) + " cents" +
                         ", wave frame " + String(roundToInt(wave_frame)) +
                         ", transpose quantize " + oscillator_scale_key_.getText() + " " +
                         oscillator_scale_type_.getText() + ", " + oscillator_scale_mode_.getText();
  oscillator_summary_.setText(summary, dontSendNotification);
  oscillator_summary_.setDescription(summary);
  if (auto* handler = oscillator_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::refreshOscillatorScaleControls() {
  if (active_oscillator_ < 0)
    return;

  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_transpose_quantize_";
  auto engineValue = [this, &prefix](const String& suffix) {
    if (auto* bridge = parameterBridge(prefix + suffix))
      return bridge->convertToEngineValue(bridge->getValue());
    return 0.0f;
  };

  const int key = jlimit(0, vital::kNotesPerOctave - 1, roundToInt(engineValue("key")));
  const int scale_index = jlimit(0, static_cast<int>(transposeQuantizeScales().size()) - 1,
                                 roundToInt(engineValue("scale")));
  const int mode = jlimit(0, 1, roundToInt(engineValue("mode")));
  oscillator_scale_key_.setSelectedId(key + 1, dontSendNotification);
  oscillator_scale_type_.setSelectedId(scale_index + 1, dontSendNotification);
  oscillator_scale_mode_.setSelectedId(mode + 1, dontSendNotification);
}

void SynthEditor::setOscillatorTransposeFromDirectControls() {
  if (active_oscillator_ < 0)
    return;
  const int octave = oscillator_octave_.getSelectedId() - 5;
  const int semitone = oscillator_semitone_.getSelectedId() - 12;
  const int transpose = jlimit(-48, 48, octave * 12 + semitone);
  setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_transpose", static_cast<float>(transpose));
  refreshOscillatorDirectControls();
  postPluginAnnouncement("Oscillator " + String(active_oscillator_ + 1) +
                                         " transpose " + String(transpose) + " semitones",
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::setOscillatorScaleFromDirectControls() {
  if (active_oscillator_ < 0)
    return;

  const int key = jlimit(0, vital::kNotesPerOctave - 1, oscillator_scale_key_.getSelectedId() - 1);
  const int scale_index = oscillator_scale_type_.getSelectedId() - 1;
  const auto& scales = transposeQuantizeScales();
  if (!isPositiveAndBelow(scale_index, static_cast<int>(scales.size())))
    return;

  const bool global = oscillator_scale_mode_.getSelectedId() == 2;
  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_transpose_quantize_";
  setParameterEngineValue(prefix + "key", static_cast<float>(key));
  setParameterEngineValue(prefix + "scale", static_cast<float>(scale_index));
  setParameterEngineValue(prefix + "mode", global ? 1.0f : 0.0f);
  refreshOscillatorDirectControls();
  postPluginAnnouncement("Oscillator " + String(active_oscillator_ + 1) +
                                         " transpose quantize " + transposeKeyName(key) + " " +
                                         scales[scale_index].name + ", " + (global ? "global" : "local"),
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::chooseWavetableFile() {
  chooseWavetableFile(WavetableCreator::kWavetableSplice);
}

void SynthEditor::chooseWavetableFile(int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;
  wavetable_chooser_ = std::make_unique<FileChooser>("Load Vital wavetable or audio file",
                                                     File(), vital::kWavetableExtensionsList);
  wavetable_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this, audioLoadStyle](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadWavetableFile(file, audioLoadStyle);
    wavetable_chooser_.reset();
  });
}

void SynthEditor::loadWavetableFile(const File& file) {
  loadWavetableFile(file, WavetableCreator::kWavetableSplice);
}

void SynthEditor::loadWavetableFile(const File& file, int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;

  bool processing_paused = false;
  try {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_)) {
      const auto extension = file.getFileExtension().toLowerCase();
      const bool is_audio_file = extension == ".wav" || extension == ".flac" ||
                                 extension == ".aif" || extension == ".aiff";
      String wavetable_string;
      AudioSampleBuffer sample_buffer;
      int sample_rate = 0;
      json data;

      if (is_audio_file) {
        auto input_stream = std::make_unique<FileInputStream>(file);
        if (!input_stream->openedOk())
          throw std::runtime_error("Unable to open audio file");

        wavetable_string = getWavetableDataString(*input_stream);
        sample_rate = loadAudioFile(sample_buffer, input_stream.release());
        if (sample_rate == 0 || sample_buffer.getNumSamples() == 0)
          throw std::runtime_error("Unable to decode audio file");
      }
      else {
        data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
        if (data.is_discarded())
          throw std::runtime_error("Unable to parse wavetable JSON");
      }

      synth_.pauseProcessing(true);
      processing_paused = true;
      if (is_audio_file) {
        creator->initFromAudioFile(sample_buffer.getReadPointer(0), sample_buffer.getNumSamples(),
                                   sample_rate, static_cast<WavetableCreator::AudioFileLoadStyle>(audioLoadStyle),
                                   getFadeStyleFromWavetableString(wavetable_string));
        creator->setName(file.getFileNameWithoutExtension().toStdString());
        creator->setAuthor(getAuthorFromWavetableString(wavetable_string).toStdString());
      }
      else {
        // Do not preflight with WavetableCreator::isValidJson() here. Older factory
        // Vital tables may omit version/name fields but jsonToState() can still
        // migrate and load them through WavetableCreator::updateJson().
        creator->jsonToState(data);
        if (creator->getName().empty())
          creator->setName(file.getFileNameWithoutExtension().toStdString());
      }
      creator->setFileLoaded(file.getFullPathName().toStdString());
      synth_.pauseProcessing(false);
      processing_paused = false;
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
      const String import_suffix = is_audio_file ? " as " + wavetableAudioLoadStyleName(audioLoadStyle) : "";
      postPluginAnnouncement("Loaded " + file.getFileNameWithoutExtension() + import_suffix +
                                               " in oscillator " + String(active_oscillator_ + 1),
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  }
  catch (const std::exception& error) {
    if (processing_paused)
      synth_.pauseProcessing(false);
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load wavetable", error.what());
    postPluginAnnouncement("Unable to load wavetable",
                                             AccessibilityHandler::AnnouncementPriority::high);
  }
}

int SynthEditor::wavetableFrameCount(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return 1;

  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (auto* wavetable = creator->getWavetable())
      return jmax(1, wavetable->numFrames());
  }
  return 1;
}

String SynthEditor::wavetableEditorSummary(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return "Wavetable editor";

  String name = "unknown";
  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (!creator->getName().empty())
      name = String(creator->getName());
  }

  return "Wavetable editor, " + name + ", " + String(wavetableFrameCount(oscillator)) + " frames";
}

void SynthEditor::setWavetableEditorVisible(int oscillator, bool visible) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const String parameter_id = focusedParameterId();
  const String focused_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  String restore_title = focused_title;
  if (focused_title == "Show wavetable editor" && visible)
    restore_title = "Hide wavetable editor";
  else if (focused_title == "Hide wavetable editor" && !visible)
    restore_title = "Show wavetable editor";

  wavetable_editor_visible_[oscillator] = visible;
  const String section = "Oscillator " + String(oscillator + 1);
  if (show_all_sections_)
    showAllSections(false);
  else
    selectSectionByName(section, false);
  restoreFocusAfterRebuild(parameter_id, persistent_focus, restore_title);

  postPluginAnnouncement(visible ? "Wavetable editor shown" : "Wavetable editor hidden",
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::applyWavetableHarmonicEdit(int oscillator, int frame, bool allFrames,
                                             int harmonic, float levelPercent, float phaseDegrees) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;
  const float amplitude = jlimit(0.0f, 4.0f, levelPercent / 100.0f) *
                          (vital::WaveFrame::kWaveformSize * 0.5f);
  const float phase = phaseDegrees * vital::kPi / 180.0f;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = std::polar(amplitude, phase);
    wave_frame->toTimeDomain();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement("Set harmonic " + String(harmonic) +
                                         (allFrames ? " on all frames" : " on frame " + String(start + 1)),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearWavetableHarmonic(int oscillator, int frame, bool allFrames, int harmonic) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = 0.0f;
    wave_frame->toTimeDomain();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement("Cleared harmonic " + String(harmonic) +
                                         (allFrames ? " on all frames" : " on frame " + String(start + 1)),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::normalizeWavetableFrame(int oscillator, int frame, bool allFrames) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement(allFrames ? "Normalized all wavetable frames" :
                                                     "Normalized wavetable frame " + String(start + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::removeWavetableFundamental(int oscillator, int frame, bool allFrames) {
  clearWavetableHarmonic(oscillator, frame, allFrames, 1);
}

void SynthEditor::loadShiftedWavetable(int oscillator, int direction) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  const File current_file(creator != nullptr ? creator->getLastFileLoaded() : std::string());
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  if (!wavetable_browser_cache_valid_) {
    refreshBrowserFileCache(cached_wavetable_browser_files_, roots, vital::kWavetableExtensionsList);
    wavetable_browser_cache_valid_ = true;
  }

  File wavetable_file = shiftedBrowserFile(cached_wavetable_browser_files_, current_file, direction);
  if (wavetable_file.existsAsFile()) {
    active_oscillator_ = oscillator;
    loadWavetableFile(wavetable_file);
    return;
  }

  postPluginAnnouncement("No wavetable file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showWavetableBrowserMenu(int oscillator, Component& target) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  if (!wavetable_browser_cache_valid_) {
    refreshBrowserFileCache(cached_wavetable_browser_files_, roots, vital::kWavetableExtensionsList);
    wavetable_browser_cache_valid_ = true;
  }

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kWavetableFolderName,
                                         cached_wavetable_browser_files_, choices);
  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                     [this, oscillator, choices](int result) {
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    active_oscillator_ = oscillator;
    loadWavetableFile((*choices)[static_cast<size_t>(result - 1)]);
  });
}

void SynthEditor::chooseSampleFile(bool granular) {
  sample_chooser_ = std::make_unique<FileChooser>(granular ? "Load granular sample" : "Load audio sample",
                                                  File(), vital::kSampleExtensionsList);
  sample_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this, granular](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadSampleFile(file, granular);
    sample_chooser_.reset();
  });
}

void SynthEditor::loadSampleFile(const File& file, bool granular) {
  static constexpr int kMaxFileSamples = 17640000;

  try {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> format_reader(format_manager.createReaderFor(file));
    if (format_reader == nullptr)
      throw std::runtime_error("Unable to decode audio file");

    const int num_samples = static_cast<int>(std::min<long long>(format_reader->lengthInSamples, kMaxFileSamples));
    if (num_samples <= 0)
      throw std::runtime_error("Audio file is empty");

    AudioSampleBuffer sample_buffer;
    sample_buffer.setSize(static_cast<int>(format_reader->numChannels), num_samples);
    format_reader->read(&sample_buffer, 0, num_samples, 0, true, true);

    auto* sample = granular ? synth_.getGranularSample() : synth_.getSample();
    if (sample == nullptr)
      throw std::runtime_error("Sample engine is unavailable");

    synth_.pauseProcessing(true);
    if (sample_buffer.getNumChannels() > 1)
      sample->loadSample(sample_buffer.getReadPointer(0), sample_buffer.getReadPointer(1),
                         num_samples, static_cast<int>(format_reader->sampleRate));
    else
      sample->loadSample(sample_buffer.getReadPointer(0), num_samples, static_cast<int>(format_reader->sampleRate));
    sample->setName(file.getFileNameWithoutExtension().toStdString());
    sample->setLastBrowsedFile(file.getFullPathName().toStdString());
    synth_.pauseProcessing(false);

    if (granular)
      setParameterEngineValue("granular_on", 1.0f);

    postPluginAnnouncement("Loaded sample " + file.getFileNameWithoutExtension(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
  catch (const std::exception& error) {
    synth_.pauseProcessing(false);
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load sample", error.what());
    postPluginAnnouncement("Unable to load sample",
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::loadShiftedSample(int direction, bool granular) {
  auto* sample = granular ? synth_.getGranularSample() : synth_.getSample();
  const File current_file(sample != nullptr ? sample->getLastBrowsedFile() : std::string());
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  if (!sample_browser_cache_valid_) {
    refreshBrowserFileCache(cached_sample_browser_files_, roots, vital::kSampleExtensionsList);
    sample_browser_cache_valid_ = true;
  }

  File sample_file = shiftedBrowserFile(cached_sample_browser_files_, current_file, direction);
  if (sample_file.existsAsFile()) {
    loadSampleFile(sample_file, granular);
    return;
  }

  postPluginAnnouncement("No sample file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showSampleBrowserMenu(Component& target, bool granular) {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  if (!sample_browser_cache_valid_) {
    refreshBrowserFileCache(cached_sample_browser_files_, roots, vital::kSampleExtensionsList);
    sample_browser_cache_valid_ = true;
  }

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kSampleFolderName,
                                         cached_sample_browser_files_, choices);
  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                     [this, choices, granular](int result) {
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    loadSampleFile((*choices)[static_cast<size_t>(result - 1)], granular);
  });
}

void SynthEditor::moveToSection(int direction) {
  const int count = section_names_.size();
  if (count == 0)
    return;
  int current = section_names_.indexOf(last_section_name);
  if (current < 0)
    current = 0;
  const int next = (current + direction + count) % count;
  selectSectionByName(section_names_[next], true);
  section_selector_.grabKeyboardFocus();
}

bool SynthEditor::focusSectionShortcut(const String& section) {
  if (!section_names_.contains(section))
    return false;

  selectSectionByName(section, true);
  return true;
}

bool SynthEditor::focusGroupShortcut(const String& group, const String& fallbackSection) {
  if (!group_names_.contains(group)) {
    if (fallbackSection.isNotEmpty())
      return focusSectionShortcut(fallbackSection);
    return false;
  }

  if (show_all_sections_) {
    if (row_focus_order_.empty())
      showAllSections(false);
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getTitle() == group) {
        ensureComponentVisible(component);
        component->grabKeyboardFocus();
        postPluginAnnouncement(group, AccessibilityHandler::AnnouncementPriority::high);
        return true;
      }
    }
  }

  const auto found = group_sections_.find(group);
  if (found == group_sections_.end() || found->second.isEmpty())
    return false;

  selectSectionByName(found->second[0], true);
  return true;
}

bool SynthEditor::focusShortcutTarget(const KeyPress& key) {
  if (dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) != nullptr)
    return false;

  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const ModifierKeys modifiers = key.getModifiers();

  if (modifiers.isCtrlDown() && !modifiers.isCommandDown() && !modifiers.isAltDown()) {
    if (key.getKeyCode() == '1') return focusGroupShortcut("Bus 1");
    if (key.getKeyCode() == '2') return focusGroupShortcut("Bus 2");
    if (key.getKeyCode() == '3') return focusGroupShortcut("Bus 3");
  }

  if (modifiers.isAltDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && character == 'm')
    return focusGroupShortcut("Modulation", "Modulation routing");

  if (modifiers.isAnyModifierKeyDown())
    return false;

  switch (character) {
    case 'o': return focusGroupShortcut("Oscillators");
    case 'x': return focusGroupShortcut("Mixer and routing", kSignalRoutingSection);
    case 'f': return focusGroupShortcut("Filters");
    case 'e': return focusGroupShortcut("Envelopes");
    case 'l': return focusGroupShortcut("LFOs");
    case 'p': return focusGroupShortcut("Effects", kEffectsChainSection);
    case 'r': return focusSectionShortcut(kSignalRoutingSection);
    case 'z': return focusGroupShortcut("Zones");
    case 'g': return focusSectionShortcut("Master and global");
    default: break;
  }

  return false;
}

bool SynthEditor::keyPressed(const KeyPress& key) {
  if ((modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) &&
      key.getKeyCode() == KeyPress::escapeKey) {
    cancelInlineTextPrompt();
    return true;
  }
  if ((key.getKeyCode() == KeyPress::returnKey || key.getKeyCode() == KeyPress::spaceKey) &&
      preset_controls_visible_ && preset_selector_.hasKeyboardFocus(true)) {
    loadSelectedPreset();
    return true;
  }
  if (lfo_mseg_controls_visible_ && handleLfoMsegShortcut(key))
    return true;
  if (focusShortcutTarget(key))
    return true;
  if (key.getModifiers().isShiftDown() &&
      CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'm') {
    announceModulationSummary();
    return true;
  }
  if (key.getTextCharacter() == ',') {
    moveToSection(-1);
    return true;
  }
  if (key.getTextCharacter() == '.') {
    moveToSection(1);
    return true;
  }
  return AudioProcessorEditor::keyPressed(key);
}

void SynthEditor::timerCallback() {
  if (refreshFilterRowsIfNeeded())
    return;
  if (routing_controls_visible_)
    refreshRoutingControls();
  for (auto& row : rows_)
    row->refresh();
  if (active_oscillator_ >= 0 && !updating_oscillator_controls_)
    refreshOscillatorDirectControls();
}

void SynthEditor::updateFullGui() {
  timerCallback();
  synth_.updateHostDisplay();
}
