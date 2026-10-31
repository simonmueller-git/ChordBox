// web_page.h — the phone / iPad page, served at / (no outside files: it must
// work on ChordBox's own hotspot with no internet)
#pragma once

static const char INDEX_HTML[] = R"CHORDBOXPAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="ChordBox">
<meta name="theme-color" content="#0B0C0E">
<title>ChordBox</title>
<style>
:root{--bg:#0B0C0E;--panel:#16171A;--line:#2A2C31;--dim:#5F626A;--mid:#A2A4AA;--ink:#ECEBE7;
--blue:#5A9BFF;--green:#35D184;--yellow:#FFD23F;--orange:#FF7A33;--red:#FF5A45;
--mono:ui-monospace,Menlo,monospace;--sans:-apple-system,BlinkMacSystemFont,"Helvetica Neue",sans-serif}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.35 var(--sans);-webkit-user-select:none;user-select:none;touch-action:manipulation}
body{padding:calc(env(safe-area-inset-top) + 8px) 12px calc(env(safe-area-inset-bottom) + 24px)}
.wrap{max-width:720px;margin:0 auto;display:grid;grid-template-columns:minmax(0,1fr);gap:12px}
.bar{display:flex;align-items:center;gap:8px;font:600 13px var(--mono);flex-wrap:wrap}
.pill{padding:5px 10px;border-radius:5px;color:#0B0C0E;font:700 12px var(--mono);border:0}
.dot{width:9px;height:9px;border-radius:50%;background:var(--red);display:inline-block}
.dot.on{background:var(--green)}
.grow{flex:1}
.card{min-width:0;background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px}
.hero{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;align-items:end}
.sym{font:700 56px/0.95 var(--sans);letter-spacing:-1px}
.sym small{font-size:26px;font-weight:600}
.rn{color:var(--green);font:600 16px var(--mono)}
.tag{color:var(--dim);font:12px var(--mono);text-transform:uppercase}
.badges{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}
.badge{font:700 12px var(--mono);padding:4px 7px;border-radius:4px;color:#0B0C0E}
.kb{display:flex;height:34px;margin-top:8px;position:relative}
.kb i{flex:1;border:1px solid #0B0C0E;background:#C9CBD0;border-radius:0 0 2px 2px}
.kb i.on{background:var(--orange)}
.kb b{position:absolute;top:0;width:4.2%;height:60%;background:#0B0C0E;border-radius:0 0 2px 2px}
.kb b.on{background:var(--orange)}
.bassline{display:flex;gap:8px;align-items:baseline;margin-top:8px;font:13px var(--mono);color:var(--mid)}
.bassline b{color:var(--orange);font-size:15px}
.row{display:grid;grid-template-columns:minmax(0,2fr) minmax(0,1fr) minmax(0,1fr);gap:8px}
button{min-width:0;font:700 15px var(--sans);color:var(--ink);background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 8px;min-height:54px}
button:active{filter:brightness(1.4)}
button.play{background:var(--orange);color:#0B0C0E;border:0;font-size:18px}
button.play.stopped{background:var(--panel);color:var(--orange);border:1px solid var(--orange)}
button.stop{color:var(--red);border-color:#5a2a25}
button.on{background:var(--yellow);color:#0B0C0E;border:0}
.gridhead{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.gridhead button{min-height:40px;padding:6px 14px}
.gridhead .where{flex:1;text-align:center;font:600 13px var(--mono);text-transform:uppercase}
.pads{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px}
.pad{aspect-ratio:1/0.82;background:#1E2024;border-radius:8px;padding:6px;display:flex;flex-direction:column;justify-content:space-between;position:relative;overflow:hidden;border:1px solid transparent}
.pad .r{font:600 11px var(--mono);color:var(--dim)}
.pad .n{font:700 22px var(--sans);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.pad .n small{font-size:13px;font-weight:600;color:var(--mid)}
.pad.snd{background:var(--ink);color:#0B0C0E}.pad.snd .n small,.pad.snd .r{color:#0B0C0E}
.pad.last{box-shadow:inset 0 -3px 0 var(--orange)}
.pad.empty{background:transparent;border:1px solid var(--line)}
.pad.held{border-color:var(--blue)}
.pad .bn{font:700 11px var(--mono);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pad .gl{display:grid;grid-template-columns:repeat(4,5px);gap:2px;align-self:flex-end}
.pad .gl i{width:5px;height:5px;background:#3A3C42;border-radius:1px}
.pad .gl i.o{background:var(--ink)}.pad .gl i.a{background:var(--red)}
.pad.playing{background:#3A220F;border-color:var(--orange)}
.pad.queued{border-color:var(--orange);border-style:dashed}
.sec{font:700 12px var(--mono);letter-spacing:.12em;text-transform:uppercase;color:var(--mid);margin:2px 0 8px}
.ctl{display:grid;grid-template-columns:72px minmax(0,1fr) 56px;align-items:center;gap:8px;margin:10px 0}
.ctl label{font:600 12px var(--mono);color:var(--mid)}
.ctl output{font:600 13px var(--mono);text-align:right}
input[type=range]{width:100%;accent-color:var(--orange);height:28px}
.seg{display:flex;gap:4px;flex-wrap:wrap;margin:6px 0 12px}
.seg button{min-height:38px;padding:6px 10px;font:600 12px var(--mono);border-radius:6px}
.seg button.sel{background:var(--orange);color:#0B0C0E;border-color:var(--orange)}
.two{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:8px}
.scenes{display:grid;grid-template-columns:repeat(8,minmax(0,1fr));gap:5px;margin-top:8px}
.scenes button{min-height:48px;padding:0;font:700 16px var(--mono)}
.scenes button.used{border-color:var(--mid)}
.scenes button.sel{box-shadow:0 0 0 2px var(--orange)}
.hint{color:var(--dim);font-size:12px;margin:6px 0 0}
details summary{font:700 12px var(--mono);letter-spacing:.12em;text-transform:uppercase;color:var(--mid);cursor:pointer}
select,input[type=password],input[type=text]{width:100%;font:16px var(--sans);padding:10px;border-radius:8px;border:1px solid var(--line);background:#0B0C0E;color:var(--ink);margin-top:8px;-webkit-user-select:text;user-select:text}
.toast{position:fixed;left:50%;top:calc(env(safe-area-inset-top) + 10px);transform:translateX(-50%);background:var(--ink);color:#0B0C0E;font:700 14px var(--mono);padding:8px 14px;border-radius:8px;opacity:0;transition:opacity .2s;pointer-events:none;z-index:9}
.toast.show{opacity:1}
.off{position:fixed;inset:0;background:rgba(11,12,14,.85);display:none;align-items:center;justify-content:center;font:700 16px var(--mono);z-index:8}
.off.show{display:flex}
</style></head><body>
<div class="toast" id="toast"></div>
<div class="off" id="off">connecting to ChordBox…</div>
<div class="wrap">
  <div class="bar">
    <button class="pill" id="scr" style="background:var(--blue)">CHORD</button>
    <span id="key" style="color:var(--blue)">F MIN</span>
    <span class="grow"></span>
    <span id="clk" style="color:var(--dim)">INT</span>
    <span id="bpm">124</span>
    <span class="dot" id="link"></span>
  </div>

  <div class="card">
    <div class="hero">
      <div><div class="sym" id="sym">ChordBox</div><div class="rn" id="rn"></div><div class="tag" id="tag">play a pad</div></div>
      <div class="badges" id="badges" style="justify-content:flex-end;max-width:160px"></div>
    </div>
    <div class="kb" id="kb"></div>
    <div class="bassline"><span>BASS</span><b id="bname">–</b><span id="bfollow"></span></div>
  </div>

  <div class="row">
    <button class="play stopped" id="bplay">▶ BASS</button>
    <button id="latch">LATCH</button>
    <button class="stop" id="stopall">STOP ALL</button>
  </div>

  <div class="card">
    <div class="gridhead">
      <button id="prev">◀</button><div class="where" id="where">–</div><button id="next">▶</button>
    </div>
    <div class="pads" id="pads"></div>
    <div class="two" style="margin-top:8px">
      <button id="roll">ROLL (hold)</button>
      <button id="star">★ STAR</button>
    </div>
    <p class="hint" id="padhint">Hold a chord to sustain it. Hold a chord and ROLL to repeat it on the beat.</p>
  </div>

  <div class="card">
    <div class="sec">Feel</div>
    <div id="sliders"></div>
    <div class="sec">Voicing</div><div class="seg" data-p="VOIC" data-o="CLOSE,OPEN,SHELL,ST9"></div>
    <div class="sec">Arpeggio</div><div class="seg" data-p="ARP" data-o="OFF,UP,DOWN,UP-DN,RANDOM,CHORD"></div>
    <div class="sec">Arp rate</div><div class="seg" data-p="ARATE" data-o="1/4,1/8,1/8T,1/16,1/16T,1/32"></div>
    <div class="sec">Rhythm (stabs: arp CHORD)</div><div class="seg" data-p="RHY" data-o="ALL,8THS,OFFBEAT,HOUSE,3-3-2,DUB,RANDOM"></div>
    <div class="sec">Echo</div><div class="seg" data-p="ECHO" data-o="OFF,1/8,3/16,1/4"></div>
    <div class="sec">Roll rate</div><div class="seg" data-p="RPT" data-o="1/4,1/8,1/8T,1/16,1/16T,1/32"></div>
    <div class="sec">Bass octave</div><div class="seg" data-p="BOCT" data-b="1" data-lo="-2" data-o="-2,-1,0,+1,+2"></div>
    <div class="sec">Bass length</div><div class="seg" data-p="BLEN" data-b="1" data-o="1 BAR,2 BARS,4 BARS"></div>
  </div>

  <div class="card">
    <div class="sec">Save</div>
    <div class="two">
      <button id="starrow">★ Chord row</button>
      <button id="starbass">★ Bass line</button>
      <button id="loop">REC loop</button>
      <button id="undo">UNDO</button>
    </div>
    <div class="scenes" id="scenes"></div>
    <p class="hint">Scenes: tap to load, hold to save everything as it is now.</p>
  </div>

  <details class="card">
    <summary>Wi-Fi and phone setup</summary>
    <p class="hint" id="wifistat"></p>
    <select id="nets"><option value="">choose a network…</option></select>
    <input type="text" id="ssid" placeholder="or type its name" autocapitalize="off" autocorrect="off">
    <input type="password" id="pass" placeholder="password">
    <button style="width:100%;margin-top:8px" id="savewifi">Save and restart ChordBox</button>
    <p class="hint">At home ChordBox joins this network: open <b>chordbox.local</b>. Away from it, ChordBox makes its own Wi-Fi called <b>ChordBox</b> (default password in the README): join it and open <b>192.168.4.1</b>. In Safari, Share → Add to Home Screen makes this an app.</p>
  </details>
</div>
<script>
const $=id=>document.getElementById(id);
let ws,S=null,dragging=null,heldPads=new Set(),rollOn=false;
const SL=[["STRUM","set","STRM",-100,100],["HUMAN","set","HUM",0,100],["SOFT","set","SOFT",0,100],
["VEL RAND","set","VRND",0,100],["SWING","bset","SWING",50,75],["DENSITY","bset","DENS",-100,100],["TEMPO","bset","TEMPO",40,240]];
function send(s){if(ws&&ws.readyState===1)ws.send(s)}
function connect(){
  ws=new WebSocket(`ws://${location.host}/ws`);
  ws.onopen=()=>{$('link').classList.add('on');$('off').classList.remove('show')};
  ws.onclose=()=>{$('link').classList.remove('on');$('off').classList.add('show');setTimeout(connect,1200)};
  ws.onmessage=e=>{try{S=JSON.parse(e.data);render()}catch(x){}};
}
// ---- building blocks
const sl=$('sliders');
for(const [lab,cmd,p,lo,hi] of SL){
  const d=document.createElement('div');d.className='ctl';
  d.innerHTML=`<label>${lab}</label><input type="range" min="${lo}" max="${hi}" step="1" data-p="${p}"><output></output>`;
  const r=d.querySelector('input'),o=d.querySelector('output');let t=0;
  r.addEventListener('input',()=>{dragging=p;o.textContent=fmt(p,+r.value);const n=Date.now();if(n-t>40){t=n;send(`${cmd} ${p} ${r.value}`)}});
  r.addEventListener('change',()=>{send(`${cmd} ${p} ${r.value}`);setTimeout(()=>dragging=null,300)});
  sl.appendChild(d);
}
function fmt(p,v){if(p=='STRM')return v==0?'OFF':(v>0?'UP ':'DN ')+Math.abs(v);if(p=='DENS')return v==0?'AS IS':(v>0?'+':'')+v;
 if(p=='TEMPO')return v;return v+'%'}
const BNAME={BOCT:'OCT',BLEN:'LEN'};
document.querySelectorAll('.seg').forEach(g=>{const lo=+(g.dataset.lo||0);g.dataset.o.split(',').forEach((o,i)=>{const b=document.createElement('button');b.textContent=o;
 b.onclick=()=>send(g.dataset.b?`bset ${BNAME[g.dataset.p]} ${i+lo}`:`set ${g.dataset.p} ${i}`);g.appendChild(b)})});
const padsEl=$('pads');
for(let i=0;i<16;i++){const d=document.createElement('div');d.className='pad';padsEl.appendChild(d);
 const down=e=>{e.preventDefault();if(heldPads.has(i))return;heldPads.add(i);d.classList.add('held');send(`pad ${i} 1`)};
 const up=e=>{e.preventDefault();if(!heldPads.has(i))return;heldPads.delete(i);d.classList.remove('held');send(`pad ${i} 0`);
   if(!heldPads.size&&rollOn){rollOn=false;send('act key0 0')}};
 d.addEventListener('touchstart',down,{passive:false});d.addEventListener('touchend',up,{passive:false});d.addEventListener('touchcancel',up,{passive:false});
 d.addEventListener('mousedown',down);d.addEventListener('mouseup',up);d.addEventListener('mouseleave',up);}
const sc=$('scenes');
for(let i=0;i<8;i++){const b=document.createElement('button');b.textContent=i+1;let t=null,saved=false;
 const start=e=>{e.preventDefault();saved=false;t=setTimeout(()=>{saved=true;send(`act storescene ${i}`)},700)};
 const end=e=>{e.preventDefault();clearTimeout(t);if(!saved)send(`act scene ${i}`)};
 b.addEventListener('touchstart',start,{passive:false});b.addEventListener('touchend',end,{passive:false});
 b.addEventListener('mousedown',start);b.addEventListener('mouseup',end);sc.appendChild(b)}
// keyboard: two octaves from the lowest note's C
const kb=$('kb');
function drawKb(notes){kb.innerHTML='';if(!notes.length){return}const base=Math.floor(Math.min(...notes)/12)*12,on=new Set(notes);
 const whites=[0,2,4,5,7,9,11],blacks={1:0,3:1,6:3,8:4,10:5};let w=0;
 for(let o=0;o<2;o++)for(const s of whites){const i=document.createElement('i');if(on.has(base+o*12+s))i.className='on';kb.appendChild(i);w++}
 for(let o=0;o<2;o++)for(const s in blacks){const b=document.createElement('b');const idx=o*7+blacks[s];b.style.left=`calc(${(idx+1)*100/14}% - 2.1%)`;
  if(on.has(base+o*12+ +s))b.className='on';kb.appendChild(b)}}
// ---- buttons
const tap=(id,cmd)=>$(id).onclick=()=>send(cmd);
tap('bplay','act play');tap('latch','act latch');tap('stopall','act stopall');tap('prev','turn -1');tap('next','turn 1');
tap('starrow','act star');tap('starbass','act bstar');tap('undo','act undo');tap('loop','act loop');
$('scr').onclick=()=>send('click');
$('star').onclick=()=>send(S&&S.mode==1?'act bstar':'act star');
const roll=$('roll');
roll.addEventListener('touchstart',e=>{e.preventDefault();if(!heldPads.size){toast('hold a chord first');return}rollOn=true;send('act key0 1')},{passive:false});
roll.addEventListener('touchend',e=>{e.preventDefault();if(rollOn){rollOn=false;send('act key0 0')}},{passive:false});
let toastT=null,lastToast='';
function toast(s){const t=$('toast');t.textContent=s;t.classList.add('show');clearTimeout(toastT);toastT=setTimeout(()=>t.classList.remove('show'),1500)}
// ---- wifi
async function scan(){try{const r=await fetch('/scan');const l=await r.json();const s=$('nets');const cur=s.value;s.innerHTML='<option value="">choose a network…</option>';
 for(const n of [...new Set(l)])if(n){const o=document.createElement('option');o.textContent=n;s.appendChild(o)}s.value=cur}catch(x){}}
document.querySelector('details').addEventListener('toggle',scan);
$('savewifi').onclick=async()=>{const ssid=$('ssid').value||$('nets').value;if(!ssid){toast('choose a network');return}
 const b=new URLSearchParams({ssid,pass:$('pass').value});const r=await fetch('/wifi',{method:'POST',body:b});toast(await r.text())};
// ---- render
const COL=['var(--ink)','var(--orange)'];
function render(){
  const bassScr=S.mode==1||S.mode==2;
  $('scr').textContent=['CHORD','BASS','EDIT','MORE','SETUP'][S.mode]||'CHORD';
  $('scr').style.background=bassScr?'var(--orange)':'var(--blue)';
  $('key').textContent=`${S.key} ${S.scale}`;
  $('clk').textContent=S.ext?'MOVE':'INT';$('clk').style.color=S.ext?'var(--green)':'var(--dim)';
  $('bpm').textContent=Math.round(S.bpm);
  const c=S.chord,b=S.bass;
  if(c.played){const r=c.sym[1]=='b'?2:1;$('sym').innerHTML=`${c.sym.slice(0,r)}<small>${c.sym.slice(r)}</small>`}
  $('rn').textContent=c.rn;$('tag').textContent=c.tag||'play a pad';drawKb(c.notes);
  const B=[];if(c.latch)B.push(['LATCH','var(--yellow)']);if(c.loop==1)B.push([`REC ${c.loopN}/8`,'var(--red)']);
  if(c.loop==2)B.push([`LOOP ${c.loopPos}/${c.loopN}`,'var(--green)']);if(c.rpt)B.push(['ROLL','var(--red)']);
  else if(c.motion)B.push(['ARP '+c.motion,'var(--orange)']);if(c.echo)B.push(['ECHO','var(--mid)']);
  $('badges').innerHTML=B.map(([t,col])=>`<span class="badge" style="background:${col}">${t}</span>`).join('');
  $('bname').textContent=b.name||'–';$('bfollow').textContent=b.chord?`→ ${b.chord}${b.note?'  '+b.note:''}`:'';
  const playing=S.run&&!b.muted;$('bplay').textContent=playing?'■ BASS':'▶ BASS';$('bplay').classList.toggle('stopped',!playing);
  $('latch').classList.toggle('on',!!c.latch);$('loop').classList.toggle('on',c.loop>0);
  $('loop').textContent=c.loop==0?'REC loop':c.loop==1?'■ play loop':'■ stop loop';
  $('where').textContent=bassScr?`${b.bank} ${b.page+1}/${b.pages}${b.gen?' · gen':''}`:`${c.bank} ${c.page+1}/${c.pages}`;
  $('padhint').textContent=bassScr?'Tap a bass line to play it (next bar while the Move runs). ★ STAR keeps the playing line.':
    'Hold a chord to sustain it. Hold a chord and ROLL to repeat it on the beat.';
  const cells=padsEl.children;
  for(let i=0;i<16;i++){const d=cells[i],held=heldPads.has(i);
    if(bassScr){const p=b.pads[i];if(!(p[1]&1)){d.className='pad empty';d.innerHTML='';continue}
      d.className='pad'+(b.play==i?' playing':'')+(b.queued==i?' queued':'')+(held?' held':'');
      let g='';for(let s=0;s<16;s++)g+=`<i class="${p[3]>>s&1?'a':p[2]>>s&1?'o':''}"></i>`;
      d.innerHTML=`<div class="bn">${p[1]&4?'★ ':''}${p[0]}</div><div class="gl">${g}</div>`;
    }else{const p=c.pads[i];if(!(p[3]&1)){d.className='pad empty';d.innerHTML='';continue}
      d.className='pad'+(p[3]&2?' snd':'')+(p[3]&4?' last':'')+(held?' held':'');
      d.innerHTML=`<div class="r">${p[2]}${c.fav[i>>2]&&(i&3)==3?' ★':''}</div><div class="n">${p[0]}<small>${p[1]}</small></div>`}}
  for(const inp of sl.querySelectorAll('input')){const p=inp.dataset.p;if(dragging==p)continue;inp.value=S.p[p];inp.nextElementSibling.textContent=fmt(p,S.p[p])}
  document.querySelectorAll('.seg').forEach(g=>{const v=S.p[g.dataset.p]-(+(g.dataset.lo||0));[...g.children].forEach((x,i)=>x.classList.toggle('sel',i==v))});
  [...sc.children].forEach((x,i)=>{x.classList.toggle('used',!!(S.scenes>>i&1));x.classList.toggle('sel',i==S.scene)});
  $('wifistat').textContent=`ChordBox is on ${S.ssid||'its own hotspot'} at http://${S.wifi}`;
  if(S.toast&&S.toast!=lastToast)toast(S.toast);lastToast=S.toast;
}
$('off').classList.add('show');connect();
</script>
</body></html>
)CHORDBOXPAGE";
