# Windows touchpad gesture capture

Settings → Input has separate **Capture three-finger gestures** and **Capture four-finger gestures** switches, independent of **Capture system keyboard shortcuts**. Both unchecked disables global gesture capture. Capture runs only during mouse capture and stops on focus loss or releasing capture.

**Forward two-finger touchpad gestures** controls scrolling and pinch separately. The existing `enabletouchpadgestures` preference is preserved for two-finger forwarding. `touchpadcapturemode` stores three/four-finger selection as independent bits (1 and 2). Old installations with touchpad gestures enabled initially retain both global gesture kinds.

Windows TouchpadGesturesController receives only selected three/four-finger kinds. Five-finger gestures are no longer captured implicitly. Supported Sunshine hosts receive physical contact events; older hosts use keyboard/mouse translation. Global tap actions do not expose physical positions and are translated separately: three-finger tap to middle click, four-finger tap to Windows+A.

Startup now completes encrypted RTSP responses at their declared frame length. Persistent connections require authenticated host version 1 acknowledgement. Legacy hosts retain separate request connections. Qt already owns a reusable QNetworkAccessManager, so the Android-specific TLS context fix is not copied into Qt.

Validation: Windows x64 Release and QML compilation, clipboard/native cursor conformance tests, RTSP framing and reuse negotiation tests, portable runtime dependency closure, and standalone MSI install. Physical gestures still need verification on a precision touchpad.
