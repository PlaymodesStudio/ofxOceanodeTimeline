meta:
    ADDON_NAME = ofxOceanodeTimeline
    ADDON_DESCRIPTION = Optional timeline, clips and automation editor for ofxOceanode
    ADDON_AUTHOR = @PlaymodesStudio
    ADDON_URL = https://github.com/PlaymodesStudio/ofxOceanodeTimeline
    ADDON_TAGS = "timeline automation oceanode"

common:
    ADDON_DEPENDENCIES = ofxOceanode ofxImGui
    ADDON_CFLAGS += -DOFX_OCEANODE_HAS_TIMELINE=1
