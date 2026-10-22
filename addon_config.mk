meta:
    ADDON_NAME = ofxOceanodeTimeline
    ADDON_DESCRIPTION = Optional timeline, clips and automation editor for ofxOceanode
    ADDON_AUTHOR = @PlaymodesStudio
    ADDON_URL = https://github.com/PlaymodesStudio/ofxOceanodeTimeline
    ADDON_TAGS = "timeline automation oceanode"

common:
    ADDON_DEPENDENCIES = ofxOceanode ofxImGui
    # A define, not ADDON_CFLAGS: projectGenerator puts cflags in Xcode's
    # OTHER_CFLAGS, which C++ files never see (OTHER_CPLUSPLUSFLAGS is set by
    # the OF project), so every .cpp compiled the timeline integration out.
    # ADDON_DEFINES -> GCC_PREPROCESSOR_DEFINITIONS reaches C, C++ and ObjC.
    ADDON_DEFINES = OFX_OCEANODE_HAS_TIMELINE
