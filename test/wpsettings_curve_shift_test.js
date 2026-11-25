const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.resolve(__dirname, "..");
const html = fs.readFileSync(path.join(root, "webui/src/wpsettings.html.in"), "utf8");
const elements = new Map(Array.from(html.matchAll(/id="([^"]+)"/g), ([, id]) => [
  id,
  {
    textContent: "",
    innerHTML: "",
    value: "",
    hidden: false,
    disabled: false,
    style: {},
    className: "",
    closest: () => null,
    querySelectorAll: () => [],
  },
]));

const scheduled = [];
let sentCommands = [];
const context = vm.createContext({
  document: {
    activeElement: null,
    getElementById(id) {
      assert(elements.has(id), "Missing HTML element: " + id);
      return elements.get(id);
    },
    querySelectorAll: () => [],
    addEventListener() {},
  },
  window: {
    setTimeout(fn) {
      scheduled.push(fn);
      return scheduled.length;
    },
    clearTimeout() {},
    setInterval() {},
    confirm: () => true,
  },
  fetch(url) {
    sentCommands.push(url);
    return Promise.resolve({ ok: true, text: async () => "Heating curve shift queued" });
  },
  updCell(id, val) {
    if (elements.has(id)) elements.get(id).textContent = val;
  },
  hmStoreHeatpumpSnapshot() {},
  hmReadHeatpumpSnapshot: () => null,
  startWebsockets() {},
  monitorWebSocket() {},
});

const script = fs.readFileSync(path.join(root, "webui/src/wpsettings.js"), "utf8");
vm.runInContext(script.slice(0, script.lastIndexOf('\ndocument.addEventListener("DOMContentLoaded"')), context);

async function run() {
  context.wpRenderHeatpumpData({
    heatpump: [
      { Topic: "TOP29", Value: 43, Description: "°C" },
      { Topic: "TOP30", Value: 30, Description: "°C" },
      { Topic: "TOP31", Value: 15, Description: "°C" },
      { Topic: "TOP32", Value: -12, Description: "°C" },
    ],
  });

  // Unavailable shift disables controls and renders N/A.
  context.wpCurveShift = { available: false, writable: false };
  context.wpSync();
  assert.equal(elements.get("wpCurveShiftValue").textContent, "N/A");
  assert.equal(elements.get("wpCurveShiftMinus").disabled, true);
  assert.equal(elements.get("wpCurveShiftPlus").disabled, true);

  // Active compensation mode enables controls and updates both text and SVG effective curve on step.
  context.wpCurveShift = { available: true, valueValid: true, writable: true, shift: 0, min: -5, max: 5 };
  context.wpSync();
  assert.equal(elements.get("wpCurveShiftValue").textContent, "0 K");
  assert.equal(elements.get("wpCurveShiftMinus").disabled, false);
  assert.equal(elements.get("wpCurveShiftPlus").disabled, false);

  context.wpStepCurveShift(1);
  assert.equal(elements.get("wpCurveShiftValue").textContent, "+1 K");
  assert(elements.get("wpCurveGraph").innerHTML.includes("wp-curve-effective-line"));
  assert(elements.get("wpCurveCaption").textContent.includes("effective shift +1 K"));
  assert.equal(scheduled.length, 1);
  const settle = () => new Promise(resolve => setImmediate(resolve));
  scheduled[0]();
  assert.equal(sentCommands.at(-1), "/command?SetHeatingCurveShift=1");
  await settle();
  assert.equal(scheduled.length, 2);
  scheduled[1]();
  await settle();

  // Clamping at boundaries disables the corresponding step button.
  context.wpCurveShift.shift = 5;
  context.wpSync();
  assert.equal(elements.get("wpCurveShiftPlus").disabled, true);
  assert.equal(elements.get("wpCurveShiftMinus").disabled, false);

  context.wpCurveShift.shift = -5;
  context.wpSync();
  assert.equal(elements.get("wpCurveShiftMinus").disabled, true);
  assert.equal(elements.get("wpCurveShiftPlus").disabled, false);

  // Out-of-range raw value remains recoverable via step buttons.
  context.wpCurveShift = { available: true, valueValid: false, rawValue: 25, writable: true, shift: 25, min: -5, max: 5 };
  context.wpSync();
  assert.equal(elements.get("wpCurveShiftValue").textContent, "Invalid (25)");
  assert.equal(elements.get("wpCurveShiftMinus").disabled, false);
  assert.equal(elements.get("wpCurveShiftPlus").disabled, false);
  context.wpStepCurveShift(-1);
  assert.equal(elements.get("wpCurveShiftValue").textContent, "-1 K");

  console.log("WP settings curve shift tests passed");
}

run().catch(error => { console.error(error); process.exitCode = 1; });
