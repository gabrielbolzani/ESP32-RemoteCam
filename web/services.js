/* Optional service controls. Preferences live on the ESP32, not localStorage. */
'use strict';
window.ServiceControls = (() => {
    const fields = {ftp:'service-ftp',web_sd:'service-web-sd',ota:'service-ota',panel_refresh:'service-panel-refresh'};
    const labels = {disabled:'Desligado',starting:'Iniciando',enabled:'Ligado',busy:'Em uso',stopping:'Desativando…',paused:'Pausado durante OTA',error:'Falha ao iniciar'};
    let options = {};
    let current = null;
    let saving = false;
    let uncertain = false;
    let generation = 0;
    let watchTimer = null;
    const $ = id => document.getElementById(id);
    function setMessage(message, error = false) {
        $('services-message').textContent = message;
        $('services-message').classList.toggle('warning', error);
    }
    function setDisabled() {
        for (const id of Object.values(fields)) $(id).disabled = saving || uncertain || !current;
        $('service-interval').disabled = saving || uncertain || !current;
        $('services-refresh').disabled = saving;
    }
    function transitionPending() {
        return current && ['ftp','web_sd','ota'].some(name => ['stopping','starting'].includes(current[name].state));
    }
    async function request(body) {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 12000);
        try {
            const config = {cache:'no-store',signal:controller.signal};
            if (body !== undefined) {
                config.method = 'POST';
                config.headers = {'Content-Type':'application/json'};
                config.body = JSON.stringify(body);
            }
            const response = await fetch('/api/services', config);
            const data = await response.json();
            if (!response.ok) throw new Error(data.message || 'Falha ao consultar serviços.');
            return data;
        } finally {
            clearTimeout(timeout);
        }
    }
    function draw(value) {
        if (!value) return;
        if (current && value.boot_id === current.boot_id && Number.isFinite(value.sample_ms) &&
            Number.isFinite(current.sample_ms) && value.sample_ms < current.sample_ms) {
            if (options.onConfig) options.onConfig(current);
            return;
        }
        current = value;
        uncertain = false;
        for (const [name,id] of Object.entries(fields)) {
            const service = value[name];
            $(id).checked = service.enabled;
            $('state-' + id).textContent = labels[service.state] || service.state;
            $('state-' + id).dataset.state = service.state;
        }
        $('service-interval').value = String(value.panel_refresh.interval_seconds);
        $('service-ftp-detail').textContent = value.ftp.state === 'stopping'
            ? 'Novas conexões bloqueadas; aguardando as sessões atuais terminarem.'
            : value.ftp.state === 'error' ? 'Não foi possível abrir a porta. O firmware tentará novamente.'
            : 'FTP anônimo de leitura. Desligar fecha a porta e libera as tarefas.';
        $('service-sd-detail').textContent = value.web_sd.state === 'stopping'
            ? 'Aguardando ' + value.web_sd.requests + ' pedido(s) e ' + value.web_sd.jobs + ' operação(ões).'
            : 'Galeria, arquivos e ZIP pela web. Não desmonta o cartão nem para o timelapse.';
        $('service-ota-detail').textContent = value.ota.state === 'stopping'
            ? 'A atualização atual termina normalmente; novos envios estão bloqueados.'
            : 'Libera novos envios de firmware. Nunca cancela uma gravação já iniciada.';
        $('panel-refresh-state').textContent = value.panel_refresh.enabled
            ? 'Atualização automática a cada ' + value.panel_refresh.interval_seconds + ' s.'
            : 'Atualização automática pausada. Botões e acompanhamento de operações continuam funcionando.';
        setDisabled();
        if (options.onConfig) options.onConfig(value);
    }
    function watch() {
        clearTimeout(watchTimer);
        if (saving || !transitionPending()) return;
        const serial = generation;
        watchTimer = setTimeout(async () => {
            try {
                const data = await request();
                if (serial !== generation || saving) return;
                draw(data.services);
                if (!transitionPending()) setMessage('Alteração aplicada. Preferências salvas na câmera.');
            } catch (error) {
                if (serial !== generation) return;
                setMessage('Aguardando confirmação do término. Você pode usar Atualizar agora.', true);
            }
            if (serial === generation) watch();
        }, 1000);
    }
    function render(value) {
        if (saving) return;
        draw(value);
        watch();
    }
    async function save(change) {
        if (saving) return;
        saving = true;
        ++generation;
        clearTimeout(watchTimer);
        setDisabled();
        setMessage('Salvando preferência…');
        try {
            if (options.beforeSave) await options.beforeSave();
            const data = await request(change);
            draw(data.services);
            setMessage(data.message || 'Preferências salvas.');
        } catch (error) {
            setMessage(error.name === 'AbortError'
                ? 'Resposta não confirmada. Atualize o estado antes de repetir.' : error.message, true);
            try {
                const fresh = await request();
                draw(fresh.services);
            } catch (_) {
                uncertain = true;
                for (const id of Object.values(fields)) $('state-' + id).textContent = 'Sem confirmação';
            }
        } finally {
            saving = false;
            setDisabled();
            if (options.afterSave) await options.afterSave();
            watch();
        }
    }
    function setup(configuration) {
        options = configuration;
        for (const [name,id] of Object.entries(fields)) {
            $(id).addEventListener('change', () => save({[name]:$(id).checked}));
        }
        $('service-interval').addEventListener('change', () => save({refresh_seconds:Number($('service-interval').value)}));
        $('services-refresh').onclick = () => options.refresh && options.refresh();
        setDisabled();
        window.addEventListener('pagehide', () => {
            ++generation;
            clearTimeout(watchTimer);
        });
    }
    return {setup,render,isSaving:()=>saving};
})();
