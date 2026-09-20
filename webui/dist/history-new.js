var hnData = null,
  hnEvents = [],
  hnViews = {},
  hnActivityView = null,
  hnSelectedTimestamp = null,
  hnRequestStart = 0,
  hnRequestEnd = 0,
  hnRequestSequence = 0,
  hnZoomBase = null,
  hnDrag = null;

var HN_HEAT_COMPRESSOR = 1,
  HN_HEAT_INTERNAL = 2,
  HN_HEAT_EXTERNAL = 4,
  HN_DHW_COMPRESSOR = 8,
  HN_DHW_INTERNAL = 16,
  HN_DHW_EXTERNAL = 32,
  HN_DEFROST = 64,
  HN_SYSTEM_ON = 128;

var hnLines = {
  dhwTarget: { key: "dhwTarget", label: "Tank setpoint", color: "#1e88e5", unit: "°C" },
  dhw: { key: "dhw", label: "Tank actual", color: "#ef6c00", unit: "°C" },
  roomTarget: { key: "roomSetpoint", label: "Room setpoint", color: "#1e88e5", unit: "°C" },
  room: { key: "room", label: "Room actual", color: "#ef6c00", unit: "°C" },
  outside: { key: "outside", label: "Outside", color: "#43a047", unit: "°C" },
  target: { key: "target", label: "Main water target", color: "#ef6c00", unit: "°C" },
  controllerTarget: { key: "controllerTarget", label: "Zone 1 direct target", color: "#8e44ad", unit: "°C" },
  inlet: { key: "inlet", label: "Return / inlet", color: "#1e88e5", unit: "°C" },
  outlet: { key: "outlet", label: "Flow / outlet", color: "#43a047", unit: "°C" },
  thermal: { key: "activeThermal", label: "Thermal output", color: "#00897b", unit: "kW" },
  electrical: { key: "electrical", label: "Electrical input", color: "#c62828", unit: "kW" },
  heatCop: { key: "heatCop", label: "Room-heating COP", color: "#2e7d32", unit: "" },
  dhwCop: { key: "dhwCop", label: "DHW COP", color: "#1565c0", unit: "" },
};

var hnCharts = [
  { id: "hnDhw", legend: "hnDhwLegend", unit: "°C", activity: "dhw", lines: [hnLines.dhwTarget, hnLines.dhw] },
  { id: "hnRoom", legend: "hnRoomLegend", unit: "°C", activity: "heat", lines: [hnLines.roomTarget, hnLines.room, hnLines.outside] },
  { id: "hnWater", legend: "hnWaterLegend", unit: "°C", activity: "heat", lines: [hnLines.target, hnLines.controllerTarget, hnLines.inlet, hnLines.outlet] },
  { id: "hnPower", legend: "hnPowerLegend", unit: "kW", activity: "compressor", lines: [hnLines.thermal, hnLines.electrical] },
  { id: "hnCop", legend: "hnCopLegend", unit: "COP", activity: "compressor", lines: [hnLines.heatCop, hnLines.dhwCop] },
];

function hnNumber(value) {
  if (value === null || value === undefined || value === "") return null;
  var number = Number(value);
  return Number.isFinite(number) ? number : null;
}
function hnValue(point, line) {
  return point ? hnNumber(point[line.key]) : null;
}
function hnFormat(value, decimals) {
  var number = hnNumber(value);
  return number === null ? "—" : number.toFixed(decimals === undefined ? 1 : decimals);
}
function hnClock(timestamp, includeDate) {
  if (!Number.isFinite(timestamp) || timestamp < 1000000000) return "N/A";
  var date = new Date(timestamp * 1000),
    clock = date.toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit", hourCycle: "h23" });
  return includeDate
    ? date.toLocaleDateString("de-DE", { day: "2-digit", month: "2-digit" }) + " " + clock
    : clock;
}
function hnDuration(seconds) {
  seconds = Math.max(0, Math.round(seconds || 0));
  var hours = Math.floor(seconds / 3600),
    minutes = Math.floor((seconds % 3600) / 60);
  return hours ? hours + " h " + minutes + " min" : minutes + " min";
}
function hnBitActive(state, mask) {
  return !!state && (Number(state.active) & mask) !== 0;
}
function hnCompressorActive(state) {
  return hnBitActive(state, HN_HEAT_COMPRESSOR | HN_DHW_COMPRESSOR | HN_DEFROST);
}
function hnSourceStates(data) {
  if (Array.isArray(data.sourceStates) && data.sourceStates.length) return data.sourceStates;
  return (data.samples || []).map(function (point) {
    var active = 0,
      dhw = point.dhwActive === true || Number(point.valve) === 1;
    if (point.defrost === true) active |= HN_DEFROST;
    else if (point.compressor === true) active |= dhw ? HN_DHW_COMPRESSOR : HN_HEAT_COMPRESSOR;
    if (point.internalHeater === true) active |= dhw ? HN_DHW_INTERNAL : HN_HEAT_INTERNAL;
    if (point.externalHeater === true) active |= dhw ? HN_DHW_EXTERNAL : HN_HEAT_EXTERNAL;
    if (point.compressor === true || Number(point.state) > 0) active |= HN_SYSTEM_ON;
    return { t: point.t, active: active, known: 255 };
  });
}
function hnStateAt(timestamp) {
  var states = (hnData && hnData.sourceStates) || [],
    result = null;
  for (var i = 0; i < states.length; i++) {
    if (Number(states[i].t) > timestamp) break;
    result = states[i];
  }
  return result || states[0] || null;
}
function hnNearestPoint(timestamp) {
  var points = (hnData && hnData.samples) || [],
    best = null,
    distance = Infinity;
  points.forEach(function (point) {
    var current = Math.abs(Number(point.t) - timestamp);
    if (current < distance) {
      best = point;
      distance = current;
    }
  });
  return best;
}

function hnLegend(id, lines) {
  document.getElementById(id).innerHTML = lines.map(function (line) {
    return '<span class="history-legend-item"><i class="history-legend-line" style="background:' +
      line.color + '"></i>' + hmEscape(line.label) + "</span>";
  }).join("");
}
function hnCanvasPosition(canvas, event) {
  var bounds = canvas.getBoundingClientRect();
  return { x: event.clientX - bounds.left, y: event.clientY - bounds.top };
}
function hnSetupInteraction(view) {
  var canvas = view.canvas;
  if (canvas._hnReady) return;
  canvas._hnReady = true;
  canvas.addEventListener("pointerdown", function (event) {
    var current = canvas.id === "hnActivity" ? hnActivityView : hnViews[canvas.id],
      point = hnCanvasPosition(canvas, event);
    if (!current || event.button !== 0 || point.x < current.left || point.x > current.width - current.right) return;
    hnDrag = { view: current, startX: point.x, currentX: point.x, pointerId: event.pointerId };
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
  });
  canvas.addEventListener("pointermove", function (event) {
    var current = canvas.id === "hnActivity" ? hnActivityView : hnViews[canvas.id],
      point = hnCanvasPosition(canvas, event);
    if (!current) return;
    if (hnDrag && hnDrag.view === current) {
      hnDrag.currentX = Math.max(current.left, Math.min(current.width - current.right, point.x));
      hnPaintAll();
      return;
    }
    if (point.x < current.left || point.x > current.width - current.right || point.y < current.top || point.y > current.height - current.bottom) return;
    hnSelectTimestamp(current.timeAtX(point.x));
  });
  canvas.addEventListener("pointerup", function (event) {
    if (!hnDrag || hnDrag.pointerId !== event.pointerId) return;
    var drag = hnDrag,
      startX = Math.min(drag.startX, drag.currentX),
      endX = Math.max(drag.startX, drag.currentX);
    hnDrag = null;
    if (endX - startX >= 8) hnZoom(drag.view.timeAtX(startX), drag.view.timeAtX(endX));
    else hnSelectTimestamp(drag.view.timeAtX(endX));
  });
  canvas.addEventListener("pointercancel", function () {
    hnDrag = null;
    hnPaintAll();
  });
}

function hnActivityMatches(state, activity) {
  if (activity === "dhw") return hnBitActive(state, HN_DHW_COMPRESSOR | HN_DHW_INTERNAL | HN_DHW_EXTERNAL);
  if (activity === "heat") return hnBitActive(state, HN_HEAT_COMPRESSOR | HN_HEAT_INTERNAL | HN_HEAT_EXTERNAL);
  return hnCompressorActive(state);
}
function hnDrawActivityShading(ctx, view) {
  if (!view.activity || !hnData) return;
  var states = hnData.sourceStates || [];
  ctx.fillStyle = view.activity === "dhw" ? "rgba(21,101,192,.08)" : "rgba(239,108,0,.07)";
  states.forEach(function (state, index) {
    if (!hnActivityMatches(state, view.activity)) return;
    var end = index + 1 < states.length ? Number(states[index + 1].t) : hnRequestEnd,
      x1 = view.xTime(Math.max(hnRequestStart, Number(state.t))),
      x2 = view.xTime(Math.min(hnRequestEnd, end));
    ctx.fillRect(x1, view.top, Math.max(1, x2 - x1), view.plotH);
  });
}
function hnCreateChart(config) {
  var canvas = document.getElementById(config.id),
    ctx = canvas.getContext("2d"),
    density = window.devicePixelRatio || 1,
    width = canvas.clientWidth || 700,
    height = 240,
    points = hnData.samples || [],
    values = [];
  config.lines.forEach(function (line) {
    points.forEach(function (point) {
      var value = hnValue(point, line);
      if (value !== null) values.push(value);
    });
  });
  canvas.width = width * density;
  canvas.height = height * density;
  ctx.setTransform(density, 0, 0, density, 0, 0);
  var min = values.length ? Math.min.apply(null, values) : 0,
    max = values.length ? Math.max.apply(null, values) : 1;
  if (min === max) { min -= 1; max += 1; }
  else {
    var padding = (max - min) * 0.08;
    min -= padding;
    max += padding;
  }
  var view = {
    canvas: canvas, ctx: ctx, width: width, height: height, points: points,
    lines: config.lines, unit: config.unit, activity: config.activity,
    min: min, max: max, left: 50, right: 12, top: 18, bottom: 34,
  };
  view.plotW = width - view.left - view.right;
  view.plotH = height - view.top - view.bottom;
  view.xTime = function (time) { return view.left + view.plotW * (time - hnRequestStart) / Math.max(1, hnRequestEnd - hnRequestStart); };
  view.timeAtX = function (x) { return hnRequestStart + (hnRequestEnd - hnRequestStart) * (x - view.left) / view.plotW; };
  view.y = function (value) { return view.top + view.plotH - view.plotH * (value - view.min) / (view.max - view.min); };
  hnViews[config.id] = view;
  hnSetupInteraction(view);
  hnLegend(config.legend, config.lines);
  hnPaintChart(view);
}
function hnPaintChart(view) {
  var ctx = view.ctx,
    includeDate = hnRequestEnd - hnRequestStart >= 43200;
  ctx.clearRect(0, 0, view.width, view.height);
  hnDrawActivityShading(ctx, view);
  ctx.font = "11px Arial";
  ctx.fillStyle = "#75818a";
  ctx.strokeStyle = "rgba(117,129,138,.22)";
  ctx.lineWidth = 1;
  for (var tick = 0; tick <= 4; tick++) {
    var value = view.max - (view.max - view.min) * tick / 4,
      y = view.top + view.plotH * tick / 4;
    ctx.beginPath(); ctx.moveTo(view.left, y); ctx.lineTo(view.width - view.right, y); ctx.stroke();
    ctx.fillText(value.toFixed(1), 4, y + 4);
  }
  ctx.fillText(view.unit, 4, view.top - 5);
  ctx.strokeStyle = "#75818a";
  ctx.beginPath(); ctx.moveTo(view.left, view.top); ctx.lineTo(view.left, view.top + view.plotH); ctx.lineTo(view.width - view.right, view.top + view.plotH); ctx.stroke();
  ctx.textAlign = "center";
  [0, .25, .5, .75, 1].forEach(function (fraction) {
    var time = hnRequestStart + (hnRequestEnd - hnRequestStart) * fraction;
    ctx.fillText(hnClock(time, includeDate), view.left + view.plotW * fraction, view.height - 12);
  });
  ctx.textAlign = "left";
  view.lines.forEach(function (line) {
    ctx.strokeStyle = line.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    var started = false;
    view.points.forEach(function (point) {
      var value = hnValue(point, line);
      if (value === null) { started = false; return; }
      var x = view.xTime(Number(point.t)), y = view.y(value);
      if (started) ctx.lineTo(x, y); else { ctx.moveTo(x, y); started = true; }
    });
    ctx.stroke();
  });
  if (hnSelectedTimestamp !== null) {
    var selected = hnNearestPoint(hnSelectedTimestamp);
    if (selected) hnPaintChartSelection(view, selected);
  }
  if (hnDrag && hnDrag.view === view) hnPaintDrag(ctx, view);
}
function hnPaintChartSelection(view, point) {
  var ctx = view.ctx,
    x = view.xTime(Number(point.t)),
    entries = view.lines.map(function (line) { return { line: line, value: hnValue(point, line) }; }),
    styles = getComputedStyle(document.documentElement),
    background = styles.getPropertyValue("--bg-elevated").trim() || "#fff",
    foreground = styles.getPropertyValue("--text-primary").trim() || "#222",
    padding = 7,
    lineHeight = 17,
    header = hnClock(Number(point.t), hnRequestEnd - hnRequestStart >= 86400),
    boxWidth;
  ctx.strokeStyle = "rgba(117,129,138,.65)";
  ctx.beginPath(); ctx.moveTo(x, view.top); ctx.lineTo(x, view.top + view.plotH); ctx.stroke();
  ctx.font = "12px Arial";
  boxWidth = ctx.measureText(header).width;
  entries.forEach(function (entry) {
    entry.text = entry.line.label + ": " + (entry.value === null ? "N/A" : hnFormat(entry.value, 2) + (entry.line.unit ? " " + entry.line.unit : ""));
    boxWidth = Math.max(boxWidth, ctx.measureText(entry.text).width + 14);
    if (entry.value !== null) {
      ctx.fillStyle = entry.line.color; ctx.beginPath(); ctx.arc(x, view.y(entry.value), 4, 0, Math.PI * 2); ctx.fill();
    }
  });
  boxWidth += padding * 2;
  var boxHeight = padding * 2 + 17 + entries.length * lineHeight,
    boxX = x + 10 + boxWidth <= view.width - 2 ? x + 10 : x - boxWidth - 10,
    boxY = view.top + 4;
  boxX = Math.max(2, Math.min(view.width - boxWidth - 2, boxX));
  ctx.fillStyle = background; ctx.strokeStyle = "rgba(117,129,138,.55)";
  ctx.fillRect(boxX, boxY, boxWidth, boxHeight); ctx.strokeRect(boxX, boxY, boxWidth, boxHeight);
  ctx.fillStyle = foreground; ctx.fillText(header, boxX + padding, boxY + padding + 11);
  entries.forEach(function (entry, index) {
    var y = boxY + padding + 25 + index * lineHeight;
    ctx.fillStyle = entry.line.color; ctx.beginPath(); ctx.arc(boxX + padding + 4, y - 4, 3, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = foreground; ctx.fillText(entry.text, boxX + padding + 13, y);
  });
}
function hnPaintDrag(ctx, view) {
  var x = Math.min(hnDrag.startX, hnDrag.currentX),
    width = Math.abs(hnDrag.currentX - hnDrag.startX);
  ctx.fillStyle = "rgba(30,136,229,.16)"; ctx.fillRect(x, view.top, width, view.plotH);
  ctx.strokeStyle = "rgba(30,136,229,.9)"; ctx.strokeRect(x, view.top, width, view.plotH);
}

var hnActivityRows = [
  { label: "Compressor", color: "#2e7d32", active: hnCompressorActive },
  { label: "Room circuit", color: "#ef6c00", active: function (s) { return hnBitActive(s, 7); } },
  { label: "DHW circuit", color: "#1565c0", active: function (s) { return hnBitActive(s, 56); } },
  { label: "Defrost", color: "#5e35b1", active: function (s) { return hnBitActive(s, HN_DEFROST); } },
  { label: "Internal heater", color: "#fb8c00", active: function (s) { return hnBitActive(s, HN_HEAT_INTERNAL | HN_DHW_INTERNAL); } },
  { label: "External heater", color: "#c62828", active: function (s) { return hnBitActive(s, HN_HEAT_EXTERNAL | HN_DHW_EXTERNAL); } },
  { label: "Circulation / idle", color: "#78909c", active: function (s) { return hnBitActive(s, HN_SYSTEM_ON) && !hnCompressorActive(s) && !hnBitActive(s, 54); } },
];
function hnDrawActivity() {
  var canvas = document.getElementById("hnActivity"),
    ctx = canvas.getContext("2d"),
    density = window.devicePixelRatio || 1,
    width = canvas.clientWidth || 800,
    height = 186,
    view = { canvas: canvas, ctx: ctx, width: width, height: height, left: 118, right: 12, top: 6, bottom: 26 };
  view.plotW = width - view.left - view.right;
  view.plotH = height - view.top - view.bottom;
  view.xTime = function (time) { return view.left + view.plotW * (time - hnRequestStart) / Math.max(1, hnRequestEnd - hnRequestStart); };
  view.timeAtX = function (x) { return hnRequestStart + (hnRequestEnd - hnRequestStart) * (x - view.left) / view.plotW; };
  canvas.width = width * density; canvas.height = height * density;
  ctx.setTransform(density, 0, 0, density, 0, 0);
  hnActivityView = view;
  hnSetupInteraction(view);
  hnPaintActivity();
}
function hnPaintActivity() {
  var view = hnActivityView;
  if (!view || !hnData) return;
  var ctx = view.ctx,
    states = hnData.sourceStates || [],
    rowHeight = 21;
  ctx.clearRect(0, 0, view.width, view.height);
  ctx.font = "11px Arial"; ctx.textBaseline = "middle";
  hnActivityRows.forEach(function (row, rowIndex) {
    var y = view.top + rowIndex * rowHeight;
    ctx.fillStyle = "#75818a"; ctx.fillText(row.label, 2, y + rowHeight / 2);
    ctx.fillStyle = "rgba(117,129,138,.10)"; ctx.fillRect(view.left, y, view.plotW, rowHeight - 1);
    states.forEach(function (state, index) {
      if (!row.active(state)) return;
      var end = index + 1 < states.length ? Number(states[index + 1].t) : hnRequestEnd,
        x1 = view.xTime(Math.max(hnRequestStart, Number(state.t))),
        x2 = view.xTime(Math.min(hnRequestEnd, end));
      ctx.fillStyle = row.color; ctx.fillRect(x1, y, Math.max(1, x2 - x1), rowHeight - 1);
    });
  });
  hnEvents.forEach(function (event) {
    if (["scheduler", "operation_mode_changed", "error_appeared"].indexOf(event.type) < 0) return;
    var x = view.xTime(Number(event.t));
    ctx.strokeStyle = event.type === "error_appeared" ? "#c62828" : "rgba(117,129,138,.75)";
    ctx.beginPath(); ctx.moveTo(x, view.top); ctx.lineTo(x, view.top + view.plotH); ctx.stroke();
  });
  if (hnSelectedTimestamp !== null) {
    var selected = hnNearestPoint(hnSelectedTimestamp);
    if (selected) {
      var selectedX = view.xTime(Number(selected.t));
      ctx.strokeStyle = "rgba(30,136,229,.95)"; ctx.beginPath(); ctx.moveTo(selectedX, view.top); ctx.lineTo(selectedX, view.top + view.plotH); ctx.stroke();
    }
  }
  ctx.fillStyle = "#75818a"; ctx.textAlign = "center";
  [0, .25, .5, .75, 1].forEach(function (fraction) {
    ctx.fillText(hnClock(hnRequestStart + (hnRequestEnd - hnRequestStart) * fraction, hnRequestEnd - hnRequestStart >= 43200), view.left + view.plotW * fraction, view.height - 10);
  });
  ctx.textAlign = "left";
  if (hnDrag && hnDrag.view === view) hnPaintDrag(ctx, view);
}

function hnSelectTimestamp(timestamp) {
  var point = hnNearestPoint(timestamp);
  if (!point) return;
  hnSelectedTimestamp = Number(point.t);
  hnPaintAll();
  hnUpdateContext(point);
}
function hnPaintAll() {
  Object.keys(hnViews).forEach(function (id) { hnPaintChart(hnViews[id]); });
  hnPaintActivity();
}
function hnOperationLabel(state) {
  if (!state) return "Unknown";
  if (hnBitActive(state, HN_DEFROST)) return "Defrost";
  if (hnBitActive(state, 56)) return "DHW heating";
  if (hnBitActive(state, 7)) return "Room heating";
  if (hnBitActive(state, HN_SYSTEM_ON)) return "Circulation / standby";
  return "Off";
}
function hnModeLabel(value) {
  var labels = [
    "Heat",
    "Cool",
    "Auto (heat)",
    "DHW",
    "Heat + DHW",
    "Cool + DHW",
    "Auto (heat) + DHW",
    "Auto (cool)",
    "Auto (cool) + DHW",
  ];
  var index = hnNumber(value);
  return index !== null && labels[index] ? labels[index] : "Unknown mode";
}
function hnUpdateContext(point) {
  var state = hnStateAt(Number(point.t)),
    operation = hnOperationLabel(state),
    demand = "No explicit demand signal is stored for this control mode.",
    result = "Room " + hnFormat(point.room) + " °C · DHW " + hnFormat(point.dhw) + " °C",
    thermal = hnFormat(point.activeThermal, 2),
    electrical = hnFormat(point.electrical, 2),
    cop = hnFormat(point.cop, 2);
  if (operation === "DHW heating" && hnNumber(point.dhw) !== null && hnNumber(point.dhwTarget) !== null) {
    demand = "DHW tank " + hnFormat(point.dhw) + " °C versus " + hnFormat(point.dhwTarget) + " °C target.";
  } else if (operation === "Room heating" && hnNumber(point.room) !== null && hnNumber(point.roomSetpoint) !== null) {
    demand = "Room " + hnFormat(point.room) + " °C versus " + hnFormat(point.roomSetpoint) + " °C target.";
  } else if (operation === "Room heating" && hnNumber(point.target) !== null) {
    demand = "Heating-water target " + hnFormat(point.target) + " °C" +
      (point.zone1RequestSemantic === "heatCurveShift" ? " with curve shift " + hnFormat(point.heatingCurveShift, 0) + " K." : ".");
  }
  document.getElementById("hnSelectedTime").textContent = hnClock(Number(point.t), true);
  document.getElementById("hnContext").innerHTML =
    '<div><span>Operation</span><strong>' + hmEscape(operation + " · " + hnModeLabel(point.mode)) + '</strong></div>' +
    '<div><span>Demand context</span><strong>' + hmEscape(demand) + '</strong></div>' +
    '<div><span>Observed result</span><strong>' + hmEscape(result) + '</strong></div>' +
    '<div><span>Power / COP</span><strong>' + hmEscape(thermal + " kW thermal · " + electrical + " kW electrical · COP " + cop) + '</strong></div>';
}

function hnSegmentSeconds(predicate) {
  var states = (hnData && hnData.sourceStates) || [], total = 0;
  states.forEach(function (state, index) {
    if (!predicate(state)) return;
    var start = Math.max(hnRequestStart, Number(state.t)),
      end = Math.min(hnRequestEnd, index + 1 < states.length ? Number(states[index + 1].t) : hnRequestEnd);
    if (end > start) total += end - start;
  });
  return total;
}
function hnRenderSummary() {
  var states = hnData.sourceStates || [], starts = 0;
  for (var i = 1; i < states.length; i++) if (!hnCompressorActive(states[i - 1]) && hnCompressorActive(states[i])) starts++;
  var efficiency = hnData.efficiency || {};
  document.getElementById("hnRuntime").textContent = hnDuration(hnSegmentSeconds(hnCompressorActive));
  document.getElementById("hnStarts").textContent = String(starts);
  document.getElementById("hnHeatRuntime").textContent = hnDuration(hnSegmentSeconds(function (s) { return hnBitActive(s, HN_HEAT_COMPRESSOR); }));
  document.getElementById("hnDhwRuntime").textContent = hnDuration(hnSegmentSeconds(function (s) { return hnBitActive(s, HN_DHW_COMPRESSOR); }));
  document.getElementById("hnHeatCop").textContent = hnFormat(efficiency.heatingCop, 2);
  document.getElementById("hnDhwCop").textContent = hnFormat(efficiency.dhwCop, 2);
}
function hnBuildCycles() {
  var states = hnData.sourceStates || [], cycles = [], current = null;
  states.forEach(function (state, index) {
    var active = hnCompressorActive(state);
    if (active && !current) current = { start: Math.max(hnRequestStart, Number(state.t)), heat: false, dhw: false, defrost: false, clipped: index === 0 };
    if (active && current) {
      current.heat = current.heat || hnBitActive(state, HN_HEAT_COMPRESSOR);
      current.dhw = current.dhw || hnBitActive(state, HN_DHW_COMPRESSOR);
      current.defrost = current.defrost || hnBitActive(state, HN_DEFROST);
    }
    if (!active && current) { current.end = Number(state.t); cycles.push(current); current = null; }
  });
  if (current) { current.end = hnRequestEnd; current.clippedEnd = true; cycles.push(current); }
  var body = document.getElementById("hnCycles");
  if (!cycles.length) { body.innerHTML = '<tr><td colspan="4">No compressor cycles in this period.</td></tr>'; return; }
  body.innerHTML = cycles.slice(-50).reverse().map(function (cycle) {
    var purpose = cycle.heat && cycle.dhw ? "Mixed" : cycle.dhw ? "DHW" : cycle.heat ? "Room heating" : cycle.defrost ? "Defrost" : "Unknown",
      startPoint = hnNearestPoint(cycle.start), endPoint = hnNearestPoint(cycle.end), result = "—";
    if (purpose === "DHW" && startPoint && endPoint && hnNumber(startPoint.dhw) !== null && hnNumber(endPoint.dhw) !== null)
      result = "Tank " + hnFormat(startPoint.dhw) + " → " + hnFormat(endPoint.dhw) + " °C";
    else if (purpose === "Room heating" && startPoint && endPoint && hnNumber(startPoint.room) !== null && hnNumber(endPoint.room) !== null)
      result = "Room " + hnFormat(startPoint.room) + " → " + hnFormat(endPoint.room) + " °C";
    return "<tr><td>" + (cycle.clipped ? "≤ " : "") + hmEscape(hnClock(cycle.start, true)) + "</td><td>" + hmEscape(purpose) +
      (cycle.defrost && purpose !== "Defrost" ? " + defrost" : "") + "</td><td>" + hmEscape(hnDuration(cycle.end - cycle.start)) +
      (cycle.clippedEnd ? " +" : "") + "</td><td>" + hmEscape(result) + "</td></tr>";
  }).join("");
}
function hnPrepareData(data) {
  data.sourceStates = hnSourceStates(data);
  (data.samples || []).forEach(function (point) {
    point.roomSetpoint = hnNumber(point.roomTarget) !== null ? point.roomTarget : point.zone1RequestSemantic === "roomTarget" ? point.zone1Request : null;
    point.controllerTarget = point.zone1RequestSemantic === "heatingWaterTarget" ? point.zone1Request : null;
    var dhw = point.dhwActive === true || Number(point.valve) === 1,
      heatProduction = hnNumber(point.heatProduction),
      dhwProduction = hnNumber(point.dhwProduction);
    point.activeThermal = dhw ? dhwProduction : heatProduction;
    if (point.activeThermal === null) point.activeThermal = hnNumber(point.power);
    point.heatCop = dhw ? null : point.cop;
    point.dhwCop = dhw ? point.cop : null;
  });
  return data;
}
function hnRender(data, preserveSelection) {
  var previousSelection = preserveSelection ? hnSelectedTimestamp : null;
  hnData = hnPrepareData(data);
  hnViews = {};
  hnDrawActivity();
  hnCharts.forEach(hnCreateChart);
  hnRenderSummary();
  hnBuildCycles();
  if ((hnData.samples || []).length)
    hnSelectTimestamp(
      previousSelection === null
        ? Number(hnData.samples[hnData.samples.length - 1].t)
        : previousSelection,
    );
  document.getElementById("hnStatus").textContent = (data.storedSampleCount || 0) + " SD samples · " + (data.samples || []).length + " display points";
}

function hnEventLabel(event) {
  var labels = { compressor_start: "Compressor start", compressor_stop: "Compressor stop", dhw_start: "DHW start", dhw_stop: "DHW stop", defrost_start: "Defrost start", defrost_stop: "Defrost stop", scheduler: "Scheduler", operation_mode_changed: "Mode change", error_appeared: "Error", error_cleared: "Error cleared", system: "System" };
  return labels[event.type] || event.type.replace(/_/g, " ");
}
function hnRenderEvents(data) {
  hnEvents = data.events || [];
  document.getElementById("hnEventStatus").textContent = (data.eventCount || hnEvents.length) + " event(s)";
  var relevant = hnEvents.filter(function (event) {
    return ["scheduler", "operation_mode_changed", "dhw_start", "dhw_stop", "defrost_start", "defrost_stop", "compressor_start", "compressor_stop", "error_appeared", "error_cleared", "system"].indexOf(event.type) >= 0;
  }).slice(-40).reverse();
  document.getElementById("hnEvents").innerHTML = relevant.length ? relevant.map(function (event) {
    var detail = event.message || "";
    if (event.type === "operation_mode_changed") detail += " · " + hnModeLabel(event.value);
    return '<div class="history-new-event"><time>' + hmEscape(hnClock(Number(event.t), true)) + '</time><strong>' + hmEscape(hnEventLabel(event)) + '</strong><span>' + hmEscape(detail) + "</span></div>";
  }).join("") : '<div class="history-note">No relevant operating events are stored for this period.</div>';
  hnPaintActivity();
}
function hnLoadEvents(start, end, sequence) {
  fetch("/eventlogapi?start=" + start + "&end=" + end, { cache: "no-store" })
    .then(function (response) { return response.json().then(function (data) { if (!response.ok) throw Error(data.error || response.status); return data; }); })
    .then(function (data) { if (sequence === hnRequestSequence) hnRenderEvents(data); })
    .catch(function (error) {
      if (sequence !== hnRequestSequence) return;
      document.getElementById("hnEventStatus").textContent = "Unavailable";
      document.getElementById("hnEvents").innerHTML = '<div class="history-note">Event context unavailable: ' + hmEscape(error.message) + "</div>";
    });
}

function hnPad(value) { return String(value).padStart(2, "0"); }
function hnSetupPickers() {
  ["hnStart", "hnEnd"].forEach(function (prefix) {
    var hours = document.getElementById(prefix + "Hour"), minutes = document.getElementById(prefix + "Minute");
    for (var hour = 0; hour < 24; hour++) hours.add(new Option(hnPad(hour), hnPad(hour)));
    for (var minute = 0; minute < 60; minute++) minutes.add(new Option(hnPad(minute), hnPad(minute)));
  });
}
function hnSetDateTime(prefix, value) {
  document.getElementById(prefix).value = value.getFullYear() + "-" + hnPad(value.getMonth() + 1) + "-" + hnPad(value.getDate());
  document.getElementById(prefix + "Hour").value = hnPad(value.getHours());
  document.getElementById(prefix + "Minute").value = hnPad(value.getMinutes());
}
var hnPresetSeconds = { "1h": 3600, "3h": 10800, "6h": 21600, "24h": 86400, "7d": 604800, "30d": 2592000, "90d": 7776000 };
function hnSyncPreset() {
  var seconds = hnPresetSeconds[document.getElementById("hnRange").value];
  if (!seconds) return false;
  var end = new Date(), start = new Date(end.getTime() - seconds * 1000);
  hnSetDateTime("hnStart", start); hnSetDateTime("hnEnd", end); return true;
}
function hnApplyPreset(load) { if (hnSyncPreset() && load) hnRefresh(); }
function hnPeriodEdited() { document.getElementById("hnRange").value = "custom"; hnZoomBase = null; document.getElementById("hnResetZoom").hidden = true; }
function hnTimestamp(id) {
  var value = document.getElementById(id).value + "T" + document.getElementById(id + "Hour").value + ":" + document.getElementById(id + "Minute").value,
    parsed = new Date(value).getTime();
  return Number.isFinite(parsed) ? Math.floor(parsed / 1000) : 0;
}
function hnLoad(start, end) {
  var status = document.getElementById("hnStatus"), sequence = ++hnRequestSequence;
  if (!start || !end || end <= start) { status.textContent = "Choose a valid start and end date."; return; }
  if (end - start > 90 * 86400) { status.textContent = "The display period is limited to 90 days."; return; }
  hnRequestStart = start; hnRequestEnd = end; hnSelectedTimestamp = null; hnEvents = [];
  status.textContent = "Reading SD history…";
  fetch("/historyapi?storage=sd&maxPoints=72&start=" + start + "&end=" + end, { cache: "no-store" })
    .then(function (response) { return response.text().then(function (body) { var data = {}; try { data = JSON.parse(body); } catch (error) {} if (!response.ok || data.error) throw Error(data.error || body || response.status); return data; }); })
    .then(function (data) { if (sequence !== hnRequestSequence) return; hnRender(data); hnLoadEvents(start, end, sequence); })
    .catch(function (error) { if (sequence === hnRequestSequence) status.textContent = "SD history unavailable: " + error.message; });
}
function hnRefresh() {
  hnSyncPreset();
  hnZoomBase = null; document.getElementById("hnResetZoom").hidden = true;
  hnLoad(hnTimestamp("hnStart"), hnTimestamp("hnEnd"));
}
function hnZoom(start, end) {
  start = Math.floor(start / 60) * 60; end = Math.ceil(end / 60) * 60;
  if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return;
  if (!hnZoomBase) hnZoomBase = { start: hnRequestStart, end: hnRequestEnd };
  document.getElementById("hnRange").value = "custom";
  hnSetDateTime("hnStart", new Date(start * 1000)); hnSetDateTime("hnEnd", new Date(end * 1000));
  document.getElementById("hnResetZoom").hidden = false; hnLoad(start, end);
}
function hnResetZoom() {
  if (!hnZoomBase) return;
  var base = hnZoomBase; hnZoomBase = null;
  hnSetDateTime("hnStart", new Date(base.start * 1000)); hnSetDateTime("hnEnd", new Date(base.end * 1000));
  document.getElementById("hnResetZoom").hidden = true; hnLoad(base.start, base.end);
}

hnSetupPickers();
hnApplyPreset(false);
hnRefresh();
window.addEventListener("resize", function () { if (hnData) hnRender(hnData, true); });
