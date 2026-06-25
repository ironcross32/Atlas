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

#pragma once

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "synth_gui_interface.h"

#include <map>

class AccessibleParameterRow;
class LineGenerator;
class ValueBridge;

class PresetComboBox : public ComboBox {
  public:
    std::function<void()> onReturnKey;

    bool keyPressed(const KeyPress& key) override {
      if (key.getKeyCode() == KeyPress::returnKey && onReturnKey) {
        onReturnKey();
        return true;
      }
      return ComboBox::keyPressed(key);
    }
};

class AccessibleComboBox : public ComboBox {
  public:
    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
};

class AccessibleTextButton : public TextButton {
  public:
    using TextButton::TextButton;

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
};

class LfoMsegKeyboardComponent : public Component {
  public:
    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<String()> getStatusText;

    LfoMsegKeyboardComponent() {
      setWantsKeyboardFocus(true);
      setTitle("LFO Editor");
      setDescription("Keyboard LFO shape editor with time, value, and curve announcements.");
      setHelpText("Left and right move in time. A adds a point. Shift A applies the selected shape. Backspace clears the shape. Up and Down change value. Shift Left and Shift Right move the point. Command Up and Command Down change curve. Comma and period move between points. G and H cycle shape presets. Semicolon and apostrophe cycle length. Minus and equals change grid. J and L move the current point. I and K change curve. C and V copy and paste points. E deletes a point.");
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed)
        return onKeyPressed(key);
      return Component::keyPressed(key);
    }

    void paint(Graphics& g) override {
      g.fillAll(Colour(0xff20242b));
      g.setColour(Colour(0xffd9dee2));
      g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
      g.setColour(Colours::white);
      g.drawText(getStatusText ? getStatusText() : String("LFO keyboard editor"),
                 getLocalBounds().reduced(12), Justification::centredLeft, true);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class Handler final : public AccessibilityHandler {
        public:
          explicit Handler(LfoMsegKeyboardComponent& component) :
              AccessibilityHandler(component, AccessibilityRole::group) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }
      };
      return std::make_unique<Handler>(*this);
    }
};

class SynthEditor : public AudioProcessorEditor, public SynthGuiInterface,
                    private Timer, private ListBoxModel {
  public:
    SynthEditor(SynthPlugin&);

    void paint(Graphics&) override;
    void resized() override;
    void updateFullGui() override;
    bool keyPressed(const KeyPress& key) override;
    std::unique_ptr<ComponentTraverser> createFocusTraverser() override;
    std::unique_ptr<ComponentTraverser> createKeyboardFocusTraverser() override;

    const std::vector<Component*>& getAccessibleFocusOrder() const { return accessibility_focus_order_; }
    const std::vector<Component*>& getKeyboardFocusOrder() const { return focus_order_; }
    void ensureComponentVisible(Component* component);

  private:
    void timerCallback() override;
    void buildSections();
    void buildGroups();
    void populateGroupSelector(const String& preferredGroup = {});
    void populateElementSelectorForGroup(const String& group, const String& preferredSection = {});
    void selectSectionByName(const String& section, bool announce);
    void showSection(int index, bool announce);
    void showAllSections(bool announce);
    std::unique_ptr<AccessibleParameterRow> createAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                         const String& sectionName) const;
    bool shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const;
    bool refreshFilterRowsIfNeeded();
    String focusedParameterId() const;
    String focusedAccessibleTitle() const;
    Component* persistentFocusedComponent() const;
    void restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                  const String& accessibleTitle = {});
    void moveToSection(int direction);
    void showNavigationMenu();
    void rebuildFocusOrder();
    void refreshPresetList();
    void populatePresetFilters();
    void filterPresetList();
    void showPresetMenu();
    void selectPresetFile(const File& file);
    void loadSelectedPreset();
    void choosePresetFile();
    void loadPresetFile(const File& file, bool preview = false);
    void chooseBankFile();
    void importBankFile(const File& file);
    void savePresetToUserDirectory();
    void savePresetAs();
    void savePresetFile(const File& file);
    void setPresetControlsVisible(bool visible);
    void updatePresetSummary();
    void configureWavetableActions(const String& sectionName);
    void refreshOscillatorDirectControls();
    void refreshOscillatorScaleControls();
    void setOscillatorTransposeFromDirectControls();
    void setOscillatorScaleFromDirectControls();
    void loadShiftedWavetable(int oscillator, int direction);
    void setRoutingControlsVisible(bool visible);
    void updateRoutingSummary();
    void refreshRoutingControls();
    void applyRoutingPreset(int preset);
    void chooseWavetableFile();
    void chooseWavetableFile(int audioLoadStyle);
    void loadWavetableFile(const File& file);
    void loadWavetableFile(const File& file, int audioLoadStyle);
    int wavetableFrameCount(int oscillator) const;
    String wavetableEditorSummary(int oscillator) const;
    void setWavetableEditorVisible(int oscillator, bool visible);
    void applyWavetableHarmonicEdit(int oscillator, int frame, bool allFrames,
                                    int harmonic, float levelPercent, float phaseDegrees);
    void clearWavetableHarmonic(int oscillator, int frame, bool allFrames, int harmonic);
    void normalizeWavetableFrame(int oscillator, int frame, bool allFrames);
    void removeWavetableFundamental(int oscillator, int frame, bool allFrames);
    void showWavetableBrowserMenu(int oscillator, Component& target);
    void chooseSampleFile(bool granular = false);
    void loadSampleFile(const File& file, bool granular = false);
    void showSampleBrowserMenu(Component& target, bool granular = false);
    void loadShiftedSample(int direction, bool granular = false);
    void setEffectChainControlsVisible(bool visible);
    void readEffectChainOrder(const String& sectionName, int* order) const;
    void writeEffectChainOrder(const String& sectionName, const int* order);
    void populateEffectChainSelector(ComboBox& selector, Label* summary, const String& sectionName) const;
    void refreshEffectChainControls();
    void moveSelectedEffect(int direction);
    void connectModulationAndPromptForAmount(const String& sourceId, const String& destinationId, Component& target);
    void promptForInitialModulationAmount(const String& sourceId, const String& destinationId, bool created, Component& target);
    void promptForParameterValue(const String& parameterId, Component& target);
    void promptForMacroName(int macroIndex, Component& target);
    void promptForCustomValue(const String& title, const String& value, Component& target,
                              std::function<void(const String&)> apply);
    void applyInlineTextPrompt();
    void applyInitialModulationAmount();
    void applyParameterValue();
    void cancelInlineTextPrompt();
    void cancelInitialModulationAmount();
    void showModulationSourceMenuForParameter(const String& destinationId, Component& target);
    ValueBridge* parameterBridge(const String& id) const;
    void setParameterEngineValue(const String& id, float engineValue);
    int getNumRows() override;
    void paintListBoxItem(int row, Graphics& graphics, int width, int height, bool selected) override;
    String getNameForRow(int row) override;
    void selectedRowsChanged(int row) override;
    void refreshModulationRoutes();
    void showSelectedModulationParameters();
    void setModulationControlsVisible(bool visible);
    void populateModulationDestinations();
    void updateModulationDestinationList();
    void setLfoMsegControlsVisible(bool visible);
    void refreshLfoMsegControls();
    void updateLfoPointSelector();
    void updateLfoMsegSummary();
    bool handleLfoMsegShortcut(const KeyPress& key);
    String lfoMsegStatusText() const;
    void applyLfoMode();
    void applyLfoCycleLength();
    void moveLfoCursor(float delta);
    void moveToLfoPoint(int direction);
    void stepLfoShapePreset(int direction);
    void stepLfoCycleLength(int direction);
    void stepLfoGrid(int direction);
    void clearLfoShape();
    void applyLfoShapePreset();
    void addLfoPoint();
    void deleteLfoPoint();
    void moveLfoPointTime(float delta);
    void moveLfoPointValue(float delta);
    void setLfoPointCurveFromCombo();
    void setLfoSmoothFromToggle();
    LineGenerator* lfoGeneratorForIndex(int lfoIndex) const;
    LineGenerator* activeLfoGenerator() const;
    int selectedLfoPointIndex() const;
    float lfoCycleQuarterNotes() const;
    float lfoGridQuarterNotes() const;
    float lfoGridAmount() const;
    float lfoPhaseToQuarterNotes(float phase) const;
    float lfoQuarterNotesToPhase(float quarterNotes) const;
    float snapLfoPhaseToGrid(float phase) const;
    String lfoPointDescriptionFor(int lfoIndex, int pointIndex) const;
    String lfoPointDescription(int pointIndex) const;
    String lfoTimeDescription(float phase) const;
    String lfoMsegStatusTextFor(int lfoIndex) const;
    String modulationDestinationsForSource(const String& sourceId) const;
    String modulationSlotTitle(int slot) const;
    void announceModulationSummary();
    bool focusShortcutTarget(const KeyPress& key);
    bool focusGroupShortcut(const String& group, const String& fallbackSection = {});
    bool focusSectionShortcut(const String& section);
    bool handleMacroShortcut(const String& parameterId, const KeyPress& key, Component& target);
    bool isMacroBipolar(int macroIndex) const;
    void setMacroBipolar(int macroIndex, bool bipolar);

    SynthPlugin& synth_;
    Label title_;
    Label instructions_;
    TextButton menu_button_ { "Menu" };
    ComboBox group_selector_;
    ComboBox section_selector_;
    Label preset_summary_;
    TextButton preset_menu_ { "Preset menu" };
    ComboBox preset_bank_;
    ComboBox preset_category_;
    TextEditor preset_search_;
    PresetComboBox preset_selector_;
    ToggleButton preset_preview_ { "Preview selected preset" };
    TextEditor preset_name_editor_;
    Label wavetable_name_;
    TextButton load_wavetable_ { "Load wavetable" };
    TextButton reset_wavetable_ { "Reset wavetable" };
    Label oscillator_summary_;
    ComboBox oscillator_octave_;
    ComboBox oscillator_semitone_;
    Slider oscillator_fine_tune_;
    Slider oscillator_wave_frame_;
    AccessibleComboBox oscillator_scale_key_;
    AccessibleComboBox oscillator_scale_type_;
    AccessibleComboBox oscillator_scale_mode_;
    Label routing_summary_;
    AccessibleComboBox routing_mode_;
    TextButton routing_default_ { "Default routing" };
    TextButton routing_serial_forward_ { "Serial filters 1 into 2" };
    TextButton routing_serial_backward_ { "Serial filters 2 into 1" };
    Label effect_chain_summary_;
    AccessibleComboBox effect_chain_selector_;
    AccessibleComboBox post_effect_order_selector_;
    AccessibleTextButton effect_move_up_ { "Move effect earlier" };
    AccessibleTextButton effect_move_down_ { "Move effect later" };
    ComboBox modulation_source_;
    ComboBox modulation_destination_group_;
    ComboBox modulation_destination_;
    TextButton add_modulation_ { "Connect modulation" };
    TextButton remove_modulation_ { "Remove selected modulation" };
    ListBox modulation_list_ { "Modulation routes", this };
    Label modulation_amount_prompt_;
    TextEditor modulation_amount_editor_;
    TextButton modulation_amount_ok_ { "OK" };
    TextButton modulation_amount_cancel_ { "Cancel" };
    Label lfo_mseg_summary_;
    ComboBox lfo_mseg_lfo_;
    ComboBox lfo_mseg_mode_;
    ComboBox lfo_mseg_cycle_;
    ComboBox lfo_mseg_grid_;
    ComboBox lfo_mseg_shape_;
    TextButton lfo_mseg_apply_shape_ { "Apply shape" };
    ComboBox lfo_mseg_point_;
    TextButton lfo_mseg_add_point_ { "Add point" };
    TextButton lfo_mseg_delete_point_ { "Delete point" };
    TextButton lfo_mseg_time_down_ { "Time earlier" };
    TextButton lfo_mseg_time_up_ { "Time later" };
    TextButton lfo_mseg_value_down_ { "Value down" };
    TextButton lfo_mseg_value_up_ { "Value up" };
    ComboBox lfo_mseg_curve_;
    ToggleButton lfo_mseg_smooth_ { "Smooth MSEG" };
    LfoMsegKeyboardComponent lfo_mseg_keyboard_;
    Viewport viewport_;
    Component rows_container_;
    std::vector<std::unique_ptr<Component>> section_headers_;
    std::vector<std::unique_ptr<AccessibleParameterRow>> rows_;
    std::map<String, std::vector<AudioProcessorParameter*>> sections_;
    std::map<String, StringArray> group_sections_;
    std::map<String, ValueBridge*> parameters_by_id_;
    StringArray group_names_;
    StringArray section_names_;
    Array<File> all_presets_;
    Array<File> filtered_presets_;
    StringArray preset_banks_;
    StringArray preset_categories_;
    StringArray modulation_source_ids_;
    StringArray modulation_destination_groups_;
    StringArray modulation_destination_all_ids_;
    StringArray modulation_destination_ids_;
    std::vector<vital::ModulationConnection*> modulation_routes_;
    std::vector<std::pair<float, float>> copied_lfo_points_;
    std::vector<File> cached_wavetable_browser_files_;
    std::vector<File> cached_sample_browser_files_;
    std::vector<Component*> focus_order_;
    std::vector<Component*> accessibility_focus_order_;
    std::vector<Component*> row_focus_order_;
    std::vector<Component*> top_level_accessibility_order_;
    std::unique_ptr<FileChooser> preset_chooser_;
    std::unique_ptr<FileChooser> wavetable_chooser_;
    std::unique_ptr<FileChooser> sample_chooser_;
    String pending_modulation_source_;
    String pending_modulation_destination_;
    String pending_parameter_value_id_;
    Component::SafePointer<Component> pending_parameter_value_focus_;
    String pending_custom_value_title_;
    std::function<void(const String&)> pending_custom_value_apply_;
    bool pending_modulation_created_ = false;
    int active_oscillator_ = -1;
    int active_lfo_index_ = 0;
    int last_filter_models_[3] = { -1, -1, -1 };
    int last_filter_styles_[3] = { -1, -1, -1 };
    int lfo_cycle_index_ = 3;
    int lfo_grid_index_ = 14;
    float lfo_cursor_phase_ = 0.0f;
    bool preset_controls_visible_ = false;
    bool show_all_sections_ = true;
    bool updating_preset_list_ = false;
    bool updating_navigation_ = false;
    bool updating_oscillator_controls_ = false;
    bool updating_routing_controls_ = false;
    bool updating_modulation_destinations_ = false;
    bool updating_lfo_mseg_controls_ = false;
    bool wavetable_browser_cache_valid_ = false;
    bool wavetable_editor_visible_[vital::kNumOscillators] = {};
    bool sample_browser_cache_valid_ = false;
    bool routing_controls_visible_ = false;
    bool effect_chain_controls_visible_ = false;
    bool modulation_controls_visible_ = false;
    bool lfo_mseg_controls_visible_ = false;
    bool modulation_amount_prompt_visible_ = false;
    bool parameter_value_prompt_visible_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthEditor)
};
