var wpValues = {};
var wpConfig = { heatMin: 20, heatMax: 65, dhwBlockAbove: 75 };
var wpCurveShift = null;
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
  updCell("wpDhwBlockValue", String(wpConfig.dhwBlockAbove));
  var curveAvailable = !!(
    wpCurveShift &&
    wpCurveShift.implementation === "curveEndpoints" &&
    wpCurveShift.writable === true
  );
  document.querySelectorAll(".wp-curve-row").forEach(function (row) {
    row.style.display = curveAvailable ? "flex" : "none";
  });
  var curveNote = document.getElementById("wpCurveUnavailable");
  if (curveNote) curveNote.style.display = curveAvailable ? "none" : "block";
  if (curveNote && !curveAvailable)
    curveNote.textContent =
      wpCurveShift && wpCurveShift.available
        ? "This sensor configuration does not adjust curve endpoints directly."
        : "Heating-curve endpoint configuration is unavailable for the current heat-pump data.";
  if (curveAvailable) {
    updCell("wpCurveBaseHigh", String(wpCurveShift.baseTargetHigh));
    updCell("wpCurveBaseLow", String(wpCurveShift.baseTargetLow));
    updCell("wpCurveEffectiveHigh", String(wpCurveShift.effectiveTargetHigh));
    updCell("wpCurveEffectiveLow", String(wpCurveShift.effectiveTargetLow));
  }
  if (wpCommandBusy)
    document
      .querySelectorAll(
        ".wp-settings-page button,.wp-settings-page select,.wp-settings-page input",
      )
      .forEach(function (control) {
        control.disabled = true;
      });
}
function wpRefresh() {
  if (wpRefreshPromise) return wpRefreshPromise;
  wpRefreshPromise = fetch("/json", { cache: "no-store" })
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (data) {
      wpItems(data).forEach(function (item) {
        wpValues[item.Topic] = item.Value;
        updCell(item.Topic + "-Value", String(item.Value));
        updCell(item.Topic + "-Description", String(item.Description));
      });
      return fetch("/wpsettingsconfig", { cache: "no-store" });
    })
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (data) {
      wpConfig = data;
      return fetch("/heatingcurveshift", { cache: "no-store" });
    })
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (data) {
      wpCurveShift = data;
      wpRefreshPromise = null;
      wpSync();
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
      wpStatus(message.trim() || "Command sent", false);
      return new Promise(function (resolve) {
        window.setTimeout(resolve, 1400);
      });
    })
    .then(function () {
      wpCommandBusy = false;
      wpSync();
      wpRefresh();
      return true;
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
  } else {
    next = Math.max(40, Math.min(100, next));
    command = "WpDhwBlockAbove";
  }
  wpConfig[field] = next;
  wpSync();
  wpQueueStep("config:" + field, command, next);
}
function wpStepCurveBase(which, delta) {
  if (
    !wpCurveShift ||
    wpCurveShift.implementation !== "curveEndpoints" ||
    wpCurveShift.writable !== true
  ) {
    wpStatus("Heating-curve endpoint configuration is unavailable", true);
    return;
  }
  var key = which === "high" ? "baseTargetHigh" : "baseTargetLow",
    current = Number(wpCurveShift[key]);
  if (!Number.isFinite(current)) return;
  wpCurveShift[key] = Math.round(current + delta);
  wpCurveShift.effectiveTargetHigh =
    Number(wpCurveShift.baseTargetHigh) + Number(wpCurveShift.shift);
  wpCurveShift.effectiveTargetLow =
    Number(wpCurveShift.baseTargetLow) + Number(wpCurveShift.shift);
  wpSync();
  wpQueueStep(
    "curve:" + which,
    which === "high" ? "SetZ1HeatCurveBaseHigh" : "SetZ1HeatCurveBaseLow",
    wpCurveShift[key],
  );
}
document.addEventListener("DOMContentLoaded", function () {
  wpRefresh();
  startWebsockets();
  monitorWebSocket();
  window.setInterval(wpRefresh, 10000);
});
