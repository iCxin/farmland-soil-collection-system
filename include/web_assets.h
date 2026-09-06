// =============================================================================
// web_assets.h - Web 监控面板页面资源（内嵌，单二进制零文件依赖）
// 设计语言：shadcn/ui 风格（设计令牌 / 卡片化组件 / 亮暗双主题）
// 页面包含：实时状态仪表盘 + 内嵌终端（SSE 日志流）+ 参数趋势图表
// =============================================================================
#pragma once

namespace soil {

inline const char* DASHBOARD_HTML = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>农田土壤参数移动采集系统 V1.0</title>
<style>
/* ---------- shadcn/ui 设计令牌 ---------- */
:root{
  --background:0 0% 100%;
  --foreground:240 10% 3.9%;
  --card:0 0% 100%;
  --muted:240 4.8% 95.9%;
  --muted-foreground:240 3.8% 46.1%;
  --border:240 5.9% 90%;
  --input:240 5.9% 90%;
  --primary:240 5.9% 10%;
  --primary-foreground:0 0% 98%;
  --secondary:240 4.8% 95.9%;
  --secondary-foreground:240 5.9% 10%;
  --accent:240 4.8% 95.9%;
  --ring:240 5% 64.9%;
  --radius:0.625rem;
  --success:142 71% 45%;
  --success-fg:142 72% 29%;
  --warning:38 92% 50%;
  --warning-fg:26 90% 37%;
  --destructive:0 84% 60%;
  --destructive-fg:0 72% 51%;
  --chart-1:221 83% 53%;
  --chart-2:160 84% 39%;
  --chart-3:262 83% 58%;
  --chart-4:38 92% 50%;
  --term-bg:240 10% 3.9%;
  --term-fg:240 5% 84%;
  --term-dim:240 4% 55%;
  --term-warn:43 96% 56%;
  --term-err:0 91% 71%;
}
.dark{
  --background:240 10% 3.9%;
  --foreground:0 0% 98%;
  --card:240 8% 5.5%;
  --muted:240 3.7% 15.9%;
  --muted-foreground:240 5% 64.9%;
  --border:240 3.7% 15.9%;
  --input:240 3.7% 15.9%;
  --primary:0 0% 98%;
  --primary-foreground:240 5.9% 10%;
  --secondary:240 3.7% 15.9%;
  --secondary-foreground:0 0% 98%;
  --accent:240 3.7% 15.9%;
  --ring:240 4.9% 83.9%;
  --success:142 70% 45%;
  --success-fg:142 70% 55%;
  --warning:38 92% 55%;
  --warning-fg:43 96% 62%;
  --destructive:0 72% 51%;
  --destructive-fg:0 91% 71%;
  --term-bg:0 0% 0%;
  --term-fg:240 5% 84%;
  --term-dim:240 4% 55%;
  --term-warn:43 96% 56%;
  --term-err:0 91% 71%;
}
*{box-sizing:border-box;margin:0;padding:0;border-color:hsl(var(--border))}
html{-webkit-font-smoothing:antialiased}
body{
  background:hsl(var(--background));color:hsl(var(--foreground));
  font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  font-size:14px;line-height:1.5;
}

/* ---------- 布局 ---------- */
.container{max-width:1440px;margin:0 auto;padding:24px 24px 40px}
header.site{
  position:sticky;top:0;z-index:20;
  background:hsl(var(--background) / .85);backdrop-filter:blur(8px);
  border-bottom:1px solid hsl(var(--border));
}
header.site .inner{max-width:1440px;margin:0 auto;padding:0 24px;height:60px;
  display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.logo{width:30px;height:30px;border-radius:calc(var(--radius) - 4px);
  background:hsl(var(--primary));color:hsl(var(--primary-foreground));
  display:flex;align-items:center;justify-content:center;font-size:15px}
header h1{font-size:15px;font-weight:600;letter-spacing:-0.01em}
header .sub{color:hsl(var(--muted-foreground));font-size:12.5px}
header .spacer{flex:1}

/* ---------- 徽章 ---------- */
.badge{display:inline-flex;align-items:center;gap:5px;border-radius:calc(var(--radius) - 4px);
  border:1px solid hsl(var(--border));padding:2px 8px;font-size:12px;font-weight:500;
  background:transparent;color:hsl(var(--muted-foreground));white-space:nowrap}
.badge .cdot{width:6px;height:6px;border-radius:99px;background:hsl(var(--muted-foreground))}
.badge.success{border-color:hsl(var(--success) / .35);color:hsl(var(--success-fg));
  background:hsl(var(--success) / .1)}
.badge.success .cdot{background:hsl(var(--success))}
.badge.warning{border-color:hsl(var(--warning) / .4);color:hsl(var(--warning-fg));
  background:hsl(var(--warning) / .12)}
.badge.warning .cdot{background:hsl(var(--warning))}
.badge.destructive{border-color:hsl(var(--destructive) / .4);color:hsl(var(--destructive-fg));
  background:hsl(var(--destructive) / .1)}
.badge.destructive .cdot{background:hsl(var(--destructive))}
.badge.secondary{background:hsl(var(--secondary));color:hsl(var(--secondary-foreground))}

/* ---------- 按钮 ---------- */
.btn{display:inline-flex;align-items:center;gap:6px;height:32px;padding:0 12px;
  border-radius:calc(var(--radius) - 2px);border:1px solid hsl(var(--border));
  background:hsl(var(--secondary));color:hsl(var(--secondary-foreground));
  font-size:12.5px;font-weight:500;cursor:pointer;transition:background .15s}
.btn:hover{background:hsl(var(--accent))}
.btn.primary{background:hsl(var(--primary));color:hsl(var(--primary-foreground));border-color:hsl(var(--primary))}
.btn.primary:hover{opacity:.88;background:hsl(var(--primary))}
.btn.ghost{background:transparent}
.btn.ghost:hover{background:hsl(var(--accent))}
.btn.active{border-color:hsl(var(--ring));color:hsl(var(--foreground));
  background:hsl(var(--accent));box-shadow:0 0 0 1px hsl(var(--ring) / .6)}
.btn:focus-visible{outline:2px solid hsl(var(--ring));outline-offset:2px}

/* ---------- 卡片 ---------- */
.card{background:hsl(var(--card));border:1px solid hsl(var(--border));
  border-radius:var(--radius);overflow:hidden}
.card-header{display:flex;align-items:center;gap:10px;padding:14px 18px;
  border-bottom:1px solid hsl(var(--border))}
.card-header .title{font-size:14px;font-weight:600;letter-spacing:-0.01em}
.card-header .desc{font-size:12px;color:hsl(var(--muted-foreground))}
.card-header .right{margin-left:auto;display:flex;align-items:center;gap:8px}

main{display:grid;grid-template-columns:1fr 400px;gap:16px}
@media(max-width:1120px){main{grid-template-columns:1fr}}
.col{display:flex;flex-direction:column;gap:16px}

/* ---------- 内嵌终端 ---------- */
#terminal{
  background:hsl(var(--term-bg));height:520px;overflow-y:auto;
  padding:12px 16px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  font-size:12.5px;line-height:1.6;
}
#terminal .ln{white-space:pre-wrap;word-break:break-all;color:hsl(var(--term-fg))}
#terminal .ln .ts{color:hsl(var(--term-dim))}
#terminal .ln .tag{color:hsl(var(--term-dim))}
#terminal .ln.warn,#terminal .ln.warn .ts,#terminal .ln.warn .tag{color:hsl(var(--term-warn))}
#terminal .ln.error,#terminal .ln.error .ts,#terminal .ln.error .tag{color:hsl(var(--term-err))}
#terminal .ln.sys .msg{color:#a78bfa}
#terminal::-webkit-scrollbar{width:8px}
#terminal::-webkit-scrollbar-thumb{background:hsl(var(--muted-foreground) / .35);border-radius:4px}
.tbar{display:flex;gap:8px;align-items:center;padding:10px 14px;border-bottom:1px solid hsl(var(--border))}
.tbar .cnt{margin-left:auto;color:hsl(var(--muted-foreground));font-size:12px;
  font-family:ui-monospace,Consolas,monospace}
.conn-dot{width:7px;height:7px;border-radius:99px;background:hsl(var(--success));
  animation:pulse 2.4s ease-in-out infinite}
.conn-dot.off{background:hsl(var(--warning));animation:none}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}

/* 最新数据点 */
.latest{display:flex;flex-wrap:wrap;gap:6px 18px;padding:10px 16px;
  border-top:1px solid hsl(var(--border));background:hsl(var(--muted) / .4);
  font-size:12px;color:hsl(var(--muted-foreground))}
.latest b{color:hsl(var(--foreground));font-weight:600;font-family:ui-monospace,Consolas,monospace}
.latest .pill{display:inline-flex;align-items:baseline;gap:5px}
.latest .u{font-size:11px}

/* ---------- KPI 统计 ---------- */
.kpis{display:grid;grid-template-columns:repeat(3,1fr)}
.kpi{padding:16px 18px;border-right:1px solid hsl(var(--border));
  border-bottom:1px solid hsl(var(--border))}
.kpis .kpi:nth-child(3n){border-right:none}
.kpis .kpi:nth-child(n+4){border-bottom:none}
.kpi .l{font-size:12px;color:hsl(var(--muted-foreground))}
.kpi .v{font-size:26px;font-weight:650;letter-spacing:-0.02em;
  font-family:ui-monospace,SFMono-Regular,Consolas,monospace;margin-top:2px}

/* ---------- 表格 ---------- */
table{width:100%;border-collapse:collapse;font-size:13px}
th{font-size:12px;font-weight:500;color:hsl(var(--muted-foreground));
  text-align:left;padding:8px 18px;border-bottom:1px solid hsl(var(--border))}
td{padding:9px 18px;border-bottom:1px solid hsl(var(--border))}
tr:last-child td{border-bottom:none}
tbody tr:hover{background:hsl(var(--muted) / .4)}
.tag{display:inline-flex;align-items:center;border-radius:999px;padding:2px 9px;
  font-size:11.5px;font-weight:500}
.tag.success{background:hsl(var(--success) / .12);color:hsl(var(--success-fg))}
.tag.warning{background:hsl(var(--warning) / .14);color:hsl(var(--warning-fg))}
.tag.destructive{background:hsl(var(--destructive) / .1);color:hsl(var(--destructive-fg))}
.tag.info{background:hsl(var(--chart-3) / .12);color:hsl(var(--chart-3))}
td.code{font-family:ui-monospace,Consolas,monospace;color:hsl(var(--muted-foreground));font-size:12px}
#q-row td{font-family:ui-monospace,Consolas,monospace;font-weight:600;font-size:15px}

/* ---------- 图表 ---------- */
.charts{display:grid;grid-template-columns:repeat(2,1fr);gap:16px;margin-top:16px}
@media(max-width:900px){.charts{grid-template-columns:1fr}}
.chart-box{background:hsl(var(--card));border:1px solid hsl(var(--border));
  border-radius:var(--radius);padding:16px 18px 12px}
.chart-box .head{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:8px}
.chart-box .head h3{font-size:13px;font-weight:500;color:hsl(var(--muted-foreground))}
.chart-box .head b{font-size:20px;font-weight:650;letter-spacing:-0.02em;
  font-family:ui-monospace,Consolas,monospace}
.chart-box .head .u{font-size:11px;color:hsl(var(--muted-foreground));font-weight:400;margin-left:3px}
canvas{width:100%;height:112px;display:block}
</style>
</head>
<body>
<header class="site">
  <div class="inner">
    <div class="logo">🌱</div>
    <h1>农田土壤参数移动采集系统</h1>
    <span class="sub">FSPMCS v1.0 · 嵌入式边缘采集终端</span>
    <span class="badge" id="b-dev">--</span>
    <span class="spacer"></span>
    <span class="badge" id="b-health"><span class="cdot"></span>健康 --</span>
    <span class="badge" id="b-net"><span class="cdot"></span>网络 --</span>
    <span class="badge secondary" id="b-state">--</span>
    <span class="badge" id="b-uptime">运行 --</span>
    <button class="btn ghost" id="btn-theme" title="切换主题">🌙</button>
  </div>
</header>

<div class="container">
  <main>
    <!-- 左列：内嵌终端 -->
    <div class="card">
      <div class="card-header">
        <span class="title">系统终端</span>
        <span class="desc">实时日志流 · Server-Sent Events</span>
        <div class="right">
          <button class="btn" id="btn-pause">暂停</button>
          <button class="btn" id="btn-clear">清空</button>
          <button class="btn active" id="btn-scroll">自动滚动</button>
        </div>
      </div>
      <div class="tbar">
        <span class="conn-dot" id="conn-dot"></span>
        <span class="cnt" id="t-count">0 行 · SSE 连接中</span>
      </div>
      <div id="terminal"></div>
      <div class="latest" id="latest">等待数据...</div>
    </div>

    <!-- 右列：状态 -->
    <div class="col">
      <div class="card">
        <div class="card-header">
          <span class="title">运行统计</span>
          <span class="desc">实时累计</span>
        </div>
        <div class="kpis">
          <div class="kpi"><div class="l">采集样本</div><div class="v" id="s-sample" style="color:hsl(var(--chart-1))">0</div></div>
          <div class="kpi"><div class="l">融合数据点</div><div class="v" id="s-fused" style="color:hsl(var(--chart-3))">0</div></div>
          <div class="kpi"><div class="l">MQTT 上传</div><div class="v" id="s-upload" style="color:hsl(var(--chart-2))">0</div></div>
          <div class="kpi"><div class="l">事务提交</div><div class="v" id="s-store">0</div></div>
          <div class="kpi"><div class="l">发送分片</div><div class="v" id="s-frag">0</div></div>
          <div class="kpi"><div class="l">恢复执行</div><div class="v" id="s-recover" style="color:hsl(var(--warning-fg))">0</div></div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <span class="title">传感器状态</span>
          <span class="desc">5 路</span>
        </div>
        <table>
          <thead><tr><th>传感器</th><th>型号</th><th style="text-align:right">状态</th></tr></thead>
          <tbody id="sensor-rows"></tbody>
        </table>
      </div>

      <div class="card">
        <div class="card-header">
          <span class="title">数据质量分布</span>
          <span class="desc">0~100 置信度分级</span>
        </div>
        <table>
          <thead><tr><th>优秀</th><th>良好</th><th>一般</th><th>差</th></tr></thead>
          <tbody><tr id="q-row"><td>0</td><td>0</td><td>0</td><td>0</td></tr></tbody>
        </table>
      </div>
    </div>
  </main>

  <!-- 趋势图表 -->
  <div class="charts">
    <div class="chart-box">
      <div class="head"><h3>土壤温度</h3><b><span id="v-temp">--</span><span class="u">°C</span></b></div>
      <canvas id="c-temp"></canvas>
    </div>
    <div class="chart-box">
      <div class="head"><h3>土壤体积含水率</h3><b><span id="v-mois">--</span><span class="u">%</span></b></div>
      <canvas id="c-mois"></canvas>
    </div>
    <div class="chart-box">
      <div class="head"><h3>土壤 pH</h3><b><span id="v-ph">--</span></b></div>
      <canvas id="c-ph"></canvas>
    </div>
    <div class="chart-box">
      <div class="head"><h3>电导率</h3><b><span id="v-ec">--</span><span class="u">uS/cm</span></b></div>
      <canvas id="c-ec"></canvas>
    </div>
  </div>
</div>

<script>
'use strict';
const $=id=>document.getElementById(id);
const term=$('terminal');
let paused=false,autoScroll=true,lineCount=0,chartData=[];

/* ---------- 主题切换（shadcn 亮/暗） ---------- */
const THEME_KEY='scs-theme';
function applyTheme(t){
  document.documentElement.classList.toggle('dark',t==='dark');
  $('btn-theme').textContent=t==='dark'?'☀️':'🌙';
  localStorage.setItem(THEME_KEY,t);
  if(chartData.length){requestAnimationFrame(drawAll);}
}
applyTheme(localStorage.getItem(THEME_KEY)||'light');
$('btn-theme').onclick=()=>{
  applyTheme(document.documentElement.classList.contains('dark')?'light':'dark');
};

/* ---------- 内嵌终端（SSE 实时流） ---------- */
function addLine(text,cls){
  if(paused)return;
  const d=document.createElement('div');
  d.className='ln '+(cls||'info');
  // 拆分 [时间][级别][模块] 前缀以便分层着色
  const m=text.match(/^(\[[^\]]+\])(\[[^\]]+\])(\[[^\]]+\])\s?(.*)$/);
  if(m){
    const msgSpan=document.createElement('span');
    msgSpan.className='msg';
    msgSpan.textContent=m[4];
    d.innerHTML='<span class="ts">'+m[1]+'</span><span class="tag">'+m[2]+m[3]+'</span> ';
    d.appendChild(msgSpan);
  }else{
    d.textContent=text;
  }
  term.appendChild(d);
  if(++lineCount>500){term.removeChild(term.firstChild);lineCount--;}
  if(autoScroll)term.scrollTop=term.scrollHeight;
  $('t-count').textContent=lineCount+' 行';
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
  es.onopen=()=>{$('conn-dot').classList.remove('off');
    $('t-count').textContent=lineCount+' 行 · 已连接';};
  es.onmessage=ev=>{
    try{
      const e=JSON.parse(ev.data);
      addLine(fmtEntry(e),levelClass(e.l));
    }catch(err){addLine(ev.data,'sys');}
  };
  es.onerror=()=>{$('conn-dot').classList.add('off');
    $('t-count').textContent=lineCount+' 行 · 重连中';};
})();

$('btn-pause').onclick=e=>{paused=!paused;
  e.target.textContent=paused?'继续':'暂停';
  e.target.classList.toggle('active',paused);};
$('btn-clear').onclick=()=>{term.innerHTML='';lineCount=0;
  $('t-count').textContent='0 行';};
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
    $('b-health').innerHTML='<span class="cdot"></span>健康 '+h;
    $('b-health').className='badge '+(h>=80?'success':h>=50?'warning':'destructive');
    $('b-net').innerHTML='<span class="cdot"></span>网络 '+(st.network.online?st.network.interface:'离线');
    $('b-net').className='badge '+(st.network.online?'success':'destructive');
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
      const c=s.status==='就绪'?'success':s.status==='休眠'?'warning':
              (s.status==='错误'||s.status==='故障')?'destructive':'info';
      return '<tr><td>'+s.name+'</td><td class="code">'+s.code+
             '</td><td style="text-align:right"><span class="tag '+c+'">'+s.status+'</span></td></tr>';
    }).join('');
    if(st.latest&&st.latest.batch!==undefined){
      $('latest').innerHTML=
        '<span class="pill">数据点 <b>#'+st.latest.batch+'</b></span>'+
        '<span class="pill">温度 <b>'+st.latest.temp.toFixed(2)+'</b><span class="u">°C</span></span>'+
        '<span class="pill">含水率 <b>'+st.latest.moisture.toFixed(2)+'</b><span class="u">%</span></span>'+
        '<span class="pill">pH <b>'+st.latest.ph.toFixed(2)+'</b></span>'+
        '<span class="pill">EC <b>'+st.latest.ec.toFixed(1)+'</b><span class="u">uS/cm</span></span>'+
        '<span class="pill">质量 <b>'+st.latest.quality+'</b><span class="u">/100</span></span>';
    }else{
      $('latest').textContent='等待数据...';
    }
  }catch(err){}
}

/* ---------- 趋势图表（颜色跟随主题令牌） ---------- */
function chartPalette(){
  const cs=getComputedStyle(document.documentElement);
  const raw=v=>cs.getPropertyValue(v).trim();          // 原始 HSL 分量 "221 83% 53%"
  const hsl=(v,a)=>a===undefined?'hsl('+raw(v)+')':'hsl('+raw(v)+' / '+a+')';
  return {border:hsl('--border'),grid:hsl('--border'),
    c1:raw('--chart-1'),c2:raw('--chart-2'),c3:raw('--chart-3'),c4:raw('--chart-4'),
    c:(t,a)=>hsl(t,a), text:hsl('--muted-foreground')};
}
function drawOne(id,key,color,pal){
  const cv=$(id),dpr=window.devicePixelRatio||1;
  const W=cv.clientWidth,H=cv.clientHeight;
  cv.width=W*dpr;cv.height=H*dpr;
  const ctx=cv.getContext('2d');ctx.scale(dpr,dpr);
  ctx.clearRect(0,0,W,H);
  const data=chartData;
  if(data.length<2){
    ctx.fillStyle=pal.text;ctx.font='12px sans-serif';
    ctx.fillText('等待数据...',10,H/2);return;
  }
  const vals=data.map(p=>p[key]);
  let mn=Math.min(...vals),mx=Math.max(...vals);
  if(mx-mn<1e-9){mx+=1;mn-=1;}
  const pad=(mx-mn)*0.18;mn-=pad;mx+=pad;
  ctx.strokeStyle=pal.grid;ctx.lineWidth=1;
  for(let i=1;i<=3;i++){const y=H*i/4;
    ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();}
  // 折线 + 曲线下方渐变填充
  const pts=vals.map((v,i)=>[
    i/(vals.length-1)*(W-6)+3,
    H-5-(v-mn)/(mx-mn)*(H-12)
  ]);
  ctx.beginPath();
  pts.forEach((p,i)=>{i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]);});
  ctx.lineTo(pts[pts.length-1][0],H);ctx.lineTo(pts[0][0],H);ctx.closePath();
  const stroke=pal.c(color), fillStroke=pal.c(color,.32);
  const grad=ctx.createLinearGradient(0,0,0,H);
  grad.addColorStop(0,fillStroke);grad.addColorStop(1,pal.c(color,.03));
  ctx.fillStyle=grad;ctx.fill();
  ctx.beginPath();
  pts.forEach((p,i)=>{i?ctx.lineTo(p[0],p[1]):ctx.moveTo(p[0],p[1]);});
  ctx.strokeStyle=stroke;ctx.lineWidth=1.8;ctx.lineJoin='round';ctx.stroke();
  // 末点标记
  const lastPt=pts[pts.length-1];
  ctx.fillStyle=stroke;ctx.beginPath();ctx.arc(lastPt[0],lastPt[1],3.2,0,7);ctx.fill();
  ctx.fillStyle='#fff';ctx.beginPath();ctx.arc(lastPt[0],lastPt[1],1.4,0,7);ctx.fill();
}
function drawAll(){
  const pal=chartPalette();
  drawOne('c-temp','temp','--chart-1',pal);
  drawOne('c-mois','moisture','--chart-2',pal);
  drawOne('c-ph','ph','--chart-3',pal);
  drawOne('c-ec','ec','--chart-4',pal);
}
async function pollData(){
  try{
    const d=await (await fetch('/api/data?count=60')).json();
    if(d.length){chartData=d;drawAll();
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
window.addEventListener('resize',()=>{if(chartData.length)drawAll();});
</script>
</body>
</html>)html";

}  // namespace soil
