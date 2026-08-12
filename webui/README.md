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

Versions are maintained centrally in `HeishaMon/custom_version.h`:

- `CUSTOM_FIRMWARE_VERSION` is the custom firmware release (`X.Y.Z`).
- `CUSTOM_WEBUI_REVISION` is incremented for a Web-UI-only update.
- The generated package version is always `X.Y.Z-web.N`.

For a firmware release, increment `CUSTOM_FIRMWARE_VERSION` and reset the Web
UI revision to `1`. For a Web-UI-only update, increment only
`CUSTOM_WEBUI_REVISION`. The firmware page displays the installed firmware,
the expected Web UI version, and whether the active SD package matches.

The package manifest also contains the latest 50 Git commit messages. The
firmware page reads this history from the active SD Web UI package, so it does
not add the commit history to the firmware image.
