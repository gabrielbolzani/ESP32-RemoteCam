const fs = require('fs');
const path = require('path');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '..');
const version = fs.readFileSync(path.join(root, 'VERSION'), 'utf8').trim();
const html = fs.readFileSync(path.join(root, 'web/index.html'), 'utf8').replaceAll('__FIRMWARE_VERSION__', version);
const gallery = fs.readFileSync(path.join(root, 'web/gallery.html'), 'utf8').replaceAll('__FIRMWARE_VERSION__', version);
const jpeg = Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDiqKKK9Y8M/9k=', 'base64');
const servicesScript = fs.readFileSync(path.join(root,'web/services.js'),'utf8');
const folderScript = fs.readFileSync(path.join(root, 'web/folder_actions.js'), 'utf8');
const emptyZip = Buffer.from('UEsFBgAAAAAAAAAAAAAAAAAAAAAAAA==', 'base64');
const profile = {resolution:8, quality:16, brightness:0, contrast:0, saturation:0, vflip:false, mirror:false};
const state = {
  services:{revision:0,boot_id:1,sample_ms:1,always_on:{wifi:true,http:true,stream:true,snapshot:true},ftp:{enabled:true,state:'enabled',clients:0,transferring:false},web_sd:{enabled:true,state:'enabled',requests:0,jobs:0},ota:{enabled:true,state:'enabled',updating:false},panel_refresh:{enabled:true,state:'enabled',interval_seconds:2}},
  version, hostname:'webcam', uptime_seconds:40,
  heap_free:150000, heap_total:300000, heap_used_pct:50, heap_min:140000, psram_free:2000000, chip_temperature_c:48.5, error:'', job:'idle',
  ftp:{enabled:true,port:21,passive_first:50000,passive_last:50001,anonymous:true,read_only:true,sessions:0,transferring:false},
  wifi:{connected:true, ip:'127.0.0.1', signal_pct:80, rssi_dbm:-50, channel:6},
  camera:{ready:true, width:640, height:480, sent:10, jpeg_bytes:30000, capture_ms:35, send_ms:20, errors:0, dropped:0, profile},
  led:{enabled:false, effective_on:false, intensity_pct:25, auto_flash:false},
  sd:{mounted:true, present:true, maintenance:false, download:false, free_bytes:1000000000, total_bytes:2000000000, job:{id:0,phase:'idle',operation:'',processed:0,total:0,path:'',destination:'',error:''}},
  timelapse:{state:'idle', saved:0, missed:0, bytes:0, interval_seconds:10, elapsed_seconds:0, next_seconds:0, session:'', profile}
};
(async () => {
  let browser;
  try {
    try {
      browser = await chromium.launch({headless:true,downloadsPath:path.join(root,'verification/browser-downloads')});
    } catch (_) {
      browser = await chromium.launch({headless:true, channel:'msedge',downloadsPath:path.join(root,'verification/browser-downloads')});
    }
    const page = await browser.newPage();
    const errors = [];
    const requests = [];
    let folderName = 'sessao-teste';
    let folderExists = true;
    let jobPolls = 0;
    page.on('pageerror', error => errors.push(error.message));
    await page.route('http://remotecam.test/**', async route => {
      const url = new URL(route.request().url());
      let body = {};
      if (route.request().method() !== 'GET') body = JSON.parse(route.request().postData() || '{}');
      requests.push({path:url.pathname,body,method:route.request().method()});
      if (url.pathname === '/assets/services.js') return route.fulfill({contentType:'application/javascript',body:servicesScript});
      if (url.pathname === '/api/services') return route.fulfill({json:{services:state.services}});
      if (url.pathname === '/assets/folder-actions.js') return route.fulfill({contentType:'application/javascript',body:folderScript});
      if (url.pathname === '/api/sd/archive') return route.fulfill({contentType:'application/zip',headers:{'Content-Disposition':'attachment; filename="sessao-teste.zip"'},body:emptyZip});
      if (url.pathname === '/') return route.fulfill({contentType:'text/html',body:html});
      if (url.pathname === '/gallery') return route.fulfill({contentType:'text/html',body:gallery});
      if (url.pathname === '/api/sd/image' || url.pathname === '/stream') return route.fulfill({contentType:'image/jpeg',body:jpeg});
      if (url.pathname === '/api/status') {
        if (['queued','renaming','deleting'].includes(state.sd.job.phase)) {
          ++jobPolls;
          if (jobPolls === 1) {
            state.sd.job.phase = state.sd.job.operation === 'rename' ? 'renaming' : 'deleting';
          } else {
            state.sd.job.phase = 'completed';
            state.sd.job.processed = state.sd.job.total;
            state.sd.maintenance = false;
            if (state.sd.job.operation === 'rename') folderName = state.sd.job.destination.split('/').pop();
            else folderExists = false;
          }
        }
        return route.fulfill({json:state});
      }
      if (url.pathname === '/api/sd/rename' || (url.pathname === '/api/sd/item' && body.recursive)) {
        const operation = url.pathname.endsWith('/rename') ? 'rename' : 'delete';
        const destination = operation === 'rename' ? '/timelapse/' + body.new_name : '';
        state.sd.maintenance = true;
        state.sd.job = {id:state.sd.job.id+1,phase:'queued',operation,path:body.path,destination,processed:0,total:operation==='delete'?5:1,error:''};
        jobPolls = 0;
        return route.fulfill({status:202,json:{job_id:state.sd.job.id,message:'aceito'}});
      }
      if (url.pathname === '/api/sd/list') {
        const currentPath = url.searchParams.get('path') || '/';
        let items;
        if (currentPath === '/' || currentPath === '/timelapse') {
          items = folderExists ? [{name:folderName,path:'/timelapse/'+folderName,directory:true,bytes:0,incomplete:false}] : [];
          if (currentPath === '/') items.push({name:'<img onerror=alert(1)>',path:'/safe.jpg',directory:false,bytes:1234,incomplete:false});
        } else {
          items = [{name:'frame_00000001.jpg',path:currentPath+'/frame_00000001.jpg',directory:false,bytes:jpeg.length,incomplete:false}];
        }
        return route.fulfill({json:{path:currentPath,items,next_offset:null}});
      }
      if (url.pathname === '/api/timelapse/start') {
        state.timelapse.state='recording';
        return route.fulfill({json:{message:'iniciado'}});
      }
      return route.fulfill({json:{message:'ok'}});
    });
    await page.goto('http://remotecam.test/');
    await page.locator('#dashboard').waitFor({state:'visible'});
    if (await page.locator('#login, input[type=password]').count()) throw new Error('Login não solicitado');
    if (await page.locator('#firmware-version').textContent() !== 'Firmware '+version) throw new Error('Versão não corresponde a VERSION');
    if (await page.locator('#chip-temperature').textContent() !== '48.5 °C') throw new Error('Temperatura ausente');
    if (await page.locator('#wifi-snr, #snr-enabled').count()) throw new Error('SNR ainda está na tela');
    if (await page.locator('#snapshot').getAttribute('href') !== '/snapshot') throw new Error('Snapshot não é direto');
    if (await page.locator('#heap').textContent() !== '50,0%') throw new Error('RAM não está em percentual utilizado');
    if (!(await page.locator('#heap-free').textContent()).includes('146.5 KiB livres')) throw new Error('RAM livre não aparece abaixo');
    if (await page.locator('#ftp-address').textContent() !== 'ftp://127.0.0.1:21/') throw new Error('Endereço FTP ausente');
    if (!(await page.locator('#ftp-state').textContent()).includes('somente leitura')) throw new Error('Modo FTP não informado');
    if (await page.locator('#resolution').inputValue() !== '8') throw new Error('Mapeamento VGA inválido');
    await page.waitForFunction(() => document.getElementById('files').textContent.includes('<img onerror'));
    if (await page.locator('#files img').count()) throw new Error('Nome interpretado como HTML');
    await page.locator('#video-toggle').click();
    await page.waitForFunction(() => document.getElementById('video').naturalWidth === 1);
    await page.locator('#video-toggle').click();
    if (!requests.some(item => item.path === '/stream')) throw new Error('Stream não usa a rota original');
    if (requests.some(item => item.path === '/api/stream/stop')) throw new Error('Fechar painel interromperia outro cliente');
    await page.locator('#tl-open').click();
    await page.fill('#tl-interval','7');
    await page.selectOption('#tl-resolution','13');
    await page.selectOption('#tl-mode','priority');
    await page.locator('#tl-confirm').click();
    await page.waitForFunction(() => document.getElementById('tl-state').textContent === 'Gravando');
    const start = requests.find(item => item.path === '/api/timelapse/start');
    if (!start || start.body.interval_seconds !== 7 || start.body.profile.resolution !== 13 || !start.body.photo_priority) throw new Error('Contrato incorreto');
    await page.locator('#open-gallery').click();
    await page.locator('#grid button', {hasText:'Abrir pasta'}).click();
    await page.waitForFunction(() => {
      const image = document.querySelector('#grid img');
      return image && image.complete && image.naturalWidth === 1;
    });
    await page.locator('#grid img').click();
    await page.locator('#viewer').waitFor({state:'visible'});
    if (await page.locator('#image-name').textContent() !== 'frame_00000001.jpg') throw new Error('Galeria não abriu a fotografia');
    await page.locator('#close').click();
    await page.locator('#current-folder-actions [data-sd-folder-action="rename"]').click();
    await page.fill('#sd-rename-input','Bloqueada');
    await page.locator('#sd-rename-confirm').click();
    await page.waitForFunction(() => document.getElementById('sd-rename-error').textContent.includes('Finalize'));
    if (requests.some(item => item.path === '/api/sd/rename')) throw new Error('Renomeação enviada durante timelapse');
    await page.locator('#sd-rename-dialog').getByRole('button',{name:'Cancelar',exact:true}).click();
    state.timelapse.state = 'completed';
    const downloadEvent = page.waitForEvent('download');
    await page.locator('#current-folder-actions [data-sd-folder-action="zip"]').click();
    const download = await downloadEvent;
    if (download.suggestedFilename() !== 'sessao-teste.zip') throw new Error('Download não é ZIP');
    if (await download.failure()) throw new Error('Falha no download simulado');
    await download.delete();
    await page.locator('#current-folder-actions [data-sd-folder-action="rename"]').click();
    await page.fill('#sd-rename-input','Sessão final');
    await page.locator('#sd-rename-confirm').click();
    await page.waitForFunction(() => document.getElementById('path').textContent === '/timelapse/Sessão final');
    const rename = requests.find(item => item.path === '/api/sd/rename');
    if (!rename || rename.body.new_name !== 'Sessão final') throw new Error('Nome não foi enviado corretamente');
    await page.locator('#current-folder-actions [data-sd-folder-action="delete"]').click();
    await page.locator('#sd-delete-confirm').click();
    if (requests.some(item => item.path === '/api/sd/item')) throw new Error('Exclusão enviada sem confirmação');
    await page.locator('#sd-delete-dialog').getByRole('button',{name:'Cancelar',exact:true}).click();
    if (requests.some(item => item.path === '/api/sd/item')) throw new Error('Cancelar iniciou exclusão');
    await page.locator('#current-folder-actions [data-sd-folder-action="delete"]').click();
    await page.fill('#sd-delete-input','EXCLUIR');
    await page.locator('#sd-delete-confirm').click();
    await page.waitForFunction(() => document.getElementById('path').textContent === '/timelapse');
    const deletion = requests.find(item => item.path === '/api/sd/item');
    if (!deletion || !deletion.body.recursive || deletion.body.confirmation !== 'EXCLUIR' || deletion.method !== 'DELETE') throw new Error('Contrato de exclusão recursiva incorreto');
    if (!(await page.locator('#current-folder-actions [data-sd-folder-action="delete"]').isDisabled())) throw new Error('Pasta raiz timelapse não protegida');
    if (errors.length) throw new Error(errors.join('\n'));
    console.log(JSON.stringify({ok:true, checks:['acesso direto sem login','versão centralizada','status','temperatura do chip', 'SNR removido', 'rotas diretas', 'galeria e ampliação','VGA enum','escape de nomes','modal timelapse','contrato de início','RAM utilizada e livre','ZIP','renomear pasta','excluir recursivamente','cancelar exclusão','bloqueio com timelapse ativo','informações de FTP anônimo'], pageErrors:errors}));
  } finally {
    if (browser) await browser.close();
  }
})().catch(error => {
  console.error(error);
  process.exitCode=1;
});
