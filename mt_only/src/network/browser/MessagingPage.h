// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace mt::network::browser {
inline constexpr char MessagingCsp[] = "default-src 'none'; script-src 'sha256-tMOjy0UV1TfqNgVI08vdD5Z2zdUtkyKlTvTTwyVbTO8='; style-src 'sha256-vnvwT6PuD8F/09a5ymjAqwJqp3oFWjY8uxO2XVHJnx4='; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'";
inline constexpr char MessagingPage[] = R"ZTHTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Messages · ZephTastic</title>
<style>
:root{color-scheme:light dark;--paper:#f6f7f2;--card:white;--ink:#172821;--muted:#52645c;--line:#cbd4c9;--accent:#126049;--button:white}
@media(prefers-color-scheme:dark){:root{--paper:#14201b;--card:#1b2c24;--ink:#e4efe6;--muted:#b1c6b6;--line:#41594b;--accent:#9ce2bd;--button:#183222}}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.5 "DM Sans",system-ui,sans-serif}main{max-width:46rem;margin:auto;padding:clamp(1rem,5vw,3.5rem) 1rem}header{display:flex;align-items:center;justify-content:space-between;gap:1rem;border-bottom:1px solid var(--line);padding-bottom:1rem}h1{font-size:clamp(1.8rem,7vw,2.65rem);line-height:1.08;letter-spacing:-.04em;margin:.15rem 0}.brand,small,#connection{color:var(--muted);font-size:.87rem}.brand{font-weight:650}button,input,select,textarea{font:inherit}button{border:1px solid var(--line);background:var(--card);color:var(--ink);border-radius:.45rem;padding:.45rem .85rem;cursor:pointer}button:disabled{opacity:.5;cursor:default}button:focus-visible,select:focus-visible,input:focus-visible,textarea:focus-visible{outline:3px solid var(--accent);outline-offset:2px}.state{text-align:right}#connection{display:block}#reconnect{margin-top:.4rem}#messages{padding:0;margin:0;list-style:none;max-height:46vh;overflow:auto}#messages li{padding:.7rem 0;border-bottom:1px solid var(--line)}#messages small{display:block}#messages p{margin:.15rem 0;overflow-wrap:anywhere;white-space:pre-wrap}#empty{color:var(--muted);padding:1rem 0}form{background:var(--card);border:1px solid var(--line);border-radius:.75rem;padding:clamp(.9rem,4vw,1.4rem);margin-top:1.25rem}.fields{display:flex;gap:.8rem;flex-wrap:wrap}.fields label{flex:1 1 12rem}label{display:block;font-weight:600}input,select,textarea{width:100%;border:1px solid var(--line);border-radius:.35rem;background:var(--paper);color:var(--ink);padding:.5rem .6rem;margin:.25rem 0 .7rem}textarea{display:block;resize:vertical;min-height:4.5rem}.send{display:flex;align-items:center;justify-content:space-between;gap:.6rem}.send button{color:var(--button);background:var(--accent);border-color:transparent;padding:.5rem 1.1rem;font-weight:650}#feedback{min-height:1.25em;margin:.65rem 0 0;color:var(--muted);font-size:.92rem}[hidden]{display:none!important}
</style></head><body><main>
<header><div><span class="brand">ZephTastic</span><h1>Messages</h1></div><div class="state"><span id="connection" role="status">Connecting…</span><button id="reconnect" type="button" hidden>Reconnect</button></div></header>
<section aria-label="Message history"><p id="empty">Messages received during this session will appear here.</p><ul id="messages" aria-live="polite" aria-relevant="additions"></ul></section>
<form id="compose"><div class="fields"><label>Channel<select id="channel" required disabled></select></label>
<label>Recipient<input id="recipient" placeholder="Broadcast" aria-describedby="destination-help" maxlength="9" autocomplete="off" spellcheck="false"></label></div>
<small id="destination-help">Leave the recipient blank to broadcast, or enter a node ID such as !1234abcd.</small>
<label for="message">Message</label><textarea id="message" required rows="3"></textarea>
<div class="send"><small id="count">0 / 233 bytes</small><button id="send" type="submit" disabled>Send message</button></div>
<p id="feedback" role="status"></p></form></main>
<script>
'use strict';
const byId = id => document.getElementById(id);
const channels = byId('channel'), draft = byId('message'), historyList = byId('messages');
const encoder = new TextEncoder(), decoder = new TextDecoder();
const sent = new Map(), seen = new Set(), channelNames = new Map();
let generation = 0, nonce = 0, self = 0, ready = false, sending = false;
function varint(value) {
  const bytes = [];
  while (value >= 128) { bytes.push((value & 127) | 128); value = Math.floor(value / 128); }
  bytes.push(value);
  return bytes;
}
const uint = (tag, value) => [...varint(tag * 8), ...varint(value)];
const fixed = (tag, value) => [tag * 8 + 5, value & 255, (value >>> 8) & 255, (value >>> 16) & 255, value >>> 24];
const nested = (tag, bytes) => [...varint(tag * 8 + 2), ...varint(bytes.length), ...bytes];
function fields(bytes) {
  const values = new Map();
  let pos = 0;
  function integer() {
    let value = 0;
    for (let n = 0; n < 10 && pos < bytes.length; n++) {
      const b = bytes[pos++];
      if (n < 5) value += (b & 127) * 2 ** (7 * n);
      if (b < 128) return value;
    }
    throw Error('Incomplete protobuf integer');
  }
  while (pos < bytes.length) {
    const key = integer(), type = key % 8, tag = Math.floor(key / 8);
    if (!tag) throw Error('Invalid protobuf field');
    if (type === 0) { values.set(tag, integer()); continue; }
    const length = type === 2 ? integer() : type === 5 ? 4 : type === 1 ? 8 : -1;
    if (length < 0 || length > bytes.length - pos) throw Error('Invalid protobuf length');
    values.set(tag, type === 5 ? new DataView(bytes.buffer,bytes.byteOffset+pos,4).getUint32(0,true) : bytes.subarray(pos,pos+length));
    pos += length;
  }
  return values;
}
const nodeId = n => '!' + n.toString(16).padStart(8,'0');
function randomId() { const v = new Uint32Array(1); do { crypto.getRandomValues(v); } while (v[0] < 4); return v[0]; }
function toRadio(text, dest, channel, id) {
  const data = [...uint(1,1), ...nested(2,encoder.encode(text))];
  const packet = [...fixed(2,dest), ...(channel ? uint(3,channel) : []), ...nested(4,data), ...fixed(6,id)];
  return new Uint8Array(nested(1,packet));
}
function reflect() {
  const size = encoder.encode(draft.value).length;
  byId('count').textContent = size + ' / 233 bytes';
  channels.disabled = !ready || !channels.options.length;
  byId('send').disabled = sending || channels.disabled || !size || size > 233;
}
function record(label, body, status) {
  const row = document.createElement('li'), meta = document.createElement('small');
  const message = document.createElement('p'), delivery = document.createElement('small');
  meta.textContent = label; message.textContent = body; delivery.textContent = status || '';
  row.append(meta,message,delivery); historyList.append(row);
  while (historyList.children.length > 100) historyList.firstElementChild.remove();
  byId('empty').hidden = true; historyList.scrollTop = historyList.scrollHeight;
  return delivery;
}
function remember(map, key, value) {
  if (map instanceof Set) map.add(key); else map.set(key,value);
  if (map.size > 150) map.delete(map.keys().next().value);
}
function incoming(bytes) {
  const envelope = fields(bytes);
  if (envelope.has(3)) self = fields(envelope.get(3)).get(1) || 0;
  if (envelope.get(7) === nonce) {
    ready = true; byId('connection').textContent = channels.options.length ? 'Connected' : 'No active channels'; reflect();
  }
  if (envelope.has(10)) {
    const channel = fields(envelope.get(10)), index = channel.get(1) || 0, role = channel.get(3);
    if (index < 8 && (role === 1 || role === 2) && !channelNames.has(index)) {
      const settings = fields(channel.get(2) || new Uint8Array());
      const name = decoder.decode(settings.get(3) || new Uint8Array()) || (role === 1 ? 'Primary' : 'Secondary');
      channelNames.set(index,name); channels.add(new Option(index+' · '+name,String(index))); reflect();
    }
  }
  if (envelope.has(16)) {
    const note = fields(envelope.get(16)), text = decoder.decode(note.get(4) || new Uint8Array());
    if (text && sent.has(note.get(1))) sent.get(note.get(1)).textContent = text;
  }
  if (!envelope.has(2)) return;
  const packet = fields(envelope.get(2));
  if (!packet.has(4)) return;
  const data = fields(packet.get(4)), payload = data.get(2) || new Uint8Array();
  if (data.get(1) === 5 && sent.has(data.get(6))) {
    const reason = fields(payload).get(3) || 0;
    sent.get(data.get(6)).textContent = reason ? 'Radio reported error '+reason : 'Radio acknowledged';
    return;
  }
  if (data.get(1) !== 1 || !payload.length) return;
  const origin = packet.get(1) || 0, id = packet.get(6) || 0, key = origin+'-'+id;
  if (id && (seen.has(key) || (origin === self && sent.has(id)))) return;
  if (id) remember(seen,key);
  const channel = packet.get(3) || 0;
  record(nodeId(origin)+' · '+(channelNames.get(channel) || 'Channel '+channel),decoder.decode(payload));
}
async function api(path, method, body) {
  const opts = {method, credentials:'same-origin', cache:'no-store', redirect:'error', signal:AbortSignal.timeout(8000)};
  if (body) { opts.headers = {'Content-Type':'application/x-protobuf'}; opts.body = body; }
  const response = await fetch('/api/v1/'+path,opts);
  if (!response.ok) throw Error('HTTP '+response.status);
  return new Uint8Array(await response.arrayBuffer());
}
function offline() {
  ready = false; reflect();
  byId('connection').textContent = 'Disconnected'; byId('reconnect').hidden = false;
}
async function connect() {
  const myGeneration = ++generation;
  ready = false; self = 0;
  do { nonce = randomId(); } while (nonce === 69420 || nonce === 69421);
  channelNames.clear(); channels.replaceChildren(); reflect();
  byId('reconnect').hidden = true; byId('connection').textContent = 'Connecting…';
  try {
    await api('toradio','PUT',new Uint8Array(uint(3,nonce)));
    const start = Date.now();
    while (myGeneration === generation) {
      const bytes = await api('fromradio','GET');
      if (myGeneration !== generation) return;
      if (bytes.length) incoming(bytes);
      if (!ready && Date.now()-start > 30000) throw Error('The radio did not finish setup');
      await new Promise(resolve => setTimeout(resolve,bytes.length ? 25 : 750));
    }
  } catch (err) {
    if (myGeneration === generation) { offline(); byId('feedback').textContent = 'Connection failed. '+err.message; }
  }
}
draft.addEventListener('input',reflect);
byId('reconnect').addEventListener('click',connect);
byId('compose').addEventListener('submit',async event => {
  event.preventDefault();
  if (byId('send').disabled) return;
  const input = byId('recipient').value.trim();
  if (input && !/^!?[0-9a-f]{8}$/i.test(input)) { byId('feedback').textContent = 'Enter an eight-digit hexadecimal node ID.'; return; }
  const dest = input ? parseInt(input.replace('!',''),16) : 0xffffffff;
  if (dest < 2 || dest === 0xfffffffe) { byId('feedback').textContent = 'Choose a valid mesh recipient.'; return; }
  const channel = Number(channels.value), text = draft.value, id = randomId();
  sending = true; reflect(); byId('feedback').textContent = '';
  const delivery = record('You · '+channelNames.get(channel)+' · '+(dest===0xffffffff?'Broadcast':nodeId(dest)),text,'Submitting…');
  remember(sent,id,delivery);
  try {
    await api('toradio','PUT',toRadio(text,dest,channel,id));
    if (delivery.textContent === 'Submitting…') delivery.textContent = 'Submitted to radio';
    if (draft.value === text) draft.value = '';
  } catch (err) { delivery.textContent = 'Submission unconfirmed. '+err.message; }
  finally { sending = false; reflect(); }
});
connect();
</script></body></html>)ZTHTML";
} // namespace mt::network::browser
