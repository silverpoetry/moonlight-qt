TEMPLATE = subdirs

# Resolve platform compile tests before using their CONFIG results to build
# the subproject graph.
load(configure)
qtCompileTest(SL)
qtCompileTest(EGL)

SUBDIRS = \
    moonlight-common-c \
    qmdnsengine \
    app \
    h264bitstream

!config_SL {
    SUBDIRS += clipboard_manifest_test
    clipboard_manifest_test.file = tests/clipboardmanifest/clipboardmanifest.pro
    clipboard_manifest_test.depends = moonlight-common-c
}

# Build the dependencies in parallel before the final app
app.depends = qmdnsengine moonlight-common-c h264bitstream
win32:!winrt {
    SUBDIRS += AntiHooking
    app.depends += AntiHooking
}
