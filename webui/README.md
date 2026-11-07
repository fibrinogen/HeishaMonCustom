# SD-hosted Custom Web UI

The custom Dashboard, Settings, Scheduler, External Sensors, Hardware,
Diagnostics, and History pages are served from the microSD card.
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

The builder prepends the sibling `bridge-time/web/date-format.js` to the
generated `common.js`. Scheduler clock, last-sync and action timestamps use
German dates (`DD.MM.YYYY HH:mm:ss`). The embedded firmware version-history
page uses the same formatter through `BridgeTime/WebDateTime.h`. API timestamps
and native browser date controls retain their existing formats. Existing
generated output is updated in place; the builder does not delete the output
directory or unrelated files.
History and History new also include the year whenever a chart label or
tooltip includes a date. History new calendar COP cards/tables and History
bar-chart periods also use German dates/months. The current generated package
is `0.2.9-web.54`.

**History new** shows electrical consumption (kWh), heat produced (kWh), and
COP for the selected chart period, today, this month, this year, and recorded
monthly/yearly history. Calendar energy totals come from `/efficiencyapi`
and use the same recorded room-heating and DHW totals as the MQTT energy
sensors. The added calendar kWh fields require matching updated firmware.
**History new 2** (`/history-new-2`) extends this comparison with operator
diagnostics: spread ($\Delta T$), target excess ($T_{\text{VL}} - T_{\text{Soll}}$),
heating curve shift, compressor modulation (Hz/A), water flow (l/min),
evaporator/icing temperature, defrost and backup-heater metrics, clickable
per-cycle analysis, 31-day/24-hour demand charts, CSV export, and storage/degree-day
settings.
Run `node test/history_new_energy_test.js` to check the page's energy displays,
missing values, and calendar refresh behavior locally.

Versions are maintained centrally in `HeishaMon/custom_version.h`:

- `CUSTOM_FIRMWARE_VERSION` is the custom firmware release (`X.Y.Z`).
- `CUSTOM_WEBUI_BUILD` is incremented for every Web UI package and is never
  reset, so every package has a unique version.
- The generated package version is always `X.Y.Z-web.N`.

For a firmware release, increment `CUSTOM_FIRMWARE_VERSION`. For a Web-UI-only
update, increment only `CUSTOM_WEBUI_BUILD`; the build number is never reset.
The firmware page displays the installed firmware,
the expected Web UI version, and whether the active SD package matches.

The package manifest also contains the latest 50 Git commit messages. The
firmware page reads this history from the active SD Web UI package, so it does
not add the commit history to the firmware image.
