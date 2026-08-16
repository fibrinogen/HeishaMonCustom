var externalData = { sensors: [] };
var externalEditingId = 0;
var externalBusy = false;
var externalRefreshPromise = null;
var externalEscape = hmEscape;
function externalStatus(m, e) {
  var x = document.getElementById("externalCommandStatus");
  x.textContent = m || "";
  x.style.color = e ? "var(--red)" : "var(--text-muted)";
}
function externalRender(d) {
  externalData = d;
  var state = document.getElementById("externalMqttState");
  state.textContent = d.mqttConnected ? "Connected" : "Disconnected";
  state.className =
    "scheduler-status-value " +
    (d.mqttConnected ? "scheduler-sync-good" : "scheduler-sync-bad");
  document.getElementById("externalCount").textContent =
    String(d.count) + " / " + String(d.maxSensors);
  var rows = document.getElementById("externalRows");
  if (!d.sensors || !d.sensors.length) {
    rows.innerHTML =
      "<tr><td colspan='7' class='scheduler-empty'>No MQTT sensors configured.</td></tr>";
    return;
  }
  rows.innerHTML = d.sensors
    .map(function (s) {
      var val = s.valid
        ? externalEscape(s.value) + " " + externalEscape(s.unit)
        : "--";
      var age =
        s.ageSeconds === null ? "--" : externalEscape(s.ageSeconds) + " s";
      return (
        '<tr><td><label class="dashboard-toggle"><input type="checkbox" ' +
        (s.enabled ? "checked " : "") +
        'onchange="externalToggle(' +
        s.id +
        ',this.checked)"><span class="dashboard-toggle-slider"></span></label></td><td class="scheduler-name">' +
        externalEscape(s.name) +
        '</td><td class="scheduler-mono">' +
        externalEscape(s.mqttTopic) +
        "</td><td>" +
        val +
        "</td><td>" +
        age +
        "</td><td>" +
        externalEscape(s.state) +
        '</td><td><div class="scheduler-actions"><button class="btn btn-ghost" onclick="externalOpenEditor(' +
        s.id +
        ')">Edit</button><button class="btn btn-danger" onclick="externalDelete(' +
        s.id +
        ')">Delete</button></div></td></tr>'
      );
    })
    .join("");
}
function externalRefresh() {
  if (externalRefreshPromise) return externalRefreshPromise;
  externalRefreshPromise = fetch("/externalsensorsapi", { cache: "no-store" })
    .then(function (r) {
      if (!r.ok) throw Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      externalRefreshPromise = null;
      externalRender(d);
      return d;
    })
    .catch(function (e) {
      externalRefreshPromise = null;
      externalStatus("Refresh failed: " + e.message, true);
    });
  return externalRefreshPromise;
}
function externalCommand(n, v) {
  if (externalBusy) return Promise.reject(Error("busy"));
  externalBusy = true;
  externalStatus("Saving ...", false);
  return fetch(
    "/externalsensorscommand?" +
      encodeURIComponent(n) +
      "=" +
      encodeURIComponent(v),
    { cache: "no-store" },
  )
    .then(function (r) {
      return r.text();
    })
    .then(function (m) {
      if (/^ERROR:/.test(m)) throw Error(m.replace(/^ERROR:\s*/, ""));
      externalBusy = false;
      externalStatus(m.replace(/^OK:\s*/, ""), false);
      return externalRefresh();
    })
    .catch(function (e) {
      externalBusy = false;
      externalStatus("Command failed: " + e.message, true);
      throw e;
    });
}
function externalFind(id) {
  return (externalData.sensors || []).find(function (s) {
    return Number(s.id) === Number(id);
  });
}
function externalEnsureHistoryFields() {
  if (document.getElementById("externalHistoryEnabled")) return;
  var host = document
    .getElementById("externalEnabled")
    .closest(".scheduler-field").parentElement;
  var field = document.createElement("div");
  field.className = "scheduler-field";
  field.innerHTML =
    '<label>History / role</label><label class="dashboard-control" style="justify-content:flex-start"><input id="externalHistoryEnabled" type="checkbox" checked> Store in history</label><select id="externalRole" class="scheduler-input"><option value="0">Normal sensor</option><option value="1">Electrical power (W)</option></select>';
  host.appendChild(field);
}
function externalOpenEditor(id) {
  externalEnsureHistoryFields();
  var s = id ? externalFind(id) : null;
  externalEditingId = s ? s.id : 0;
  document.getElementById("externalEditorTitle").textContent = s
    ? "Edit MQTT sensor"
    : "Add MQTT sensor";
  document.getElementById("externalName").value = s ? s.name : "";
  document.getElementById("externalTopic").value = s ? s.mqttTopic : "";
  document.getElementById("externalUnit").value = s ? s.unit : "";
  document.getElementById("externalTimeout").value = s
    ? s.staleTimeoutSeconds
    : 120;
  document.getElementById("externalEnabled").checked = s ? !!s.enabled : true;
  document.getElementById("externalHistoryEnabled").checked = s
    ? s.historyEnabled !== false
    : true;
  document.getElementById("externalRole").value = String(s ? s.role || 0 : 0);
  document.getElementById("externalModal").classList.add("open");
  document.getElementById("externalName").focus();
}
function externalCloseEditor() {
  document.getElementById("externalModal").classList.remove("open");
}
function externalSaveEditor() {
  var name = document.getElementById("externalName").value.trim(),
    topic = document.getElementById("externalTopic").value.trim(),
    unit = document.getElementById("externalUnit").value.trim(),
    timeout = Number(document.getElementById("externalTimeout").value);
  if (
    !name ||
    !topic ||
    !Number.isInteger(timeout) ||
    timeout < 5 ||
    timeout > 86400
  ) {
    externalStatus("Please enter valid sensor fields.", true);
    return;
  }
  externalCommand(
    "save",
    JSON.stringify({
      id: externalEditingId,
      enabled: document.getElementById("externalEnabled").checked,
      historyEnabled: document.getElementById("externalHistoryEnabled").checked,
      role: Number(document.getElementById("externalRole").value),
      name: name,
      mqttTopic: topic,
      unit: unit,
      staleTimeoutSeconds: timeout,
    }),
  ).then(externalCloseEditor);
}
function externalToggle(id, on) {
  var s = externalFind(id);
  if (s)
    externalCommand(
      "save",
      JSON.stringify(Object.assign({}, s, { enabled: on })),
    );
}
function externalDelete(id) {
  var s = externalFind(id);
  if (s && confirm('Delete sensor "' + s.name + '"?'))
    externalCommand("delete", id);
}
document.addEventListener("keydown", function (e) {
  if (e.key === "Escape") externalCloseEditor();
});
document.addEventListener("DOMContentLoaded", function () {
  externalRefresh();
  window.setInterval(externalRefresh, 10000);
});
