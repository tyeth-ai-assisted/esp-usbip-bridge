/*
 * USB/IP Bridge — browser-side script for the status page.
 *
 * This file is embedded verbatim into the served HTML (see
 * EMBED_TXTFILES in main/CMakeLists.txt and stream_html_page() in
 * http_server.c).  Keep it self-contained: it must run in a plain
 * <script> tag with no module loader and no build step.
 *
 * Validate syntax with:  node --check main/web_app.js
 */
'use strict';

/* Refresh all GPIO pin values, wire labels, directions and pull states. */
function rp() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins', true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    r.pins.forEach(function (p) {
      var e = document.getElementById('v' + p.name);
      if (e) {
        e.textContent = p.value ? 'HIGH' : 'LOW';
        e.className = p.value ? 'hi' : 'lo';
      }
      var w = document.getElementById('w' + p.name);
      if (w) {
        w.value = p.wire;
        w.onchange = function () { sw(this); };
      }
      var d = document.getElementById('d' + p.name);
      if (d) { d.value = p.dir; }
    });
    if (r.pulls) {
      r.pulls.forEach(function (p) {
        var s = document.getElementById('pl' + p.header);
        if (s) { s.value = p.state; }
      });
    }
  };
  x.send();
}

/* Set a header-pin pull resistor via its IO-expander line. */
function spul(sel) {
  var exp = sel.getAttribute('data-exp');
  var v = sel.value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + exp, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  var b;
  if (v == 'up') { b = '{"direction":"out","value":1}'; }
  else if (v == 'down') { b = '{"direction":"out","value":0}'; }
  else { b = '{"direction":"in"}'; }
  x.send(b);
}

/* Switch between the GPIO-pins and USB-devices tabs. */
function st(n) {
  ['p', 'u', 'h', 'm'].forEach(function (x) {
    document.getElementById('t' + x).className = '';
    document.getElementById('s' + x).className = 'sec';
  });
  document.getElementById('t' + n).className = 'on';
  document.getElementById('s' + n).className = 'sec on';
}

/* Change a pin's direction (IN / OUT). */
function sd(p) {
  var d = document.getElementById('d' + p).value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.send(JSON.stringify({ direction: d }));
}

/* Toggle a pin's output level. */
function tg(p) {
  var x = new XMLHttpRequest();
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.open('POST', '/api/pins/' + p + '/toggle', true);
  x.send();
}

/* Read a single pin and update its cell. */
function rr(p) {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins/' + p, true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    var e = document.getElementById('v' + p);
    if (e) {
      e.textContent = r.value ? 'HIGH' : 'LOW';
      e.className = r.value ? 'hi' : 'lo';
    }
  };
  x.send();
}

/* Save a DUT wire name typed into a pin row.  Updates only the saved
   pin's value display from the response instead of calling rp(), which
   would overwrite every wire input field and could clear a name the user
   is still typing in another row. */
function sw(elem) {
  var p = elem.id.replace(/^w/, '');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      var r = JSON.parse(x.responseText);
      var e = document.getElementById('v' + p);
      if (e) {
        e.textContent = r.value ? 'HIGH' : 'LOW';
        e.className = r.value ? 'hi' : 'lo';
      }
    }
  };
  x.send(JSON.stringify({ wire: elem.value }));
}

/* Hostname editing inline UI. */
function he() {
  var h = document.getElementById('hostname');
  var inp = document.getElementById('hn-input');
  inp.value = h.textContent;
  h.style.display = 'none';
  inp.style.display = 'inline';
  document.getElementById('hn-edit').style.display = 'none';
  document.getElementById('hn-save').style.display = 'inline';
  document.getElementById('hn-cancel').style.display = 'inline';
  inp.focus();
  inp.select();
}

function hs() {
  var inp = document.getElementById('hn-input');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/hostname', true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      document.getElementById('hostname').textContent = inp.value;
      hc();
    }
  };
  x.send(JSON.stringify({ hostname: inp.value }));
}

function hc() {
  document.getElementById('hostname').style.display = 'inline';
  document.getElementById('hn-input').style.display = 'none';
  document.getElementById('hn-edit').style.display = 'inline';
  document.getElementById('hn-save').style.display = 'none';
  document.getElementById('hn-cancel').style.display = 'none';
}

/* Board ID editing inline UI. */
function be() {
  var h = document.getElementById('board_id');
  var inp = document.getElementById('bi-input');
  inp.value = h.textContent;
  h.style.display = 'none';
  inp.style.display = 'inline';
  document.getElementById('bi-edit').style.display = 'none';
  document.getElementById('bi-save').style.display = 'inline';
  document.getElementById('bi-cancel').style.display = 'inline';
  inp.focus();
  inp.select();
}

function bs() {
  var inp = document.getElementById('bi-input');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/board_id', true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      document.getElementById('board_id').textContent = inp.value;
      bc();
    }
  };
  x.send(JSON.stringify({ board_id: inp.value }));
}

function bc() {
  document.getElementById('board_id').style.display = 'inline';
  document.getElementById('bi-input').style.display = 'none';
  document.getElementById('bi-edit').style.display = 'inline';
  document.getElementById('bi-save').style.display = 'none';
  document.getElementById('bi-cancel').style.display = 'none';
}

/* Config export: GET /api/config and download as <hostname>-config.json. */
function cfgExport() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/config', true);
  x.onload = function () {
    if (x.status != 200) return;
    var b = new Blob([x.responseText], { type: 'application/json' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    var hn = 'usbip';
    try { hn = JSON.parse(x.responseText).hostname || 'usbip'; } catch (e) {}
    a.download = hn + '-config.json';
    document.body.appendChild(a);
    a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 0);
  };
  x.send();
}

/* Config import: create a temporary file input, click it, and POST
   the selected file to /api/config. */
function cfgImport() {
  var inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = 'application/json,.json';
  inp.style.display = 'none';
  inp.onchange = function () {
    if (!inp.files || !inp.files[0]) return;
    var f = inp.files[0];
    var fr = new FileReader();
    fr.onload = function () {
      var x = new XMLHttpRequest();
      x.open('POST', '/api/config', true);
      x.setRequestHeader('Content-Type', 'application/json');
      x.onload = function () {
        if (x.status != 200) {
          alert('Import failed: HTTP ' + x.status);
          return;
        }
        var r;
        try { r = JSON.parse(x.responseText); } catch (e) {}
        if (r && r.error) {
          alert('Import failed: ' + r.error);
          return;
        }
        var msg = 'Config imported.';
        if (r) {
          msg = 'Config imported (' + r.applied + ' wires applied, ' +
                r.skipped + ' skipped).';
        }
        alert(msg + ' Reloading...');
        location.reload();
      };
      x.send(fr.result);
    };
    fr.readAsText(f);
  };
  document.body.appendChild(inp);
  inp.click();
  setTimeout(function () { inp.remove(); }, 100);
}

/* Hold-to-confirm clear config: user must hold the button for 3 seconds. */
var cfgClearTimer = null;
var cfgClearStartTime = 0;
var cfgClearHeld = false;
var CLEAR_HOLD_MS = 3000;

function cfgClearStart(e) {
  if (cfgClearTimer) return;
  /* Prevent default to avoid text selection etc. */
  e.preventDefault();
  cfgClearHeld = true;
  cfgClearStartTime = Date.now();
  var btn = document.getElementById('cfgClearBtn');
  if (btn) {
    btn.style.transition = 'none';
    btn.style.backgroundSize = '0% 100%';
    /* Force reflow so the 0-size takes effect before the transition starts. */
    btn.offsetHeight;
    btn.style.transition = 'background-size ' + CLEAR_HOLD_MS + 'ms linear';
    btn.style.backgroundSize = '100% 100%';
  }
  cfgClearTimer = setTimeout(function () {
    if (!cfgClearHeld) return;
    cfgClearHeld = false;
    cfgClearTimer = null;
    var x = new XMLHttpRequest();
    x.open('POST', '/api/config/clear', true);
    x.setRequestHeader('Content-Type', 'application/json');
    x.onload = function () {
      if (x.status == 200) {
        location.reload();
      } else {
        alert('Clear failed: HTTP ' + x.status);
        var btn2 = document.getElementById('cfgClearBtn');
        if (btn2) { btn2.style.transition = 'none'; btn2.style.backgroundSize = '0% 100%'; }
      }
    };
    x.onerror = function () {
      alert('Clear failed: network error');
      var btn2 = document.getElementById('cfgClearBtn');
      if (btn2) { btn2.style.transition = 'none'; btn2.style.backgroundSize = '0% 100%'; }
    };
    x.send();
  }, CLEAR_HOLD_MS);
}

function cfgClearCancel() {
  if (!cfgClearHeld) return;
  cfgClearHeld = false;
  if (cfgClearTimer) {
    clearTimeout(cfgClearTimer);
    cfgClearTimer = null;
  }
  var btn = document.getElementById('cfgClearBtn');
  if (btn) {
    btn.style.transition = 'none';
    btn.style.backgroundSize = '0% 100%';
  }
}

/* ---------------------------------------------------------------------
 *  API token: sent as a Bearer token on every non-GET request
 * ------------------------------------------------------------------- */
function tokGet() {
  try { return localStorage.getItem('usbipToken') || ''; } catch (e) { return ''; }
}

(function () {
  var open = XMLHttpRequest.prototype.open;
  var send = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (m) {
    this._m = m;
    return open.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function () {
    var t = tokGet();
    if (t && this._m && this._m.toUpperCase() != 'GET') {
      this.setRequestHeader('Authorization', 'Bearer ' + t);
    }
    return send.apply(this, arguments);
  };
})();

function tokSave() {
  try { localStorage.setItem('usbipToken', document.getElementById('tok').value); } catch (e) {}
  authStatus();
}

function tokSet() {
  var v = document.getElementById('tok').value;
  if (!confirm(v ? 'Require this token for changes on the bridge?' : 'Clear the bridge token (no auth)?')) return;
  api('POST', '/api/auth/token', { token: v }, function (ok, r) {
    if (!ok) { alert('Failed: ' + (r && r.error)); return; }
    tokSave();
  });
}

function authStatus() {
  api('GET', '/api/auth', null, function (ok, r) {
    var e = document.getElementById('authst');
    if (ok && e) e.textContent = r.auth_required ? '(bridge requires a token)' : '(no token required)';
  });
}

/* JSON request helper: cb(ok, body) */
function api(method, path, body, cb) {
  var x = new XMLHttpRequest();
  x.open(method, path, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    var r = null;
    try { r = JSON.parse(x.responseText); } catch (e) {}
    if (cb) cb(x.status >= 200 && x.status < 300, r, x.status);
  };
  x.onerror = function () { if (cb) cb(false, { error: 'network error' }, 0); };
  x.send(body === null || body === undefined ? null : JSON.stringify(body));
}

function esc(s) {
  return String(s === null || s === undefined ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

/* ---------------------------------------------------------------------
 *  USB devices and hub port power
 * ------------------------------------------------------------------- */
function forceOn() {
  var f = document.getElementById('force');
  return !!(f && f.checked);
}

function pw(port, action) {
  var body = { force: forceOn() };
  if (action == 'cycle') body.off_ms = 1000;
  api('POST', '/api/ports/' + encodeURIComponent(port) + '/' + action, body, function (ok, r) {
    if (!ok) alert(port + ' ' + action + ': ' + (r && r.error));
    setTimeout(refreshUsb, action == 'cycle' ? 2500 : 600);
  });
}

function allPorts(action) {
  if (!confirm('Switch ' + action + ' every per-port switched hub port?')) return;
  api('POST', '/api/ports/' + action, null, function (ok, r) {
    if (!ok) alert(r && r.error);
    setTimeout(refreshUsb, 1000);
  });
}

function powerButtons(port) {
  var p = esc(port);
  return '<button onclick="pw(\'' + p + '\',\'on\')">On</button>' +
         '<button onclick="pw(\'' + p + '\',\'off\')">Off</button>' +
         '<button onclick="pw(\'' + p + '\',\'cycle\')">Cycle</button>';
}

function loadDevs() {
  api('GET', '/api/usb/devices', null, function (ok, r) {
    var tb = document.getElementById('ud');
    if (!tb || !ok) return;
    var h = '';
    r.devices.forEach(function (d) {
      var pwr = d.virtual ? '<span class="muted">virtual</span>'
              : d.hub ? (esc(d.port_power_status || '?') + ' ' + powerButtons(d.busid))
              : '<span class="muted">root port</span>';
      h += '<tr><td><strong>' + esc(d.busid) + '</strong></td>' +
           '<td>' + esc(d.name) + '</td>' +
           '<td>' + esc(d.vid) + ':' + esc(d.pid) + '</td>' +
           '<td>' + esc(d.manufacturer) + ' ' + esc(d.product) + '</td>' +
           '<td>' + esc(d.serial) + '</td>' +
           '<td>' + esc(d.speed) + '</td>' +
           '<td>' + esc(d.max_power_ma) + '</td>' +
           '<td>' + pwr + '</td></tr>';
    });
    tb.innerHTML = h || '<tr><td colspan="8" class="muted">No USB devices</td></tr>';
  });
}

function showSettings(s) {
  if (!s) return;
  var e = document.getElementById('setEnf'), r = document.getElementById('setRst');
  if (e && document.activeElement !== e) e.checked = !!s.enforce_per_port_switching;
  if (r && document.activeElement !== r) r.checked = !!s.restore_port_power;
}

function saveSettings() {
  api('POST', '/api/settings', {
    enforce_per_port_switching: document.getElementById('setEnf').checked,
    restore_port_power: document.getElementById('setRst').checked
  }, function (ok, r) {
    if (!ok) alert('Settings: ' + (r && r.error));
    showSettings(r);
  });
}

function restorePorts() {
  api('POST', '/api/ports/restore', null, function (ok, r) {
    document.getElementById('rstst').textContent = ok ? (r.ports + ' port(s) restored') : (r && r.error);
    setTimeout(refreshUsb, 1000);
  });
}

function hubChars(c) {
  if (!c) return '';
  return 'wHubCharacteristics ' + esc(c.raw) + ': power switching <strong' +
         (c.power_switching == 'per-port' ? '' : ' class="warn"') + '>' + esc(c.power_switching) + '</strong>, ' +
         'over-current ' + esc(c.over_current_protection) + ', ' + (c.compound ? 'compound, ' : '') +
         'TT think ' + esc(c.tt_think_time_fs_bits) + ' FS bits, ' +
         (c.port_indicators ? 'port indicators, ' : '') +
         'PwrOn2PwrGood ' + esc(c.pwr_on_to_pwr_good_ms) + ' ms, controller ' + esc(c.hub_contr_current_ma) + ' mA';
}

function loadHubs() {
  api('GET', '/api/usb/hubs', null, function (ok, r) {
    var el = document.getElementById('hubs');
    if (!el || !ok) return;
    showSettings(r.settings);
    if (!r.hubs.length) { el.innerHTML = 'No hubs attached'; return; }
    var h = '';
    r.hubs.forEach(function (hub) {
      h += '<div class="hub"><strong>' + esc(hub.path) + '</strong> ' + esc(hub.vid) + ':' + esc(hub.pid) +
           ' ' + esc(hub.product) + ' &mdash; ' + esc(hub.num_ports) + ' ports<br>' + hubChars(hub.characteristics) + '</div>';
      h += '<table><thead><tr><th>Port</th><th>Power</th><th>Saved</th><th>Link</th><th>Speed</th><th>Attached</th><th>Action</th></tr></thead><tbody>';
      hub.ports.forEach(function (p) {
        var dev = p.device ? (esc(p.device.type) + ' ' + esc(p.device.vid) + ':' + esc(p.device.pid)) : '';
        h += '<tr><td>' + esc(p.path) + '</td>' +
             '<td class="' + (p.power == 'on' ? 'hi' : 'lo') + '">' + esc(p.power) + (p.user_off ? ' (switched off)' : '') +
             (p.over_current ? ' <span class="lo">OVER-CURRENT</span>' : '') + '</td>' +
             '<td>' + esc(p.desired) + (p.mismatch ? ' <span class="warn">mismatch</span>' : '') + '</td>' +
             '<td>' + (p.connected ? 'connected' : '<span class="muted">-</span>') + '</td>' +
             '<td>' + esc(p.speed) + '</td><td>' + dev + '</td>' +
             '<td>' + powerButtons(p.path) + '</td></tr>';
      });
      h += '</tbody></table>';
    });
    el.innerHTML = h;
  });
}

function refreshUsb() {
  loadDevs();
  loadHubs();
}

/* ---------------------------------------------------------------------
 *  I2C strand mux
 * ------------------------------------------------------------------- */
function muxErr(ok, r) {
  if (!ok) alert((r && r.error) || 'failed');
  loadMux();
}

function muxSel(dut) { api('POST', '/api/select', { dut: dut }, muxErr); }
function muxIso() { api('POST', '/api/isolate', null, muxErr); }
function muxCh(g, ch, closed) {
  api('POST', '/api/groups/' + encodeURIComponent(g) + '/channel', { channel: ch, closed: closed }, muxErr);
}
function muxGrpSel(g, ch) {
  api('POST', '/api/groups/' + encodeURIComponent(g) + '/select/' + ch, null, muxErr);
}

function loadMux() {
  api('GET', '/api/status', null, function (ok, r) {
    if (!ok) return;
    document.getElementById('mact').textContent = r.active || 'none (isolated)';
    document.getElementById('mman').textContent = r.manual ? '(manual)' : '';
    var h = '';
    r.duts.forEach(function (d) {
      h += '<button class="' + (d.active ? 'act' : '') + '" onclick="muxSel(\'' + esc(d.name) + '\')">' +
           esc(d.name) + ' <span class="muted">' + esc(d.group) + ':' + d.channel + '</span></button> ';
    });
    document.getElementById('mduts').innerHTML = h ? '<p class="sub">' + h + '</p>' : '<p class="sub muted">No DUTs in the topology</p>';
    var g = '';
    r.groups.forEach(function (grp) {
      g += '<div class="hub"><strong>' + esc(grp.name) + '</strong> ' + esc(grp.addresses.join(', ')) + '</div><p class="sub">';
      grp.channels.forEach(function (c) {
        g += '<label><input type="checkbox"' + (c.closed ? ' checked' : '') +
             ' onchange="muxCh(\'' + esc(grp.name) + '\',' + c.channel + ',this.checked)"> ch' + c.channel +
             (c.dut ? ' <span class="muted">' + esc(c.dut) + '</span>' : '') + '</label> ' +
             '<button onclick="muxGrpSel(\'' + esc(grp.name) + '\',' + c.channel + ')">only</button> &nbsp; ';
      });
      g += '</p>';
    });
    document.getElementById('mgroups').innerHTML = g;
  });
}

function muxProbe() {
  api('GET', '/api/probe', null, function (ok, r) {
    document.getElementById('mprobe').textContent = ok
      ? 'found: ' + (r.scan.join(', ') || 'nothing') : (r && r.error);
  });
}

function topoLoad() {
  api('GET', '/api/topology', null, function (ok, r) {
    if (ok) document.getElementById('mtopo').value = JSON.stringify(r, null, 2);
  });
}

function topoSave() {
  var t;
  try { t = JSON.parse(document.getElementById('mtopo').value); } catch (e) { alert('Invalid JSON: ' + e); return; }
  api('PUT', '/api/topology', t, function (ok, r) {
    document.getElementById('mtst').textContent = ok ? (r.saved ? 'saved' : ('applied, not saved: ' + r.note)) : (r && r.error);
    loadMux();
  });
}

/* Refresh pin values, wire names, directions and pull states on initial
   load.  Wire names are also pre-filled server-side in the HTML, but this
   keeps the live state (values/pulls) current too. */
rp();
document.getElementById('tok').value = tokGet();
authStatus();
refreshUsb();
loadMux();
topoLoad();
setInterval(function () {
  var u = document.getElementById('su'), hb = document.getElementById('sh');
  if ((u && u.className.indexOf('on') >= 0) || (hb && hb.className.indexOf('on') >= 0)) refreshUsb();
}, 3000);
