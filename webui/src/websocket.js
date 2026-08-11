var bConnected=false;
var inactivityTimeout=30000;
var lastActivityTime=Date.now();
var websocketMonitorTimer=null;
var websocketReconnectTimer=null;
var websocketShuttingDown=false;
function monitorWebSocket(){
  if(websocketMonitorTimer)clearInterval(websocketMonitorTimer);
  websocketMonitorTimer=setInterval(function(){
    if(typeof oWebsocket!=='undefined'&&Date.now()-lastActivityTime>inactivityTimeout&&oWebsocket.readyState===WebSocket.OPEN){
      console.log('Inactivity detected, reconnecting...');
      oWebsocket.close();
    }
  },inactivityTimeout);
}
function attemptReconnect(){
  if(websocketShuttingDown||bConnected||websocketReconnectTimer)return;
  websocketReconnectTimer=setTimeout(function(){websocketReconnectTimer=null;if(!websocketShuttingDown&&!bConnected){console.log('Reconnecting...');startWebsockets();}},1000);
}
function startWebsockets(){
  if(websocketShuttingDown)return;
  if(typeof oWebsocket!=='undefined'&&(oWebsocket.readyState===WebSocket.OPEN||oWebsocket.readyState===WebSocket.CONNECTING))return;
  lastActivityTime=Date.now();
  if(typeof MozWebSocket!='undefined'){
    oWebsocket=new MozWebSocket('ws://'+location.host);
  } else if(typeof WebSocket!='undefined'){
    oWebsocket=new WebSocket('ws://'+location.host+'/ws');
  }
  if(oWebsocket){
    oWebsocket.onopen=function(){bConnected=true;lastActivityTime=Date.now();};
    oWebsocket.onclose=function(){bConnected=false;attemptReconnect();};
    oWebsocket.onerror=function(e){console.log('WS error:',e);};
    oWebsocket.onmessage=function(evt){
      lastActivityTime=Date.now();
      if(evt.data.startsWith('{')){
        var j=JSON.parse(evt.data);
        if(j.logMsg!=null){
          var obj=document.getElementById('cli');
          if(!obj)return;
          var chk=document.getElementById('autoscroll');
          obj.value+=j.logMsg+'\n';
          if(chk&&chk.checked)obj.scrollTop=obj.scrollHeight;
        } else if(j.data){
          if(j.data.stats){
            updStat('wifi',j.data.stats.wifi);
            updStat('ethernet',j.data.stats.ethernet);
            updStat('memory',j.data.stats.memory);
            updStat('correct',j.data.stats.correct);
            updStat('mqtt',j.data.stats.mqtt);
            updStat('uptime',j.data.stats.uptime);
            updStat('rules',j.data.stats.rules);
          } else if(j.data.heishavalues){
            updCell(j.data.heishavalues.topic+'-Value',j.data.heishavalues.value);
            updCell(j.data.heishavalues.topic+'-Description',j.data.heishavalues.description);
          } else if(j.data.dallasvalues){
            var dID=j.data.dallasvalues.sensorID;
            if(j.data.dallasvalues.value!==undefined)updCell('SensorID-'+dID+'-Temperature',j.data.dallasvalues.value);
            if(j.data.dallasvalues.present!==undefined)updDallasPresence(dID,j.data.dallasvalues.present);
          } else if(j.data.s0values){
            updCell('s0port-'+j.data.s0values.s0port+'-Watt',j.data.s0values.Watt);
            updCell('s0port-'+j.data.s0values.s0port+'-Watthour',j.data.s0values.Watthour);
            updCell('s0port-'+j.data.s0values.s0port+'-WatthourTotal',j.data.s0values.WatthourTotal);
          } else if(j.data.dallasRescan){
            refreshDallasTable();
          }
        }
      } else {
        var obj=document.getElementById('cli');
        if(!obj)return;
        var chk=document.getElementById('autoscroll');
        obj.value+=evt.data+'\n';
        if(chk&&chk.checked)obj.scrollTop=obj.scrollHeight;
      }
    };
  }
}
function closeWebsocketForNavigation(){
  websocketShuttingDown=true;
  bConnected=false;
  if(websocketMonitorTimer){clearInterval(websocketMonitorTimer);websocketMonitorTimer=null;}
  if(websocketReconnectTimer){clearTimeout(websocketReconnectTimer);websocketReconnectTimer=null;}
  if(typeof oWebsocket!=='undefined'){
    oWebsocket.onclose=null;
    oWebsocket.onerror=null;
    if(oWebsocket.readyState===WebSocket.OPEN||oWebsocket.readyState===WebSocket.CONNECTING)oWebsocket.close();
  }
}
window.addEventListener('pagehide',closeWebsocketForNavigation);
window.addEventListener('beforeunload',closeWebsocketForNavigation);
function updStat(id,val){
  var el=document.getElementById(id);
  if(el){
    // Special handling for WiFi disconnected state
    if(id === 'wifi' && (val === -1 || val === '-1' || parseInt(val) < 0)){
      el.textContent = 'not connected';
      // Remove the % symbol that follows
      var percentSpan = el.nextElementSibling;
      if(percentSpan && percentSpan.textContent === '%'){
        percentSpan.style.display = 'none';
      }
    } else {
      el.textContent = val != null ? val : '';
      // Show % symbol again if it was hidden
      var percentSpan = el.nextElementSibling;
      if(percentSpan && percentSpan.textContent === '%'){
        percentSpan.style.display = '';
      }
    }
  }
  
  if ((el) && (id == 'wifi') && (val!==undefined)){
    var w=parseInt(val);
    var label=el.previousElementSibling;
    var dot=label.previousElementSibling;
    if(dot&&dot.classList.contains('status-dot')){
      dot.className='status-dot';
      if(w===-1||w<0)dot.classList.add('disconnected');
      else if(w>=75)dot.classList.add('excellent');
      else if(w>=50)dot.classList.add('good');
      else if(w>=25)dot.classList.add('fair');
      else dot.classList.add('poor');
    }
  }

  if (id === 'rules') {
    var chip = document.getElementById('rulesChip');
    if (chip) {
      var count = parseInt(val);
      var valueEl = chip.querySelector('.chip-value');
      if (count > 0) {
        chip.className = 'status-chip rules-active';
        valueEl.textContent = 'ACTIVE';
      } else {
        chip.className = 'status-chip rules-inactive';
        valueEl.textContent = 'INACTIVE';
      }
    }
    return;
  }
}

function updCell(id,val){
  if(typeof dashboardDisplayValue==='function' && id.indexOf('TOP')===0 && id.slice(-6)==='-Value'){
    val=dashboardDisplayValue(id.slice(0,-6),val);
  }
  var el=document.getElementById(id);
  if(el&&el.textContent!==val){
    el.classList.remove('update-effect');
    void el.offsetWidth;
    el.textContent=val;
    el.classList.add('update-effect');
  }
}

function updDallasPresence(sID,present){
  var statusCell=document.getElementById('SensorID-'+sID+'-Status');
  if(!statusCell)return;
  var row=statusCell.parentElement;
  if(row)row.style.opacity=present?'':'0.6';
  statusCell.style.color=present?'':'var(--danger,#f44336)';
  if(present){
    delete statusCell.dataset.offlineSince;
    statusCell.classList.remove('offline-duration');
    statusCell.textContent='OK';
  } else {
    statusCell.dataset.offlineSince=Date.now();
    statusCell.classList.add('offline-duration');
    statusCell.textContent=formatOfflineDuration(false,0);
  }
}

function formatOfflineDuration(present,lastSeenSeconds){
  if(present)return'OK';
  if(lastSeenSeconds==null||lastSeenSeconds<0)return'Offline (never seen)';
  var s=lastSeenSeconds;
  if(s<60)return'Offline for '+s+'s';
  if(s<3600)return'Offline for '+Math.floor(s/60)+'m';
  if(s<86400)return'Offline for '+Math.floor(s/3600)+'h';
  return'Offline for '+Math.floor(s/86400)+'d';
}

setInterval(function(){
  document.querySelectorAll('.offline-duration[data-offline-since]').forEach(function(cell){
    var seconds=Math.floor((Date.now()-cell.dataset.offlineSince)/1000);
    cell.textContent=formatOfflineDuration(false,seconds);
  });
},15000);
