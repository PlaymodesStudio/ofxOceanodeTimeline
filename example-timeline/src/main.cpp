#include "ofApp.h"
int main() {
    ofSetupOpenGL(1280, 800, OF_WINDOW);
    ofRunApp(std::make_shared<ofApp>());
}
