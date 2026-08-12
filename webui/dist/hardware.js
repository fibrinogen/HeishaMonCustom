var hardwareRefreshPromise = null;

function hardwareDisplay(value) {
  return value === undefined || value === null || value === '' ? '--' : String(value);
}

function hardwareRender(data) {
  document.getElementById('hardwareModel').textContent = hardwareDisplay(data.model);
  document.getElementById('hardwareType').textContent = hardwareDisplay(data.type);
  document.getElementById('hardwareCapacity').textContent = hardwareDisplay(data.capacity);
  document.getElementById('hardwarePower').textContent = hardwareDisplay(data.power);
  document.getElementById('heatingMode').textContent = hardwareDisplay(data.heatingMode);
  document.getElementById('zone1Sensor').textContent = hardwareDisplay(data.zone1Sensor);
  document.getElementById('zone2Sensor').textContent = hardwareDisplay(data.zone2Sensor);
  document.getElementById('roomHeaterState').textContent = hardwareDisplay(data.roomHeaterState);
  document.getElementById('bufferInstalled').textContent = hardwareDisplay(data.bufferInstalled);
  document.getElementById('bufferTankDelta').textContent = hardwareDisplay(data.bufferTankDelta);
  document.getElementById('dhwInstalled').textContent = hardwareDisplay(data.dhwInstalled);
  document.getElementById('dhwHeaterState').textContent = hardwareDisplay(data.dhwHeaterState);
  document.getElementById('coolingMode').textContent = hardwareDisplay(data.coolingMode);
  document.getElementById('solarMode').textContent = hardwareDisplay(data.solarMode);
  document.getElementById('pumpFlowMode').textContent = hardwareDisplay(data.pumpFlowMode);
  document.getElementById('liquidType').textContent = hardwareDisplay(data.liquidType);
  document.getElementById('externalSensor').textContent = hardwareDisplay(data.externalSensor);
  document.getElementById('antiFreezeMode').textContent = hardwareDisplay(data.antiFreezeMode);
  
}

function hardwareRefresh() {
  if (hardwareRefreshPromise) return hardwareRefreshPromise;
  
  hardwareRefreshPromise = fetch('/hardwareapi', {cache: 'no-store'})
    .then(function(response) {
      if (!response.ok) throw new Error('HTTP ' + response.status);
      return response.json();
    })
    .then(function(data) {
      hardwareRefreshPromise = null;
      hardwareRender(data);
      return data;
    })
    .catch(function(error) {
      hardwareRefreshPromise = null;
      console.error('Hardware refresh failed: ' + error.message);
      throw error;
  });
  return hardwareRefreshPromise;
}

document.addEventListener('DOMContentLoaded', function() {
  hardwareRefresh();
});
