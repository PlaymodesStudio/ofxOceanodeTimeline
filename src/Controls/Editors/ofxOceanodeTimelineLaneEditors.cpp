#include "ofxOceanodeTimelineLaneEditors.h"
#include "ofxOceanodeTimelineLaneEditorUi.h"

ofxOceanodeTimelineLaneEditor& ofxOceanodeTimelineLaneEditors::forType(ofxOceanodeTimelineLaneType type) {
    switch(type) {
        case ofxOceanodeTimelineLaneType::Step: return step;
        case ofxOceanodeTimelineLaneType::Curve: return curve;
        case ofxOceanodeTimelineLaneType::PianoRoll: return pianoRoll;
        case ofxOceanodeTimelineLaneType::Number: return number;
        case ofxOceanodeTimelineLaneType::Dropdown: return dropdown;
        case ofxOceanodeTimelineLaneType::MultiSlider: return multiSlider;
        case ofxOceanodeTimelineLaneType::MultiValue: return multiValue;
        case ofxOceanodeTimelineLaneType::MultiGate: return multiGate;
        default: return step; // Legacy Wave lanes are migrated by the model.
    }
}
float ofxOceanodeTimelineLaneEditors::keyboardReserve(ofxOceanodeTimelineLaneType type) {
    using namespace ofxOceanodeTimelineLaneEditorUi;
    return type == ofxOceanodeTimelineLaneType::PianoRoll
        ? kPianoKeyboardWidth + kPianoScrollbarWidth + 9.0f : 0.0f;
}
float ofxOceanodeTimelineLaneEditors::defaultHeight(ofxOceanodeTimelineLaneType type) {
    return type == ofxOceanodeTimelineLaneType::PianoRoll ? 270.0f : 190.0f;
}
