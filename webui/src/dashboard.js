var dashboardValues={};
var dashboardWorkflow={type:'none',stage:'idle',previousMode:-1,message:'Loading workflow status ...'};
var dashboardWpConfig={heatMin:20,heatMax:65,dhwBlockAbove:75};
var dashboardRefreshTimer=null;
var dashboardRefreshPromise=null;
var dashboardCommandBusy=false;
var dashboardStepTimers={};
var dashboardStepDebounceMs=1000;
var dashboardSemantic=null;
var dashboardCurveShift=null;
var dashboardTemperatureTopics={TOP5:1,TOP6:1,TOP7:1,TOP9:1,TOP10:1,TOP14:1,TOP29:1,TOP30:1,TOP31:1,TOP32:1,TOP42:1,TOP56:1,TOP70:1};
function dashboardDisplayValue(topic,value){
  if(dashboardTemperatureTopics[topic]){
    var numeric=Number(value);
    if(!Number.isFinite(numeric)||numeric < -60||numeric > 150)return 'N/A';
  }
  return String(value);
}
function renderDashboardHeatRequest(){
  var row=document.getElementById('TOP27-Semantic-Row');
  var label=document.getElementById('TOP27-Semantic-Label');
  var value=document.getElementById('TOP27-Semantic-Value');
  var unit=document.getElementById('TOP27-Semantic-Unit');
  var control=document.getElementById('TOP27-Semantic-Control');
  var recover=document.getElementById('TOP27-Semantic-Recover');
  if(!row||!label||!value||!unit||!control)return;
  if(!dashboardSemantic||dashboardSemantic.semantic==='heatCurveShift'){row.style.display='none';return;}
  row.style.display='flex';
  label.textContent=dashboardSemantic.label||'Zone 1 request';
  unit.textContent=dashboardSemantic.unit||'';
  var raw=Number(dashboardSemantic.rawValue);
  var rawValid=dashboardSemantic.rawValidForSemantic===true&&Number.isFinite(raw);
  var semanticKnown=dashboardSemantic.semanticKnown===true||(!!dashboardSemantic.semantic&&dashboardSemantic.semantic!=='unknown');
  var editable=semanticKnown&&dashboardSemantic.writable===true;
  value.textContent=rawValid?String(raw):(Number.isFinite(raw)?(semanticKnown?'Invalid (raw: '+String(raw)+')':'Unknown (raw: '+String(raw)+')'):'N/A');
  control.querySelectorAll('button').forEach(function(button){button.style.display=editable?'inline-flex':'none';});
  if(recover){recover.textContent=dashboardSemantic.semantic==='heatCurveShift'?'Set 0':'Set valid';recover.style.display=editable&&!rawValid?'inline-flex':'none';}
}
function renderDashboardCurveShift(){
  var row=document.getElementById('HeatingCurveShift-Row');
  var control=document.getElementById('HeatingCurveShift-Control');
  var value=document.getElementById('HeatingCurveShift-Value');
  var endpoint=dashboardCurveShift&&dashboardCurveShift.implementation==='curveEndpoints';
  var available=!!(dashboardCurveShift&&dashboardCurveShift.available);
  if(row)row.style.display=available?'flex':'none';
  if(value)value.textContent=available&&dashboardCurveShift.valueValid!==false?String(dashboardCurveShift.shift):(available&&dashboardCurveShift.rawValue!==null?'Invalid (raw: '+String(dashboardCurveShift.rawValue)+')':'N/A');
  if(control)control.querySelectorAll('button').forEach(function(button){button.disabled=!available||dashboardCurveShift.writable!==true;});
  var high=document.getElementById('HeatingCurveBaseHigh-Value'),low=document.getElementById('HeatingCurveBaseLow-Value');
  var effectiveHigh=document.getElementById('HeatingCurveEffectiveHigh-Value'),effectiveLow=document.getElementById('HeatingCurveEffectiveLow-Value');
  if(high)high.textContent=endpoint?String(dashboardCurveShift.baseTargetHigh):'N/A';
  if(low)low.textContent=endpoint?String(dashboardCurveShift.baseTargetLow):'N/A';
  if(effectiveHigh)effectiveHigh.textContent=endpoint?String(dashboardCurveShift.effectiveTargetHigh):'N/A';
  if(effectiveLow)effectiveLow.textContent=endpoint?String(dashboardCurveShift.effectiveTargetLow):'N/A';
  [high,low].forEach(function(span){if(span){var r=span.closest('.dashboard-row');if(r)r.style.display=endpoint?'flex':'none';}});
  [effectiveHigh,effectiveLow].forEach(function(span){if(span){var r=span.closest('.dashboard-row');if(r)r.style.display=endpoint?'flex':'none';}});
}
function dashboardItems(data){
  return [].concat(data.heatpump||[],data['heatpump extra']||[],data['heatpump optional']||[]);
}
function renderDashboard(data){
  var items=dashboardItems(data);
  items.forEach(function(item){
    dashboardValues[item.Topic]=item.Value;
  });
  items.forEach(function(item){
    updCell(item.Topic+'-Value',dashboardDisplayValue(item.Topic,item.Value));
    updCell(item.Topic+'-Description',String(item.Description));
  });
  renderDashboardHeatRequest();
  syncDashboardControls();
}
function refreshDashboard(){
  if(dashboardRefreshPromise)return dashboardRefreshPromise;
  dashboardRefreshPromise=fetch('/json',{cache:'no-store'}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.json();
  }).then(renderDashboard).then(function(){return fetch('/zone1heatsemantic',{cache:'no-store'});}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.json();
  }).then(function(data){dashboardSemantic=data;renderDashboardHeatRequest();return fetch('/heatingcurveshift',{cache:'no-store'});}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.json();
  }).then(function(data){dashboardCurveShift=data;renderDashboardCurveShift();return refreshDashboardWorkflow();}).then(function(){return fetch('/wpsettingsconfig',{cache:'no-store'});}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.json();
  }).then(function(data){dashboardWpConfig=data;dashboardRefreshPromise=null;syncDashboardControls();}).catch(function(error){
    dashboardRefreshPromise=null;
    setDashboardStatus('Update failed: '+error.message,true);
  });
  return dashboardRefreshPromise;
}
function refreshDashboardWorkflow(){
  return fetch('/dashboardworkflow',{cache:'no-store'}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.json();
  }).then(function(data){
    dashboardWorkflow=data;
    syncDashboardControls();
  }).catch(function(error){
    setDashboardStatus('Workflow status failed: '+error.message,true);
  });
}
function setGauge(topic,max){
  var path=document.getElementById(topic+'-Gauge');
  var value=parseFloat(dashboardValues[topic]);
  if(path&&!isNaN(value)){
    var percent=Math.max(0,Math.min(100,value/max*100));
    path.style.strokeDashoffset=String(100-percent);
  }
}
function setToggle(id,topic){
  var toggle=document.getElementById(id);
  if(toggle)toggle.checked=Number(dashboardValues[topic])!==0;
}
function setDashboardSelectValue(id,value){
  var select=document.getElementById(id);
  if(select&&document.activeElement!==select&&!isNaN(Number(value)))select.value=String(value);
}
function syncDashboardControls(){
  if(!dashboardCommandBusy)document.querySelectorAll('.dashboard-page button,.dashboard-page input,.dashboard-page select').forEach(function(control){control.disabled=false;});
  setToggle('heatpumpToggle','TOP0');
  var operationMode=Number(dashboardValues.TOP4);if(operationMode===7)operationMode=2;if(operationMode===8)operationMode=6;
  setDashboardSelectValue('dashboardOperationMode',operationMode);
  setDashboardSelectValue('dashboardQuietMode',dashboardValues.TOP18);
  setDashboardSelectValue('dashboardPowerfulMode',dashboardValues.TOP17);
  var busy=dashboardWorkflow.type!=='none';
  var startDhw=document.getElementById('startDhwButton');
  var cancelDhw=document.getElementById('cancelDhwButton');
  var startSterilization=document.getElementById('startSterilizationButton');
  var cancelSterilization=document.getElementById('cancelSterilizationButton');
  if(startDhw)startDhw.disabled=busy||Number(dashboardValues.TOP2)!==0;
  if(cancelDhw)cancelDhw.disabled=dashboardWorkflow.type!=='dhw';
  if(startSterilization)startSterilization.disabled=busy||Number(dashboardValues.TOP69)!==0;
  if(cancelSterilization)cancelSterilization.disabled=dashboardWorkflow.type!=='sterilization';
  var workflowMessage=dashboardWorkflow.message;
  if(!busy&&Number(dashboardValues.TOP69)!==0)workflowMessage='Sterilization is active outside the dashboard workflow';
  else if(!busy&&Number(dashboardValues.TOP2)!==0)workflowMessage='Force DHW is active outside the dashboard workflow';
  if(workflowMessage)setDashboardStatus(workflowMessage,false);
  setGauge('TOP1',35);
  setGauge('TOP8',120);
  if(dashboardCommandBusy)document.querySelectorAll('.dashboard-page button,.dashboard-page input,.dashboard-page select').forEach(function(control){control.disabled=true;});
}
function setDashboardStatus(message,isError){
  var status=document.getElementById('dashboardCommandStatus');
  if(status){status.textContent=message||'';status.style.color=isError?'var(--red)':'var(--text-muted)';}
}
function sendDashboardCommand(command,value){
  if(dashboardCommandBusy){setDashboardStatus('Please wait for the current command to finish',false);return Promise.resolve(false);}
  dashboardCommandBusy=true;
  syncDashboardControls();
  setDashboardStatus('Sending '+command+' ...',false);
  return fetch('/command?'+encodeURIComponent(command)+'='+encodeURIComponent(value),{cache:'no-store'}).then(function(response){
    if(!response.ok)throw new Error('HTTP '+response.status);
    return response.text();
  }).then(function(message){
    setDashboardStatus(message.trim()||'Command sent',false);
    return new Promise(function(resolve){window.setTimeout(resolve,1400);});
  }).then(function(){
    dashboardCommandBusy=false;
    syncDashboardControls();
    refreshDashboard();
    return true;
  }).catch(function(error){
    dashboardCommandBusy=false;
    syncDashboardControls();
    setDashboardStatus('Command failed: '+error.message,true);
    throw error;
  });
}
function setDashboardToggle(toggle,command){
  toggle.disabled=true;
  sendDashboardCommand(command,toggle.checked?1:0).catch(function(){toggle.checked=!toggle.checked;}).then(function(){toggle.disabled=false;});
}
function setDashboardSelect(select,command){
  select.disabled=true;
  sendDashboardCommand(command,select.value).then(function(){select.disabled=false;},function(){select.disabled=false;});
}
function sendDashboardWorkflow(action){
  return sendDashboardCommand('DashboardWorkflow',action);
}
function startDashboardWorkflow(type){
  var label=type==='dhw'?'forced DHW':'forced sterilization';
  if(!window.confirm('Start '+label+' cycle?'))return;
  sendDashboardWorkflow(type==='dhw'?'start_dhw':'start_sterilization');
}
function cancelDashboardWorkflow(type){
  var label=type==='dhw'?'forced DHW':'forced sterilization';
  if(!window.confirm('Cancel '+label+' and restore the previous operating mode?'))return;
  sendDashboardWorkflow(type==='dhw'?'cancel_dhw':'cancel_sterilization');
}
function queueDashboardStep(command,topic,value){
  if(dashboardStepTimers[topic])window.clearTimeout(dashboardStepTimers[topic]);
  setDashboardStatus('Waiting to send '+command+' ...',false);
  dashboardStepTimers[topic]=window.setTimeout(function(){
    delete dashboardStepTimers[topic];
    sendDashboardCommand(command,value).catch(function(){});
  },dashboardStepDebounceMs);
}
function stepDashboardValue(command,topic,delta,min,max){
  var current=parseFloat(dashboardValues[topic]);
  if(isNaN(current))return;
  var next=Math.max(min,Math.min(max,Math.round(current+delta)));
  dashboardValues[topic]=next;
  if(topic==='TOP27')renderDashboardHeatRequest();
  else updCell(topic+'-Value',String(next));
  queueDashboardStep(command,topic,next);
}
function stepZone1Heat(delta){
  if(dashboardSemantic&&dashboardSemantic.semantic==='heatCurveShift'){stepHeatingCurveShift(delta);return;}
  if(!dashboardSemantic||dashboardSemantic.writable!==true||dashboardSemantic.semanticKnown!==true){
    setDashboardStatus('Zone 1 request is not writable for the current configuration',true);
    return;
  }
  var current=Number(dashboardSemantic.rawValue);
  var min=Number(dashboardSemantic.min);
  var max=Number(dashboardSemantic.max);
  var rawValid=dashboardSemantic.rawValidForSemantic===true&&Number.isFinite(current);
  if(!rawValid)current=dashboardSemantic.semantic==='heatCurveShift'?0:min;
  if(!Number.isFinite(min)||!Number.isFinite(max)){
    setDashboardStatus('Zone 1 request value is unavailable',true);
    return;
  }
  var next=Math.max(min,Math.min(max,Math.round(current+delta)));
  var command=dashboardSemantic.semantic==='heatCurveShift'?'SetHeatingCurveShift':
    dashboardSemantic.semantic==='heatingWaterTarget'?'SetZ1HeatingWaterTarget':
    dashboardSemantic.semantic==='roomTarget'?'SetZ1RoomTarget':null;
  if(command===null){setDashboardStatus('Unknown Zone 1 request semantics',true);return;}
  dashboardSemantic.rawValue=next;
  dashboardSemantic.rawValidForSemantic=true;
  renderDashboardHeatRequest();
  queueDashboardStep(command,'TOP27',next);
}
function stepHeatingCurveShift(delta){
  if(!dashboardCurveShift||dashboardCurveShift.available!==true||dashboardCurveShift.writable!==true){setDashboardStatus('Heating curve shift is not writable for the current configuration',true);return;}
  var current=Number(dashboardCurveShift.shift),min=Number(dashboardCurveShift.min),max=Number(dashboardCurveShift.max);
  if(!Number.isFinite(current))current=0;
  var next=Math.max(min,Math.min(max,Math.round(current+delta)));
  dashboardCurveShift.shift=next;dashboardCurveShift.valueValid=true;renderDashboardCurveShift();
  queueDashboardStep('SetHeatingCurveShift','HeatingCurveShift',next);
}
function stepHeatingCurveBase(which,delta){
  if(!dashboardCurveShift||dashboardCurveShift.implementation!=='curveEndpoints'||dashboardCurveShift.writable!==true){setDashboardStatus('Heating curve base is not writable for the current configuration',true);return;}
  var key=which==='high'?'baseTargetHigh':'baseTargetLow',current=Number(dashboardCurveShift[key]);
  if(!Number.isFinite(current))return;
  dashboardCurveShift[key]=Math.round(current+delta);
  dashboardCurveShift.effectiveTargetHigh=Number(dashboardCurveShift.baseTargetHigh)+Number(dashboardCurveShift.shift);
  dashboardCurveShift.effectiveTargetLow=Number(dashboardCurveShift.baseTargetLow)+Number(dashboardCurveShift.shift);
  renderDashboardCurveShift();
  queueDashboardStep(which==='high'?'SetZ1HeatCurveBaseHigh':'SetZ1HeatCurveBaseLow','HeatingCurveBase'+which, dashboardCurveShift[key]);
}
function recoverZone1Heat(){
  if(!dashboardSemantic||dashboardSemantic.writable!==true||dashboardSemantic.semanticKnown!==true)return;
  var min=Number(dashboardSemantic.min),max=Number(dashboardSemantic.max);
  var next=dashboardSemantic.semantic==='heatCurveShift'?0:min;
  if(!Number.isFinite(next)||next<min||next>max)return;
  var command=dashboardSemantic.semantic==='heatCurveShift'?'SetHeatingCurveShift':
    dashboardSemantic.semantic==='heatingWaterTarget'?'SetZ1HeatingWaterTarget':
    dashboardSemantic.semantic==='roomTarget'?'SetZ1RoomTarget':null;
  if(command===null)return;
  dashboardSemantic.rawValue=next;
  dashboardSemantic.rawValidForSemantic=true;
  renderDashboardHeatRequest();
  queueDashboardStep(command,'TOP27',next);
}
document.addEventListener('DOMContentLoaded',function(){
  refreshDashboard();
  startWebsockets();
  monitorWebSocket();
  dashboardRefreshTimer=window.setInterval(refreshDashboard,10000);
});
