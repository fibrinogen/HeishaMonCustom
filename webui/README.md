# SD-hosted Custom Web UI

The custom Dashboard, Settings, Scheduler, External Sensors, Smart DHW,
Hardware, Diagnostics, and History pages are served from the microSD card.
The original HeishaMon Home, firmware update, network settings, rules, and
recovery pages remain embedded in the firmware.

Build the upload package with:

```bash
python3 scripts/build_webui_package.py
```

Upload `webui/dist/heishamon-webui.tar` in the **Web UI Update** section of
the firmware page. The controller writes the inactive A/B slot, validates the
manifest, and only then switches to it. A failed upload leaves the active Web
UI untouched.

The source files in `webui/src` are independent of the upstream HeishaMon
firmware sources to keep future upstream merges small.

Increment `webui/version.txt` whenever browser-visible files change. Its value
is used for the manifest and cache-busting asset URLs independently of the
firmware/custom-feature version.
