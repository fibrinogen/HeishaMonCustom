const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.resolve(__dirname, "..");
const html = fs.readFileSync(path.join(root, "webui/src/history-new.html.in"), "utf8");
const elements = new Map(Array.from(html.matchAll(/id="([^"]+)"/g), ([, id]) =>
  [id, { textContent: "", innerHTML: "" }]));
const context = vm.createContext({
  document: { getElementById(id) { assert(elements.has(id), "Missing HTML element: " + id); return elements.get(id); } },
  hmEscape(value) { return String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;"); },
});
vm.runInContext(fs.readFileSync(path.join(root, "../bridge-time/web/date-format.js"), "utf8"), context);
const script = fs.readFileSync(path.join(root, "webui/src/history-new.js"), "utf8");
vm.runInContext(script.slice(0, script.lastIndexOf("\nhnSetupPickers();")), context);
const text = id => elements.get(id).textContent;
const markup = id => elements.get(id).innerHTML;

async function run() {
  const data = {
    current: {
      day: { period: "2026-10-10", electricalKWh: 2, heatKWh: 8, totalCop: 4 },
      month: { period: "2026-10", electricalKWh: 50.125, heatKWh: 175, totalCop: 3.49 },
      year: { period: "2026", electricalKWh: 0, heatKWh: 0, totalCop: null },
    },
    monthly: [
      { period: "2026-08", days: 0, electricalKWh: 0, heatKWh: 0, totalCop: null },
      { period: "2026-09", days: 30, electricalKWh: 100, heatKWh: 300, heatCop: 3.5, dhwCop: 2.5, totalCop: 3 },
      { period: "2026-10", days: 10, electricalKWh: 50.125, heatKWh: 175, heatCop: 4, dhwCop: 3, totalCop: 3.49 },
    ],
    yearly: [{ period: "2026", days: 40, electricalKWh: 150.125, heatKWh: 475, heatCop: 3.8, dhwCop: 2.8, totalCop: 3.16 }],
  };
  context.hnRenderCalendarCop(data);
  assert.equal(text("hnCurrentDayElectrical"), "2.00 kWh");
  assert.equal(text("hnCurrentDayHeat"), "8.00 kWh");
  assert.equal(text("hnCurrentDayCop"), "4.00");
  assert.equal(text("hnCurrentDayPeriod"), "10.10.2026");
  assert.equal(text("hnCurrentMonthPeriod"), "10.2026");
  assert.equal(text("hnCurrentMonthElectrical"), "50.13 kWh");
  assert.equal(text("hnCurrentYearElectrical"), "0.00 kWh");
  assert.equal(text("hnCurrentYearCop"), "—");
  assert.equal(text("hnCalendarCopStatus"), "2 stored month(s) · 1 stored year(s)");
  assert(markup("hnMonthlyCop").indexOf("10.2026") < markup("hnMonthlyCop").indexOf("09.2026"));
  assert(!markup("hnMonthlyCop").includes("08.2026"));
  assert(markup("hnYearlyCop").includes("<td>150.13</td><td>475.00</td>"));

  // Older firmware can provide COP without the new kWh fields.
  context.hnRenderCalendarCop({ current: { day: { totalCop: 3 } } });
  assert.equal(text("hnCurrentDayElectrical"), "—");
  assert.equal(text("hnCurrentDayCop"), "3.00");
  assert(markup("hnMonthlyCop").includes('colspan="6"'));

  context.hnData = { sourceStates: [], efficiency: { totalElectricalKWh: 3, totalThermalKWh: 12, totalCop: 4 } };
  context.hnRenderSummary();
  assert.equal(text("hnSelectedElectrical"), "3.00 kWh");
  assert.equal(text("hnSelectedHeat"), "12.00 kWh");
  assert.equal(text("hnSelectedCop"), "4.00");

  let requests = 0;
  context.fetch = async url => {
    assert.equal(url, "/efficiencyapi");
    requests++;
    return { ok: true, text: async () => JSON.stringify(data) };
  };
  const settle = () => new Promise(resolve => setImmediate(resolve));
  context.hnLoadCalendarCop();
  context.hnLoadCalendarCop(); // Concurrent requests share the pending load.
  await settle();
  assert.equal(requests, 1);
  data.current.day.electricalKWh = 3;
  context.hnLoadCalendarCop();
  await settle();
  assert.equal(requests, 2);
  assert.equal(text("hnCurrentDayElectrical"), "3.00 kWh");

  context.fetch = async () => ({ ok: false, text: async () => JSON.stringify({ error: "SD busy <retry>" }) });
  context.hnLoadCalendarCop();
  await settle();
  assert.equal(text("hnCalendarCopStatus"), "Unavailable");
  assert.equal(text("hnCurrentDayElectrical"), "—");
  assert.equal(text("hnCurrentDayCop"), "—");
  assert(markup("hnMonthlyCop").includes("SD busy &lt;retry&gt;"));
  assert(!context.hnCalendarCopLoading);

  // Also test History new 2 operator view.
  const html2 = fs.readFileSync(path.join(root, "webui/src/history-new-2.html.in"), "utf8");
  const elements2 = new Map(Array.from(html2.matchAll(/id="([^"]+)"/g), ([, id]) =>
    [id, { textContent: "", innerHTML: "", value: "144", hidden: false, classList: { toggle() {} } }]));
  const context2 = vm.createContext({
    window: { devicePixelRatio: 1, addEventListener() {} },
    document: { getElementById(id) { assert(elements2.has(id), "Missing History new 2 HTML element: " + id); return elements2.get(id); } },
    hmEscape(value) { return String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;"); },
  });
  vm.runInContext(fs.readFileSync(path.join(root, "../bridge-time/web/date-format.js"), "utf8"), context2);
  const script2 = fs.readFileSync(path.join(root, "webui/src/history-new-2.js"), "utf8");
  vm.runInContext(script2.slice(0, script2.lastIndexOf("\nh2SetupPickers();")), context2);
  const text2 = id => elements2.get(id).textContent;
  const markup2 = id => elements2.get(id).innerHTML;

  context2.h2RequestStart = 1760000000;
  context2.h2RequestEnd = 1760003600;
  const prepared = context2.h2PrepareData({
    sourceStates: [
      { t: 1760000000, active: 0, known: 255 },
      { t: 1760000600, active: 1 | 128, known: 255 },
      { t: 1760002400, active: 64 | 2 | 128, known: 255 },
      { t: 1760003000, active: 0, known: 255 },
    ],
    efficiency: {
      totalElectricalKWh: 1.5,
      totalThermalKWh: 6.0,
      heatingElectricalKWh: 1.2,
      dhwElectricalKWh: 0.3,
      heatingThermalKWh: 5.1,
      dhwThermalKWh: 0.9,
      heatingCop: 4.25,
      dhwCop: 3.0,
      totalCop: 4.0,
    },
    samples: [
      { t: 1760000600, inlet: 28.0, outlet: 32.5, target: 31.0, hz: 20, current: 3.2, flow: 14.5, outside: 2.0, evaOutlet: -1.5, power: 4.5, electrical: 1.0, cop: 4.5, heatingCurveShift: -1, mode: 0, valve: 0, dhwActive: false },
      { t: 1760001800, inlet: 29.0, outlet: 34.0, target: 31.0, hz: 34, current: 4.8, flow: 15.0, outside: 2.0, evaOutlet: -3.0, power: 5.2, electrical: 1.3, cop: 4.0, heatingCurveShift: -1, mode: 0, valve: 0, dhwActive: false },
      { t: 1760002400, inlet: 30.0, outlet: 26.0, target: 31.0, hz: 20, current: 1.5, flow: 20.0, outside: 2.0, evaOutlet: 5.0, power: -5.5, electrical: 0.5, cop: null, heatingCurveShift: -1, mode: 0, valve: 0, dhwActive: false, defrost: true },
    ],
  });
  assert.equal(prepared.samples[0].spread, 4.5);
  assert.equal(prepared.samples[0].targetDelta, 1.5);
  assert.equal(prepared.samples[0].activeThermal, 4.5);
  context2.h2Data = prepared;
  context2.h2RenderSummary();
  assert.equal(text2("h2Runtime"), "40 min");
  assert.equal(text2("h2Starts"), "1");
  assert.equal(text2("h2AvgCycle"), "Ø 40 min / start");
  assert.equal(text2("h2DefrostCount"), "1");
  assert.equal(text2("h2DefrostRuntime"), "10 min total");
  assert.equal(text2("h2HeaterRuntime"), "10 min");
  assert.equal(text2("h2SelectedElectrical"), "1.50 kWh");
  assert.equal(text2("h2SelectedElectricalSplit"), "Heat 1.20 kWh · DHW 0.30 kWh");
  assert.equal(text2("h2SelectedHeat"), "6.00 kWh");
  assert.equal(text2("h2HeatCop"), "4.25");
  assert.equal(text2("h2SelectedCop"), "4.00");
  assert.equal(text2("h2DhwCop"), "DHW COP 3.00");

  context2.h2BuildCycles();
  assert(markup2("h2Cycles").includes("Room heating + defrost"));
  assert(markup2("h2Cycles").includes("25 / 34 Hz"));
  assert(markup2("h2Cycles").includes("1.62 kWh th · COP 4.22"));

  context2.h2UpdateContext(prepared.samples[1]);
  assert(markup2("h2Context").includes("ΔT 5.0 K"));
  assert(markup2("h2Context").includes("34 Hz"));

  context2.h2RenderCalendarCop({
    heatingDegreeDayBase: 16.5,
    current: {
      day: { period: "2026-10-10", electricalKWh: 2, heatKWh: 8, heatConsumptionKWh: 1.5, dhwConsumptionKWh: 0.5, heatProductionKWh: 6.5, dhwProductionKWh: 1.5, heatCop: 4.33, dhwCop: 3.0, totalCop: 4.0 },
      month: { period: "2026-10", electricalKWh: 50, heatKWh: 175, totalCop: 3.5 },
      year: { period: "2026", electricalKWh: 0, heatKWh: 0, totalCop: null },
    },
    demandDaily: [
      { period: "2026-10-10", heatConsumptionKWh: 1.5, dhwConsumptionKWh: 0.5, heatProductionKWh: 6.5, dhwProductionKWh: 1.5, compressorSeconds: 5400, compressorStarts: 3, outsideAverage: 6.5, heatingDegreeDays: 10.0 },
    ],
    monthly: [
      { period: "2026-10", days: 10, electricalKWh: 50, heatKWh: 175, heatConsumptionKWh: 40, dhwConsumptionKWh: 10, heatProductionKWh: 145, dhwProductionKWh: 30, heatCop: 3.63, dhwCop: 3.0, totalCop: 3.5 },
    ],
    yearly: [],
  });
  assert.equal(text2("h2CurrentDayElectrical"), "2.00 kWh (1.5 / 0.5)");
  assert.equal(text2("h2CurrentDayHeat"), "8.00 kWh (6.5 / 1.5)");
  assert.equal(text2("h2CurrentDayCop"), "4.33 / 3.00 / 4.00");
  assert.equal(text2("h2DegreeDayBaseLabel"), "16.5 °C");
  assert(markup2("h2DailyTable").includes("10.10.2026"));
  assert(markup2("h2DailyTable").includes("0.20")); // 2.0 kWh / 10.0 K·d
  assert(markup2("h2MonthlyCop").includes("50.00 (40.0/10.0)"));

  console.log("History new energy tests passed");
}

run().catch(error => { console.error(error); process.exitCode = 1; });
