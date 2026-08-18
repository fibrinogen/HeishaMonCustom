var wpValues = {};
var wpConfig = { heatMin: 20, heatMax: 65 };
var wpCurveShift = null;
var wpCurveDraft = null;
var wpCurveDirty = false;
var wpRefreshPromise = null;
var wpCommandBusy = false;
var wpStepTimers = {};
var wpStepDebounceMs = 1000;
function wpItems(data) {
  return [].concat(
    data.heatpump || [],
    data["heatpump extra"] || [],
    data["heatpump optional"] || [],
  );
}
function wpStatus(message, isError) {
  var el = document.getElementById("wpSettingsStatus");
  if (el) {
    el.textContent = message || "";
    el.style.color = isError ? "var(--red)" : "var(--text-muted)";
  }
}
function wpClearUpdateError() {
  var el = document.getElementById("wpSettingsStatus");
  if (el && el.textContent.indexOf("Update failed:") === 0) wpStatus("", false);
}
function wpFetchJson(path, label, retryInvalidJson) {
  return fetch(path, { cache: "no-store" })
    .then(function (response) {
      if (!response.ok)
        throw new Error(label + " request failed: HTTP " + response.status);
      return response.text();
    })
    .then(function (body) {
      try {
        if (!body.trim()) throw new Error("empty response");
        return JSON.parse(body);
      } catch (parseError) {
        var error = new Error(
          label +
            " returned invalid or incomplete JSON (" +
            body.length +
            " bytes)",
        );
        error.invalidJson = true;
        throw error;
      }
    })
    .catch(function (error) {
      if (retryInvalidJson !== false && error.invalidJson) {
        return new Promise(function (resolve) {
          window.setTimeout(resolve, 250);
        }).then(function () {
          return wpFetchJson(path, label, false);
        });
      }
      throw error;
    });
}
function wpSetSelect(id, topic) {
  var el = document.getElementById(id);
  if (el && document.activeElement !== el && wpValues[topic] !== undefined)
    el.value = String(wpValues[topic]);
}
function wpPumpPercent(rawValue) {
  return Math.max(
    0,
    Math.min(100, Math.round(((Number(rawValue) - 64) / (254 - 64)) * 100)),
  );
}
function wpPumpRaw(percentValue) {
  return Math.round((Number(percentValue) / 100) * (254 - 64) + 64);
}
function wpSync() {
  if (!wpCommandBusy)
    document
      .querySelectorAll(
        ".wp-settings-page button,.wp-settings-page select,.wp-settings-page input",
      )
      .forEach(function (control) {
        control.disabled = false;
      });
  var schedule = document.getElementById("wpScheduleToggle");
  if (schedule) schedule.checked = Number(wpValues.TOP13) !== 0;
  wpSetSelect("wpZones", "TOP94");
  var slider = document.getElementById("wpMaxPumpSlider");
  if (
    slider &&
    document.activeElement !== slider &&
    !isNaN(Number(wpValues.TOP95))
  ) {
    var pumpPercent = wpPumpPercent(wpValues.TOP95);
    slider.value = String(pumpPercent);
    updCell("wpMaxPumpDraft", String(pumpPercent) + " %");
    updCell("wpMaxPumpCurrent", String(pumpPercent) + " %");
  }
  var buffer = document.getElementById("wpBufferDeltaRow");
  if (buffer) {
    var bufferDisabled = Number(wpValues.TOP99) === 0;
    buffer.style.opacity = bufferDisabled ? ".45" : "1";
    buffer.querySelectorAll("button").forEach(function (button) {
      button.disabled = bufferDisabled;
    });
  }
  updCell("wpHeatMinValue", String(wpConfig.heatMin));
  updCell("wpHeatMaxValue", String(wpConfig.heatMax));
  wpSyncCurve();
  if (wpCommandBusy)
    document
      .querySelectorAll(
        ".wp-settings-page button,.wp-settings-page select,.wp-settings-page input",
      )
      .forEach(function (control) {
        control.disabled = true;
      });
}
function wpCurveSource() {
  return {
    targetAtCold: Number(wpValues.TOP29),
    targetAtWarm: Number(wpValues.TOP30),
    // SetCurves outside.low is TOP32 (cold); outside.high is TOP31 (warm).
    outsideCold: Number(wpValues.TOP32),
    outsideWarm: Number(wpValues.TOP31),
  };
}
function wpCurveDataAvailable(curve) {
  return [
    curve.targetAtCold,
    curve.targetAtWarm,
    curve.outsideCold,
    curve.outsideWarm,
  ].every(function (value) {
    return Number.isFinite(value) && value >= -50 && value <= 100;
  });
}
function wpCurveReadInputs() {
  function value(id) {
    var input = document.getElementById(id).value.trim();
    return input === "" ? NaN : Number(input);
  }
  return {
    outsideCold: value("wpCurveOutsideCold"),
    targetAtCold: value("wpCurveTargetCold"),
    outsideWarm: value("wpCurveOutsideWarm"),
    targetAtWarm: value("wpCurveTargetWarm"),
  };
}
function wpCurveValidation(curve) {
  var values = [
    curve.targetAtCold,
    curve.targetAtWarm,
    curve.outsideCold,
    curve.outsideWarm,
  ];
  if (
    !values.every(function (value) {
      return Number.isInteger(value) && value >= -50 && value <= 100;
    })
  )
    return "All curve values must be whole degrees between -50 and 100 °C.";
  if (curve.outsideCold >= curve.outsideWarm)
    return "The cold outside point must be below the warm outside point.";
  if (curve.targetAtCold < curve.targetAtWarm)
    return "The cold-weather water target must not be below the warm-weather target.";
  var shift = Number(wpCurveShift && wpCurveShift.shift);
  if (
    Number.isFinite(shift) &&
    (curve.targetAtCold + shift > 100 || curve.targetAtWarm + shift < -50)
  )
    return "The curve plus the active shift exceeds the supported range.";
  return "";
}
function wpCurveSetInputs(curve) {
  document.getElementById("wpCurveOutsideCold").value = String(
    curve.outsideCold,
  );
  document.getElementById("wpCurveTargetCold").value = String(
    curve.targetAtCold,
  );
  document.getElementById("wpCurveOutsideWarm").value = String(
    curve.outsideWarm,
  );
  document.getElementById("wpCurveTargetWarm").value = String(
    curve.targetAtWarm,
  );
}
function wpSyncCurve() {
  var source = wpCurveSource();
  var available = wpCurveDataAvailable(source);
  var active = document.activeElement;
  var editing = active && active.closest && active.closest(".wp-curve-editor");
  if (available && !wpCurveDirty && !editing) {
    wpCurveDraft = source;
    wpCurveSetInputs(source);
  }
  if (!wpCurveDraft && available) {
    wpCurveDraft = source;
    wpCurveSetInputs(source);
  }

  var mode = document.getElementById("wpCurveMode");
  if (mode) {
    if (!available) mode.textContent = "Curve data unavailable";
    else if (Number(wpValues.TOP76) === 0)
      mode.textContent = "Compensation curve active";
    else if (Number(wpValues.TOP76) === 1)
      mode.textContent = "Direct mode · curve inactive";
    else mode.textContent = "Operating mode unknown";
  }
  document
    .querySelectorAll(".wp-curve-editor input,.wp-curve-editor button")
    .forEach(function (control) {
      control.disabled = !available || wpCommandBusy;
    });
  var curve = wpCurveDraft || source;
  var validation = available
    ? wpCurveValidation(curve)
    : "Waiting for valid Panasonic TOP29–TOP32 values.";
  var validationElement = document.getElementById("wpCurveValidation");
  if (validationElement) {
    validationElement.textContent =
      validation ||
      (wpCurveDirty
        ? "Curve has unsaved changes."
        : "Curve matches the stored Panasonic values.");
    validationElement.className =
      "wp-curve-validation " +
      (validation ? "invalid" : wpCurveDirty ? "dirty" : "valid");
  }
  var apply = document.getElementById("wpCurveApply");
  if (apply)
    apply.disabled =
      !available || !!validation || !wpCurveDirty || wpCommandBusy;
  wpRenderCurveGraph(curve, available);
}
function wpCurveInputChanged() {
  wpCurveDraft = wpCurveReadInputs();
  wpCurveDirty = true;
  wpSyncCurve();
}
function wpResetCurveDraft() {
  var source = wpCurveSource();
  if (!wpCurveDataAvailable(source)) return;
  wpCurveDraft = source;
  wpCurveDirty = false;
  wpCurveSetInputs(source);
  wpSyncCurve();
}
function wpCurveSvgLine(x1, y1, x2, y2, className) {
  return (
    '<line x1="' +
    x1 +
    '" y1="' +
    y1 +
    '" x2="' +
    x2 +
    '" y2="' +
    y2 +
    '" class="' +
    className +
    '"></line>'
  );
}
function wpRenderCurveGraph(curve, available) {
  var svg = document.getElementById("wpCurveGraph");
  var caption = document.getElementById("wpCurveCaption");
  var effectiveLegend = document.getElementById("wpCurveEffectiveLegend");
  if (!svg) return;
  if (!available || !curve || !wpCurveDataAvailable(curve)) {
    svg.innerHTML =
      '<text x="320" y="150" text-anchor="middle" class="wp-curve-empty">Curve data unavailable</text>';
    if (caption) caption.textContent = "Waiting for heat-pump curve data ...";
    if (effectiveLegend) effectiveLegend.hidden = true;
    return;
  }
  var shift = Number(wpCurveShift && wpCurveShift.shift);
  if (!Number.isFinite(shift)) shift = 0;
  if (effectiveLegend) effectiveLegend.hidden = shift === 0;
  var xMin =
    Math.floor((Math.min(curve.outsideCold, curve.outsideWarm) - 5) / 5) * 5;
  var xMax =
    Math.ceil((Math.max(curve.outsideCold, curve.outsideWarm) + 5) / 5) * 5;
  var yMin =
    Math.floor(
      (Math.min(
        curve.targetAtCold,
        curve.targetAtWarm,
        curve.targetAtCold + shift,
        curve.targetAtWarm + shift,
      ) -
        5) /
        5,
    ) * 5;
  var yMax =
    Math.ceil(
      (Math.max(
        curve.targetAtCold,
        curve.targetAtWarm,
        curve.targetAtCold + shift,
        curve.targetAtWarm + shift,
      ) +
        5) /
        5,
    ) * 5;
  if (xMax === xMin) xMax = xMin + 10;
  if (yMax === yMin) yMax = yMin + 10;
  var left = 64,
    right = 616,
    top = 20,
    bottom = 250;
  function x(value) {
    return left + ((value - xMin) / (xMax - xMin)) * (right - left);
  }
  function y(value) {
    return bottom - ((value - yMin) / (yMax - yMin)) * (bottom - top);
  }
  var content = "";
  for (var tick = 0; tick <= 5; tick++) {
    var xValue = xMin + ((xMax - xMin) * tick) / 5;
    var yValue = yMin + ((yMax - yMin) * tick) / 5;
    var px = x(xValue),
      py = y(yValue);
    content += wpCurveSvgLine(px, top, px, bottom, "wp-curve-grid");
    content += wpCurveSvgLine(left, py, right, py, "wp-curve-grid");
    content +=
      '<text x="' +
      px +
      '" y="272" text-anchor="middle" class="wp-curve-axis-text">' +
      Math.round(xValue) +
      "</text>";
    content +=
      '<text x="52" y="' +
      (py + 4) +
      '" text-anchor="end" class="wp-curve-axis-text">' +
      Math.round(yValue) +
      "</text>";
  }
  content += wpCurveSvgLine(left, top, left, bottom, "wp-curve-axis");
  content += wpCurveSvgLine(left, bottom, right, bottom, "wp-curve-axis");
  content +=
    '<text x="340" y="296" text-anchor="middle" class="wp-curve-axis-label">Outside temperature (°C)</text>' +
    '<text x="15" y="135" text-anchor="middle" transform="rotate(-90 15 135)" class="wp-curve-axis-label">Water target (°C)</text>';
  content += wpCurveSvgLine(
    x(curve.outsideCold),
    y(curve.targetAtCold),
    x(curve.outsideWarm),
    y(curve.targetAtWarm),
    "wp-curve-line",
  );
  [
    [curve.outsideCold, curve.targetAtCold],
    [curve.outsideWarm, curve.targetAtWarm],
  ].forEach(function (point) {
    content +=
      '<circle cx="' +
      x(point[0]) +
      '" cy="' +
      y(point[1]) +
      '" r="5" class="wp-curve-point"></circle>';
  });
  if (shift !== 0) {
    content += wpCurveSvgLine(
      x(curve.outsideCold),
      y(curve.targetAtCold + shift),
      x(curve.outsideWarm),
      y(curve.targetAtWarm + shift),
      "wp-curve-effective-line",
    );
  }
  svg.innerHTML = content;
  if (caption) {
    caption.textContent =
      curve.outsideCold +
      " °C outside → " +
      curve.targetAtCold +
      " °C water · " +
      curve.outsideWarm +
      " °C outside → " +
      curve.targetAtWarm +
      " °C water" +
      (shift ? " · effective shift " + (shift > 0 ? "+" : "") + shift + " K" : "");
  }
}
function wpApplyCurve() {
  var curve = wpCurveReadInputs();
  var validation = wpCurveValidation(curve);
  if (validation) {
    wpStatus(validation, true);
    return;
  }
  if (
    !window.confirm(
      "Apply the Zone 1 heating curve to the Panasonic controller now?",
    )
  )
    return;
  wpSend("SetZ1HeatCurve", JSON.stringify(curve))
    .then(function (response) {
      if (!response) return;
      if (
        response.indexOf("Zone 1 heating curve queued:") !== 0 &&
        response.indexOf("Zone 1 heating curve already has requested values") !== 0
      ) {
        wpStatus(response, true);
        return;
      }
      wpCurveDirty = false;
      wpCurveDraft = curve;
      wpSyncCurve();
    })
    .catch(function () {});
}
function wpRefresh() {
  if (wpRefreshPromise) return wpRefreshPromise;
  wpRefreshPromise = wpFetchJson("/json", "Heat-pump data")
    .then(function (data) {
      wpItems(data).forEach(function (item) {
        wpValues[item.Topic] = item.Value;
        updCell(item.Topic + "-Value", String(item.Value));
        updCell(item.Topic + "-Description", String(item.Description));
      });
      return wpFetchJson("/wpsettingsconfig", "WP configuration");
    })
    .then(function (data) {
      wpConfig = data;
      return wpFetchJson("/heatingcurveshift", "Heating-curve state");
    })
    .then(function (data) {
      wpCurveShift = data;
      wpRefreshPromise = null;
      wpSync();
      wpClearUpdateError();
    })
    .catch(function (e) {
      wpRefreshPromise = null;
      wpStatus("Update failed: " + e.message, true);
    });
  return wpRefreshPromise;
}
function wpSend(command, value) {
  if (wpCommandBusy) {
    wpStatus("Please wait for the current command to finish", false);
    return Promise.resolve(false);
  }
  wpCommandBusy = true;
  wpSync();
  wpStatus("Sending " + command + " ...", false);
  return fetch(
    "/command?" + encodeURIComponent(command) + "=" + encodeURIComponent(value),
    { cache: "no-store" },
  )
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.text();
    })
    .then(function (message) {
      var response = message.trim() || "Command sent";
      wpStatus(response, false);
      return new Promise(function (resolve) {
        window.setTimeout(function () {
          resolve(response);
        }, 1400);
      });
    })
    .then(function (response) {
      wpCommandBusy = false;
      wpSync();
      wpRefresh();
      return response;
    })
    .catch(function (e) {
      wpCommandBusy = false;
      wpSync();
      wpStatus("Command failed: " + e.message, true);
      throw e;
    });
}
function wpQueueStep(key, command, value) {
  if (wpStepTimers[key]) window.clearTimeout(wpStepTimers[key]);
  wpStatus("Waiting to send " + command + " ...", false);
  wpStepTimers[key] = window.setTimeout(function () {
    delete wpStepTimers[key];
    wpSend(command, value).catch(function () {});
  }, wpStepDebounceMs);
}
function wpStep(command, topic, delta, min, max) {
  var current = Number(wpValues[topic]);
  if (isNaN(current)) return;
  var next = Math.max(min, Math.min(max, Math.round(current + delta)));
  wpValues[topic] = next;
  updCell(topic + "-Value", String(next));
  wpQueueStep("topic:" + topic, command, next);
}
function wpSelectCommand(select, command) {
  select.disabled = true;
  wpSend(command, select.value).then(
    function () {
      select.disabled = false;
    },
    function () {
      select.disabled = false;
    },
  );
}
function wpToggle(toggle, command) {
  toggle.disabled = true;
  wpSend(command, toggle.checked ? 1 : 0).then(
    function () {
      toggle.disabled = false;
    },
    function () {
      toggle.checked = !toggle.checked;
      toggle.disabled = false;
    },
  );
}
function wpSetMaxPump() {
  wpSend(
    "SetMaxPumpDuty",
    wpPumpRaw(document.getElementById("wpMaxPumpSlider").value),
  );
}
function wpSetServicePump(state) {
  if (state && !window.confirm("Run the water pump in 100% service mode?"))
    return;
  wpSend("SetPump", state);
}
function wpResetError() {
  if (window.confirm("Reset the current heat pump error?"))
    wpSend("SetReset", 1);
}
function wpForceDefrost() {
  if (window.confirm("Start the force defrost routine?"))
    wpSend("SetForceDefrost", 1);
}
function wpConfigStep(field, delta) {
  var next = Number(wpConfig[field]) + delta;
  var command = "";
  if (field === "heatMin") {
    next = Math.max(20, Math.min(wpConfig.heatMax, next));
    command = "WpHeatMin";
  } else if (field === "heatMax") {
    next = Math.max(wpConfig.heatMin, Math.min(100, next));
    command = "WpHeatMax";
  } else return;
  wpConfig[field] = next;
  wpSync();
  wpQueueStep("config:" + field, command, next);
}
document.addEventListener("DOMContentLoaded", function () {
  wpRefresh();
  startWebsockets();
  monitorWebSocket();
  window.setInterval(wpRefresh, 10000);
});
