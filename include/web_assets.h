// =============================================================================
// web_assets.h - Web 监控面板页面资源（内嵌，单二进制零文件依赖）
// 页面包含：实时状态仪表盘 + 内嵌终端（SSE 日志流）+ 参数趋势图表
// =============================================================================
#pragma once

namespace soil {

inline const char* DASHBOARD_HTML = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>农田土壤参数移动采集系统 V1.0</title>
<style>
:root{
  --bg:#0d1117; --panel:#161b22; --border:#30363d; --text:#c9d1d9;
  --dim:#8b949e; --green:#3fb950; --yellow:#d29922; --red:#f85149;
  --blue:#58a6ff; --purple:#bc8cff;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:"Microsoft YaHei","PingFang SC",sans-serif;font-size:14px}
header{display:flex;align-items:center;gap:14px;padding:12px 20px;background:var(--panel);
  border-bottom:1px solid var(--border);flex-wrap:wrap}
header h1{font-size:18px;color:#fff;font-weight:600}
header .sub{color:var(--dim);font-size:12px}
.badge{padding:3px 10px;border-radius:12px;font-size:12px;border:1px solid var(--border);background:#0d1117}
.badge.ok{color:var(--green);border-color:var(--green)}
.badge.warn{color:var(--yellow);border-color:var(--yellow)}
.badge.err{color:var(--red);border-color:var(--red)}
.badge.info{color:var(--blue)}
#url{color:var(--dim);font-size:12px;margin-left:auto;font-family:Consolas,monospace}
main{display:grid;grid-template-columns:1fr 380px;gap:14px;padding:14px 20px}
@media(max-width:1100px){main{grid-template-columns:1fr}}
.card{background:var(--panel);border:1px solid var(--border);border-radius:8px;overflow:hidden}
.card h2{font-size:14px;padding:10px 14px;border-bottom:1px solid var(--border);color:#fff;
  display:flex;align-items:center;gap:8px}
.card h2 .dot{width:8px;height:8px;border-radius:50%;background:var(--green);
  animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
/* ---- 终端 ---- */
#terminal{background:#000;height:520px;overflow-y:auto;padding:10px 12px;
  font-family:Consolas,"Courier New",monospace;font-size:12.5px;line-height:1.55}
#terminal .ln{white-space:pre-wrap;word-break:break-all}
#terminal .info{color:#c9d1d9}
#terminal .warn{color:var(--yellow)}
#terminal .error{color:var(--red)}
#terminal .sys{color:var(--purple)}
.tbar{display:flex;gap:8px;padding:8px 14px;border-bottom:1px solid var(--border);align-items:center}
.tbar button{background:#21262d;color:var(--text);border:1px solid var(--border);
  border-radius:6px;padding:4px 12px;cursor:pointer;font-size:12px}
.tbar button:hover{background:#30363d}
.tbar button.active{color:var(--green);border-color:var(--green)}
.tbar .cnt{margin-left:auto;color:var(--dim);font-size:12px;font-family:Consolas,monospace}
/* ---- 状态卡片 ---- */
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;background:var(--border)}
.stat{background:var(--panel);padding:12px 14px}
.stat .v{font-size:22px;font-weight:700;font-family:Consolas,monospace}
.stat .l{color:var(--dim);font-size:12px;margin-top:2px}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:8px 14px;border-bottom:1px solid var(--border);text-align:left}
th{color:var(--dim);font-weight:400;font-size:12px}
.tag{padding:2px 8px;border-radius:10px;font-size:12px}
.tag.rdy{background:#12261e;color:var(--green)}
.tag.sleep{background:#26220f;color:var(--yellow)}
.tag.err{background:#2d1517;color:var(--red)}
.tag.deg{background:#1f1a2e;color:var(--purple)}
/* ---- 图表 ---- */
.charts{display:grid;grid-template-columns:repeat(2,1fr);gap:14px;padding:0 20px 20px}
@media(max-width:900px){.charts{grid-template-columns:1fr}}
.chart-box{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:10px}
.chart-box h3{font-size:13px;color:var(--dim);margin-bottom:6px;font-weight:400}
.chart-box h3 b{color:var(--text);font-size:16px;font-family:Consolas,monospace;float:right}
canvas{width:100%;height:110px;display:block}
#latest{padding:10px 14px;color:var(--dim);font-size:12px;border-top:1px solid var(--border);
  font-family:Consolas,monospace}
</style>
</head>
<body>
<header>
  <h1>🌱 农田土壤参数移动采集系统</h1>
  <span class="sub">V1.0 · 嵌入式边缘数据采集终端</span>
  <span class="badge info" id="b-dev">--</span>
  <span class="badge ok" id="b-health">健康 --</span>
  <span class="badge info" id="b-net">网络 --</span>
  <span class="badge info" id="b-state">--</span>
  <span class="badge" id="b-uptime">运行 --</span>
  <span id="url"></span>
</header>

<main>
  <div class="card">
    <h2><span class="dot"></span>系统终端（实时日志流）</h2>
    <div class="tbar">
      <button id="btn-pause">⏸ 暂停</button>
      <button id="btn-clear">🗑 清空</button>
      <button id="btn-scroll" class="active">⬇ 自动滚动</button>
      <span class="cnt" id="t-count">0 行 · SSE 已连接</span>
    </div>
    <div id="terminal"></div>
    <div id="latest">--</div>
  </div>

  <div style="display:flex;flex-direction:column;gap:14px">
    <div class="card">
      <h2><span class="dot"></span>运行统计</h2>
      <div class="stats">
        <div class="stat"><div class="v" id="s-sample" style="color:var(--blue)">0</div><div class="l">采集样本</div></div>
        <div class="stat"><div class="v" id="s-fused" style="color:var(--purple)">0</div><div class="l">融合数据点</div></div>
        <div class="stat"><div class="v" id="s-upload" style="color:var(--green)">0</div><div class="l">MQTT上传</div></div>
        <div class="stat"><div class="v" id="s-store" style="color:var(--blue)">0</div><div class="l">事务提交</div></div>
        <div class="stat"><div class="v" id="s-frag" style="color:var(--green)">0</div><div class="l">发送分片</div></div>
        <div class="stat"><div class="v" id="s-recover" style="color:var(--yellow)">0</div><div class="l">恢复执行</div></div>
      </div>
    </div>
    <div class="card">
      <h2><span class="dot"></span>传感器状态</h2>
      <table>
        <tr><th>传感器</th><th>型号</th><th>状态</th></tr>
        <tbody id="sensor-rows"></tbody>
      </table>
    </div>
    <div class="card">
      <h2><span class="dot"></span>数据质量分布</h2>
      <table>
        <tr><th>优秀</th><th>良好</th><th>一般</th><th>差</th></tr>
        <tbody><tr id="q-row"><td>0</td><td>0</td><td>0</td><td>0</td></tr></tbody>
      </table>
    </div>
  </div>
</main>

<div class="charts">
  <div class="chart-box"><h3>土壤温度 (°C)<b id="v-temp">--</b></h3><canvas id="c-temp"></canvas></div>
  <div class="chart-box"><h3>土壤体积含水率 (%)<b id="v-mois">--</b></h3><canvas id="c-mois"></canvas></div>
  <div class="chart-box"><h3>土壤 pH<b id="v-ph">--</b></h3><canvas id="c-ph"></canvas></div>
  <div class="chart-box"><h3>电导率 (uS/cm)<b id="v-ec">--</b></h3><canvas id="c-ec"></canvas></div>
</div>

<script>
'use strict';
const $=id=>document.getElementById(id);
const term=$('terminal');
let paused=false,autoScroll=true,lineCount=0;
$('url').textContent=location.host;

/* ---------- 内嵌终端（SSE 实时流） ---------- */
function addLine(text,cls){
  if(paused)return;
  const d=document.createElement('div');
  d.className='ln '+(cls||'info');
  d.textContent=text;
  term.appendChild(d);
  if(++lineCount>500){term.removeChild(term.firstChild);lineCount--;}
  if(autoScroll)term.scrollTop=term.scrollHeight;
  $('t-count').textContent=lineCount+' 行 · SSE 已连接';
}
function levelClass(l){return l===2?'error':l===1?'warn':'info';}
function fmtEntry(e){return '['+e.t+']['+e.lv+']['+e.m+'] '+e.msg;}

(async function backfill(){
  try{
    const r=await fetch('/api/logs?count=120');
    const arr=await r.json();
    arr.forEach(e=>addLine(fmtEntry(e),levelClass(e.l)));
  }catch(err){addLine('日志回填失败: '+err,'error');}
  const es=new EventSource('/api/stream');
  es.onmessage=ev=>{
    try{
      const e=JSON.parse(ev.data);
      addLine(fmtEntry(e),levelClass(e.l));
    }catch(err){addLine(ev.data,'sys');}
  };
  es.onerror=()=>{$('t-count').textContent=lineCount+' 行 · SSE 重连中...';};
})();

$('btn-pause').onclick=e=>{paused=!paused;
  e.target.textContent=paused?'▶ 继续':'⏸ 暂停';
  e.target.classList.toggle('active',paused);};
$('btn-clear').onclick=()=>{term.innerHTML='';lineCount=0;};
$('btn-scroll').onclick=e=>{autoScroll=!autoScroll;
  e.target.classList.toggle('active',autoScroll);
  if(autoScroll)term.scrollTop=term.scrollHeight;};

/* ---------- 状态轮询 ---------- */
function fmtDur(s){
  if(s<60)return s+'秒';
  if(s<3600)return Math.floor(s/60)+'分'+(s%60)+'秒';
  return Math.floor(s/3600)+'时'+Math.floor(s%3600/60)+'分';
}
async function pollStatus(){
  try{
    const st=await (await fetch('/api/status')).json();
    $('b-dev').textContent=st.device_id;
    const h=st.health;
    $('b-health').textContent='健康 '+h;
    $('b-health').className='badge '+(h>=80?'ok':h>=50?'warn':'err');
    $('b-net').textContent='网络 '+(st.network.online?st.network.interface:'离线');
    $('b-net').className='badge '+(st.network.online?'ok':'err');
    $('b-state').textContent=st.device_state;
    $('b-uptime').textContent='运行 '+fmtDur(st.uptime_s);
    $('s-sample').textContent=st.sensing.samples;
    $('s-fused').textContent=st.fusion.points;
    $('s-upload').textContent=st.comm.uploaded;
    $('s-store').textContent=st.storage.commits;
    $('s-frag').textContent=st.comm.fragments;
    $('s-recover').textContent=st.autonomy.recoveries;
    $('q-row').innerHTML='<td>'+st.quality.excellent+'</td><td>'+st.quality.good+
      '</td><td>'+st.quality.fair+'</td><td>'+st.quality.poor+'</td>';
    $('sensor-rows').innerHTML=st.sensors.map(s=>{
      const c=s.status==='就绪'?'rdy':s.status==='休眠'?'sleep':
              (s.status==='错误'||s.status==='故障')?'err':'deg';
      return '<tr><td>'+s.name+'</td><td style="font-family:monospace;color:var(--dim)">'+
             s.code+'</td><td><span class="tag '+c+'">'+s.status+'</span></td></tr>';
    }).join('');
    if(st.latest&&st.latest.batch!==undefined){
      $('latest').textContent='最新数据点 #'+st.latest.batch+'  温度 '+
        st.latest.temp.toFixed(2)+'°C  含水率 '+st.latest.moisture.toFixed(2)+
        '%  pH '+st.latest.ph.toFixed(2)+'  EC '+st.latest.ec.toFixed(1)+
        'uS/cm  质量 '+st.latest.quality+'/100';
    }
  }catch(err){}
}

/* ---------- 趋势图表 ---------- */
function drawChart(id,key,color,unit,dec){
  const cv=$(id),dpr=window.devicePixelRatio||1;
  const W=cv.clientWidth,H=cv.clientHeight;
  cv.width=W*dpr;cv.height=H*dpr;
  const ctx=cv.getContext('2d');ctx.scale(dpr,dpr);
  ctx.clearRect(0,0,W,H);
  const data=chartData;
  if(data.length<2){ctx.fillStyle='#8b949e';ctx.fillText('等待数据...',10,H/2);return;}
  const vals=data.map(p=>p[key]);
  let mn=Math.min(...vals),mx=Math.max(...vals);
  if(mx-mn<1e-9){mx+=1;mn-=1;}
  const pad=(mx-mn)*0.15;mn-=pad;mx+=pad;
  // 网格
  ctx.strokeStyle='#21262d';ctx.lineWidth=1;
  for(let i=0;i<=3;i++){const y=H*i/3;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();}
  // 曲线
  ctx.strokeStyle=color;ctx.lineWidth=1.8;ctx.beginPath();
  vals.forEach((v,i)=>{
    const x=i/(vals.length-1)*(W-4)+2, y=H-4-(v-mn)/(mx-mn)*(H-8);
    i?ctx.lineTo(x,y):ctx.moveTo(x,y);
  });
  ctx.stroke();
  // 末点
  const lx=W-2, ly=H-4-(vals[vals.length-1]-mn)/(mx-mn)*(H-8);
  ctx.fillStyle=color;ctx.beginPath();ctx.arc(lx-2,ly,3,0,7);ctx.fill();
}
let chartData=[];
async function pollData(){
  try{
    const d=await (await fetch('/api/data?count=60')).json();
    if(d.length){chartData=d;
      drawChart('c-temp','temp','#58a6ff');
      drawChart('c-mois','moisture','#3fb950');
      drawChart('c-ph','ph','#bc8cff');
      drawChart('c-ec','ec','#d29922');
      const last=d[d.length-1];
      $('v-temp').textContent=last.temp.toFixed(2);
      $('v-mois').textContent=last.moisture.toFixed(2);
      $('v-ph').textContent=last.ph.toFixed(2);
      $('v-ec').textContent=last.ec.toFixed(1);
    }
  }catch(err){}
}
pollStatus();pollData();
setInterval(pollStatus,2000);
setInterval(pollData,5000);
</script>
</body>
</html>)html";

}  // namespace soil
