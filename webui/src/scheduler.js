var schedulerData = { entries: [], events: [] };
var schedulerEditingId = 0;
var schedulerBusy = false;
var schedulerRefreshPromise = null;
var schedulerLogEvents = [];
var schedulerLogOffset = 0;
var schedulerLogTotal = 0;
var schedulerLogBusy = false;
var schedulerLogPageSize = 20;
var schedulerDays = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
var schedulerActionLabels = {
  force_dhw_on: "Force DHW on",
  force_dhw_off: "Force DHW off",
  heatpump_on: "Heat pump on",
  heatpump_off: "Heat pump off",
  set_operation_mode: "Set operating mode",
  set_dhw_target: "Set DHW target",
  set_heat_curve_shift: "Set heating curve shift",
  set_z1_heating_water_target: "Set heating water target",
  set_z1_room_target: "Unsupported legacy room target",
  set_z1_request: "Legacy Zone 1 request",
  set_quiet_mode: "Set quiet mode",
};
var schedulerSemantic = null;
var schedulerCurveShift = null;
var schedulerConditionLabels = {
  none: "No condition",
  dhw_temperature: "DHW temperature",
  outside_temperature: "Outside temperature",
  room_temperature: "Zone 1 room temperature",
  main_inlet_temperature: "Main inlet temperature",
  main_outlet_temperature: "Main outlet temperature",
  three_way_valve: "Three-way valve (0=Room, 1=DHW)",
  force_dhw: "Force DHW (0=Off, 1=On)",
};
var schedulerEscape = hmEscape;
function schedulerDayText(mask) {
  var result = [];
  for (var i = 0; i < 7; i++)
    if (mask & (1 << i)) result.push(schedulerDays[i]);
  return result.join(", ");
}
var schedulerPad = hmPad;
function schedulerSemanticMatches(action) {
  return (
    (action === "set_heat_curve_shift" &&
      schedulerCurveShift &&
      schedulerCurveShift.available === true &&
      schedulerCurveShift.writable === true) ||
    (action === "set_z1_heating_water_target" &&
      schedulerSemantic &&
      schedulerSemantic.semantic === "heatingWaterTarget")
  );
}
function schedulerEntryActions(entry) {
  var actions =
    entry && Array.isArray(entry.actions) && entry.actions.length
      ? entry.actions
      : entry && entry.action
    ? [{ action: entry.action, value: entry.actionValue }]
    : [];
  return actions.map(function (action) {
    return action.action === "force_dhw"
      ? { action: "force_dhw_on", value: 0 }
      : action;
  });
}
function schedulerActionText(action) {
  var value = action.value === undefined ? action.actionValue : action.value;
  var label = schedulerActionLabels[action.action] || action.action;
  if (action.action === "set_operation_mode") {
    var modes = [
      "Heat only",
      "Cool only",
      "Auto",
      "DHW only",
      "Heat + DHW",
      "Cool + DHW",
      "Auto + DHW",
    ];
    return label + ": " + (modes[value] || value);
  }
  if (action.action === "set_quiet_mode")
    return (
      label +
      ": " +
      (Number(value) === 0 ? "Off" : "Level " + value)
    );
  if (action.action === "set_dhw_target") return label + ": " + value + " °C";
  if (action.action === "set_heat_curve_shift")
    return (
      label +
      ": " +
      value +
      " K" +
      (schedulerSemanticMatches(action.action) ? "" : " [INCOMPATIBLE]")
    );
  if (action.action === "set_z1_heating_water_target")
    return (
      label +
      ": " +
      value +
      " °C" +
      (schedulerSemanticMatches(action.action) ? "" : " [INCOMPATIBLE]")
    );
  if (action.action === "set_z1_room_target")
    return label + ": " + value + " [REVIEW REQUIRED]";
  if (action.action === "set_z1_request")
    return label + ": " + value + " [REVIEW REQUIRED]";
  return label;
}
function schedulerActionsText(entry) {
  return schedulerEntryActions(entry)
    .map(function (action, index) {
      return index + 1 + ". " + schedulerActionText(action);
    })
    .join(" → ");
}
function schedulerConditionText(entry) {
  var c = entry.conditions || [];
  if (!c.length && entry.conditionField && entry.conditionField !== "none")
    c = [
      {
        source: "local",
        field: entry.conditionField,
        operator: entry.conditionOperator,
        value: entry.conditionValue,
      },
    ];
  if (!c.length) return "Always";
  var groups = [[]];
  c.forEach(function (x, index) {
    if (index > 0 && String(x.join || "and").toLowerCase() === "or")
      groups.push([]);
    var label =
      x.source === "mqtt"
        ? "MQTT sensor " + x.sensorId
        : schedulerConditionLabels[x.field] || x.field;
    groups[groups.length - 1].push(
      label + " " + x.operator + " " + x.value,
    );
  });
  return groups
    .map(function (group) {
      var text = group.join(" AND ");
      return group.length > 1 && groups.length > 1 ? "(" + text + ")" : text;
    })
    .join(" OR ");
}
function schedulerSetStatus(message, isError) {
  var el = document.getElementById("schedulerCommandStatus");
  if (el) {
    el.textContent = message || "";
    el.style.color = isError ? "var(--red)" : "var(--text-muted)";
  }
}
function schedulerSelectDays(mask) {
  document.querySelectorAll(".scheduler-day input").forEach(function (input) {
    input.checked = !!(mask & (1 << Number(input.dataset.day)));
  });
}
function schedulerExternalSensors() {
  return (schedulerData.externalSensors || []).filter(function (s) {
    return s.enabled;
  });
}
function schedulerConditionRow(condition) {
  condition = condition || {
    source: "local",
    field: "dhw_temperature",
    operator: "<",
    value: 42,
  };
  var row = document.createElement("div");
  row.className = "scheduler-condition-row";
  var join = document.createElement("select");
  join.className = "scheduler-input scheduler-condition-join";
  join.innerHTML =
    '<option value="and">AND</option><option value="or">OR</option>';
  join.value = condition.join || "and";
  var source = document.createElement("select");
  source.className = "scheduler-input scheduler-condition-source";
  source.innerHTML =
    '<option value="local">Panasonic</option><option value="mqtt">External MQTT</option>';
  source.value = condition.source || "local";
  var field = document.createElement("select");
  field.className = "scheduler-input scheduler-condition-field";
  var op = document.createElement("select");
  op.className = "scheduler-input scheduler-condition-operator";
  op.innerHTML =
    '<option value="<">&lt;</option><option value="<=">&le;</option><option value="==">=</option><option value=">=">&ge;</option><option value=">">&gt;</option>';
  op.value = condition.operator || "<";
  var value = document.createElement("input");
  value.className = "scheduler-input scheduler-condition-value";
  value.type = "number";
  value.min = "-100";
  value.max = "200";
  value.step = "0.5";
  value.value = condition.value === undefined ? 0 : condition.value;
  var remove = document.createElement("button");
  remove.type = "button";
  remove.className = "btn btn-danger";
  remove.textContent = "×";
  remove.onclick = function () {
    row.remove();
    schedulerUpdateConditionJoins();
  };
  function fill() {
    if (source.value === "local") {
      field.innerHTML =
        '<option value="dhw_temperature">DHW temperature</option><option value="outside_temperature">Outside temperature</option><option value="room_temperature">Zone 1 room temperature</option><option value="main_inlet_temperature">Main inlet temperature</option><option value="main_outlet_temperature">Main outlet temperature</option><option value="three_way_valve">Three-way valve (0=Room, 1=DHW)</option><option value="force_dhw">Force DHW (0=Off, 1=On)</option>';
      field.value = condition.field || "dhw_temperature";
    } else {
      field.innerHTML = schedulerExternalSensors()
        .map(function (s) {
          return (
            '<option value="' +
            s.id +
            '">' +
            schedulerEscape(s.name) +
            " (" +
            schedulerEscape(s.unit || "") +
            ")</option>"
          );
        })
        .join("");
      field.value = String(
        condition.sensorId || (schedulerExternalSensors()[0] || {}).id || "",
      );
    }
  }
  source.onchange = fill;
  row.appendChild(join);
  row.appendChild(source);
  row.appendChild(field);
  row.appendChild(op);
  row.appendChild(value);
  row.appendChild(remove);
  fill();
  return row;
}
function schedulerUpdateConditionJoins() {
  Array.prototype.slice
    .call(document.querySelectorAll(".scheduler-condition-row"))
    .forEach(function (row, index) {
      var join = row.querySelector(".scheduler-condition-join");
      join.disabled = index === 0;
      join.style.visibility = index === 0 ? "hidden" : "visible";
      if (index === 0) join.value = "and";
    });
}
function schedulerSetConditions(conditions) {
  var box = document.getElementById("schedulerConditions");
  box.innerHTML = "";
  (conditions || []).slice(0, 4).forEach(function (c) {
    box.appendChild(schedulerConditionRow(c));
  });
  schedulerUpdateConditionJoins();
}
function schedulerAddCondition() {
  var box = document.getElementById("schedulerConditions");
  if (box.children.length >= 4) {
    schedulerSetStatus("At most four conditions are supported.", true);
    return;
  }
  box.appendChild(schedulerConditionRow());
  schedulerUpdateConditionJoins();
}
function schedulerReadConditions() {
  return Array.prototype.slice
    .call(document.querySelectorAll(".scheduler-condition-row"))
    .map(function (row, index) {
      var source = row.querySelector(".scheduler-condition-source").value,
        field = row.querySelector(".scheduler-condition-field"),
        condition = {
          join:
            index === 0
              ? "and"
              : row.querySelector(".scheduler-condition-join").value,
          source: source,
          operator: row.querySelector(".scheduler-condition-operator").value,
          value: Number(row.querySelector(".scheduler-condition-value").value),
        };
      if (source === "local") condition.field = field.value;
      else condition.sensorId = Number(field.value);
      return condition;
    });
}
function schedulerRender(data) {
  schedulerData = data;
  schedulerSemantic = data.zone1HeatRequest || null;
  schedulerCurveShift = data.heatingCurveShift || null;
  document.getElementById("schedulerLocalTime").textContent =
    data.localTime || "Time unavailable";
  var timeStatus = document.getElementById("schedulerTimeStatus");
  timeStatus.textContent = data.ntpSynchronized
    ? "Last NTP sync: " + (data.lastNtpSync || "")
    : data.timeValid
      ? "Clock valid · NTP pending"
      : "Unavailable - paused";
  timeStatus.className =
    "scheduler-status-value " +
    (data.timeValid ? "scheduler-sync-good" : "scheduler-sync-bad");
  document.getElementById("schedulerActiveCount").textContent =
    String(data.enabledCount) +
    " / " +
    String(data.count) +
    " (" +
    String(data.maxEntries) +
    " max)";
  document.getElementById("schedulerPendingCount").textContent = String(
    data.pendingActions,
  );
  var ps = document.getElementById("schedulerPanasonicState");
  ps.textContent = data.panasonicSchedulerKnown
    ? data.panasonicSchedulerEnabled
      ? "Enabled - possible conflict"
      : "Disabled"
    : "Unavailable";
  ps.className =
    "scheduler-status-value " +
    (data.panasonicSchedulerEnabled
      ? "scheduler-sync-bad"
      : "scheduler-sync-good");
  document.getElementById("schedulerNextAction").textContent =
    data.nextScheduledAction || "None configured";
  var last = data.events && data.events.length ? data.events[0] : null;
  document.getElementById("schedulerLastAction").textContent = last
    ? last.time + " · " + last.name + " -> " + last.result + " · " + last.detail
    : "No action since boot";
  var globalToggle = document.getElementById("schedulerGlobalToggle");
  globalToggle.checked = !!data.enabled;
  globalToggle.disabled = schedulerBusy;
  document.getElementById("schedulerGlobalLabel").textContent = data.enabled
    ? "Enabled"
    : "Paused";
  var rows = document.getElementById("schedulerRows");
  if (!data.entries || !data.entries.length) {
    rows.innerHTML =
      "<tr><td colspan='7' class='scheduler-empty'>No schedules configured. Add the first one above.</td></tr>";
  } else {
    rows.innerHTML = data.entries
      .slice()
      .sort(function (a, b) {
        var timeDifference =
          Number(a.hour) * 60 +
          Number(a.minute) -
          (Number(b.hour) * 60 + Number(b.minute));
        if (timeDifference) return timeDifference;
        var nameDifference = String(a.name || "").localeCompare(
          String(b.name || ""),
        );
        return nameDifference || Number(a.id) - Number(b.id);
      })
      .map(function (entry) {
        return (
          '<tr><td><label class="dashboard-toggle"><input type="checkbox" ' +
          (entry.enabled ? "checked " : "") +
          'onchange="schedulerToggleEntry(' +
          entry.id +
          ',this.checked)"><span class="dashboard-toggle-slider"></span></label></td><td class="scheduler-name">' +
          schedulerEscape(entry.name) +
          "</td><td>" +
          schedulerEscape(schedulerDayText(entry.days)) +
          '</td><td class="scheduler-mono">' +
          schedulerPad(entry.hour) +
          ":" +
          schedulerPad(entry.minute) +
          "</td><td>" +
          schedulerEscape(schedulerConditionText(entry)) +
          "</td><td>" +
          schedulerEscape(schedulerActionsText(entry)) +
          '</td><td><div class="scheduler-actions"><button class="btn btn-ghost" onclick="schedulerRun(' +
          entry.id +
          ')">Run</button><button class="btn btn-ghost" onclick="schedulerOpenEditor(' +
          entry.id +
          ')">Edit</button><button class="btn btn-danger" onclick="schedulerDelete(' +
          entry.id +
          ')">Delete</button></div></td></tr>'
        );
      })
      .join("");
  }
}
function schedulerParsePersistentEvent(event) {
  var message = String(event.message || "");
  var match = /^#(\d+)\s+(.*?)\s+->\s+([^:]+):\s*(.*)$/.exec(message);
  return {
    t: Number(event.t),
    id: match ? Number(match[1]) : Number(event.value),
    name: match ? match[2] : "#" + Number(event.value),
    result: match ? match[3].trim() : "stored",
    detail: match ? match[4] : message,
  };
}
function schedulerRenderPersistentEvents(error) {
  var events = document.getElementById("schedulerEvents");
  var status = document.getElementById("schedulerLogStatus");
  var loadMore = document.getElementById("schedulerLoadMore");
  if (!schedulerLogEvents.length) {
    events.innerHTML =
      "<tr><td colspan='4' class='scheduler-empty'>" +
      schedulerEscape(
        error ||
          (schedulerLogBusy
            ? "Reading scheduler executions from SD card ..."
            : "No scheduler executions stored on SD card."),
      ) +
      "</td></tr>";
  } else {
    events.innerHTML = schedulerLogEvents
      .map(function (event) {
        var resultClass = String(event.result)
          .toLowerCase()
          .replace(/[^a-z0-9_-]+/g, "-");
        return (
          '<tr><td class="scheduler-mono">' +
          schedulerEscape(hmFormatDateTime24(event.t * 1000)) +
          "</td><td>" +
          schedulerEscape(event.name || "#" + event.id) +
          '</td><td class="scheduler-event-result ' +
          schedulerEscape(resultClass) +
          '">' +
          schedulerEscape(event.result) +
          "</td><td>" +
          schedulerEscape(event.detail) +
          "</td></tr>"
        );
      })
      .join("");
  }
  if (status) {
    status.textContent = error
      ? error
      : schedulerLogBusy
        ? "Reading SD card ..."
        : "Showing " +
          schedulerLogEvents.length +
          " of " +
          schedulerLogTotal +
          " stored executions";
  }
  if (loadMore) {
    loadMore.disabled = schedulerLogBusy;
    loadMore.hidden =
      !schedulerLogBusy && schedulerLogOffset >= schedulerLogTotal;
    loadMore.textContent = schedulerLogBusy ? "Loading ..." : "Load more";
  }
}
function schedulerLoadEvents(reset) {
  if (schedulerLogBusy) return Promise.resolve();
  if (reset) {
    schedulerLogEvents = [];
    schedulerLogOffset = 0;
    schedulerLogTotal = 0;
  }
  schedulerLogBusy = true;
  schedulerRenderPersistentEvents();
  return fetch(
    "/schedulerlogapi?offset=" +
      schedulerLogOffset +
      "&limit=" +
      schedulerLogPageSize,
    { cache: "no-store" },
  )
    .then(function (response) {
      if (!response.ok) throw new Error("HTTP " + response.status);
      return response.json();
    })
    .then(function (data) {
      if (data.error) throw new Error(data.error);
      if (data.source !== "sd") throw new Error("Unexpected log source");
      var page = (data.events || []).map(schedulerParsePersistentEvent);
      schedulerLogEvents = schedulerLogEvents.concat(page);
      schedulerLogOffset += page.length;
      schedulerLogTotal = Number(data.eventCount) || 0;
      schedulerLogBusy = false;
      schedulerRenderPersistentEvents();
    })
    .catch(function (error) {
      schedulerLogBusy = false;
      schedulerRenderPersistentEvents(
        "Could not read scheduler log from SD card: " + error.message,
      );
    });
}
function schedulerLoadMoreEvents() {
  return schedulerLoadEvents(false);
}
function schedulerRefresh() {
  if (schedulerRefreshPromise) return schedulerRefreshPromise;
  schedulerRefreshPromise = fetch("/schedulerapi", { cache: "no-store" })
    .then(function (response) {
      if (!response.ok) throw new Error("HTTP " + response.status);
      return response.json();
    })
    .then(function (data) {
      schedulerRefreshPromise = null;
      schedulerRender(data);
      return data;
    })
    .catch(function (error) {
      schedulerRefreshPromise = null;
      schedulerSetStatus("Refresh failed: " + error.message, true);
      throw error;
    });
  return schedulerRefreshPromise;
}
function schedulerCommand(name, value) {
  if (schedulerBusy) {
    schedulerSetStatus("Please wait for the current scheduler command.", false);
    return Promise.reject(new Error("busy"));
  }
  schedulerBusy = true;
  schedulerSetStatus("Saving ...", false);
  return fetch("/schedulercommand", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: encodeURIComponent(name) + "=" + encodeURIComponent(value),
    cache: "no-store",
  })
    .then(function (response) {
      if (!response.ok) throw new Error("HTTP " + response.status);
      return response.text();
    })
    .then(function (message) {
      if (/^ERROR:/m.test(message))
        throw new Error(message.replace(/^ERROR:\s*/, "").trim());
      schedulerSetStatus(
        message.replace(/^OK:\s*/, "").trim() || "Done",
        false,
      );
      return schedulerRefresh();
    })
    .then(function (data) {
      schedulerBusy = false;
      schedulerRender(data);
      return data;
    })
    .catch(function (error) {
      schedulerBusy = false;
      if (error.message !== "busy")
        schedulerSetStatus("Command failed: " + error.message, true);
      throw error;
    });
}
function schedulerSetGlobal(enabled) {
  schedulerCommand("enabled", enabled ? 1 : 0);
}
function schedulerFind(id) {
  return (schedulerData.entries || []).find(function (entry) {
    return Number(entry.id) === Number(id);
  });
}
function schedulerToggleEntry(id, enabled) {
  var entry = schedulerFind(id);
  if (!entry) return;
  var copy = Object.assign({}, entry, { enabled: enabled });
  schedulerCommand("save", JSON.stringify(copy)).catch(function () {
    schedulerRefresh();
  });
}
function schedulerRun(id) {
  schedulerCommand("run", id);
}
function schedulerDelete(id) {
  var entry = schedulerFind(id);
  if (entry && window.confirm('Delete schedule "' + entry.name + '"?'))
    schedulerCommand("delete", id);
}
function schedulerOpenEditor(id) {
  var entry = id ? schedulerFind(id) : null;
  schedulerEditingId = entry ? entry.id : 0;
  document.getElementById("schedulerEditorTitle").textContent = entry
    ? "Edit schedule"
    : "Add schedule";
  document.getElementById("schedulerName").value = entry ? entry.name : "";
  document.getElementById("schedulerTime").value = entry
    ? schedulerPad(entry.hour) + ":" + schedulerPad(entry.minute)
    : "06:00";
  document.getElementById("schedulerEntryEnabled").checked = entry
    ? !!entry.enabled
    : true;
  var mask = entry ? Number(entry.days) : 31;
  document.querySelectorAll(".scheduler-day input").forEach(function (input) {
    input.checked = !!(mask & (1 << Number(input.dataset.day)));
  });
  schedulerSetActions(
    entry
      ? schedulerEntryActions(entry)
      : [{ action: "force_dhw_on", value: 0 }],
  );
  var conditions =
    entry && entry.conditions
      ? entry.conditions
      : entry && entry.conditionField && entry.conditionField !== "none"
        ? [
            {
              source: "local",
              field: entry.conditionField,
              operator: entry.conditionOperator,
              value: entry.conditionValue,
            },
          ]
        : [];
  schedulerSetConditions(conditions);
  document.getElementById("schedulerModal").classList.add("open");
  document.getElementById("schedulerName").focus();
}
function schedulerCloseEditor() {
  document.getElementById("schedulerModal").classList.remove("open");
}
function schedulerActionOptions(select, selectedAction) {
  select.innerHTML =
    '<option value="force_dhw_on">Force DHW on</option><option value="force_dhw_off">Force DHW off</option><option value="heatpump_on">Heat pump on</option><option value="heatpump_off">Heat pump off</option><option value="set_operation_mode">Set operating mode</option><option value="set_dhw_target">Set DHW target</option><option value="set_heat_curve_shift">Set heating curve shift</option><option value="set_z1_heating_water_target">Set heating water target</option><option value="set_z1_room_target" disabled>Unsupported room target (review required)</option><option value="set_z1_request" disabled>Legacy Zone 1 request (review required)</option><option value="set_quiet_mode">Set quiet mode</option>';
  ["set_heat_curve_shift", "set_z1_heating_water_target"].forEach(function (action) {
    var option = select.querySelector('option[value="' + action + '"]');
    option.disabled = !schedulerSemanticMatches(action);
  });
  select.value = selectedAction || "force_dhw_on";
}
function schedulerActionChanged(select, selectedValue) {
  var row = select.closest(".scheduler-action-row");
  var action = select.value;
  var container = row.querySelector(".scheduler-action-value");
  if (action === "set_operation_mode") {
    container.innerHTML =
      '<select class="scheduler-input scheduler-action-value-input"><option value="0">Heat only</option><option value="1">Cool only</option><option value="2">Auto</option><option value="3">DHW only</option><option value="4">Heat + DHW</option><option value="5">Cool + DHW</option><option value="6">Auto + DHW</option></select>';
  } else if (action === "set_quiet_mode") {
    container.innerHTML =
      '<select class="scheduler-input scheduler-action-value-input"><option value="0">Off</option><option value="1">Level 1</option><option value="2">Level 2</option><option value="3">Level 3</option></select>';
  } else if (action === "set_dhw_target") {
    container.innerHTML =
      '<input class="scheduler-input scheduler-action-value-input" type="number" min="40" max="75" step="1" value="45">';
  } else if (action === "set_heat_curve_shift") {
    container.innerHTML =
      '<input class="scheduler-input scheduler-action-value-input" type="number" min="-5" max="5" step="1" value="0">';
  } else if (action === "set_z1_heating_water_target") {
    var min =
      schedulerSemantic && Number.isFinite(Number(schedulerSemantic.min))
        ? schedulerSemantic.min
        : 20;
    var max =
      schedulerSemantic && Number.isFinite(Number(schedulerSemantic.max))
        ? schedulerSemantic.max
        : 65;
    container.innerHTML =
      '<input class="scheduler-input scheduler-action-value-input" type="number" min="' +
      min +
      '" max="' +
      max +
      '" step="1" value="' +
      min +
      '">';
  } else if (action === "set_z1_room_target") {
    container.innerHTML =
      '<input class="scheduler-input scheduler-action-value-input" type="number" min="10" max="35" step="1" value="20">';
  } else if (action === "force_dhw_on" || action === "force_dhw_off") {
    container.innerHTML =
      '<input class="scheduler-action-value-input" type="hidden" value="0"><span class="dashboard-muted">Direct Panasonic command; operating mode is not changed</span>';
  } else {
    container.innerHTML =
      '<input class="scheduler-action-value-input" type="hidden" value="0"><span class="dashboard-muted">No value required</span>';
  }
  if (selectedValue !== undefined)
    container.querySelector(".scheduler-action-value-input").value =
      String(selectedValue);
}
function schedulerActionRow(action) {
  action = action || { action: "force_dhw_on", value: 0 };
  var row = document.createElement("div");
  row.className = "scheduler-action-row";
  var type = document.createElement("select");
  type.className = "scheduler-input scheduler-action-type";
  schedulerActionOptions(type, action.action);
  type.onchange = function () {
    schedulerActionChanged(type);
  };
  var value = document.createElement("div");
  value.className = "scheduler-action-value";
  var up = document.createElement("button");
  up.type = "button";
  up.className = "btn btn-ghost scheduler-action-up";
  up.textContent = "↑";
  up.title = "Move action earlier";
  up.onclick = function () {
    if (row.previousElementSibling)
      row.parentNode.insertBefore(row, row.previousElementSibling);
    schedulerUpdateActionButtons();
  };
  var down = document.createElement("button");
  down.type = "button";
  down.className = "btn btn-ghost scheduler-action-down";
  down.textContent = "↓";
  down.title = "Move action later";
  down.onclick = function () {
    if (row.nextElementSibling)
      row.parentNode.insertBefore(row.nextElementSibling, row);
    schedulerUpdateActionButtons();
  };
  var remove = document.createElement("button");
  remove.type = "button";
  remove.className = "btn btn-danger scheduler-action-remove";
  remove.textContent = "×";
  remove.title = "Remove action";
  remove.onclick = function () {
    row.remove();
    schedulerUpdateActionButtons();
  };
  row.appendChild(type);
  row.appendChild(value);
  row.appendChild(up);
  row.appendChild(down);
  row.appendChild(remove);
  schedulerActionChanged(type, action.value);
  return row;
}
function schedulerUpdateActionButtons() {
  var rows = Array.prototype.slice.call(
    document.querySelectorAll(".scheduler-action-row"),
  );
  rows.forEach(function (row, index) {
    row.querySelector(".scheduler-action-up").disabled = index === 0;
    row.querySelector(".scheduler-action-down").disabled =
      index === rows.length - 1;
    row.querySelector(".scheduler-action-remove").disabled = rows.length === 1;
  });
}
function schedulerSetActions(actions) {
  var box = document.getElementById("schedulerActionsEditor");
  box.innerHTML = "";
  (actions || []).slice(0, 4).forEach(function (action) {
    box.appendChild(schedulerActionRow(action));
  });
  if (!box.children.length) box.appendChild(schedulerActionRow());
  schedulerUpdateActionButtons();
}
function schedulerAddAction() {
  var box = document.getElementById("schedulerActionsEditor");
  if (box.children.length >= 4) {
    schedulerSetStatus("At most four actions are supported.", true);
    return;
  }
  box.appendChild(schedulerActionRow());
  schedulerUpdateActionButtons();
}
function schedulerReadActions() {
  return Array.prototype.slice
    .call(document.querySelectorAll(".scheduler-action-row"))
    .map(function (row) {
      return {
        action: row.querySelector(".scheduler-action-type").value,
        value: Number(
          row.querySelector(".scheduler-action-value-input").value,
        ),
      };
    });
}
function schedulerSaveEditor() {
  var name = document.getElementById("schedulerName").value.trim();
  var time = document.getElementById("schedulerTime").value;
  if (!name) {
    schedulerSetStatus("A schedule name is required.", true);
    return;
  }
  if (!time || time.indexOf(":") < 0) {
    schedulerSetStatus("A valid execution time is required.", true);
    return;
  }
  var days = 0;
  document.querySelectorAll(".scheduler-day input").forEach(function (input) {
    if (input.checked) days |= 1 << Number(input.dataset.day);
  });
  if (!days) {
    schedulerSetStatus("Select at least one weekday.", true);
    return;
  }
  var parts = time.split(":");
  var hour = Number(parts[0]);
  var minute = Number(parts[1]);
  if (
    !Number.isInteger(hour) ||
    hour < 0 ||
    hour > 23 ||
    !Number.isInteger(minute) ||
    minute < 0 ||
    minute > 59
  ) {
    schedulerSetStatus("Time must be between 00:00 and 23:59.", true);
    return;
  }
  var actions = schedulerReadActions();
  var validActions = [
    "force_dhw_on",
    "force_dhw_off",
    "heatpump_on",
    "heatpump_off",
    "set_operation_mode",
    "set_dhw_target",
    "set_heat_curve_shift",
    "set_z1_heating_water_target",
    "set_quiet_mode",
  ];
  if (!actions.length || actions.length > 4) {
    schedulerSetStatus("One to four actions are required.", true);
    return;
  }
  for (var ai = 0; ai < actions.length; ai++) {
    var action = actions[ai].action;
    var actionValue = actions[ai].value;
    if (validActions.indexOf(action) < 0 || !Number.isFinite(actionValue)) {
      schedulerSetStatus("Select a valid action for step " + (ai + 1) + ".", true);
      return;
    }
    if (
      (action === "set_heat_curve_shift" &&
        (actionValue < -5 || actionValue > 5)) ||
      (action === "set_z1_heating_water_target" &&
        (actionValue < Number(schedulerSemantic ? schedulerSemantic.min : 20) ||
          actionValue >
            Number(schedulerSemantic ? schedulerSemantic.max : 65))) ||
      (action === "set_operation_mode" &&
        (actionValue < 0 || actionValue > 6)) ||
      (action === "set_dhw_target" &&
        (actionValue < 40 || actionValue > 75)) ||
      (action === "set_quiet_mode" &&
        (actionValue < 0 || actionValue > 3))
    ) {
      schedulerSetStatus(
        "Action value for step " + (ai + 1) + " is outside its supported range.",
        true,
      );
      return;
    }
    if (
      (action === "set_heat_curve_shift" ||
        action === "set_z1_heating_water_target") &&
      !schedulerSemanticMatches(action)
    ) {
      schedulerSetStatus(
        "Action " + (ai + 1) + " does not match the current Zone 1 control configuration.",
        true,
      );
      return;
    }
  }
  var conditions = schedulerReadConditions();
  if (conditions.length > 4) {
    schedulerSetStatus("At most four conditions are supported.", true);
    return;
  }
  for (var ci = 0; ci < conditions.length; ci++) {
    if (
      !Number.isFinite(conditions[ci].value) ||
      conditions[ci].value < -100 ||
      conditions[ci].value > 200 ||
      (conditions[ci].source === "mqtt" && !conditions[ci].sensorId)
    ) {
      schedulerSetStatus("Condition is outside its supported range.", true);
      return;
    }
  }
  var payload = {
    id: schedulerEditingId,
    enabled: document.getElementById("schedulerEntryEnabled").checked,
    name: name,
    days: days,
    hour: hour,
    minute: minute,
    actions: actions,
    action: actions[0].action,
    actionValue: actions[0].value,
    conditions: conditions,
  };
  schedulerCommand("save", JSON.stringify(payload)).then(function () {
    schedulerCloseEditor();
  });
}
document.addEventListener("keydown", function (event) {
  if (event.key === "Escape") schedulerCloseEditor();
});
document.addEventListener("DOMContentLoaded", function () {
  schedulerRefresh();
  schedulerLoadEvents(true);
  window.setInterval(schedulerRefresh, 10000);
});
