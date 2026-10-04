/**
 * app.js - Web Dashboard client
 *
 * Handles session authentication, WebSocket communication, dashboard data,
 * control actions, Learn Mode, settings and OTA UI.
 */

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// Settings
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

const WS_URL = `ws://${window.location.hostname}/ws`;
const RECONNECT_INTERVAL = 3000;
const PING_INTERVAL = 30000;

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Runtime state
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

let ws = null;
let reconnectTimer = null;
let pingTimer = null;
let isConnected = false;
let isAuthenticated = false;
let sessionToken = null;
let vehicleData = {};
let canMonitorActive = false;
let canMonitorBus = 0;
let canMonitorLines = [];
let canRecordingActive = false;

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ DOM references
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

const connectionStatus = document.getElementById('connection-status');
const notification = document.getElementById('notification');
let notifTimeout = null;

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Session token retrieval
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

async function fetchSessionToken() {
    try {
        const res = await fetch('/api/session-token', { credentials: 'same-origin' });
        if (!res.ok) {
            throw new Error('Unauthorized - please log in again');
        }
        const data = await res.json();
        sessionToken = data.token;
        return true;
    } catch (e) {
        console.error('Failed to fetch session token:', e);
        showNotification('⚠️ Authentication error - refresh the page');
        return false;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// WebSocket connection
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

async function connectWebSocket() {
    if (ws && ws.readyState === WebSocket.OPEN) return;
    
    // Fetch a valid token before connecting (or refresh if expired).
    const gotToken = await fetchSessionToken();
    if (!gotToken) {
        scheduleReconnect();
        return;
    }
    
    try {
        ws = new WebSocket(WS_URL);
    } catch (e) {
        console.error('WebSocket error:', e);
        scheduleReconnect();
        return;
    }
    
    ws.onopen = function() {
        console.log('🔌 WebSocket connected - authenticating...');
        isConnected = true;
        isAuthenticated = false;
        clearTimeout(reconnectTimer);
        
        // The first message must be auth.
        ws.send(JSON.stringify({ type: 'auth', token: sessionToken }));
    };
    
    ws.onclose = function() {
        console.log('❌ WebSocket disconnected');
        isConnected = false;
        isAuthenticated = false;
        canMonitorActive = false;
        canRecordingActive = false;
        updateCanMonitorStatus('Stopped (connection closed)');
        connectionStatus.textContent = 'Disconnected';
        connectionStatus.classList.remove('connected');
        showNotification('❌ Connection lost');
        stopPing();
        scheduleReconnect();
    };
    
    ws.onerror = function(err) {
        console.error('WebSocket error:', err);
    };
    
    ws.onmessage = function(event) {
        try {
            const data = JSON.parse(event.data);
            handleMessage(data);
        } catch (e) {
            console.warn('Message parse error:', e);
        }
    };
}

function scheduleReconnect() {
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connectWebSocket, RECONNECT_INTERVAL);
}

function startPing() {
    stopPing();
    pingTimer = setInterval(() => {
        if (ws && ws.readyState === WebSocket.OPEN && isAuthenticated) {
            ws.send(JSON.stringify({ type: 'ping' }));
        }
    }, PING_INTERVAL);
}

function stopPing() {
    clearInterval(pingTimer);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// Message handling
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

function handleMessage(data) {
    switch (data.type) {
        case 'need_auth':
            // Server expects auth (or rejected the token) - retry.
            if (sessionToken) {
                ws.send(JSON.stringify({ type: 'auth', token: sessionToken }));
            }
            break;
            
        case 'auth_failed':
            console.warn('Session token invalid or expired');
            isAuthenticated = false;
            sessionToken = null;
            showNotification('⚠️ Session expired - retrying...');
            break;
            
        case 'welcome':
            console.log('CarTouch:', data.message);
            isAuthenticated = true;
            connectionStatus.textContent = 'Connected';
            connectionStatus.classList.add('connected');
            showNotification('✅ Connected to CarTouch');
            startPing();
            sendCommand2('can_record_status', {});
            sendCommand2('obd_dtc_status', {});
            loadCanRecordings();
            break;
            
        case 'rate_limited':
            showNotification('⏳ Please wait');
            break;
            
        case 'pong':
            break;
            
        case 'vehicle_data':
            vehicleData = data;
            updateDashboard(data);
            break;

        case 'module_status':
            updateModuleStatusList(data.modules);
            break;

        case 'can_diagnostics':
            updateCanDiagnostics(data);
            break;

        case 'can_monitor_state':
            canMonitorActive = !!data.active;
            if (canMonitorActive && (data.bus === 0 || data.bus === 1)) {
                canMonitorBus = data.bus;
                document.getElementById('can-monitor-bus').value = String(data.bus);
            }
            updateCanMonitorStatus(canMonitorActive
                ? `Listening on CAN${canMonitorBus + 1}`
                : 'Stopped');
            break;

        case 'can_monitor_error':
            updateCanMonitorStatus(data.message || 'Monitor could not start');
            break;

        case 'can_record_queued':
            updateCanRecorderStatus({ message: 'Request queued' });
            break;

        case 'can_record_error':
            updateCanRecorderStatus({ message: data.message || 'Recording request failed' });
            break;

        case 'can_record_state': {
            const wasRecording = canRecordingActive;
            canRecordingActive = !!data.active;
            updateCanRecorderStatus(data);
            if (wasRecording && !canRecordingActive) loadCanRecordings();
            break;
        }

        case 'can_record_files_changed':
            loadCanRecordings();
            break;

        case 'obd_dtc_queued':
            updateObdDtcStatus({ message: 'Request queued' });
            break;

        case 'obd_dtc_error':
            updateObdDtcStatus({ message: data.message || 'OBD request failed' });
            break;

        case 'obd_dtc_state':
            updateObdDtcStatus(data);
            break;

        case 'can_frame':
            appendCanMonitorFrame(data);
            break;
            
        case 'status':
            showNotification(data.message);
            break;
            
        case 'ack':
            break;
            
        // === Learn Mode messages ===
        case 'learn_state':
            handleLearnState(data);
            break;
            
        case 'learn_error':
            showNotification('⚠️ ' + (data.message || 'Learning error'));
            break;
            
        case 'learn_saved':
            if (data.success) {
                showNotification('✅ Command saved (still unverified)');
                closeLearnWizard();
                refreshCustomVehicleList();
            } else {
                showNotification('❌ Save failed');
            }
            break;
            
        case 'verify_sent':
            handleVerifySent(data);
            break;
            
        case 'verify_error':
            showNotification('⚠️ ' + (data.message || 'Test send failed'));
            break;
            
        case 'verify_confirmed':
            refreshCustomVehicleList();
            break;
            
        default:
            console.log('Unknown message:', data);
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Dashboard update
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

function updateDashboard(data) {
    const speedEl = document.getElementById('speed-display');
    if (speedEl) {
        speedEl.innerHTML = `${data.speed ?? '--'} <small>km/h</small>`;
    }
    
    const rpmEl = document.getElementById('dash-rpm');
    if (rpmEl) rpmEl.textContent = data.rpm ?? '--';
    
    const tempEl = document.getElementById('dash-temp');
    if (tempEl) tempEl.textContent = data.coolantTemp == null ? '-- °C' : `${data.coolantTemp} °C`;
    
    const batteryEl = document.getElementById('dash-battery');
    if (batteryEl) {
        batteryEl.textContent = Number.isFinite(data.battery) && data.battery > 0
            ? `${data.battery.toFixed(1)} V`
            : 'N/A';
    }
    
    const fuelEl = document.getElementById('dash-fuel');
    if (fuelEl) fuelEl.textContent = data.fuel == null ? '--%' : `${data.fuel}%`;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Send command
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

function sendCommand(command) {
    if (!isConnected || !isAuthenticated || !ws || ws.readyState !== WebSocket.OPEN) {
        showNotification('⚠️ Not connected');
        return;
    }
    
    const msg = JSON.stringify({
        type: 'command',
        command: command
    });
    
    ws.send(msg);
    console.log('📤 Command sent:', command);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Notification display
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

function showNotification(message) {
    notification.textContent = message;
    notification.classList.add('show');
    
    clearTimeout(notifTimeout);
    notifTimeout = setTimeout(() => {
        notification.classList.remove('show');
    }, 2500);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// UI events
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

document.querySelectorAll('.ctrl-btn[data-cmd]').forEach(btn => {
    btn.addEventListener('click', function() {
        const cmd = this.getAttribute('data-cmd');
        sendCommand(cmd);
        
        this.style.transform = 'scale(0.92)';
        setTimeout(() => {
            this.style.transform = '';
        }, 150);
    });
});

document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', function() {
        document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        this.classList.add('active');
        
        const tab = this.getAttribute('data-tab');
        document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
        document.getElementById(`tab-${tab}`).classList.add('active');
        
        // Refresh profiles whenever the Learn tab is opened.
        if (tab === 'learn') {
            refreshCustomVehicleList();
        }
    });
});

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// Password change
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

async function checkPasswordStatus() {
    try {
        const res = await fetch('/api/status', { credentials: 'same-origin' });
        if (!res.ok) return;
        const data = await res.json();
        const warningGroup = document.getElementById('password-warning-group');
        if (warningGroup) {
            warningGroup.style.display = data.usingDefaultPassword ? 'block' : 'none';
        }
        // The active vehicle banner is also refreshed by this response.
        const banner = document.getElementById('active-vehicle-banner');
        if (banner && data.activeVehicle) {
            banner.textContent = 'Active vehicle: ' + data.activeVehicle;
        }
    } catch (e) {
        console.warn('Password status check error:', e);
    }
}

function setupPasswordForm() {
    const form = document.getElementById('password-form');
    if (!form) return;
    
    form.addEventListener('submit', async function(e) {
        e.preventDefault();
        
        const newUser = document.getElementById('new-user').value.trim();
        const newPass = document.getElementById('new-pass').value;
        const confirmPass = document.getElementById('confirm-pass').value;
        const msgEl = document.getElementById('password-form-msg');
        
        if (newPass.length < 8) {
            msgEl.textContent = 'Password must be at least 8 characters';
            msgEl.style.color = '#e74c3c';
            return;
        }
        if (newPass !== confirmPass) {
            msgEl.textContent = 'Passwords do not match';
            msgEl.style.color = '#e74c3c';
            return;
        }
        
        try {
            const body = new URLSearchParams();
            if (newUser) body.append('newUser', newUser);
            body.append('newPass', newPass);
            body.append('confirmPass', confirmPass);
            
            const res = await fetch('/api/change-password', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: body.toString()
            });
            const data = await res.json();
            
            if (data.success) {
                msgEl.textContent = '✅ Password changed successfully. Please log in again...';
                msgEl.style.color = '#2ecc71';
                form.reset();
                // The server invalidated the previous session; refresh after a short delay.
                // so the user can log in again with the new password.
                setTimeout(() => { window.location.reload(); }, 2000);
            } else {
                msgEl.textContent = '⚠️ ' + (data.error || 'Password change failed');
                msgEl.style.color = '#e74c3c';
            }
        } catch (err) {
            console.error('Password change error:', err);
            msgEl.textContent = '⚠️ Server communication error';
            msgEl.style.color = '#e74c3c';
        }
    });
}

function setupWifiForm() {
    const form = document.getElementById('wifi-form');
    if (!form) return;
    const msg = document.getElementById('wifi-form-msg');
    async function send(params, okText) {
        try {
            const res = await fetch('/api/wifi', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params.toString()
            });
            const data = await res.json();
            msg.textContent = data.success ? okText : ('Error: ' + (data.error || 'failed'));
            msg.style.color = data.success ? '#2ecc71' : '#e74c3c';
            if (data.success) setTimeout(refreshDeviceInfo, 15000);
        } catch (e) {
            msg.textContent = 'Server communication error';
            msg.style.color = '#e74c3c';
        }
    }
    form.addEventListener('submit', function(e) {
        e.preventDefault();
        const p = new URLSearchParams();
        p.append('ssid', document.getElementById('wifi-ssid-input').value.trim());
        p.append('pass', document.getElementById('wifi-pass-input').value);
        send(p, 'Connecting... the new IP appears here in about 15 seconds. The CarTouch AP stays on.');
    });
    document.getElementById('wifi-forget-btn').addEventListener('click', function() {
        const p = new URLSearchParams();
        p.append('forget', '1');
        send(p, 'Saved router removed.');
    });
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Initialization
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

async function refreshDeviceInfo(){
  try{
    const r=await fetch('/api/status',{credentials:'same-origin'});
    if(!r.ok)return;
    const d=await r.json();
    const fw=document.getElementById('firmwareVersion');
    if(fw)fw.textContent='Firmware: '+(d.firmwareVersion||'unknown');
    const ble=document.getElementById('bleStatus');
    if(ble)ble.textContent='BLE: '+(d.bleConnected?'Connected':(d.bleEnabled?'Ready':'Disabled'));
    const wifi=document.getElementById('wifi-info'); if(wifi)wifi.textContent='Mode: '+(d.wifiMode||'Offline');
    const ip=document.getElementById('wifi-ip'); if(ip)ip.textContent='IP: '+(d.ip||'--');
    const ws2=document.getElementById('wifi-ssid'); if(ws2)ws2.textContent=d.wifiSsid?('Saved router: '+d.wifiSsid+(d.apIp?(' | AP IP: '+d.apIp):'')):(d.apIp?('AP IP: '+d.apIp):'');
    const can=document.getElementById('can-info'); if(can)can.textContent='CAN: '+((d.modules||[]).find(m=>m.id===2)?.state||'UNKNOWN');
    const tx=document.getElementById('can-tx-pin'); if(tx && Number.isInteger(d.canTxPin)) tx.value=d.canTxPin;
    const rx=document.getElementById('can-rx-pin'); if(rx && Number.isInteger(d.canRxPin)) rx.value=d.canRxPin;
    const speed=document.getElementById('can-speed'); if(speed && d.canSpeed) speed.value=String(d.canSpeed);
    const listen=document.getElementById('can-listen-only'); if(listen) listen.checked=!!d.listenOnlyMode;
    const can1Cs=document.getElementById('can1-cs-pin'); if(can1Cs && Number.isInteger(d.can1CsPin)) can1Cs.value=d.can1CsPin;
    const can1Int=document.getElementById('can1-int-pin'); if(can1Int && Number.isInteger(d.can1IntPin)) can1Int.value=d.can1IntPin;
    const can1Speed=document.getElementById('can1-speed'); if(can1Speed && d.can1Speed) can1Speed.value=String(d.can1Speed);
    const can1Listen=document.getElementById('can1-listen-only'); if(can1Listen) can1Listen.checked=!!d.can1ListenOnly;
    const obdCanBus=document.getElementById('obd-can-bus'); if(obdCanBus && (d.obdCanBus===0 || d.obdCanBus===1)) obdCanBus.value=String(d.obdCanBus);
    const vehicleCanBus=document.getElementById('vehicle-can-bus'); if(vehicleCanBus && (d.vehicleCanBus===0 || d.vehicleCanBus===1)) vehicleCanBus.value=String(d.vehicleCanBus);
    const learnCanBus=document.getElementById('learn-can-bus'); if(learnCanBus && (d.learnCanBus===0 || d.learnCanBus===1)) learnCanBus.value=String(d.learnCanBus);
    updateStorageHealth(d);
    updateModuleStatusList(d.modules);
  }catch(e){
    console.error('Device status refresh failed:', e);
    const storage=document.getElementById('storage-health');
    if(storage) storage.textContent='Storage status unavailable';
  }
}

function formatStorageBytes(bytes) {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value < 0) return 'unknown';
    if (value < 1024) return `${value} B`;
    const units = ['KB', 'MB', 'GB'];
    let size = value / 1024;
    let unit = units[0];
    for (let i = 1; size >= 1024 && i < units.length; ++i) {
        size /= 1024;
        unit = units[i];
    }
    return `${size.toFixed(1)} ${unit}`;
}

function updateStorageHealth(data) {
    const element = document.getElementById('storage-health');
    const choice = document.getElementById('dbc-storage-choice');
    if (!element) return;
    const internal = data.internalStorage || {};
    const sd = data.sd || {};
    const selected = data.storageChoice && data.storageChoice.db;
    if (['auto', 'internal', 'sd'].includes(selected)) {
        const changed = dbcStorageChoiceSaved !== selected;
        dbcStorageChoiceSaved = selected;
        if (changed && dbcProfilesCache.length) {
            populateDbcProfileSelectors(dbcProfilesCache, dbcFilesCache);
        }
    }
    if (choice && choice.dataset.pending !== '1' && ['auto', 'internal', 'sd'].includes(selected)) {
        choice.value = selected;
    }
    const storageChoices = data.storageChoice || {};
    [
        ['prof', 'profile-storage-choice'],
        ['rec', 'recording-storage-choice'],
        ['bak', 'backup-storage-choice']
    ].forEach(([category, id]) => {
        const select = document.getElementById(id);
        const value = storageChoices[category];
        if (select && select.dataset.pending !== '1' && ['auto', 'internal', 'sd'].includes(value)) {
            select.value = value;
        }
    });
    const internalText = `Internal ${internal.state || 'unknown'}: ${formatStorageBytes(internal.freeBytes)} free / ${formatStorageBytes(internal.totalBytes)}`;
    const sdText = `SD ${sd.state || 'unknown'}: ${formatStorageBytes(sd.freeBytes)} free / ${formatStorageBytes(sd.totalBytes)}`;
    element.textContent = `${internalText} | ${sdText} | DBC: ${selected || 'auto'} | Profiles: ${storageChoices.prof || 'auto'} | Recordings: ${storageChoices.rec || 'auto'} | Backups: ${storageChoices.bak || 'auto'}`;
}

async function dbcRequestJson(url, options) {
    const response = await fetch(url, { credentials: 'same-origin', ...options });
    let result;
    try {
        result = await response.json();
    } catch (error) {
        throw new Error(`Server returned an invalid response (${response.status})`);
    }
    if (!response.ok) throw new Error(result.error || `Request failed (${response.status})`);
    return result;
}

function setDbcManagerStatus(message, isError) {
    const element = document.getElementById('dbc-manager-status');
    if (!element) return;
    element.textContent = message;
    element.style.color = isError ? '#e74c3c' : '#aaa';
}

let dbcFilesCache = [];
let dbcProfilesCache = [];
let dbcStorageChoiceSaved = 'auto';

function populateDbcProfileSelectors(profiles, files) {
    const profileSelect = document.getElementById('dbc-profile-select');
    const fileSelect = document.getElementById('dbc-profile-file');
    const saveButton = document.getElementById('btn-dbc-profile-save');
    if (!profileSelect || !fileSelect || !saveButton) return;
    const previousId = profileSelect.value;
    profileSelect.replaceChildren();
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = profiles.length ? 'Select a custom profile' : 'No custom profiles';
    profileSelect.appendChild(placeholder);
    profiles.forEach((profile) => {
        const option = document.createElement('option');
        option.value = String(profile.id);
        option.textContent = [profile.name, profile.brand, profile.model].filter(Boolean).join(' — ');
        profileSelect.appendChild(option);
    });
    if (profiles.some((profile) => String(profile.id) === previousId)) {
        profileSelect.value = previousId;
    } else if (profiles.length) {
        profileSelect.value = String(profiles[0].id);
    }

    function updateFileOptions() {
        const selectedProfile = profiles.find((profile) => String(profile.id) === profileSelect.value);
        const currentName = selectedProfile && selectedProfile.dbcFile ? selectedProfile.dbcFile : '';
        fileSelect.replaceChildren();
        const none = document.createElement('option');
        none.value = '';
        none.textContent = 'No DBC';
        fileSelect.appendChild(none);

        const userFiles = files.filter((file) => !file.builtin);
        const preferredLocation = dbcStorageChoiceSaved === 'sd' ? 'sd' : 'internal';
        const byName = new Map();
        userFiles.forEach((file) => {
            if (!byName.has(file.name) || file.location === preferredLocation) byName.set(file.name, file);
        });
        byName.forEach((file, name) => {
            const option = document.createElement('option');
            option.value = name;
            option.textContent = `${name} (${file.location})`;
            fileSelect.appendChild(option);
        });
        if (currentName && !byName.has(currentName)) {
            const missing = document.createElement('option');
            missing.value = currentName;
            missing.textContent = `${currentName} (unavailable — select No DBC to clear)`;
            fileSelect.appendChild(missing);
        }
        fileSelect.value = currentName;
        fileSelect.disabled = profiles.length === 0;
        saveButton.disabled = profiles.length === 0;
    }

    profileSelect.disabled = profiles.length === 0;
    profileSelect.onchange = updateFileOptions;
    updateFileOptions();
}

function renderDbcFiles(files, truncated) {
    const container = document.getElementById('dbc-file-list');
    if (!container) return;
    container.replaceChildren();
    files.forEach((file) => {
        const row = document.createElement('div');
        row.className = 'dbc-file-row';
        const title = document.createElement('strong');
        title.textContent = `${file.name}${file.builtin ? ' (built-in)' : ''}`;
        row.appendChild(title);

        const details = document.createElement('span');
        details.textContent = `${file.location} · ${formatStorageBytes(file.size)} · ${file.messages} messages`;
        row.appendChild(details);
        const provenance = document.createElement('span');
        provenance.textContent = `Source: ${file.source || 'unknown'} · License: ${file.license || 'unverified'}`;
        row.appendChild(provenance);
        const actions = document.createElement('div');
        actions.className = 'dbc-file-actions';

        const exportLink = document.createElement('a');
        const downloadQuery = new URLSearchParams({ name: file.name, location: file.location });
        exportLink.href = `/api/dbc/download?${downloadQuery.toString()}`;
        exportLink.textContent = 'Export';
        exportLink.setAttribute('download', file.name);
        actions.appendChild(exportLink);

        if (!file.builtin) {
            const removeButton = document.createElement('button');
            removeButton.type = 'button';
            removeButton.textContent = 'Delete';
            removeButton.addEventListener('click', async () => {
                if (!window.confirm(`Delete ${file.name} from ${file.location}? This cannot be undone.`)) return;
                removeButton.disabled = true;
                try {
                    const body = new URLSearchParams({
                        name: file.name,
                        location: file.location,
                        confirm: '1'
                    });
                    await dbcRequestJson('/api/dbc/delete', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: body.toString()
                    });
                    setDbcManagerStatus(`${file.name} deleted.`, false);
                    await refreshDbcManager();
                } catch (error) {
                    setDbcManagerStatus(error.message, true);
                    removeButton.disabled = false;
                }
            });
            actions.appendChild(removeButton);
        }
        row.appendChild(actions);
        container.appendChild(row);
    });
    if (truncated) {
        const note = document.createElement('p');
        note.className = 'muted';
        note.textContent = 'The DBC list is truncated; not every stored file is shown.';
        container.appendChild(note);
    }
}

async function refreshDbcManager() {
    try {
        const [dbcResult, profileResult] = await Promise.all([
            dbcRequestJson('/api/dbc/list'),
            dbcRequestJson('/api/vehicles/custom')
        ]);
        const files = Array.isArray(dbcResult.files) ? dbcResult.files : [];
        const profiles = Array.isArray(profileResult.profiles) ? profileResult.profiles : [];
        dbcFilesCache = files;
        dbcProfilesCache = profiles;
        renderDbcFiles(files, !!dbcResult.truncated);
        populateDbcProfileSelectors(profiles, files);
        setDbcManagerStatus(`${files.length} DBC file(s) listed.`, false);
    } catch (error) {
        console.error('DBC manager refresh failed:', error);
        setDbcManagerStatus(error.message, true);
    }
}

function setupDbcManager() {
    const storageChoice = document.getElementById('dbc-storage-choice');
    const storageButton = document.getElementById('btn-dbc-storage-save');
    const profileButton = document.getElementById('btn-dbc-profile-save');
    const uploadButton = document.getElementById('btn-dbc-upload');
    if (!storageChoice || !storageButton || !profileButton || !uploadButton) return;

    storageChoice.addEventListener('change', () => {
        storageChoice.dataset.pending = '1';
    });
    storageButton.addEventListener('click', async () => {
        storageButton.disabled = true;
        try {
            const body = new URLSearchParams({ category: 'db', choice: storageChoice.value });
            await dbcRequestJson('/api/storage', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: body.toString()
            });
            storageChoice.dataset.pending = '0';
            setDbcManagerStatus('DBC storage preference saved.', false);
            await refreshDeviceInfo();
        } catch (error) {
            setDbcManagerStatus(error.message, true);
        } finally {
            storageButton.disabled = false;
        }
    });

    profileButton.addEventListener('click', async () => {
        const profileId = document.getElementById('dbc-profile-select').value;
        if (!profileId) {
            setDbcManagerStatus('Select a custom profile first.', true);
            return;
        }
        profileButton.disabled = true;
        try {
            const body = new URLSearchParams({
                id: profileId,
                dbcFile: document.getElementById('dbc-profile-file').value
            });
            await dbcRequestJson('/api/vehicles/custom/set-dbc', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: body.toString()
            });
            setDbcManagerStatus('Profile DBC setting saved.', false);
            await refreshDbcManager();
        } catch (error) {
            setDbcManagerStatus(error.message, true);
        } finally {
            profileButton.disabled = false;
        }
    });

    uploadButton.addEventListener('click', async () => {
        const input = document.getElementById('dbc-upload-file');
        const file = input && input.files && input.files[0];
        if (!file) {
            setDbcManagerStatus('Choose a .dbc file first.', true);
            return;
        }
        uploadButton.disabled = true;
        try {
            const form = new FormData();
            form.append('file', file, file.name);
            const result = await dbcRequestJson('/api/dbc/upload', { method: 'POST', body: form });
            setDbcManagerStatus(`Uploaded ${result.name} to ${result.location}.`, false);
            input.value = '';
            await Promise.all([refreshDbcManager(), refreshDeviceInfo()]);
        } catch (error) {
            setDbcManagerStatus(error.message, true);
        } finally {
            uploadButton.disabled = false;
        }
    });

    refreshDbcManager();
}

function setupStorageSettings() {
    const status = document.getElementById('storage-manager-status');
    document.querySelectorAll('[data-storage-category]').forEach((select) => {
        select.addEventListener('change', () => {
            select.dataset.pending = '1';
        });
    });
    document.querySelectorAll('[data-storage-save]').forEach((button) => {
        button.addEventListener('click', async () => {
            const category = button.dataset.storageSave;
            const select = document.querySelector(`[data-storage-category="${category}"]`);
            if (!select || !['prof', 'rec', 'bak'].includes(category)) return;
            button.disabled = true;
            try {
                const body = new URLSearchParams({ category, choice: select.value });
                await dbcRequestJson('/api/storage', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: body.toString()
                });
                select.dataset.pending = '0';
                if (status) status.textContent = 'Storage preference saved.';
                await refreshDeviceInfo();
            } catch (error) {
                if (status) status.textContent = error.message;
            } finally {
                button.disabled = false;
            }
        });
    });
    const reset = document.getElementById('btn-storage-reset');
    if (reset) {
        reset.addEventListener('click', async () => {
            reset.disabled = true;
            try {
                await dbcRequestJson('/api/storage', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: new URLSearchParams({ reset: '1' }).toString()
                });
                document.querySelectorAll('[data-storage-category]').forEach((select) => {
                    select.dataset.pending = '0';
                });
                const dbcChoice = document.getElementById('dbc-storage-choice');
                if (dbcChoice) dbcChoice.dataset.pending = '0';
                if (status) status.textContent = 'All storage choices reset to automatic.';
                await refreshDeviceInfo();
            } catch (error) {
                if (status) status.textContent = error.message;
            } finally {
                reset.disabled = false;
            }
        });
    }
}


function setupCanConfigForm(){
  const form=document.getElementById('can-config-form');
  if(!form)return;
  form.addEventListener('submit',async function(e){
    e.preventDefault();
    const msg=document.getElementById('can-config-msg');
    const body=new URLSearchParams();
    body.append('txPin',document.getElementById('can-tx-pin').value);
    body.append('rxPin',document.getElementById('can-rx-pin').value);
    body.append('speed',document.getElementById('can-speed').value);
    body.append('listenOnly',document.getElementById('can-listen-only').checked?'true':'false');
    body.append('can1CsPin',document.getElementById('can1-cs-pin').value);
    body.append('can1IntPin',document.getElementById('can1-int-pin').value);
    body.append('can1Speed',document.getElementById('can1-speed').value);
    body.append('can1ListenOnly',document.getElementById('can1-listen-only').checked?'true':'false');
    body.append('obdCanBus',document.getElementById('obd-can-bus').value);
    body.append('learnCanBus',document.getElementById('learn-can-bus').value);
    body.append('vehicleCanBus',document.getElementById('vehicle-can-bus').value);
    try{
      const res=await fetch('/api/can-config',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
      const data=await res.json();
      msg.textContent=data.message||data.error||'CAN settings update failed';
      msg.style.color=data.success?'#2ecc71':'#e74c3c';
    }catch(err){
      msg.textContent='Server communication error';
      msg.style.color='#e74c3c';
    }
  });
}

function updateCanMonitorStatus(text) {
    const status = document.getElementById('can-monitor-status');
    if (status) status.textContent = text;
}

function appendCanMonitorFrame(frame) {
    const output = document.getElementById('can-monitor-frames');
    if (!output || !frame || typeof frame.id !== 'number' || !Array.isArray(frame.data)) return;
    const width = frame.extended ? 8 : 3;
    const id = `0x${frame.id.toString(16).toUpperCase().padStart(width, '0')}`;
    const bytes = frame.remote ? 'RTR' : frame.data
        .slice(0, Math.min(8, frame.length))
        .map(value => Number(value).toString(16).toUpperCase().padStart(2, '0'))
        .join(' ');
    const dropped = Number(frame.dropped) || 0;
    canMonitorLines.push(`${frame.timestamp} ${frame.bus} ${frame.extended ? 'EXT' : 'STD'} ${id} [${frame.length}] ${bytes}${dropped ? ` (dropped: ${dropped})` : ''}`);
    if (canMonitorLines.length > 100) canMonitorLines.shift();
    output.textContent = canMonitorLines.join('\n');
    output.scrollTop = output.scrollHeight;
    if (dropped) updateCanMonitorStatus(`Listening on ${frame.bus}; ${dropped} frame(s) dropped`);
}

function setupCanMonitor() {
    const start = document.getElementById('btn-can-monitor-start');
    const stop = document.getElementById('btn-can-monitor-stop');
    const bus = document.getElementById('can-monitor-bus');
    if (!start || !stop || !bus) return;
    start.addEventListener('click', () => {
        canMonitorBus = Number(bus.value);
        canMonitorLines = [];
        document.getElementById('can-monitor-frames').textContent = '';
        sendCommand2('can_monitor_start', { bus: canMonitorBus });
    });
    stop.addEventListener('click', () => sendCommand2('can_monitor_stop', {}));
}

function updateCanRecorderStatus(data) {
    const status = document.getElementById('can-record-status');
    const download = document.getElementById('can-record-download');
    if (!status) return;
    if (data.message) {
        status.textContent = data.message;
        return;
    }
    const active = !!data.active;
    const mask = Number(data.busMask) || 0;
    const buses = [mask & 1 ? 'CAN1' : '', mask & 2 ? 'CAN2' : ''].filter(Boolean).join(' + ');
    status.textContent = active
        ? `Recording ${buses}: ${Number(data.frames) || 0} frames, ${Number(data.dropped) || 0} dropped`
        : (data.error || `Stopped: ${Number(data.frames) || 0} frames, ${Number(data.dropped) || 0} dropped`);
    const file = String(data.file || '');
    if (download && /^\/can\d{4}\.csv$/.test(file)) {
        download.href = `/api/recordings/download?name=${encodeURIComponent(file.slice(1))}`;
        download.textContent = `Download ${file.slice(1)}`;
        download.hidden = false;
    }
}

async function loadCanRecordings() {
    const list = document.getElementById('can-record-files');
    if (!list) return;
    try {
        const response = await fetch('/api/recordings', { credentials: 'same-origin' });
        if (!response.ok) return;
        const data = await response.json();
        const rows = (Array.isArray(data.recordings) ? data.recordings : []).map(recording => {
            if (!/^can\d{4}\.csv$/.test(recording.name)) return document.createTextNode('');
            const link = document.createElement('a');
            link.href = `/api/recordings/download?name=${encodeURIComponent(recording.name)}`;
            link.textContent = `${recording.name} (${Number(recording.bytes) || 0} bytes)`;
            link.download = recording.name;
            const row = document.createElement('p');
            const remove = document.createElement('button');
            remove.type = 'button';
            remove.textContent = 'Delete';
            remove.addEventListener('click', () => {
                if (window.confirm(`Delete ${recording.name}? This cannot be undone.`)) {
                    sendCommand2('can_record_delete', { name: recording.name });
                }
            });
            row.append(link, ' ', remove);
            return row;
        });
        list.replaceChildren(...rows);
        if (data.truncated) list.append(document.createTextNode('List limited to 50 recordings'));
    } catch (_) {
        list.textContent = 'Recording list unavailable';
    }
}

function setupCanRecorder() {
    const bus = document.getElementById('can-record-bus');
    const start = document.getElementById('btn-can-record-start');
    const stop = document.getElementById('btn-can-record-stop');
    if (!bus || !start || !stop) return;
    start.addEventListener('click', () => sendCommand2('can_record_start', { bus: Number(bus.value) }));
    stop.addEventListener('click', () => sendCommand2('can_record_stop', {}));
}

function updateObdDtcStatus(data) {
    const status = document.getElementById('dtc-status');
    const list = document.getElementById('dtc-list');
    const read = document.getElementById('btn-dtc-read');
    const clear = document.getElementById('btn-dtc-clear');
    if (!status) return;
    if (data.message) {
        status.textContent = data.message;
        return;
    }

    const state = Number(data.state);
    const operation = Number(data.operation);
    const stateNames = ['Idle', 'Reading DTCs', 'Receiving DTC continuation',
        'Waiting for ECU clear acknowledgement', 'Complete', 'Failed'];
    const busy = state >= 1 && state <= 3;
    if (read) read.disabled = busy;
    if (clear) clear.disabled = busy;
    if (state === 5) {
        const nrc = Number(data.responseCode) || 0;
        status.textContent = nrc
            ? `Failed (ECU response code 0x${nrc.toString(16).toUpperCase().padStart(2, '0')})`
            : `Failed (error ${Number(data.error) || 0})`;
    } else if (state === 4 && operation === 2) {
        status.textContent = 'ECU confirmed DTC clear';
    } else {
        status.textContent = stateNames[state] || 'Unknown state';
    }

    if (!list) return;
    const codes = Array.isArray(data.dtcs) ? data.dtcs : [];
    list.replaceChildren(...codes.map(code => {
        const item = document.createElement('li');
        item.textContent = `0x${Number(code).toString(16).toUpperCase().padStart(4, '0')}`;
        return item;
    }));
    if (state === 4 && operation === 1 && codes.length === 0) {
        const item = document.createElement('li');
        item.textContent = 'No stored DTCs';
        list.append(item);
    }
}

function setupObdDtc() {
    const read = document.getElementById('btn-dtc-read');
    const clear = document.getElementById('btn-dtc-clear');
    if (!read || !clear) return;
    read.addEventListener('click', () => sendCommand2('obd_dtc_read', {}));
    clear.addEventListener('click', () => {
        if (window.confirm('Clear stored diagnostic trouble codes from the ECU?')) {
            sendCommand2('obd_dtc_clear', {});
        }
    });
}

function updateModuleStatusList(modules) {
    const moduleList = document.getElementById('module-status-list');
    if (!moduleList || !Array.isArray(modules)) return;

    moduleList.replaceChildren(...modules.map(module => {
        const row = document.createElement('div');
        const name = document.createElement('span');
        const state = document.createElement('strong');
        row.className = 'module-status-row';
        name.textContent = module.name || 'Module';
        state.textContent = module.state || 'UNKNOWN';
        state.className = `module-state-${String(module.state || '').toLowerCase().replace(/[^a-z_-]/g, '')}`;
        row.append(name, state);
        return row;
    }));
}

function updateCanDiagnostics(data) {
    const interfaceName = data.interface === 'CAN2' ? 'CAN2' : 'CAN1';
    const diagnostics = document.getElementById(interfaceName === 'CAN2' ? 'can2-diagnostics' : 'can-diagnostics');
    if (!diagnostics) return;
    if (!data.driverReady) {
        diagnostics.textContent = `${interfaceName} unavailable`;
        return;
    }
    const bus = data.busOff ? 'BUS-OFF' : (data.busActive ? 'ACTIVE' : 'INACTIVE');
    const mode = data.listenOnly ? 'LISTEN-ONLY' : 'NORMAL';
    diagnostics.textContent = `${interfaceName} RX ${data.rxFrames ?? 0} | TX ${data.txFrames ?? 0} | Errors ${data.errorCount ?? 0} | ${bus} | ${mode}`;
}

document.addEventListener('DOMContentLoaded', function() {
    console.log('CarTouch Web UI loaded');
    showNotification('🚗 Connecting...');
    connectWebSocket();
    checkPasswordStatus();
    refreshDeviceInfo();
    setInterval(refreshDeviceInfo, 5000);
    setupPasswordForm();
    setupWifiForm();
    setupCanConfigForm();
    setupDbcManager();
    setupStorageSettings();
    setupCanMonitor();
    setupCanRecorder();
    setupObdDtc();
    setupLearnMode();
});

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Learn Mode logic
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■


// See README.md.
// ⚠️ This code never accesses CAN Bus directly; all actions go through
// authenticated WebSocket messages and the device enforces its own safety
// rules (Listen-Only during learning and VERIFIED before execution).
//

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Custom profile list (REST)
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

const LEARN_LABELS = [
    { value: 'lock_all', text: 'Lock all doors' },
    { value: 'unlock_all', text: 'Unlock all doors' },
    { value: 'unlock_driver', text: 'Unlock driver door' },
    { value: 'window_fl_up', text: 'Front-left window up' },
    { value: 'window_fl_down', text: 'Front-left window down' },
    { value: 'window_fr_up', text: 'Front-right window up' },
    { value: 'window_fr_down', text: 'Front-right window down' },
    { value: 'window_rl_up', text: 'Rear-left window up' },
    { value: 'window_rl_down', text: 'Rear-left window down' },
    { value: 'window_rr_up', text: 'Rear-right window up' },
    { value: 'window_rr_down', text: 'Rear-right window down' },
    { value: 'all_windows_up', text: 'All windows up' },
    { value: 'all_windows_down', text: 'All windows down' },
    { value: 'sunroof_open', text: 'Sunroof open' },
    { value: 'sunroof_close', text: 'Sunroof close' },
    { value: 'sunroof_tilt', text: 'Tilt sunroof' },
    { value: 'trunk_open', text: 'Open trunk' },
    { value: 'trunk_lock', text: 'Lock trunk' },
    { value: 'mirror_fold', text: 'Fold mirrors' },
    { value: 'mirror_unfold', text: 'Unfold mirrors' },
    { value: 'alarm_arm', text: 'Arm alarm' },
    { value: 'alarm_disarm', text: 'Disarm alarm' },
];

let currentLearnCandidates = [];
let selectedCandidateIndex = -1;
let currentLearnLabel = null;
let currentLearnDisplayName = null;
let activeProfileId = null;            // Profile currently used for learning/manual entry

function setupLearnMode() {
    // Fill command-label dropdowns.
    const wizardTitle = document.getElementById('learn-wizard-label-name');
    populateLabelSelect('learn-label-select');
    populateLabelSelect('manual-label-select');
    
    document.getElementById('btn-new-profile').addEventListener('click', createNewProfile);
    document.getElementById('btn-learn-start').addEventListener('click', startLearnWizard);
    document.getElementById('btn-manual-entry-open').addEventListener('click', openManualEntryModal);
    document.getElementById('btn-manual-save').addEventListener('click', submitManualEntry);
    document.getElementById('btn-manual-cancel').addEventListener('click', closeManualEntryModal);
    document.getElementById('btn-wizard-action').addEventListener('click', onWizardActionClick);
    document.getElementById('btn-wizard-cancel').addEventListener('click', closeLearnWizard);
    document.getElementById('btn-export-profile').addEventListener('click', exportSelectedProfile);
    document.getElementById('btn-import-profile').addEventListener('click', () => {
        document.getElementById('import-file-input').click();
    });
    document.getElementById('import-file-input').addEventListener('change', handleImportFile);
    
    document.getElementById('btn-verify-send').addEventListener('click', sendVerifyCommand);
    document.getElementById('btn-verify-success').addEventListener('click', () => confirmVerifyResult(true));
    document.getElementById('btn-verify-fail').addEventListener('click', () => confirmVerifyResult(false));
    document.getElementById('btn-verify-close').addEventListener('click', closeVerifyModal);
    
    refreshCustomVehicleList();
}

function populateLabelSelect(elementId) {
    const select = document.getElementById(elementId);
    if (!select) return;
    select.innerHTML = '';
    LEARN_LABELS.forEach(l => {
        const opt = document.createElement('option');
        opt.value = l.value;
        opt.textContent = l.text;
        select.appendChild(opt);
    });
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Export / Import
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

async function refreshCustomVehicleList() {
    try {
        const res = await fetch('/api/vehicles/custom', { credentials: 'same-origin' });
        if (!res.ok) return;
        const data = await res.json();
        if (Array.isArray(data.profiles)) {
            dbcProfilesCache = data.profiles;
            populateDbcProfileSelectors(data.profiles, dbcFilesCache);
        }

        const listEl = document.getElementById('learn-profile-list');
        const selectEl = document.getElementById('learn-active-profile-select');
        if (!listEl || !selectEl) return;
        
        listEl.innerHTML = '';
        selectEl.innerHTML = '<option value="">-- Select a profile for learning/manual entry --</option>';
        
        (data.profiles || []).forEach(p => {
            const item = document.createElement('div');
            item.className = 'profile-item';
            item.innerHTML = `
                <div>
                    <div class="profile-name">${escapeHtml(p.name)}</div>
                    <div class="profile-meta">${escapeHtml(p.brand || '')} ${escapeHtml(p.model || '')} - ${p.commandCount} Command</div>
                </div>
                <div class="profile-item-actions">
                    <button data-action="select" data-id="${p.id}">Activate</button>
                    <button data-action="commands" data-id="${p.id}">Commands</button>
                    <button data-action="delete" data-id="${p.id}">Delete</button>
                </div>
            `;
            listEl.appendChild(item);
            
            const opt = document.createElement('option');
            opt.value = p.id;
            opt.textContent = `${p.name} (${p.commandCount} Command)`;
            selectEl.appendChild(opt);
        });
        
        listEl.querySelectorAll('button[data-action="select"]').forEach(btn => {
            btn.addEventListener('click', () => selectActiveVehicle(btn.dataset.id));
        });
        listEl.querySelectorAll('button[data-action="delete"]').forEach(btn => {
            btn.addEventListener('click', () => deleteProfile(btn.dataset.id));
        });
        listEl.querySelectorAll('button[data-action="commands"]').forEach(btn => {
            btn.addEventListener('click', () => showProfileCommands(btn.dataset.id));
        });
    } catch (e) {
        console.warn('Failed to load profile list:', e);
    }
}

function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str || '';
    return div.innerHTML;
}

async function createNewProfile() {
    const name = prompt('Name for this vehicle (for example, My Car):');
    if (!name) return;
    
    try {
        const body = new URLSearchParams();
        body.append('name', name);
        const res = await fetch('/api/vehicles/custom/new', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json();
        if (data.success) {
            showNotification('✅ Profile created');
            refreshCustomVehicleList();
        } else {
            showNotification('❌ ' + (data.error || 'Profile creation failed'));
        }
    } catch (e) {
        console.error(e);
        showNotification('⚠️ Server communication error');
    }
}

async function selectActiveVehicle(id) {
    try {
        const body = new URLSearchParams();
        body.append('id', id);
        const res = await fetch('/api/vehicles/custom/select', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json();
        if (data.success) {
            showNotification('🚗 Vehicle selected as active');
            const banner = document.getElementById('active-vehicle-banner');
            if (banner) checkPasswordStatus();                                  // status includes activeVehicle too
        } else {
            showNotification('❌ Profile selection failed');
        }
    } catch (e) {
        console.error(e);
    }
}

async function deleteProfile(id) {
    if (!confirm('Are you sure? This profile and all learned commands will be deleted.')) return;
    
    try {
        const body = new URLSearchParams();
        body.append('id', id);
        const res = await fetch('/api/vehicles/custom/delete', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json();
        if (data.success) {
            showNotification('🗑 Profile deleted');
            refreshCustomVehicleList();
        }
    } catch (e) {
        console.error(e);
    }
}

// Display a profile command list with verification status and verification buttons.
async function showProfileCommands(id) {
    try {
        const res = await fetch('/api/vehicles/custom', { credentials: 'same-origin' });
        const data = await res.json();
        const profile = (data.profiles || []).find(p => String(p.id) === String(id));
        if (!profile) return;
        
        // Export is used to load the complete profile, not only the summary.
        const exportRes = await fetch(`/api/vehicles/custom/export?id=${id}`, { credentials: 'same-origin' });
        const fullProfile = await exportRes.json();
        
        let html = `<h3>Commands for “${escapeHtml(fullProfile.name)}”</h3>`;
        (fullProfile.commands || []).forEach(cmd => {
            const isVerified = cmd.status === 'verified';
            html += `
                <div class="command-row">
                    <span>${escapeHtml(cmd.displayName || cmd.label)}
                        <span class="${isVerified ? 'command-status-verified' : 'command-status-unverified'}">
                            ${isVerified ? '✅ Verified' : '⚠️ Unverified'}
                        </span>
                    </span>
                    ${!isVerified ? `<button class="command-verify-btn" data-label="${escapeHtml(cmd.label)}" data-canid="${cmd.canId}" data-canidhex="0x${cmd.canId.toString(16).toUpperCase()}" data-data="${(cmd.data||[]).join(',')}">Verify</button>` : ''}
                </div>
            `;
        });
        
        // Display in a simple modal using the same candidate-list structure.
        const listEl = document.getElementById('learn-wizard-candidates');
        document.getElementById('learn-wizard-label-name').textContent = fullProfile.name;
        document.getElementById('learn-wizard-status').innerHTML = html;
        document.getElementById('learn-wizard-progress-container').classList.add('hidden');
        listEl.classList.add('hidden');
        document.getElementById('btn-wizard-action').classList.add('hidden');
        document.getElementById('learn-wizard-modal').classList.remove('hidden');
        
        document.querySelectorAll('.command-verify-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                closeLearnWizard();
                const dataArr = btn.dataset.data ? btn.dataset.data.split(',').map(Number) : [];
                openVerifyModal(id, btn.dataset.label, btn.dataset.canidhex, dataArr);
            });
        });
    } catch (e) {
        console.error(e);
        showNotification('⚠️ Failed to display commands');
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Learn wizard (WebSocket)
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

async function exportSelectedProfile() {
    const selectEl = document.getElementById('learn-active-profile-select');
    const id = selectEl.value;
    if (!id) {
        showNotification('⚠️ Select a profile from the list first');
        return;
    }
    
    window.location.href = `/api/vehicles/custom/export?id=${id}`;
}

function handleImportFile(e) {
    const file = e.target.files[0];
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = async function(evt) {
        try {
            const jsonText = evt.target.result;
            // Perform minimal client-side validation before sending.
            JSON.parse(jsonText);
            
            const body = new URLSearchParams();
            body.append('json', jsonText);
            const res = await fetch('/api/vehicles/custom/import', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: body.toString()
            });
            const data = await res.json();
            if (data.success) {
                showNotification('✅ Profile imported (all commands marked unverified)');
                refreshCustomVehicleList();
            } else {
                showNotification('❌ ' + (data.error || 'Invalid file'));
            }
        } catch (err) {
            console.error(err);
            showNotification('⚠️ Invalid JSON file');
        }
    };
    reader.readAsText(file);
    e.target.value = '';        // Reset so selecting the same file again also works.
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ manual entry
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

function startLearnWizard() {
    const selectEl = document.getElementById('learn-active-profile-select');
    activeProfileId = selectEl.value;
    if (!activeProfileId) {
        showNotification('⚠️ Select or create a profile first');
        return;
    }
    
    const labelSelect = document.getElementById('learn-label-select');
    currentLearnLabel = labelSelect.value;
    currentLearnDisplayName = labelSelect.options[labelSelect.selectedIndex].text;
    
    selectedCandidateIndex = -1;
    currentLearnCandidates = [];
    
    document.getElementById('learn-wizard-label-name').textContent = currentLearnDisplayName;
    document.getElementById('learn-wizard-status').textContent =
        'In this mode the device only listens to CAN Bus traffic and sends no commands.';
    document.getElementById('learn-wizard-progress-container').classList.add('hidden');
    document.getElementById('learn-wizard-candidates').classList.add('hidden');
    document.getElementById('btn-wizard-action').classList.remove('hidden');
    document.getElementById('btn-wizard-action').textContent = '▶ Start Baseline Capture';
    document.getElementById('learn-wizard-modal').classList.remove('hidden');
    
    sendCommand2('learn_start', {
        label: currentLearnLabel,
        displayName: currentLearnDisplayName,
        profileId: parseInt(activeProfileId, 10)
    });
}

function closeLearnWizard() {
    document.getElementById('learn-wizard-modal').classList.add('hidden');
    document.getElementById('btn-wizard-action').classList.remove('hidden');
    sendCommand2('learn_cancel', {});
}

function onWizardActionClick() {
    const btn = document.getElementById('btn-wizard-action');
    const currentText = btn.textContent;
    
    if (currentText.includes('Start Baseline Capture')) {
        sendCommand2('learn_capture_baseline', {});
    } else if (currentText.includes('Start Action Capture')) {
        sendCommand2('learn_confirm_action', {});
    } else if (currentText.includes('Save')) {
        if (selectedCandidateIndex < 0) {
            showNotification('⚠️ Select a candidate first');
            return;
        }
        sendCommand2('learn_save', { candidateIndex: selectedCandidateIndex, profileId: parseInt(activeProfileId) });
    }
}

// Poll the state every 300 ms while the modal is open. Baseline/action
// capture is time-based and the server also pushes learn_state messages,
// so this polling is an additional reliability layer.
let learnPollTimer = null;
function startLearnPolling() {
    stopLearnPolling();
    learnPollTimer = setInterval(() => {
        if (!document.getElementById('learn-wizard-modal').classList.contains('hidden')) {
            sendCommand2('learn_get_state', {});
        } else {
            stopLearnPolling();
        }
    }, 300);
}
function stopLearnPolling() {
    if (learnPollTimer) clearInterval(learnPollTimer);
    learnPollTimer = null;
}

function handleLearnState(data) {
    const statusEl = document.getElementById('learn-wizard-status');
    const progressContainer = document.getElementById('learn-wizard-progress-container');
    const progressFill = document.getElementById('learn-wizard-progress-fill');
    const candidatesEl = document.getElementById('learn-wizard-candidates');
    const actionBtn = document.getElementById('btn-wizard-action');
    
    switch (data.state) {
        case 'baseline_capture':
            progressContainer.classList.remove('hidden');
            progressFill.style.width = (data.progress || 0) + '%';
            candidatesEl.classList.add('hidden');
            statusEl.textContent = '⏺ Capturing baseline... Do not press the vehicle button yet.';
            actionBtn.textContent = '⏳ Capturing...';
            startLearnPolling();
            break;
            
        case 'waiting_action':
            progressContainer.classList.add('hidden');
            statusEl.textContent = '✅ Baseline complete. When you click “Start Action Capture”, immediately press the physical vehicle button.';
            actionBtn.textContent = '▶ Start Action Capture';
            stopLearnPolling();
            break;
            
        case 'action_capture':
            progressContainer.classList.remove('hidden');
            progressFill.style.width = (data.progress || 0) + '%';
            statusEl.textContent = '🔴 Press the vehicle button now!';
            actionBtn.textContent = '⏳ Capturing...';
            startLearnPolling();
            break;
            
        case 'candidates_ready':
            progressContainer.classList.add('hidden');
            stopLearnPolling();
            currentLearnCandidates = data.candidates || [];
            statusEl.textContent = `${currentLearnCandidates.length} candidate(s) found. Select the candidate matching the physical button press:`;
            renderCandidates();
            actionBtn.textContent = '💾 Save Selected Candidate';
            break;
            
        case 'idle':
            stopLearnPolling();
            break;
    }
}

function renderCandidates() {
    const candidatesEl = document.getElementById('learn-wizard-candidates');
    candidatesEl.innerHTML = '';
    candidatesEl.classList.remove('hidden');
    
    currentLearnCandidates.forEach((c, idx) => {
        const item = document.createElement('div');
        item.className = 'candidate-item' + (selectedCandidateIndex === idx ? ' selected' : '');
        const dataHex = (c.data || []).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
        item.innerHTML = `
            <div>${escapeHtml(c.canIdHex)} <span class="candidate-tag">${c.isExtended ? '29-bit extended' : '11-bit standard'} · ${c.isNew ? '[New]' : '[Changed]'} (x${c.seenCount})</span></div>
            <div>${dataHex}</div>
        `;
        item.addEventListener('click', () => {
            selectedCandidateIndex = idx;
            renderCandidates();
            showNotification('✅ Candidate selected - click Save');
        });
        candidatesEl.appendChild(item);
    });
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Verify Command
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

function openManualEntryModal() {
    const selectEl = document.getElementById('learn-active-profile-select');
    activeProfileId = selectEl.value;
    if (!activeProfileId) {
        showNotification('⚠️ Select or create a profile first');
        return;
    }
    
    document.getElementById('manual-can-id').value = '';
    document.getElementById('manual-data-hex').value = '';
    document.getElementById('manual-extended').checked = false;
    document.getElementById('manual-entry-error').textContent = '';
    document.getElementById('manual-entry-modal').classList.remove('hidden');
}

function closeManualEntryModal() {
    document.getElementById('manual-entry-modal').classList.add('hidden');
}

async function submitManualEntry() {
    const errorEl = document.getElementById('manual-entry-error');
    const labelSelect = document.getElementById('manual-label-select');
    const label = labelSelect.value;
    const displayName = labelSelect.options[labelSelect.selectedIndex].text;
    const canIdStr = document.getElementById('manual-can-id').value.trim();
    const extended = document.getElementById('manual-extended').checked;
    const dataHex = document.getElementById('manual-data-hex').value.trim();
    
    if (!canIdStr) {
        errorEl.textContent = 'Enter a CAN ID';
        return;
    }
    
    const canId = parseInt(canIdStr, 16);
    const maxId = extended ? 0x1FFFFFFF : 0x7FF;
    if (isNaN(canId) || canId > maxId || canId < 0) {
        errorEl.textContent = 'CAN ID is invalid or out of range';
        return;
    }
    
    if (!dataHex) {
        errorEl.textContent = 'Enter at least one data byte';
        return;
    }
    
    try {
        const body = new URLSearchParams();
        body.append('profileId', activeProfileId);
        body.append('label', label);
        body.append('displayName', displayName);
        body.append('canId', canIdStr);
        body.append('extended', extended ? '1' : '0');
        body.append('dataHex', dataHex);
        
        const res = await fetch('/api/vehicles/custom/manual-add', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body.toString()
        });
        const data = await res.json();
        
        if (data.success) {
            closeManualEntryModal();
            showNotification('✅ Manual command saved (unverified)');
            refreshCustomVehicleList();
        } else {
            errorEl.textContent = data.error || 'Save failed';
        }
    } catch (e) {
        console.error(e);
        errorEl.textContent = 'Server communication error';
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// Shared WebSocket send helper
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

let verifyContext = { profileId: null, label: null, token: 0 };

function openVerifyModal(profileId, label, canIdHex, dataArr) {
    verifyContext = { profileId, label, token: 0 };
    
    const dataHexStr = (dataArr || []).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
    document.getElementById('verify-info-text').textContent =
        `By sending this command, the following message will be transmitted directly on the vehicle CAN Bus:\n\n` +
        `CAN ID: ${canIdHex}\nData: ${dataHexStr}\n\n` +
        `This action may have an immediate physical effect on the vehicle (for example, unlocking a door). ` +
        `Make sure the vehicle is in a safe state before continuing.\n\n` +
        `⚠️ Note: some newer vehicles using rolling codes may not respond to this method.`;
    document.getElementById('verify-result-text').textContent = '';
    document.getElementById('verify-modal').classList.remove('hidden');
}

function closeVerifyModal() {
    document.getElementById('verify-modal').classList.add('hidden');
}

function sendVerifyCommand() {
    if (!verifyContext.profileId || !verifyContext.label) return;
    // Any previous token is invalid until this send succeeds and the
    // server returns a fresh one in verify_sent.
    verifyContext.token = 0;
    sendCommand2('verify_command', {
        vehicleId: parseInt(verifyContext.profileId),
        label: verifyContext.label
    });
}

function handleVerifySent(data) {
    const resultEl = document.getElementById('verify-result-text');
    if (data.success) {
        // The server opens a pending-verification transaction bound to
        // this token/profile/label; verify_confirm must echo it back so an
        // arbitrary confirmation cannot mark a command VERIFIED.
        verifyContext.token = data.verifyToken || 0;
        resultEl.textContent = '✅ Command sent. Did the vehicle respond correctly?';
    } else {
        verifyContext.token = 0;
        resultEl.textContent = '❌ Send failed: ' + (data.error || '');
    }
}

function confirmVerifyResult(success) {
    if (!verifyContext.profileId || !verifyContext.label) return;
    sendCommand2('verify_confirm', {
        vehicleId: parseInt(verifyContext.profileId),
        label: verifyContext.label,
        success: success,
        verifyToken: verifyContext.token || 0
    });
    showNotification(success ? '✅ Command verified' : '⚠️ Command remains unverified');
    closeVerifyModal();
    refreshCustomVehicleList();
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// This helper is for Learn Mode WebSocket messages, not direct CAN access.
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// The same authentication/connection checks are used for arbitrary Learn Mode payloads.
//

function sendCommand2(type, payload) {
    if (!isConnected || !isAuthenticated || !ws || ws.readyState !== WebSocket.OPEN) {
        showNotification('⚠️ Not connected');
        return;
    }
    
    const msg = Object.assign({ type: type }, payload);
    ws.send(JSON.stringify(msg));
}
