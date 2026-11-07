var h2Data = null,
  h2ArchiveData = null,
  h2Events = [],
  h2CyclesList = [],
  h2Views = {},
  h2BarViews = {},
  h2ActivityView = null,
  h2SelectedTimestamp = null,
  h2RequestStart = 0,
  h2RequestEnd = 0,
  h2RequestSequence = 0,
  h2ZoomBase = null,
  h2Drag = null,
  h2CalendarCopLoading = false,
  h2ActiveTab = "story";

var H2_HEAT_COMPRESSOR = 1,
  H2_HEAT_INTERNAL = 2,
  H2_HEAT_EXTERNAL = 4,
  H2_DHW_COMPRESSOR = 8,
  H2_DHW_INTERNAL = 16,
  H2_DHW_EXTERNAL = 32,
  H2_DEFROST = 64,
  H2_SYSTEM_ON = 128;

var h2Lines = {
  target: { key: "target", label: "Main water target", color: "#ef6c00", unit: "°C" },
  controllerTarget: { key: "controllerTarget", label: "Zone 1 direct target", color: "#8e44ad", unit: "°C" },
  heatOutlet: { key: "heatOutlet", label: "Flow / outlet", color: "#43a047", unit: "°C" },
  heatInlet: { key: "heatInlet", label: "Return / inlet", color: "#1e88e5", unit: "°C" },
  spread: { key: "spread", label: "Spread ΔT (VL − RL)", color: "#00897b", unit: "K" },
  targetDelta: { key: "targetDelta", label: "Target excess (VL − Soll)", color: "#d81b60", unit: "K" },
  curveShift: { key: "heatingCurveShift", label: "Heating curve shift", color: "#6a1b9a", unit: "K" },
  hz: { key: "hz", label: "Compressor frequency", color: "#8e44ad", unit: "Hz" },
  current: { key: "current", label: "Compressor current", color: "#fb8c00", unit: "A" },
  flow: { key: "flow", label: "Water flow", color: "#00897b", unit: "l/min" },
  dhwTarget: { key: "dhwTarget", label: "Tank setpoint", color: "#1e88e5", unit: "°C" },
  dhw: { key: "dhw", label: "Tank actual", color: "#ef6c00", unit: "°C" },
  dhwOutlet: { key: "dhwOutlet", label: "DHW flow", color: "#43a047", unit: "°C" },
  dhwInlet: { key: "dhwInlet", label: "DHW return", color: "#90caf9", unit: "°C" },
  outside: { key: "outside", label: "Outside", color: "#43a047", unit: "°C" },
  evaOutlet: { key: "evaOutlet", label: "Evaporator outlet", color: "#1e88e5", unit: "°C" },
  roomTarget: { key: "roomSetpoint", label: "Room setpoint", color: "#1e88e5", unit: "°C" },
  room: { key: "room", label: "Room actual", color: "#ef6c00", unit: "°C" },
  thermal: { key: "activeThermal", label: "Thermal output", color: "#00897b", unit: "kW" },
  electrical: { key: "electrical", label: "Electrical input", color: "#c62828", unit: "kW" },
  heatCop: { key: "heatCop", label: "Room-heating COP", color: "#2e7d32", unit: "" },
  dhwCop: { key: "dhwCop", label: "DHW COP", color: "#1565c0", unit: "" },
};

var h2Charts = [
  { id: "h2Water", legend: "h2WaterLegend", unit: "°C", activity: "heat", lines: [h2Lines.target, h2Lines.controllerTarget, h2Lines.heatOutlet, h2Lines.heatInlet] },
  { id: "h2Delta", legend: "h2DeltaLegend", unit: "K", activity: "heat", zeroLine: true, lines: [h2Lines.spread, h2Lines.targetDelta, h2Lines.curveShift] },
  { id: "h2Modulation", legend: "h2ModulationLegend", unit: "Hz / A", activity: "compressor", lines: [h2Lines.hz, h2Lines.current] },
  { id: "h2Flow", legend: "h2FlowLegend", unit: "l/min", activity: "compressor", lines: [h2Lines.flow] },
  { id: "h2Dhw", legend: "h2DhwLegend", unit: "°C", activity: "dhw", lines: [h2Lines.dhwTarget, h2Lines.dhw, h2Lines.dhwOutlet, h2Lines.dhwInlet] },
  { id: "h2Eva", legend: "h2EvaLegend", unit: "°C", activity: "defrost", lines: [h2Lines.outside, h2Lines.evaOutlet] },
  { id: "h2Room", legend: "h2RoomLegend", unit: "°C", activity: "heat", lines: [h2Lines.roomTarget, h2Lines.room, h2Lines.outside] },
  { id: "h2Power", legend: "h2PowerLegend", unit: "kW", activity: "compressor", lines: [h2Lines.thermal, h2Lines.electrical] },
  { id: "h2Cop", legend: "h2CopLegend", unit: "COP", activity: "compressor", lines: [h2Lines.heatCop, h2Lines.dhwCop] },
];

var h2BarSeries = {
  production: [
    { key: "heatProductionKWh", label: "Heat thermal", color: "#ef6c00", unit: "kWh" },
    { key: "dhwProductionKWh", label: "DHW thermal", color: "#1565c0", unit: "kWh" },
  ],
  cop: [
    { key: "heatCop", label: "Heat", color: "#ef6c00", unit: "COP" },
    { key: "dhwCop", label: "DHW", color: "#1565c0", unit: "COP" },
    { key: "totalCop", label: "Total", color: "#2e7d32", unit: "COP" },
  ],
  demandDaily: [
    { key: "heatingDegreeDays", label: "Heating degree days", color: "#00897b", unit: "K·d" },
    { key: "heatConsumptionKWh", label: "Heat electrical", color: "#d81b60", unit: "kWh" },
    { key: "dhwConsumptionKWh", label: "DHW electrical", color: "#fb8c00", unit: "kWh" },
  ],
  demandHourly: [
    { key: "heatingDegreeDifference", label: "Degree difference", color: "#00897b", unit: "K" },
    { key: "heatConsumptionKWh", label: "Heat electrical", color: "#d81b60", unit: "kWh" },
    { key: "dhwConsumptionKWh", label: "DHW electrical", color: "#fb8c00", unit: "kWh" },
  ],
};

function h2Number(value) {
  if (value === null || value === undefined || value === "") return null;
  var number = Number(value);
  return Number.isFinite(number) ? number : null;
}
function h2Value(point, line) {
  return point ? h2Number(point[line.key]) : null;
}
function h2Format(value, decimals) {
  var number = h2Number(value);
  return number === null ? "—" : number.toFixed(decimals === undefined ? 1 : decimals);
}
function h2Energy(value) {
  var formatted = h2Format(value, 2);
  return formatted === "—" ? formatted : formatted + " kWh";
}
function h2EnergyWithSplit(total, heat, dhw) {
  var main = h2Energy(total);
  if (main === "—") return main;
  var h = h2Number(heat), d = h2Number(dhw);
  if (h === null || d === null) return main;
  return main + " (" + h.toFixed(1) + " / " + d.toFixed(1) + ")";
}
function h2Clock(timestamp, includeDate) {
  if (!Number.isFinite(timestamp) || timestamp < 1000000000) return "N/A";
  var date = new Date(timestamp * 1000),
    clock = date.toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit", hourCycle: "h23" });
  return includeDate
    ? date.toLocaleDateString("de-DE", { day: "2-digit", month: "2-digit", year: "numeric" }) + " " + clock
    : clock;
}
function h2Duration(seconds) {
  seconds = Math.max(0, Math.round(seconds || 0));
  var hours = Math.floor(seconds / 3600),
    minutes = Math.floor((seconds % 3600) / 60);
  return hours ? hours + " h " + minutes + " min" : minutes + " min";
}
function h2BitActive(state, mask) {
  return !!state && (Number(state.active) & mask) !== 0;
}
function h2CompressorActive(state) {
  return h2BitActive(state, H2_HEAT_COMPRESSOR | H2_DHW_COMPRESSOR | H2_DEFROST);
}
function h2SourceStates(data) {
  if (Array.isArray(data.sourceStates) && data.sourceStates.length) return data.sourceStates;
  return (data.samples || []).map(function (point) {
    var active = 0,
      dhw = point.dhwActive === true || Number(point.valve) === 1;
    if (point.defrost === true) active |= H2_DEFROST;
    else if (point.compressor === true) active |= dhw ? H2_DHW_COMPRESSOR : H2_HEAT_COMPRESSOR;
    if (point.internalHeater === true) active |= dhw ? H2_DHW_INTERNAL : H2_HEAT_INTERNAL;
    if (point.externalHeater === true) active |= dhw ? H2_DHW_EXTERNAL : H2_HEAT_EXTERNAL;
    if (point.compressor === true || Number(point.state) > 0) active |= H2_SYSTEM_ON;
    return { t: point.t, active: active, known: 255 };
  });
}
function h2StateAt(timestamp) {
  var states = (h2Data && h2Data.sourceStates) || [],
    result = null;
  for (var i = 0; i < states.length; i++) {
    if (Number(states[i].t) > timestamp) break;
    result = states[i];
  }
  return result || states[0] || null;
}
function h2NearestPoint(timestamp) {
  var points = (h2Data && h2Data.samples) || [],
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

function h2Legend(id, lines) {
  var el = document.getElementById(id);
  if (!el) return;
  el.innerHTML = lines.map(function (line) {
    return '<span class="history-legend-item"><i class="history-legend-line" style="background:' +
      line.color + '"></i>' + hmEscape(line.label) + "</span>";
  }).join("");
}
function h2CanvasPosition(canvas, event) {
  var bounds = canvas.getBoundingClientRect();
  return { x: event.clientX - bounds.left, y: event.clientY - bounds.top };
}
function h2SetupInteraction(view) {
  var canvas = view.canvas;
  if (!canvas || canvas._h2Ready) return;
  canvas._h2Ready = true;
  canvas.addEventListener("pointerdown", function (event) {
    var current = canvas.id === "h2Activity" ? h2ActivityView : h2Views[canvas.id],
      point = h2CanvasPosition(canvas, event);
    if (!current || event.button !== 0 || point.x < current.left || point.x > current.width - current.right) return;
    h2Drag = { view: current, startX: point.x, currentX: point.x, pointerId: event.pointerId };
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
  });
  canvas.addEventListener("pointermove", function (event) {
    var current = canvas.id === "h2Activity" ? h2ActivityView : h2Views[canvas.id],
      point = h2CanvasPosition(canvas, event);
    if (!current) return;
    if (h2Drag && h2Drag.view === current) {
      h2Drag.currentX = Math.max(current.left, Math.min(current.width - current.right, point.x));
      h2PaintAll();
      return;
    }
    if (point.x < current.left || point.x > current.width - current.right || point.y < current.top || point.y > current.height - current.bottom) return;
    h2SelectTimestamp(current.timeAtX(point.x));
  });
  canvas.addEventListener("pointerup", function (event) {
    if (!h2Drag || h2Drag.pointerId !== event.pointerId) return;
    var drag = h2Drag,
      startX = Math.min(drag.startX, drag.currentX),
      endX = Math.max(drag.startX, drag.currentX);
    h2Drag = null;
    if (endX - startX >= 8) h2Zoom(drag.view.timeAtX(startX), drag.view.timeAtX(endX));
    else h2SelectTimestamp(drag.view.timeAtX(endX));
  });
  canvas.addEventListener("pointercancel", function () {
    h2Drag = null;
    h2PaintAll();
  });
}

function h2ActivityMatches(state, activity) {
  if (activity === "dhw") return h2BitActive(state, H2_DHW_COMPRESSOR | H2_DHW_INTERNAL | H2_DHW_EXTERNAL);
  if (activity === "heat") return h2BitActive(state, H2_HEAT_COMPRESSOR | H2_HEAT_INTERNAL | H2_HEAT_EXTERNAL);
  if (activity === "defrost") return h2BitActive(state, H2_DEFROST);
  return h2CompressorActive(state);
}
function h2DrawActivityShading(ctx, view) {
  if (!view.activity || !h2Data) return;
  var states = h2Data.sourceStates || [];
  ctx.fillStyle = view.activity === "dhw"
    ? "rgba(21,101,192,.08)"
    : view.activity === "defrost"
      ? "rgba(94,53,177,.12)"
      : "rgba(239,108,0,.07)";
  states.forEach(function (state, index) {
    if (!h2ActivityMatches(state, view.activity)) return;
    var end = index + 1 < states.length ? Number(states[index + 1].t) : h2RequestEnd,
      x1 = view.xTime(Math.max(h2RequestStart, Number(state.t))),
      x2 = view.xTime(Math.min(h2RequestEnd, end));
    ctx.fillRect(x1, view.top, Math.max(1, x2 - x1), view.plotH);
  });
}
function h2CreateChart(config) {
  var canvas = document.getElementById(config.id);
  if (!canvas || !canvas.getContext) return;
  var ctx = canvas.getContext("2d"),
    density = window.devicePixelRatio || 1,
    width = canvas.clientWidth || 700,
    height = 230,
    points = h2Data.samples || [],
    values = [];
  config.lines.forEach(function (line) {
    points.forEach(function (point) {
      var value = h2Value(point, line);
      if (value !== null) values.push(value);
    });
  });
  if (config.zeroLine) values.push(0);
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
    zeroLine: !!config.zeroLine,
    min: min, max: max, left: 50, right: 12, top: 18, bottom: 34,
  };
  view.plotW = width - view.left - view.right;
  view.plotH = height - view.top - view.bottom;
  view.xTime = function (time) { return view.left + view.plotW * (time - h2RequestStart) / Math.max(1, h2RequestEnd - h2RequestStart); };
  view.timeAtX = function (x) { return h2RequestStart + (h2RequestEnd - h2RequestStart) * (x - view.left) / view.plotW; };
  view.y = function (value) { return view.top + view.plotH - view.plotH * (value - view.min) / (view.max - view.min); };
  h2Views[config.id] = view;
  h2SetupInteraction(view);
  h2Legend(config.legend, config.lines);
  h2PaintChart(view);
}
function h2PaintChart(view) {
  var ctx = view.ctx,
    includeDate = h2RequestEnd - h2RequestStart >= 43200;
  ctx.clearRect(0, 0, view.width, view.height);
  h2DrawActivityShading(ctx, view);
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
  if (view.zeroLine && view.min < 0 && view.max > 0) {
    var zeroY = view.y(0);
    ctx.strokeStyle = "rgba(117,129,138,.55)";
    ctx.beginPath(); ctx.moveTo(view.left, zeroY); ctx.lineTo(view.width - view.right, zeroY); ctx.stroke();
  }
  ctx.fillText(view.unit, 4, view.top - 5);
  ctx.strokeStyle = "#75818a";
  ctx.beginPath(); ctx.moveTo(view.left, view.top); ctx.lineTo(view.left, view.top + view.plotH); ctx.lineTo(view.width - view.right, view.top + view.plotH); ctx.stroke();
  ctx.textAlign = "center";
  [0, 0.25, 0.5, 0.75, 1].forEach(function (fraction) {
    var time = h2RequestStart + (h2RequestEnd - h2RequestStart) * fraction;
    ctx.fillText(h2Clock(time, includeDate), view.left + view.plotW * fraction, view.height - 12);
  });
  ctx.textAlign = "left";
  view.lines.forEach(function (line) {
    ctx.strokeStyle = line.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    var started = false;
    view.points.forEach(function (point) {
      var val = h2Value(point, line);
      if (val === null) { started = false; return; }
      var x = view.xTime(Number(point.t)), y = view.y(val);
      if (started) ctx.lineTo(x, y); else { ctx.moveTo(x, y); started = true; }
    });
    ctx.stroke();
  });
  if (h2SelectedTimestamp !== null) {
    var selected = h2NearestPoint(h2SelectedTimestamp);
    if (selected) h2PaintChartSelection(view, selected);
  }
  if (h2Drag && h2Drag.view === view) h2PaintDrag(ctx, view);
}
function h2PaintChartSelection(view, point) {
  var ctx = view.ctx,
    x = view.xTime(Number(point.t)),
    entries = view.lines.map(function (line) { return { line: line, value: h2Value(point, line) }; }),
    styles = getComputedStyle(document.documentElement),
    background = styles.getPropertyValue("--bg-elevated").trim() || "#fff",
    foreground = styles.getPropertyValue("--text-primary").trim() || "#222",
    padding = 7,
    lineHeight = 17,
    header = h2Clock(Number(point.t), h2RequestEnd - h2RequestStart >= 86400),
    boxWidth;
  ctx.strokeStyle = "rgba(117,129,138,.65)";
  ctx.beginPath(); ctx.moveTo(x, view.top); ctx.lineTo(x, view.top + view.plotH); ctx.stroke();
  ctx.font = "12px Arial";
  boxWidth = ctx.measureText(header).width;
  entries.forEach(function (entry) {
    entry.text = entry.line.label + ": " + (entry.value === null ? "N/A" : h2Format(entry.value, 2) + (entry.line.unit ? " " + entry.line.unit : ""));
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
function h2PaintDrag(ctx, view) {
  var x = Math.min(h2Drag.startX, h2Drag.currentX),
    width = Math.abs(h2Drag.currentX - h2Drag.startX);
  ctx.fillStyle = "rgba(30,136,229,.16)"; ctx.fillRect(x, view.top, width, view.plotH);
  ctx.strokeStyle = "rgba(30,136,229,.9)"; ctx.strokeRect(x, view.top, width, view.plotH);
}

var h2ActivityRows = [
  { label: "Compressor", color: "#2e7d32", active: h2CompressorActive },
  { label: "Room circuit", color: "#ef6c00", active: function (s) { return h2BitActive(s, 7); } },
  { label: "DHW circuit", color: "#1565c0", active: function (s) { return h2BitActive(s, 56); } },
  { label: "Defrost", color: "#5e35b1", active: function (s) { return h2BitActive(s, H2_DEFROST); } },
  { label: "Internal heater", color: "#fb8c00", active: function (s) { return h2BitActive(s, H2_HEAT_INTERNAL | H2_DHW_INTERNAL); } },
  { label: "External heater", color: "#c62828", active: function (s) { return h2BitActive(s, H2_HEAT_EXTERNAL | H2_DHW_EXTERNAL); } },
  { label: "Circulation / idle", color: "#78909c", active: function (s) { return h2BitActive(s, H2_SYSTEM_ON) && !h2CompressorActive(s) && !h2BitActive(s, 54); } },
];
function h2DrawActivity() {
  var canvas = document.getElementById("h2Activity");
  if (!canvas || !canvas.getContext) return;
  var ctx = canvas.getContext("2d"),
    density = window.devicePixelRatio || 1,
    width = canvas.clientWidth || 800,
    height = 186,
    view = { canvas: canvas, ctx: ctx, width: width, height: height, left: 118, right: 12, top: 6, bottom: 26 };
  view.plotW = width - view.left - view.right;
  view.plotH = height - view.top - view.bottom;
  view.xTime = function (time) { return view.left + view.plotW * (time - h2RequestStart) / Math.max(1, h2RequestEnd - h2RequestStart); };
  view.timeAtX = function (x) { return h2RequestStart + (h2RequestEnd - h2RequestStart) * (x - view.left) / view.plotW; };
  canvas.width = width * density; canvas.height = height * density;
  ctx.setTransform(density, 0, 0, density, 0, 0);
  h2ActivityView = view;
  h2SetupInteraction(view);
  h2PaintActivity();
}
function h2PaintActivity() {
  var view = h2ActivityView;
  if (!view || !h2Data) return;
  var ctx = view.ctx,
    states = h2Data.sourceStates || [],
    rowHeight = 21;
  ctx.clearRect(0, 0, view.width, view.height);
  ctx.font = "11px Arial"; ctx.textBaseline = "middle";
  h2ActivityRows.forEach(function (row, rowIndex) {
    var y = view.top + rowIndex * rowHeight;
    ctx.fillStyle = "#75818a"; ctx.fillText(row.label, 2, y + rowHeight / 2);
    ctx.fillStyle = "rgba(117,129,138,.10)"; ctx.fillRect(view.left, y, view.plotW, rowHeight - 1);
    states.forEach(function (state, index) {
      if (!row.active(state)) return;
      var end = index + 1 < states.length ? Number(states[index + 1].t) : h2RequestEnd,
        x1 = view.xTime(Math.max(h2RequestStart, Number(state.t))),
        x2 = view.xTime(Math.min(h2RequestEnd, end));
      ctx.fillStyle = row.color; ctx.fillRect(x1, y, Math.max(1, x2 - x1), rowHeight - 1);
    });
  });
  h2Events.forEach(function (event) {
    if (["scheduler", "operation_mode_changed", "error_appeared"].indexOf(event.type) < 0) return;
    var x = view.xTime(Number(event.t));
    ctx.strokeStyle = event.type === "error_appeared" ? "#c62828" : "rgba(117,129,138,.75)";
    ctx.beginPath(); ctx.moveTo(x, view.top); ctx.lineTo(x, view.top + view.plotH); ctx.stroke();
  });
  if (h2SelectedTimestamp !== null) {
    var selected = h2NearestPoint(h2SelectedTimestamp);
    if (selected) {
      var selectedX = view.xTime(Number(selected.t));
      ctx.strokeStyle = "rgba(30,136,229,.95)"; ctx.beginPath(); ctx.moveTo(selectedX, view.top); ctx.lineTo(selectedX, view.top + view.plotH); ctx.stroke();
    }
  }
  ctx.fillStyle = "#75818a"; ctx.textAlign = "center";
  [0, 0.25, 0.5, 0.75, 1].forEach(function (fraction) {
    ctx.fillText(h2Clock(h2RequestStart + (h2RequestEnd - h2RequestStart) * fraction, h2RequestEnd - h2RequestStart >= 43200), view.left + view.plotW * fraction, view.height - 10);
  });
  ctx.textAlign = "left";
  if (h2Drag && h2Drag.view === view) h2PaintDrag(ctx, view);
}

function h2SelectTimestamp(timestamp) {
  var point = h2NearestPoint(timestamp);
  if (!point) return;
  h2SelectedTimestamp = Number(point.t);
  h2PaintAll();
  h2UpdateContext(point);
}
function h2PaintAll() {
  Object.keys(h2Views).forEach(function (id) { h2PaintChart(h2Views[id]); });
  h2PaintActivity();
}
function h2OperationLabel(state) {
  if (!state) return "Unknown";
  if (h2BitActive(state, H2_DEFROST)) return "Defrost";
  if (h2BitActive(state, 56)) return "DHW heating";
  if (h2BitActive(state, 7)) return "Room heating";
  if (h2BitActive(state, H2_SYSTEM_ON)) return "Circulation / standby";
  return "Off";
}
function h2ModeLabel(value) {
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
  var index = h2Number(value);
  return index !== null && labels[index] ? labels[index] : "Unknown mode";
}
function h2UpdateContext(point) {
  var state = h2StateAt(Number(point.t)),
    operation = h2OperationLabel(state),
    valve = (point.dhwActive === true || Number(point.valve) === 1) ? "DHW" : "Heat",
    demand = "Water target " + h2Format(point.target) + " °C",
    hydraulics = "VL " + h2Format(point.outlet) + " °C · RL " + h2Format(point.inlet) + " °C · ΔT " + h2Format(point.spread, 1) + " K · Flow " + h2Format(point.flow, 1) + " l/min",
    modulation = h2Format(point.hz, 0) + " Hz · " + h2Format(point.current, 1) + " A · Eva " + h2Format(point.evaOutlet) + " °C (Out " + h2Format(point.outside) + " °C)",
    powerCop = h2Format(point.activeThermal, 2) + " kW th · " + h2Format(point.electrical, 2) + " kW el · COP " + h2Format(point.cop, 2);
  if (operation === "DHW heating" && h2Number(point.dhw) !== null && h2Number(point.dhwTarget) !== null) {
    demand = "DHW tank " + h2Format(point.dhw) + " °C vs " + h2Format(point.dhwTarget) + " °C target";
  } else if (h2Number(point.roomSetpoint) !== null && h2Number(point.room) !== null) {
    demand = "Room " + h2Format(point.room) + " °C vs " + h2Format(point.roomSetpoint) + " °C target (VL target " + h2Format(point.target) + " °C)";
  } else if (h2Number(point.heatingCurveShift) !== null) {
    demand += " · Shift " + (Number(point.heatingCurveShift) > 0 ? "+" : "") + h2Format(point.heatingCurveShift, 0) + " K · VL−Soll " + h2Format(point.targetDelta, 1) + " K";
  }
  document.getElementById("h2SelectedTime").textContent = h2Clock(Number(point.t), true);
  document.getElementById("h2Context").innerHTML =
    '<div><span>Operation &amp; valve</span><strong>' + hmEscape(operation + " · " + h2ModeLabel(point.mode) + " · Valve " + valve) + "</strong></div>" +
    '<div><span>Demand &amp; control</span><strong>' + hmEscape(demand) + "</strong></div>" +
    '<div><span>Hydraulics &amp; spread</span><strong>' + hmEscape(hydraulics) + "</strong></div>" +
    '<div><span>Modulation &amp; icing</span><strong>' + hmEscape(modulation) + "</strong></div>" +
    '<div><span>Power &amp; COP</span><strong>' + hmEscape(powerCop) + "</strong></div>";
}

function h2SegmentSeconds(predicate) {
  var states = (h2Data && h2Data.sourceStates) || [], total = 0;
  states.forEach(function (state, index) {
    if (!predicate(state)) return;
    var start = Math.max(h2RequestStart, Number(state.t)),
      end = Math.min(h2RequestEnd, index + 1 < states.length ? Number(states[index + 1].t) : h2RequestEnd);
    if (end > start) total += end - start;
  });
  return total;
}
function h2CountTransitions(predicate) {
  var states = (h2Data && h2Data.sourceStates) || [], count = 0;
  for (var i = 1; i < states.length; i++) {
    if (!predicate(states[i - 1]) && predicate(states[i])) count++;
  }
  return count;
}
function h2RenderSummary() {
  var compressorSec = h2SegmentSeconds(h2CompressorActive),
    heatSec = h2SegmentSeconds(function (s) { return h2BitActive(s, H2_HEAT_COMPRESSOR); }),
    dhwSec = h2SegmentSeconds(function (s) { return h2BitActive(s, H2_DHW_COMPRESSOR); }),
    defrostSec = h2SegmentSeconds(function (s) { return h2BitActive(s, H2_DEFROST); }),
    internalSec = h2SegmentSeconds(function (s) { return h2BitActive(s, H2_HEAT_INTERNAL | H2_DHW_INTERNAL); }),
    externalSec = h2SegmentSeconds(function (s) { return h2BitActive(s, H2_HEAT_EXTERNAL | H2_DHW_EXTERNAL); }),
    starts = h2CountTransitions(h2CompressorActive),
    defrostCount = h2CountTransitions(function (s) { return h2BitActive(s, H2_DEFROST); }),
    periodSec = Math.max(1, (h2RequestEnd - h2RequestStart) || Number(h2Data && h2Data.rangeSeconds) || 86400),
    dutyPct = Math.min(100, Math.round((compressorSec / periodSec) * 100)),
    efficiency = (h2Data && h2Data.efficiency) || {};

  document.getElementById("h2Runtime").textContent = h2Duration(compressorSec);
  document.getElementById("h2Duty").textContent = "Duty " + dutyPct + "% of period";
  document.getElementById("h2Starts").textContent = String(starts);
  document.getElementById("h2AvgCycle").textContent = starts > 0
    ? "Ø " + h2Duration(compressorSec / starts) + " / start"
    : compressorSec > 0 ? "Continuous run" : "No starts";
  document.getElementById("h2HeatRuntime").textContent = h2Duration(heatSec);
  document.getElementById("h2HeatShare").textContent = compressorSec > 0
    ? Math.round((heatSec / compressorSec) * 100) + "% of runtime"
    : "0% of runtime";
  document.getElementById("h2DhwRuntime").textContent = h2Duration(dhwSec);
  document.getElementById("h2DhwShare").textContent = compressorSec > 0
    ? Math.round((dhwSec / compressorSec) * 100) + "% of runtime"
    : "0% of runtime";
  document.getElementById("h2DefrostCount").textContent = String(defrostCount);
  document.getElementById("h2DefrostRuntime").textContent = h2Duration(defrostSec) + " total";
  document.getElementById("h2HeaterRuntime").textContent = h2Duration(internalSec + externalSec);
  document.getElementById("h2HeaterDetail").textContent = "Int " + h2Duration(internalSec) + " · Ext " + h2Duration(externalSec);

  document.getElementById("h2SelectedElectrical").textContent = h2Energy(efficiency.totalElectricalKWh);
  document.getElementById("h2SelectedElectricalSplit").textContent =
    "Heat " + h2Energy(efficiency.heatingElectricalKWh) + " · DHW " + h2Energy(efficiency.dhwElectricalKWh);
  document.getElementById("h2SelectedHeat").textContent = h2Energy(efficiency.totalThermalKWh);
  document.getElementById("h2SelectedHeatSplit").textContent =
    "Heat " + h2Energy(efficiency.heatingThermalKWh) + " · DHW " + h2Energy(efficiency.dhwThermalKWh);
  document.getElementById("h2HeatCop").textContent = h2Format(efficiency.heatingCop, 2);
  document.getElementById("h2HeatPowerAvg").textContent =
    heatSec > 0 && h2Number(efficiency.heatingThermalKWh) !== null
      ? "Ø " + h2Format(Number(efficiency.heatingThermalKWh) / (heatSec / 3600), 2) + " kW th"
      : "Room heating";
  document.getElementById("h2SelectedCop").textContent = h2Format(efficiency.totalCop, 2);
  document.getElementById("h2DhwCop").textContent = "DHW COP " + h2Format(efficiency.dhwCop, 2);
}

function h2BuildCycles() {
  var states = (h2Data && h2Data.sourceStates) || [],
    points = (h2Data && h2Data.samples) || [],
    cycles = [],
    current = null;
  states.forEach(function (state, index) {
    var active = h2CompressorActive(state);
    if (active && !current) {
      current = { start: Math.max(h2RequestStart, Number(state.t)), heat: false, dhw: false, defrost: false, clipped: index === 0 };
    }
    if (active && current) {
      current.heat = current.heat || h2BitActive(state, H2_HEAT_COMPRESSOR);
      current.dhw = current.dhw || h2BitActive(state, H2_DHW_COMPRESSOR);
      current.defrost = current.defrost || h2BitActive(state, H2_DEFROST);
    }
    if (!active && current) {
      current.end = Number(state.t);
      cycles.push(current);
      current = null;
    }
  });
  if (current) {
    current.end = h2RequestEnd;
    current.clippedEnd = true;
    cycles.push(current);
  }
  cycles.forEach(function (cycle) {
    var hzSum = 0, hzCount = 0, hzMax = 0,
      spreadSum = 0, spreadCount = 0,
      thKWh = 0, elKWh = 0;
    for (var i = 0; i < points.length; i++) {
      var p = points[i], t = Number(p.t);
      if (t < cycle.start || t > cycle.end) continue;
      var hz = h2Number(p.hz);
      if (hz !== null && hz > 0) {
        hzSum += hz;
        hzCount++;
        if (hz > hzMax) hzMax = hz;
      }
      var sp = h2Number(p.spread);
      if (sp !== null) {
        spreadSum += sp;
        spreadCount++;
      }
      if (i > 0) {
        var prev = points[i - 1],
          t0 = Math.max(cycle.start, Number(prev.t)),
          t1 = Math.min(cycle.end, t);
        if (t1 > t0 && t - Number(prev.t) <= 3600 && !prev.defrost && !p.defrost) {
          var pTh0 = h2Number(prev.activeThermal), pTh1 = h2Number(p.activeThermal),
            pEl0 = h2Number(prev.electrical), pEl1 = h2Number(p.electrical);
          if (pTh0 !== null && pTh1 !== null && pTh0 >= 0 && pTh1 >= 0) thKWh += ((pTh0 + pTh1) * 0.5) * ((t1 - t0) / 3600);
          if (pEl0 !== null && pEl1 !== null && pEl0 >= 0 && pEl1 >= 0) elKWh += ((pEl0 + pEl1) * 0.5) * ((t1 - t0) / 3600);
        }
      }
    }
    cycle.hzAvg = hzCount ? hzSum / hzCount : null;
    cycle.hzMax = hzCount ? hzMax : null;
    cycle.spreadAvg = spreadCount ? spreadSum / spreadCount : null;
    cycle.thermalKWh = thKWh > 0 ? thKWh : null;
    cycle.electricalKWh = elKWh > 0 ? elKWh : null;
    cycle.cop = thKWh > 0 && elKWh > 0.01 ? thKWh / elKWh : null;
    cycle.purpose = cycle.heat && cycle.dhw ? "Mixed" : cycle.dhw ? "DHW" : cycle.heat ? "Room heating" : cycle.defrost ? "Defrost" : "Unknown";
  });
  h2CyclesList = cycles;
  var body = document.getElementById("h2Cycles");
  if (!cycles.length) {
    body.innerHTML = '<tr><td colspan="6">No compressor cycles in this period.</td></tr>';
    return;
  }
  body.innerHTML = cycles.slice(-50).reverse().map(function (cycle) {
    var startPoint = h2NearestPoint(cycle.start),
      endPoint = h2NearestPoint(cycle.end),
      modulationText = cycle.hzAvg !== null ? h2Format(cycle.hzAvg, 0) + " / " + h2Format(cycle.hzMax, 0) + " Hz" : "—",
      tempText = "—",
      energyText = cycle.thermalKWh !== null
        ? h2Format(cycle.thermalKWh, 2) + " kWh th · COP " + h2Format(cycle.cop, 2)
        : "—";
    if (cycle.purpose === "DHW" && startPoint && endPoint && h2Number(startPoint.dhw) !== null && h2Number(endPoint.dhw) !== null) {
      tempText = "Tank " + h2Format(startPoint.dhw) + " → " + h2Format(endPoint.dhw) + " °C";
    } else if (endPoint && h2Number(endPoint.outlet) !== null) {
      tempText = "VL " + h2Format(endPoint.outlet) + " °C (Soll " + h2Format(endPoint.target) + " °C)";
      if (cycle.spreadAvg !== null) tempText += " · Ø ΔT " + h2Format(cycle.spreadAvg, 1) + " K";
    }
    return '<tr class="history-clickable-row" onclick="h2ZoomCycle(' + Math.floor(cycle.start) + "," + Math.ceil(cycle.end) + ')" title="Click to zoom into this cycle"><td>' +
      (cycle.clipped ? "≤ " : "") + hmEscape(h2Clock(cycle.start, true)) + "</td><td>" +
      hmEscape(cycle.purpose + (cycle.defrost && cycle.purpose !== "Defrost" ? " + defrost" : "")) + "</td><td>" +
      hmEscape(h2Duration(cycle.end - cycle.start) + (cycle.clippedEnd ? " +" : "")) + "</td><td>" +
      hmEscape(modulationText) + "</td><td>" +
      hmEscape(tempText) + "</td><td>" +
      hmEscape(energyText) + "</td></tr>";
  }).join("");
}

function h2ZoomCycle(start, end) {
  var pad = Math.max(300, Math.round((end - start) * 0.15));
  h2Zoom(start - pad, end + pad);
}

function h2PrepareData(data) {
  data.sourceStates = h2SourceStates(data);
  (data.samples || []).forEach(function (point) {
    point.roomSetpoint = h2Number(point.roomTarget) !== null ? point.roomTarget : point.zone1RequestSemantic === "roomTarget" ? point.zone1Request : null;
    point.controllerTarget = point.zone1RequestSemantic === "heatingWaterTarget" ? point.zone1Request : null;
    var dhw = point.dhwActive === true || Number(point.valve) === 1,
      inlet = h2Number(point.inlet),
      outlet = h2Number(point.outlet),
      target = h2Number(point.target),
      power = h2Number(point.power),
      heatProduction = h2Number(point.heatProduction),
      dhwProduction = h2Number(point.dhwProduction);
    point.heatInlet = dhw ? null : inlet;
    point.heatOutlet = dhw ? null : outlet;
    point.dhwInlet = dhw ? inlet : null;
    point.dhwOutlet = dhw ? outlet : null;
    point.spread = inlet !== null && outlet !== null ? outlet - inlet : null;
    point.targetDelta = !dhw && outlet !== null && target !== null ? outlet - target : null;
    point.activeThermal = power !== null ? power : (dhw ? dhwProduction : heatProduction);
    point.heatCop = dhw ? null : point.cop;
    point.dhwCop = dhw ? point.cop : null;
  });
  return data;
}

function h2Render(data, preserveSelection) {
  var previousSelection = preserveSelection ? h2SelectedTimestamp : null;
  h2Data = h2PrepareData(data);
  h2Views = {};
  if (h2ActiveTab === "story") {
    h2DrawActivity();
    h2Charts.forEach(h2CreateChart);
  }
  h2RenderSummary();
  h2BuildCycles();
  if ((h2Data.samples || []).length) {
    h2SelectTimestamp(
      previousSelection === null
        ? Number(h2Data.samples[h2Data.samples.length - 1].t)
        : previousSelection
    );
  }
  document.getElementById("h2Status").textContent =
    (data.storedSampleCount || 0) + " SD samples · " + (data.samples || []).length + " display points · " + (data.intervalSeconds || 60) + " s interval";
}

function h2BarPeriodLabel(period) {
  if (!period) return "—";
  if (period.length === 16) return period.slice(11);
  return btDate(period);
}
function h2BarValue(item, series) {
  var value = Number(item && item[series.key]);
  return item && item[series.key] !== null && item[series.key] !== undefined && Number.isFinite(value) ? value : null;
}
function h2SetupBarInteraction(canvas) {
  if (!canvas || canvas._h2BarReady) return;
  canvas._h2BarReady = true;
  canvas.addEventListener("pointermove", function (event) {
    var view = h2BarViews[canvas.id];
    if (!view) return;
    var point = h2CanvasPosition(canvas, event), closest = null;
    view.bars.forEach(function (bar) {
      if (point.x >= bar.x && point.x <= bar.x + bar.width && point.y >= bar.y && point.y <= bar.bottom) closest = bar;
    });
    if (view.hover !== closest) {
      view.hover = closest;
      h2PaintBarChart(view);
    }
  });
  canvas.addEventListener("pointerleave", function () {
    var view = h2BarViews[canvas.id];
    if (!view || !view.hover) return;
    view.hover = null;
    h2PaintBarChart(view);
  });
}
function h2PaintBarChart(view) {
  var ctx = view.ctx,
    width = view.width,
    height = view.height,
    left = view.left,
    top = view.top,
    plotWidth = view.plotWidth,
    plotHeight = view.plotHeight,
    max = view.max;
  ctx.clearRect(0, 0, width, height);
  ctx.font = "11px Arial";
  ctx.fillStyle = "#75818a";
  ctx.strokeStyle = "rgba(117,129,138,.22)";
  ctx.lineWidth = 1;
  for (var tick = 0; tick <= 4; tick++) {
    var value = max - (max * tick) / 4,
      y = top + (plotHeight * tick) / 4;
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(left + plotWidth, y); ctx.stroke();
    ctx.fillText(value >= 100 ? value.toFixed(0) : value.toFixed(1), 3, y + 4);
  }
  ctx.strokeStyle = "#75818a";
  ctx.beginPath(); ctx.moveTo(left, top); ctx.lineTo(left, top + plotHeight); ctx.lineTo(left + plotWidth, top + plotHeight); ctx.stroke();
  ctx.fillText(view.unit, 3, top - 6);
  view.bars = [];
  var groupWidth = plotWidth / Math.max(1, view.items.length),
    barWidth = Math.max(3, Math.min(28, (groupWidth * 0.72) / view.series.length));
  view.items.forEach(function (item, itemIndex) {
    var center = left + groupWidth * (itemIndex + 0.5),
      barsWidth = barWidth * view.series.length;
    view.series.forEach(function (series, seriesIndex) {
      var val = h2BarValue(item, series);
      if (val === null) return;
      var barHeight = (plotHeight * Math.max(0, val)) / max,
        x = center - barsWidth / 2 + seriesIndex * barWidth,
        y = top + plotHeight - barHeight;
      ctx.fillStyle = series.color;
      ctx.fillRect(x + 1, y, Math.max(1, barWidth - 2), barHeight);
      view.bars.push({ x: x + 1, y: y, width: Math.max(1, barWidth - 2), bottom: top + plotHeight, value: val, item: item, series: series });
    });
    ctx.save();
    ctx.fillStyle = "#75818a";
    ctx.textAlign = view.items.length > 8 ? "right" : "center";
    ctx.translate(center, top + plotHeight + 13);
    if (view.items.length > 8) ctx.rotate(-Math.PI / 4);
    ctx.fillText(h2BarPeriodLabel(item.period), 0, 0);
    ctx.restore();
  });
  if (view.hover) {
    var hover = view.hover,
      text = hover.series.label + " · " + btDate(hover.item.period) + " · " + hover.value.toFixed(view.unit === "COP" ? 2 : 1) + " " + (hover.series.unit || view.unit),
      styles = getComputedStyle(document.documentElement),
      background = styles.getPropertyValue("--bg-elevated").trim() || "#fff",
      foreground = styles.getPropertyValue("--text-primary").trim() || "#222",
      padding = 7;
    ctx.font = "12px Arial";
    var boxWidth = ctx.measureText(text).width + padding * 2,
      boxX = Math.max(2, Math.min(width - boxWidth - 2, hover.x + hover.width / 2 + 8)),
      boxY = Math.max(2, hover.y - 32);
    ctx.fillStyle = background; ctx.strokeStyle = "rgba(117,129,138,.55)";
    ctx.fillRect(boxX, boxY, boxWidth, 25); ctx.strokeRect(boxX, boxY, boxWidth, 25);
    ctx.fillStyle = foreground; ctx.fillText(text, boxX + padding, boxY + 17);
  }
}
function h2DrawBarChart(id, legendId, items, series, unit) {
  h2Legend(legendId, series);
  var canvas = document.getElementById(id);
  if (!canvas || !canvas.getContext) return;
  var context = canvas.getContext("2d"),
    width = canvas.clientWidth || 600,
    height = 260,
    density = window.devicePixelRatio || 1,
    values = [];
  series.forEach(function (entry) {
    (items || []).forEach(function (item) {
      var val = h2BarValue(item, entry);
      if (val !== null) values.push(val);
    });
  });
  canvas.width = width * density;
  canvas.height = height * density;
  context.setTransform(density, 0, 0, density, 0, 0);
  if (!items || !items.length || !values.length) {
    h2BarViews[id] = null;
    context.clearRect(0, 0, width, height);
    context.fillStyle = "#75818a";
    context.font = "12px Arial";
    context.fillText("No stored efficiency data is available.", 12, 24);
    return;
  }
  var max = Math.max.apply(null, values);
  if (!Number.isFinite(max) || max <= 0) max = 1;
  max *= 1.1;
  var view = {
    canvas: canvas, ctx: context, width: width, height: height,
    left: 52, top: 20, plotWidth: Math.max(1, width - 66), plotHeight: 190,
    max: max, items: items, series: series, unit: unit, bars: [], hover: null,
  };
  h2BarViews[id] = view;
  h2SetupBarInteraction(canvas);
  h2PaintBarChart(view);
}

function h2CalendarCopRows(id, periods) {
  var rows = (periods || []).filter(function (period) {
    return period && Number(period.days) > 0;
  }).slice().reverse();
  document.getElementById(id).innerHTML = rows.length ? rows.map(function (period) {
    var elText = h2Format(period.electricalKWh, 2),
      heatText = h2Format(period.heatKWh, 2);
    if (h2Number(period.heatConsumptionKWh) !== null && h2Number(period.dhwConsumptionKWh) !== null) {
      elText += " (" + h2Format(period.heatConsumptionKWh, 1) + "/" + h2Format(period.dhwConsumptionKWh, 1) + ")";
    }
    if (h2Number(period.heatProductionKWh) !== null && h2Number(period.dhwProductionKWh) !== null) {
      heatText += " (" + h2Format(period.heatProductionKWh, 1) + "/" + h2Format(period.dhwProductionKWh, 1) + ")";
    }
    return "<tr><td>" + hmEscape(btDate(period.period) || "—") + "</td><td>" +
      hmEscape(elText) + "</td><td>" +
      hmEscape(heatText) + "</td><td>" +
      hmEscape(h2Format(period.heatCop, 2)) + "</td><td>" +
      hmEscape(h2Format(period.dhwCop, 2)) + "</td><td>" +
      hmEscape(h2Format(period.totalCop, 2)) + "</td></tr>";
  }).join("") : '<tr><td colspan="6">No stored energy data.</td></tr>';
  return rows.length;
}
function h2RenderDailyTable(days) {
  var rows = (days || []).filter(function (d) {
    return d && (Number(d.heatConsumptionKWh) > 0 || Number(d.dhwConsumptionKWh) > 0 || Number(d.compressorStarts) > 0 || h2Number(d.heatingDegreeDays) !== null);
  }).slice().reverse();
  var body = document.getElementById("h2DailyTable");
  if (!body) return;
  if (!rows.length) {
    body.innerHTML = '<tr><td colspan="9">No daily summary records available.</td></tr>';
    return;
  }
  body.innerHTML = rows.map(function (d) {
    var el = (Number(d.heatConsumptionKWh) || 0) + (Number(d.dhwConsumptionKWh) || 0),
      th = (Number(d.heatProductionKWh) || 0) + (Number(d.dhwProductionKWh) || 0),
      hdd = h2Number(d.heatingDegreeDays),
      spec = hdd !== null && hdd >= 0.5 && el > 0 ? h2Format(el / hdd, 2) : "—",
      cop = th > 0 && el > 0 ? h2Format(th / el, 2) : "—",
      starts = d.compressorStarts !== undefined ? String(d.compressorStarts) : "—",
      runtime = d.compressorSeconds !== undefined ? h2Duration(d.compressorSeconds) : "—",
      outside = h2Number(d.outsideAverage) !== null ? h2Format(d.outsideAverage, 1) + " °C" : "—";
    return '<tr class="history-clickable-row" onclick="h2OpenDay(\'' + hmEscape(d.period) + '\')" title="Open this day in Operating story"><td>' +
      hmEscape(btDate(d.period)) + "</td><td>" +
      hmEscape(outside) + "</td><td>" +
      hmEscape(h2Format(hdd, 1)) + "</td><td>" +
      hmEscape(starts) + "</td><td>" +
      hmEscape(runtime) + "</td><td>" +
      hmEscape(h2Format(el, 2)) + "</td><td>" +
      hmEscape(th > 0 ? h2Format(th, 2) : "—") + "</td><td>" +
      hmEscape(spec) + "</td><td>" +
      hmEscape(cop) + "</td></tr>";
  }).join("");
}
function h2OpenDay(period) {
  if (!period || period.length !== 10) return;
  var start = new Date(period + "T00:00:00"),
    end = new Date(period + "T23:59:00");
  if (!Number.isFinite(start.getTime()) || !Number.isFinite(end.getTime())) return;
  document.getElementById("h2Range").value = "custom";
  h2SetDateTime("h2Start", start);
  h2SetDateTime("h2End", end);
  h2ShowTab("story");
  h2Load(Math.floor(start.getTime() / 1000), Math.floor(end.getTime() / 1000));
}
function h2RenderCalendarCop(data) {
  h2ArchiveData = data || {};
  var current = h2ArchiveData.current || {},
    monthly = h2ArchiveData.monthly || [],
    yearly = h2ArchiveData.yearly || [],
    demandDaily = h2ArchiveData.demandDaily || [],
    demandHourly = h2ArchiveData.demandHourly || [],
    monthCount = h2CalendarCopRows("h2MonthlyCop", monthly),
    yearCount = h2CalendarCopRows("h2YearlyCop", yearly);
  ["day", "month", "year"].forEach(function (key) {
    var period = current[key] || {},
      prefix = "h2Current" + key.charAt(0).toUpperCase() + key.slice(1),
      copText = h2Format(period.totalCop, 2);
    if (copText !== "—" && (h2Number(period.heatCop) !== null || h2Number(period.dhwCop) !== null)) {
      copText = h2Format(period.heatCop, 2) + " / " + h2Format(period.dhwCop, 2) + " / " + copText;
    }
    document.getElementById(prefix + "Period").textContent = btDate(period.period);
    document.getElementById(prefix + "Electrical").textContent = h2EnergyWithSplit(period.electricalKWh, period.heatConsumptionKWh, period.dhwConsumptionKWh);
    document.getElementById(prefix + "Heat").textContent = h2EnergyWithSplit(period.heatKWh, period.heatProductionKWh, period.dhwProductionKWh);
    document.getElementById(prefix + "Cop").textContent = copText;
  });
  if (h2Number(h2ArchiveData.heatingDegreeDayBase) !== null) {
    document.getElementById("h2DegreeDayBaseLabel").textContent = h2Format(h2ArchiveData.heatingDegreeDayBase, 1) + " °C";
  }
  h2RenderDailyTable(demandDaily);
  if (h2ActiveTab === "energy") {
    h2DrawBarChart("h2DailyDemand", "h2DailyDemandLegend", demandDaily, h2BarSeries.demandDaily, "K·d / kWh");
    h2DrawBarChart("h2HourlyDemand", "h2HourlyDemandLegend", demandHourly, h2BarSeries.demandHourly, "K / kWh");
    h2DrawBarChart("h2MonthlyProduction", "h2MonthlyProductionLegend", monthly, h2BarSeries.production, "kWh");
    h2DrawBarChart("h2MonthlyCopChart", "h2MonthlyCopLegend", monthly, h2BarSeries.cop, "COP");
    h2DrawBarChart("h2YearlyProduction", "h2YearlyProductionLegend", yearly, h2BarSeries.production, "kWh");
    h2DrawBarChart("h2YearlyCopChart", "h2YearlyCopLegend", yearly, h2BarSeries.cop, "COP");
  }
  document.getElementById("h2CalendarCopStatus").textContent =
    monthCount + " stored month(s) · " + yearCount + " stored year(s)";
}
function h2LoadCalendarCop() {
  if (h2CalendarCopLoading) return;
  h2CalendarCopLoading = true;
  document.getElementById("h2CalendarCopStatus").textContent = "Updating…";
  fetch("/efficiencyapi", { cache: "no-store" })
    .then(function (response) {
      return response.text().then(function (body) {
        var data = {};
        try { data = JSON.parse(body); } catch (error) {}
        if (!response.ok || data.error) throw Error(data.error || body || response.status);
        return data;
      });
    })
    .then(function (data) {
      h2CalendarCopLoading = false;
      h2RenderCalendarCop(data);
    })
    .catch(function (error) {
      h2CalendarCopLoading = false;
      h2RenderCalendarCop({});
      document.getElementById("h2CalendarCopStatus").textContent = "Unavailable";
      document.getElementById("h2MonthlyCop").innerHTML =
        '<tr><td colspan="6">Calendar energy unavailable: ' + hmEscape(error.message) + "</td></tr>";
      document.getElementById("h2YearlyCop").innerHTML =
        '<tr><td colspan="6">Calendar energy unavailable.</td></tr>';
    });
}

function h2EventLabel(event) {
  var labels = {
    compressor_start: "Compressor start", compressor_stop: "Compressor stop",
    dhw_start: "DHW start", dhw_stop: "DHW stop",
    defrost_start: "Defrost start", defrost_stop: "Defrost stop",
    scheduler: "Scheduler", operation_mode_changed: "Mode change",
    error_appeared: "Error", error_cleared: "Error cleared", system: "System",
  };
  return labels[event.type] || event.type.replace(/_/g, " ");
}
function h2RenderEvents(data) {
  h2Events = data.events || [];
  document.getElementById("h2EventStatus").textContent = (data.eventCount || h2Events.length) + " event(s)";
  var relevant = h2Events.filter(function (event) {
    return ["scheduler", "operation_mode_changed", "dhw_start", "dhw_stop", "defrost_start", "defrost_stop", "compressor_start", "compressor_stop", "error_appeared", "error_cleared", "system"].indexOf(event.type) >= 0;
  }).slice(-40).reverse();
  document.getElementById("h2Events").innerHTML = relevant.length ? relevant.map(function (event) {
    var detail = event.message || "";
    if (event.type === "operation_mode_changed") detail += " · " + h2ModeLabel(event.value);
    return '<div class="history-new-event"><time>' + hmEscape(h2Clock(Number(event.t), true)) + "</time><strong>" + hmEscape(h2EventLabel(event)) + "</strong><span>" + hmEscape(detail) + "</span></div>";
  }).join("") : '<div class="history-note">No relevant operating events are stored for this period.</div>';
  h2PaintActivity();
}
function h2LoadEvents(start, end, sequence) {
  fetch("/eventlogapi?start=" + start + "&end=" + end, { cache: "no-store" })
    .then(function (response) { return response.json().then(function (data) { if (!response.ok) throw Error(data.error || response.status); return data; }); })
    .then(function (data) { if (sequence === h2RequestSequence) h2RenderEvents(data); })
    .catch(function (error) {
      if (sequence !== h2RequestSequence) return;
      document.getElementById("h2EventStatus").textContent = "Unavailable";
      document.getElementById("h2Events").innerHTML = '<div class="history-note">Event context unavailable: ' + hmEscape(error.message) + "</div>";
    });
}

function h2DownloadCsv(filename, content) {
  var blob = new Blob([content], { type: "text/csv;charset=utf-8" }),
    url = URL.createObjectURL(blob),
    link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
}
function h2ExportSamplesCsv() {
  var samples = (h2Data && h2Data.samples) || [];
  if (!samples.length) return;
  var header = "timestamp,datetime,outside_c,inlet_c,outlet_c,target_c,spread_k,target_delta_k,dhw_c,dhw_target_c,room_c,room_target_c,flow_lmin,compressor_hz,current_a,eva_outlet_c,thermal_kw,electrical_kw,cop,curve_shift_k,mode,valve,defrost\n",
    rows = samples.map(function (p) {
      return [
        p.t, '"' + h2Clock(Number(p.t), true) + '"',
        p.outside ?? "", p.inlet ?? "", p.outlet ?? "", p.target ?? "",
        p.spread !== null && p.spread !== undefined ? p.spread.toFixed(2) : "",
        p.targetDelta !== null && p.targetDelta !== undefined ? p.targetDelta.toFixed(2) : "",
        p.dhw ?? "", p.dhwTarget ?? "", p.room ?? "", p.roomSetpoint ?? "",
        p.flow ?? "", p.hz ?? "", p.current ?? "", p.evaOutlet ?? "",
        p.activeThermal ?? "", p.electrical ?? "", p.cop ?? "",
        p.heatingCurveShift ?? "", p.mode ?? "", p.valve ?? "", p.defrost ? 1 : 0,
      ].join(",");
    }).join("\n");
  h2DownloadCsv("heishamon-history-" + h2RequestStart + "-" + h2RequestEnd + ".csv", header + rows + "\n");
}
function h2ExportCyclesCsv() {
  if (!h2CyclesList.length) return;
  var header = "start_timestamp,end_timestamp,start_time,purpose,duration_seconds,hz_avg,hz_max,spread_avg_k,thermal_kwh,electrical_kwh,cop\n",
    rows = h2CyclesList.map(function (c) {
      return [
        Math.round(c.start), Math.round(c.end), '"' + h2Clock(c.start, true) + '"',
        '"' + c.purpose + '"', Math.round(c.end - c.start),
        c.hzAvg !== null ? c.hzAvg.toFixed(1) : "",
        c.hzMax !== null ? c.hzMax.toFixed(1) : "",
        c.spreadAvg !== null ? c.spreadAvg.toFixed(2) : "",
        c.thermalKWh !== null ? c.thermalKWh.toFixed(3) : "",
        c.electricalKWh !== null ? c.electricalKWh.toFixed(3) : "",
        c.cop !== null ? c.cop.toFixed(2) : "",
      ].join(",");
    }).join("\n");
  h2DownloadCsv("heishamon-cycles-" + h2RequestStart + "-" + h2RequestEnd + ".csv", header + rows + "\n");
}

function h2ShowTab(tab) {
  h2ActiveTab = tab;
  document.getElementById("h2StoryView").hidden = tab !== "story";
  document.getElementById("h2EnergyView").hidden = tab !== "energy";
  document.getElementById("h2StorageView").hidden = tab !== "storage";
  document.getElementById("h2TabStory").classList.toggle("active", tab === "story");
  document.getElementById("h2TabEnergy").classList.toggle("active", tab === "energy");
  document.getElementById("h2TabStorage").classList.toggle("active", tab === "storage");
  if (tab === "story" && h2Data) h2Render(h2Data, true);
  if (tab === "energy" && h2ArchiveData) h2RenderCalendarCop(h2ArchiveData);
}

function h2SetIntervalValue() {
  fetch("/historycommand?interval=" + document.getElementById("h2Interval").value).then(h2LoadStorageStatus);
}
function h2SetRetentionValue() {
  fetch("/historycommand?retention=" + document.getElementById("h2Retention").value).then(h2LoadStorageStatus);
}
function h2SetElectricalSource() {
  fetch("/historycommand?electricalSource=" + encodeURIComponent(document.getElementById("h2ElectricalSource").value)).then(h2LoadStorageStatus);
}
function h2SetDegreeDayBase() {
  fetch("/historycommand?degreeDayBase=" + encodeURIComponent(document.getElementById("h2DegreeDayBase").value)).then(function () {
    h2LoadStorageStatus();
    h2LoadCalendarCop();
  });
}
function h2LoadElectricalSources() {
  fetch("/externalsensorsapi", { cache: "no-store" })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      var select = document.createElement("select");
      select.id = "h2ElectricalSource";
      select.onchange = h2SetElectricalSource;
      var p = document.createElement("option");
      p.value = "panasonic";
      p.textContent = "Panasonic";
      select.appendChild(p);
      (d.sensors || []).filter(function (s) { return Number(s.role) === 1; }).forEach(function (s) {
        var o = document.createElement("option");
        o.value = "external:" + s.id;
        o.textContent = s.name + " (MQTT)";
        select.appendChild(o);
      });
      document.getElementById("h2ElectricalSourceControl").replaceChildren(select);
      fetch("/history/status", { cache: "no-store" })
        .then(function (r) { return r.json(); })
        .then(function (s) {
          select.value = s.electricalSourceId ? "external:" + s.electricalSourceId : "panasonic";
        });
    })
    .catch(function () {
      document.getElementById("h2ElectricalSourceControl").textContent = "Unavailable";
    });
}
function h2LoadStorageStatus() {
  fetch("/history/status", { cache: "no-store" })
    .then(function (r) { if (!r.ok) throw Error(r.status); return r.json(); })
    .then(function (s) {
      document.getElementById("h2Interval").value = String(Math.max(1, Math.min(10, Math.round((s.intervalSeconds || 60) / 60))));
      document.getElementById("h2Retention").value = String(s.sdRetentionDays === undefined ? 30 : s.sdRetentionDays);
      if (h2Number(s.degreeDayBase) !== null) {
        document.getElementById("h2DegreeDayBase").value = Number(s.degreeDayBase).toFixed(1);
      }
      document.getElementById("h2StorageStatus").textContent = s.sdActive
        ? "SD card active"
        : "SD card unavailable" + (s.sdLastError && s.sdLastError !== "None" ? ": " + s.sdLastError : "");
    })
    .catch(function () {
      document.getElementById("h2StorageStatus").textContent = "Storage status unavailable";
    });
}

function h2Pad(value) { return String(value).padStart(2, "0"); }
function h2SetupPickers() {
  ["h2Start", "h2End"].forEach(function (prefix) {
    var hours = document.getElementById(prefix + "Hour"), minutes = document.getElementById(prefix + "Minute");
    for (var hour = 0; hour < 24; hour++) hours.add(new Option(h2Pad(hour), h2Pad(hour)));
    for (var minute = 0; minute < 60; minute++) minutes.add(new Option(h2Pad(minute), h2Pad(minute)));
  });
}
function h2SetDateTime(prefix, value) {
  document.getElementById(prefix).value = value.getFullYear() + "-" + h2Pad(value.getMonth() + 1) + "-" + h2Pad(value.getDate());
  document.getElementById(prefix + "Hour").value = h2Pad(value.getHours());
  document.getElementById(prefix + "Minute").value = h2Pad(value.getMinutes());
}
var h2PresetSeconds = { "1h": 3600, "3h": 10800, "6h": 21600, "24h": 86400, "7d": 604800, "30d": 2592000, "90d": 7776000 };
function h2SyncPreset() {
  var seconds = h2PresetSeconds[document.getElementById("h2Range").value];
  if (!seconds) return false;
  var end = new Date(), start = new Date(end.getTime() - seconds * 1000);
  h2SetDateTime("h2Start", start); h2SetDateTime("h2End", end); return true;
}
function h2ApplyPreset(load) { if (h2SyncPreset() && load) h2Refresh(); }
function h2PeriodEdited() { document.getElementById("h2Range").value = "custom"; h2ZoomBase = null; document.getElementById("h2ResetZoom").hidden = true; }
function h2Timestamp(id) {
  var value = document.getElementById(id).value + "T" + document.getElementById(id + "Hour").value + ":" + document.getElementById(id + "Minute").value,
    parsed = new Date(value).getTime();
  return Number.isFinite(parsed) ? Math.floor(parsed / 1000) : 0;
}
function h2StepPeriod(direction) {
  var start = h2Timestamp("h2Start"),
    end = h2Timestamp("h2End");
  if (!start || !end || end <= start) return;
  var span = end - start,
    now = Math.floor(Date.now() / 1000),
    nextStart = start + direction * span,
    nextEnd = end + direction * span;
  if (direction > 0 && nextEnd > now) {
    nextEnd = now;
    nextStart = now - span;
  }
  document.getElementById("h2Range").value = "custom";
  h2ZoomBase = null;
  document.getElementById("h2ResetZoom").hidden = true;
  h2SetDateTime("h2Start", new Date(nextStart * 1000));
  h2SetDateTime("h2End", new Date(nextEnd * 1000));
  h2Load(nextStart, nextEnd);
}
function h2Load(start, end) {
  var status = document.getElementById("h2Status"),
    sequence = ++h2RequestSequence,
    maxPointsEl = document.getElementById("h2MaxPoints"),
    maxPoints = maxPointsEl ? Number(maxPointsEl.value) || 144 : 144;
  if (!start || !end || end <= start) { status.textContent = "Choose a valid start and end date."; return; }
  if (end - start > 90 * 86400) { status.textContent = "The display period is limited to 90 days."; return; }
  h2RequestStart = start; h2RequestEnd = end; h2SelectedTimestamp = null; h2Events = [];
  status.textContent = "Reading SD history…";
  fetch("/historyapi?storage=sd&maxPoints=" + maxPoints + "&start=" + start + "&end=" + end, { cache: "no-store" })
    .then(function (response) { return response.text().then(function (body) { var data = {}; try { data = JSON.parse(body); } catch (error) {} if (!response.ok || data.error) throw Error(data.error || body || response.status); return data; }); })
    .then(function (data) {
      if (sequence !== h2RequestSequence) return;
      h2Render(data);
      h2LoadEvents(start, end, sequence);
      h2LoadCalendarCop();
    })
    .catch(function (error) { if (sequence === h2RequestSequence) status.textContent = "SD history unavailable: " + error.message; });
}
function h2Refresh() {
  h2SyncPreset();
  h2ZoomBase = null; document.getElementById("h2ResetZoom").hidden = true;
  h2Load(h2Timestamp("h2Start"), h2Timestamp("h2End"));
}
function h2Zoom(start, end) {
  start = Math.floor(start / 60) * 60; end = Math.ceil(end / 60) * 60;
  if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return;
  if (!h2ZoomBase) h2ZoomBase = { start: h2RequestStart, end: h2RequestEnd };
  document.getElementById("h2Range").value = "custom";
  h2SetDateTime("h2Start", new Date(start * 1000)); h2SetDateTime("h2End", new Date(end * 1000));
  document.getElementById("h2ResetZoom").hidden = false; h2Load(start, end);
}
function h2ResetZoom() {
  if (!h2ZoomBase) return;
  var base = h2ZoomBase; h2ZoomBase = null;
  h2SetDateTime("h2Start", new Date(base.start * 1000)); h2SetDateTime("h2End", new Date(base.end * 1000));
  document.getElementById("h2ResetZoom").hidden = true; h2Load(base.start, base.end);
}

h2SetupPickers();
h2ApplyPreset(false);
h2LoadElectricalSources();
h2LoadStorageStatus();
h2Refresh();
window.addEventListener("resize", function () {
  if (h2ActiveTab === "story" && h2Data) h2Render(h2Data, true);
  if (h2ActiveTab === "energy" && h2ArchiveData) h2RenderCalendarCop(h2ArchiveData);
});
