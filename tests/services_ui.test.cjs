const fs = require('fs');
const path = require('path');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '..');
const version = fs.readFileSync(path.join(root,'VERSION'),'utf8').trim();
const html = fs.readFileSync(path.join(root,'web/index.html'),'utf8').replaceAll('__FIRMWARE_VERSION__',version);
const scripts = {
  '/assets/folder-actions.js':fs.readFileSync(path.join(root,'web/folder_actions.js'),'utf8'),
  '/assets/services.js':fs.readFileSync(path.join(root,'web/services.js'),'utf8')
};
const services = {
  revision:0,boot_id:1,sample_ms:0,always_on:{wifi:true,http:true,stream:true,snapshot:true},
  ftp:{enabled:true,state:'enabled',clients:0,transferring:false},
  web_sd:{enabled:true,state:'enabled',requests:0,jobs:0},
  ota:{enabled:true,state:'enabled',updating:false},
  panel_refresh:{enabled:true,state:'enabled',interval_seconds:2}
};
const profile={resolution:8,quality:16,brightness:0,contrast:0,saturation:0,vflip:false,mirror:false};
const state={
  version,hostname:'webcam',services,uptime_seconds:40,heap_free:150000,heap_min:140000,heap_total:300000,heap_used_pct:50,psram_free:1000000,chip_temperature_c:45,error:'',job:'idle',ota:false,
  wifi:{connected:true,ip:'127.0.0.1',signal_pct:70,rssi_dbm:-55,channel:6},
  ftp:{enabled:true,requested:true,port:21,sessions:0,transferring:false},
  camera:{ready:true,width:640,height:480,sent:10,jpeg_bytes:30000,capture_ms:30,send_ms:20,errors:0,dropped:0,profile},
  led:{enabled:false,effective_on:false,intensity_pct:25,auto_flash:false},
  sd:{mounted:true,present:true,maintenance:false,download:false,total_bytes:20000000,free_bytes:10000000,job:{id:0,phase:'idle'}},
  timelapse:{state:'idle',saved:0,missed:0,bytes:0,interval_seconds:10,elapsed_seconds:0,next_seconds:0,session:'',profile}
};
function derive(){
  services.sample_ms=Date.now();
  services.ftp.state=services.ftp.enabled?(services.ftp.clients?'busy':'enabled'):(services.ftp.clients?'stopping':'disabled');
  services.web_sd.state=services.web_sd.enabled?(services.web_sd.requests+services.web_sd.jobs?'busy':'enabled'):(services.web_sd.requests+services.web_sd.jobs?'stopping':'disabled');
  services.ota.state=services.ota.enabled?(services.ota.updating?'busy':'enabled'):(services.ota.updating?'stopping':'disabled');
  services.panel_refresh.state=services.panel_refresh.enabled?'enabled':'disabled';
  state.ftp.enabled=services.ftp.enabled;
  state.ftp.requested=services.ftp.enabled;
  state.ftp.sessions=services.ftp.clients;
  state.ftp.transferring=services.ftp.transferring;
}
(async()=>{
  let browser;
  try{
    try{browser=await chromium.launch({headless:true});}
    catch(_){browser=await chromium.launch({headless:true,channel:'msedge'});}
    const page=await browser.newPage();
    const errors=[];
    const requests=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.route('http://services.test/**',async route=>{
      const request=route.request();
      const url=new URL(request.url());
      const body=request.method()==='POST'?JSON.parse(request.postData()||'{}'):null;
      requests.push({path:url.pathname,method:request.method(),body});
      if(scripts[url.pathname])return route.fulfill({contentType:'application/javascript',body:scripts[url.pathname]});
      if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
      if(url.pathname==='/api/services'){
        if(body){
          const known=['ftp','web_sd','ota','panel_refresh','refresh_seconds'];
          if(Object.keys(body).some(key=>!known.includes(key)))return route.fulfill({status:400,json:{message:'Serviço protegido.'}});
          for(const key of ['ftp','web_sd','ota','panel_refresh'])if(key in body)services[key].enabled=body[key];
          if('refresh_seconds' in body)services.panel_refresh.interval_seconds=body.refresh_seconds;
          ++services.revision;
        }
        derive();
        return route.fulfill({json:{services,saved:Boolean(body),message:'Preferências salvas.'}});
      }
      if(url.pathname==='/api/status'){derive();return route.fulfill({json:state});}
      if(url.pathname.startsWith('/api/sd/')){
        if(!services.web_sd.enabled)return route.fulfill({status:503,json:{code:'service_disabled',service:'web_sd',message:'Acesso web ao SD desativado.'}});
        return route.fulfill({json:{path:'/',items:[],next_offset:null}});
      }
      if(url.pathname==='/api/update')return route.fulfill({status:services.ota.enabled?200:503,json:{message:services.ota.enabled?'aceito':'OTA desativado.'}});
      if(url.pathname==='/snapshot'||url.pathname==='/stream')return route.fulfill({status:200,contentType:'image/jpeg',body:Buffer.from([255,216,255,217])});
      return route.fulfill({json:{message:'ok'}});
    });
    await page.goto('http://services.test/');
    await page.waitForFunction(()=>!document.getElementById('service-ftp').disabled);
    if(await page.locator('#services input[role=switch]').count()!==4)throw new Error('Número de chaves incorreto');
    if(await page.locator('#service-wifi,#service-stream,#service-snapshot').count())throw new Error('Chaves críticas indevidas');
    await page.uncheck('#service-panel-refresh');
    await page.waitForFunction(()=>!document.getElementById('service-panel-refresh').disabled&&!document.getElementById('service-panel-refresh').checked);
    let count=requests.filter(item=>item.path==='/api/status').length;
    await page.waitForTimeout(2300);
    if(requests.filter(item=>item.path==='/api/status').length!==count)throw new Error('Polling não parou');
    await page.click('#services-refresh');
    await page.waitForTimeout(200);
    if(requests.filter(item=>item.path==='/api/status').length<=count)throw new Error('Leitura manual bloqueada');
    await page.selectOption('#service-interval','5');
    await page.waitForFunction(()=>!document.getElementById('service-interval').disabled&&document.getElementById('service-interval').value==='5');
    await page.reload();
    await page.waitForFunction(()=>!document.getElementById('service-panel-refresh').disabled);
    if(await page.isChecked('#service-panel-refresh'))throw new Error('Preferência pausada não restaurada');
    if(await page.inputValue('#service-interval')!=='5')throw new Error('Intervalo não restaurado');
    services.ftp.clients=1;
    services.ftp.transferring=true;
    await page.click('#services-refresh');
    await page.waitForFunction(()=>document.getElementById('state-service-ftp').textContent==='Em uso');
    await page.uncheck('#service-ftp');
    await page.waitForFunction(()=>document.getElementById('state-service-ftp').textContent.includes('Desativando'));
    if(!services.ftp.transferring)throw new Error('Transferência em andamento foi cortada');
    services.ftp.clients=0;
    services.ftp.transferring=false;
    await page.waitForFunction(()=>document.getElementById('state-service-ftp').textContent==='Desligado');
    if(requests.some(item=>item.path.includes('cancel')))throw new Error('Desligar serviço enviou cancelamento');
    const stale=JSON.parse(JSON.stringify(services));
    stale.sample_ms=0;
    stale.ftp.enabled=true;
    stale.ftp.state='enabled';
    await page.evaluate(value=>ServiceControls.render(value),stale);
    if(await page.isChecked('#service-ftp'))throw new Error('Resposta atrasada sobrescreveu configuração');
    state.timelapse.state='recording';
    await page.uncheck('#service-web-sd');
    await page.waitForFunction(()=>document.getElementById('state-service-web-sd').textContent==='Desligado');
    if(state.timelapse.state!=='recording')throw new Error('Timelapse interrompido pela chave SD');
    if(await page.locator('#tl-stop').isDisabled())throw new Error('Controle do timelapse bloqueado');
    if(await page.locator('#video-toggle').isDisabled())throw new Error('Vídeo bloqueado');
    if(await page.getAttribute('#snapshot','href')!=='/snapshot')throw new Error('Snapshot alterado');
    const statuses=await page.evaluate(async()=>Promise.all(['/stream','/snapshot','/api/sd/list'].map(async url=>(await fetch(url)).status)));
    if(statuses.join(',')!=='200,200,503')throw new Error('Serviços críticos não ficaram independentes');
    await page.uncheck('#service-ota');
    await page.waitForFunction(()=>document.getElementById('ota-submit').disabled);
    await page.check('#service-web-sd');
    await page.waitForFunction(()=>!document.getElementById('service-web-sd').disabled);
    state.timelapse.state='idle';
    services.web_sd.jobs=1;
    await page.uncheck('#service-web-sd');
    await page.waitForFunction(()=>document.getElementById('state-service-web-sd').textContent.includes('Desativando'));
    services.web_sd.jobs=0;
    await page.waitForFunction(()=>document.getElementById('state-service-web-sd').textContent==='Desligado');
    await page.check('#service-web-sd');
    await page.waitForFunction(()=>document.getElementById('state-service-web-sd').textContent==='Ligado');
    if(await page.locator('#sd-refresh').isDisabled())throw new Error('Acesso SD não reativou');
    await page.check('#service-ota');
    await page.waitForFunction(()=>!document.getElementById('ota-submit').disabled);
    await page.selectOption('#service-interval','2');
    await page.waitForFunction(()=>!document.getElementById('service-panel-refresh').disabled);
    await page.check('#service-panel-refresh');
    await page.waitForFunction(()=>!document.getElementById('service-panel-refresh').disabled);
    count=requests.filter(item=>item.path==='/api/status').length;
    await page.waitForTimeout(2300);
    if(requests.filter(item=>item.path==='/api/status').length<=count)throw new Error('Polling não retomou');
    const invalid=await page.evaluate(async()=>fetch('/api/services',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({wifi:false})}).then(response=>response.status));
    if(invalid!==400||!services.always_on.wifi)throw new Error('Wi-Fi não ficou protegido');
    if(errors.length)throw new Error(errors.join('\n'));
    console.log(JSON.stringify({ok:true,checks:['quatro chaves','preferências restauradas','pausa e retomada do polling','leitura manual','FTP finaliza antes de desligar','ignora resposta atrasada','web SD independente do timelapse','OTA bloqueada quando desligada','operações SD drenadas','Wi-Fi e rotas críticas protegidos'],pageErrors:errors}));
  }finally{if(browser)await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
