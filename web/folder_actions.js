/* Folder operations shared by dashboard and gallery. No mutation is retried. */
'use strict';
window.SdFolders = (() => {
    let options = {};
    let busy = false;
    let accessEnabled = true;
    let target = null;
    let renameDialog, deleteDialog, renameInput, deleteInput, renameError, deleteError;
    let progressBox, progressText, progressBar;
    const active = new Set(['queued', 'scanning', 'deleting', 'renaming']);
    const normalize = path => path.length > 1 ? path.replace(/\/+$/, '') : path;
    const contains = (parent, child) => {
        parent = normalize(parent).toLowerCase();
        child = normalize(child).toLowerCase();
        return parent === '/' || child === parent || child.startsWith(parent + '/');
    };
    const parentOf = path => normalize(path).slice(0, normalize(path).lastIndexOf('/')) || '/';
    const protectedPath = path => ['/', '/timelapse'].includes(normalize(path).toLowerCase());
    const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    function element(tag, text, id) {
        const node = document.createElement(tag);
        if (text !== undefined) node.textContent = text;
        if (id) node.id = id;
        return node;
    }
    function tell(message, error = false) {
        if (options.showMessage) options.showMessage(message, error);
    }
    function setBusy(value) {
        busy = value;
        for (const button of document.querySelectorAll('[data-sd-folder-action]')) {
            button.disabled = value || (button.dataset.sdFolderAction !== 'cancel' && (!accessEnabled || button.dataset.protected === 'true'));
        }
        if (options.onBusy) options.onBusy(value);
    }
    function setAccessEnabled(value) {
        accessEnabled = Boolean(value);
        setBusy(busy);
    }
    async function api(path, body, method = 'POST') {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), 12000);
        try {
            const configuration = {cache:'no-store', signal:controller.signal};
            if (body !== undefined) {
                configuration.method = method;
                configuration.headers = {'Content-Type':'application/json'};
                configuration.body = JSON.stringify(body);
            }
            const response = await fetch(path, configuration);
            const data = await response.json();
            if (!response.ok) throw new Error(data.message || 'Falha HTTP ' + response.status);
            return data;
        } finally {
            clearTimeout(timer);
        }
    }
    async function ready() {
        if (options.beforeAction) await options.beforeAction();
        for (let attempt = 0; attempt < 20; ++attempt) {
            const state = await api('/api/status');
            if (state.services?.web_sd.enabled===false || state.services?.web_sd.state==='paused') {setAccessEnabled(false);throw new Error('Ative o acesso web ao SD na seção Serviços do painel.');}
            if (!state.sd.mounted) throw new Error('Cartão não montado.');
            if (state.sd.maintenance || ['starting','recording','stopping'].includes(state.timelapse.state)) {
                throw new Error('Finalize o timelapse ou aguarde a manutenção do cartão.');
            }
            if (!state.sd.download) return;
            await delay(250);
        }
        throw new Error('Há um download ativo. Aguarde sua conclusão.');
    }
    function showProgress(job) {
        progressBox.hidden = false;
        const names = {queued:'Operação na fila',scanning:'Conferindo conteúdo da pasta',deleting:'Excluindo conteúdo',renaming:'Renomeando pasta',completed:'Operação concluída',failed:'Operação falhou'};
        progressText.textContent = (names[job.phase] || job.phase) + (job.total ? ' · ' + job.processed + '/' + job.total + ' itens' : '') + ' · ' + job.path;
        if (job.phase === 'deleting' && job.total) {
            progressBar.max = job.total;
            progressBar.value = job.processed;
        } else if (job.phase === 'completed') {
            progressBar.max = 1;
            progressBar.value = 1;
        } else {
            progressBar.removeAttribute('value');
        }
    }
    async function waitJob(id) {
        for (;;) {
            const state = await api('/api/status');
            const job = state.sd.job;
            if (!job || job.id !== id) throw new Error('O estado da operação mudou. Atualize a página antes de repetir.');
            showProgress(job);
            if (job.phase === 'completed') return job;
            if (job.phase === 'failed') {
                const suffix = job.operation === 'delete' && job.processed ? ' A exclusão foi parcial: ' + job.processed + ' itens removidos.' : '';
                const error = new Error((job.error || 'Operação não concluída.') + suffix);
                error.job = job;
                throw error;
            }
            await delay(700);
        }
    }
    async function finishJob(id) {
        const job = await waitJob(id);
        if (options.onChange) await options.onChange(job);
        tell(job.operation === 'rename' ? 'Pasta renomeada.' : 'Pasta e conteúdo excluídos.');
    }
    async function submit(operation) {
        if (busy || !target) return;
        const chosen = {...target};
        const errorLabel = operation === 'rename' ? renameError : deleteError;
        errorLabel.textContent = '';
        setBusy(true);
        try {
            await ready();
            let result;
            if (operation === 'rename') {
                const name = renameInput.value.trim();
                if (!name || /[\\/]/.test(name) || name === '.' || name === '..') throw new Error('Informe apenas o novo nome da pasta.');
                result = await api('/api/sd/rename', {path:chosen.path,new_name:name});
                renameDialog.close();
            } else {
                if (deleteInput.value !== 'EXCLUIR') throw new Error('Digite EXCLUIR para confirmar.');
                result = await api('/api/sd/item', {path:chosen.path,confirmation:'EXCLUIR',recursive:true}, 'DELETE');
                deleteDialog.close();
            }
            await finishJob(result.job_id);
        } catch (error) {
            const message = error.name === 'AbortError' ? 'Resposta não confirmada. A operação pode continuar na placa; atualize a página antes de repetir.' : error.message;
            errorLabel.textContent = message;
            if (error.job && options.onChange) await options.onChange(error.job);
            tell(message, true);
        } finally {
            setBusy(false);
        }
    }
    function makeDialog(operation) {
        const dialog = element('dialog', undefined, 'sd-' + operation + '-dialog');
        const form = element('form');
        const title = element('h2', operation === 'rename' ? 'Renomear pasta' : 'Excluir pasta e todo o conteúdo');
        const path = element('p', '', 'sd-' + operation + '-path');
        const description = element('p', operation === 'rename' ? 'Informe somente o novo nome. Itens existentes nunca serão sobrescritos.' : 'Todas as fotos, arquivos e subpastas serão apagados. Esta ação não pode ser desfeita.');
        const label = element('label', operation === 'rename' ? 'Novo nome' : 'Digite EXCLUIR para confirmar');
        const input = element('input', undefined, 'sd-' + operation + '-input');
        input.required = true;
        input.autocomplete = 'off';
        input.maxLength = operation === 'rename' ? 160 : 7;
        if (operation === 'delete') input.pattern = 'EXCLUIR';
        const error = element('p', '', 'sd-' + operation + '-error');
        error.className = 'warning';
        const buttons = element('div');
        buttons.className = 'row';
        const confirm = element('button', operation === 'rename' ? 'Renomear' : 'Excluir definitivamente', 'sd-' + operation + '-confirm');
        confirm.type = 'submit';
        confirm.dataset.sdFolderAction = operation;
        confirm.className = operation === 'delete' ? 'danger' : 'primary';
        const cancel = element('button', 'Cancelar');
        cancel.type = 'button';
        cancel.dataset.sdFolderAction = 'cancel';
        cancel.onclick = () => dialog.close();
        form.onsubmit = event => {event.preventDefault(); submit(operation);};
        buttons.append(confirm, cancel);
        form.append(title, path, description, label, input, error, buttons);
        dialog.appendChild(form);
        dialog.addEventListener('cancel', event => {if (busy) event.preventDefault();});
        document.body.appendChild(dialog);
        return {dialog,input,error};
    }
    function open(operation, entry) {
        if (busy || !accessEnabled || protectedPath(entry.path) || entry.protected) return;
        target = entry;
        const dialog = operation === 'rename' ? renameDialog : deleteDialog;
        document.getElementById('sd-' + operation + '-path').textContent = entry.path;
        if (operation === 'rename') {
            renameInput.value = entry.name;
            renameError.textContent = '';
        } else {
            deleteInput.value = '';
            deleteError.textContent = '';
        }
        dialog.showModal();
        if (operation === 'rename') renameInput.select();
        else deleteInput.focus();
    }
    async function download(path) {
        if (!accessEnabled) {tell('Ative o acesso web ao SD na seção Serviços.',true);return;}
        if (busy) return;
        setBusy(true);
        try {
            await ready();
            progressBox.hidden = true;
            tell('Download ZIP solicitado. Aguarde o navegador concluir.');
            window.location.assign('/api/sd/archive?path=' + encodeURIComponent(path));
        } catch (error) {
            tell(error.message, true);
        } finally {
            setBusy(false);
        }
    }
    function addActions(container, entry) {
        if (!entry.directory) return;
        for (const [action, text] of [['zip','Baixar ZIP'],['rename','Renomear'],['delete','Excluir pasta']]) {
            const button = element('button', text);
            button.type = 'button';
            button.dataset.sdFolderAction = action;
            button.dataset.path = entry.path;
            button.dataset.protected = String(action !== 'zip' && (entry.protected || protectedPath(entry.path)));
            button.disabled = busy || !accessEnabled || button.dataset.protected === 'true';
            if (action === 'delete') button.className = 'danger';
            button.onclick = () => action === 'zip' ? download(entry.path) : open(action, entry);
            container.appendChild(button);
        }
    }
    async function resume() {
        if (busy) return;
        try {
            const state = await api('/api/status');
            setAccessEnabled(state.services?.web_sd.enabled!==false && state.services?.web_sd.state!=='paused');
            if (state.sd.job && active.has(state.sd.job.phase)) {
                setBusy(true);
                await finishJob(state.sd.job.id);
            }
        } catch (error) {
            tell(error.message, true);
        } finally {
            setBusy(false);
        }
    }
    function setup(configuration) {
        options = configuration;
        const rename = makeDialog('rename');
        renameDialog = rename.dialog;
        renameInput = rename.input;
        renameError = rename.error;
        const remove = makeDialog('delete');
        deleteDialog = remove.dialog;
        deleteInput = remove.input;
        deleteError = remove.error;
        progressBox = element('section', undefined, 'sd-folder-progress');
        progressBox.hidden = true;
        progressText = element('p');
        progressBar = element('progress');
        progressBar.style.width = '100%';
        progressBox.append(progressText, progressBar);
        (document.getElementById(configuration.progressTarget) || document.querySelector('main')).appendChild(progressBox);
    }
    return {setup,addActions,download,resume,contains,parentOf,setAccessEnabled};
})();
