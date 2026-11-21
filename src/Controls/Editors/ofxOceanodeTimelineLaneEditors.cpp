#include "ofxOceanodeTimelineLaneEditors.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"

ofxOceanodeTimelineLaneEditor& ofxOceanodeTimelineLaneEditors::forType(ofxOceanodeTimelineLaneType type) {
    switch(type) {
        case ofxOceanodeTimelineLaneType::Step: return step;
        case ofxOceanodeTimelineLaneType::Curve: return curve;
        case ofxOceanodeTimelineLaneType::MultiCurve: return multiCurve;
        case ofxOceanodeTimelineLaneType::PianoRoll: return pianoRoll;
        case ofxOceanodeTimelineLaneType::Number: return number;
        case ofxOceanodeTimelineLaneType::Dropdown: return dropdown;
        case ofxOceanodeTimelineLaneType::Color: return color;
        case ofxOceanodeTimelineLaneType::MultiSlider: return multiSlider;
        case ofxOceanodeTimelineLaneType::MultiValue: return multiValue;
        case ofxOceanodeTimelineLaneType::MultiGate: return multiGate;
        case ofxOceanodeTimelineLaneType::DrumGrid: return drumGrid;
        case ofxOceanodeTimelineLaneType::Lfo: return lfo;
        default: return step; // Legacy Wave lanes are migrated by the model.
    }
}
float ofxOceanodeTimelineLaneEditors::keyboardReserve(ofxOceanodeTimelineLaneType type) {
    using namespace ofxOceanodeTimelineLaneEditorUi;
    if(type == ofxOceanodeTimelineLaneType::DrumGrid) return ofxOceanodeTimelineDrumGridEditor::headerWidth();
    return type == ofxOceanodeTimelineLaneType::PianoRoll
        ? kPianoKeyboardWidth + kPianoScrollbarWidth + 9.0f : 0.0f;
}
float ofxOceanodeTimelineLaneEditors::defaultHeight(ofxOceanodeTimelineLaneType type) {
    // An unfolded lane starts compact (half of what it used to open at):
    // drag its bottom edge for more room.
    if(type == ofxOceanodeTimelineLaneType::DrumGrid) return 160.0f;
    if(type == ofxOceanodeTimelineLaneType::Lfo) return 115.0f;
    if(type == ofxOceanodeTimelineLaneType::MultiCurve) return 106.0f; // a Curve plus its tab strip
    return type == ofxOceanodeTimelineLaneType::PianoRoll ? 240.0f : 95.0f;
}
