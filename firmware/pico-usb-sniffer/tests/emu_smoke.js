// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, LibreTap contributors.
//
// Smoke test of the emulator build (emu_capture.c in place of capture.c) in rp2040js, the RP2040 emulator
// (github.com/wokwi/rp2040js, MIT; not part of this repository: npm install rp2040js somewhere and pass its
// dist/cjs/index.js). Boots the firmware the way the boot ROM would, enumerates it over the emulated USB as a
// CDC device, sends commands, streams 20 loops of canned traffic, checks the records, and reports how many
// (emulated) CPU cycles the filter needs per packet.
//
// usage: node emu_smoke.js RP2040JS_INDEX FIRMWARE.bin FIRMWARE.sym emu_traffic.h
'use strict';
const fs = require('fs');

const [rpPath, binPath, symPath, trafficPath] = process.argv.slice(2);
const { Simulator, USBCDC, createSetupPacket, DataDirection, SetupType, SetupRecipient } = require(rpPath);

const defs = {};
for (const m of fs.readFileSync(trafficPath, 'utf8').matchAll(/#define (\w+) (\d+)/g)) defs[m[1]] = +m[2];
const LOOPS = 20;

// symbols (nm -n) for the profile
const syms = fs.readFileSync(symPath, 'utf8').split('\n').map((l) => l.split(' ')).filter((p) => p.length === 3 && /[tT]/.test(p[1]))
  .map((p) => ({ addr: parseInt(p[0], 16), name: p[2] })).sort((a, b) => a.addr - b.addr);
function symAt(pc) {
  let lo = 0, hi = syms.length - 1, best = null;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (syms[mid].addr <= pc) { best = syms[mid]; lo = mid + 1; } else hi = mid - 1;
  }
  return best ? best.name : '?';
}

const sim = new Simulator();
sim.rp2040.logger = { debug() {}, info() {}, warn() {}, error(c, m) { console.error(c, m); } };
const mcu = sim.rp2040;
const bin = fs.readFileSync(binPath);
mcu.flash.set(bin);
// the boot ROM's job: the first 256 bytes of flash (boot2) go to the end of SRAM and run there
for (let i = 0; i < 256; i += 4) mcu.writeUint32(0x20041f00 + i, bin.readUInt32LE(i));
mcu.core.SP = 0x20042000;
mcu.core.PC = 0x20041f00;

const cdc = new USBCDC(mcu.usbCtrl);
// rp2040js's helper addresses SET_CONTROL_LINE_STATE to the device (bmRequestType 0x20); hosts (Linux cdc-acm,
// Windows usbser) address it to the interface (0x21), which is what the firmware's USB stack accepts
cdc.cdcSetControlLineState = function (value = 3, interfaceNumber = 0) {
  this.usb.sendSetupPacket(createSetupPacket({ dataDirection: DataDirection.HostToDevice, type: SetupType.Class,
    recipient: SetupRecipient.Interface, bRequest: 0x22, wValue: value, wIndex: interfaceNumber, wLength: 0 }));
  this.initialized = true;
};
let connected = false;
let rx = Buffer.alloc(0);
cdc.onDeviceConnected = () => { connected = true; };
cdc.onSerialData = (b) => { rx = Buffer.concat([rx, Buffer.from(b)]); };

const profile = new Map();
let profiling = false;
const cycleNanos = 1e9 / 125e6;
function run(cycles, until) {
  const end = mcu.core.cycles + cycles;
  while (mcu.core.cycles < end) {
    for (let i = 0; i < 20000; i++) {
      if (mcu.core.waiting) { mcu.clock.tick(mcu.clock.nanosToNextAlarm); continue; }
      const pc = mcu.core.PC;
      const c = mcu.core.executeInstruction();
      mcu.clock.tick(c * cycleNanos);
      if (profiling) { const s = symAt(pc); profile.set(s, (profile.get(s) || 0) + c); }
    }
    if (until && until()) return true;
  }
  return until ? until() : true;
}
function send(s) { for (const ch of Buffer.from(s)) cdc.sendSerialByte(ch); }
let failures = 0;
function check(ok, what) { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failures++; }

// records (the reader LibreTap implements)
function crc16(buf) {
  let crc = 0xffff;
  for (const b of buf) { crc ^= b << 8; for (let k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff; }
  return crc;
}
function parse(buf) {
  const recs = [];
  let i = 0, skipped = 0;
  while (i + 8 <= buf.length) {
    if (buf[i] !== 0xa5) { i++; skipped++; continue; }
    const type = buf[i + 1], len = buf.readUInt16LE(i + 2);
    if (type < 1 || type > 8 || len > 2048) { i++; skipped++; continue; }
    if (i + 10 + len > buf.length) break;
    if (crc16(buf.subarray(i + 1, i + 8 + len)) !== buf.readUInt16LE(i + 8 + len)) { i++; skipped++; continue; }
    recs.push({ type, time: buf.readUInt32LE(i + 4), p: buf.subarray(i + 8, i + 8 + len) });
    i += 10 + len;
  }
  return { recs, skipped };
}

console.log('boot and USB enumeration');
check(run(250e6, () => connected && rx.includes('p - Stop streaming')), 'enumerated as a CDC device; DTR → help printed');
const help = rx.toString('latin1');
check(/USB Sniffer Stream 1\.0\.0 \(protocol 1\)/.test(help), 'help names the firmware');
check(/e - Capture speed\s*: Full/.test(help) && /k - Keep idle traffic\s*: Disabled/.test(help) && /o - Output format\s*: Binary/.test(help), 'settings in usb-sniffer-lite\'s format');

rx = Buffer.alloc(0);
send('v');
check(run(50e6, () => rx.includes('\n')) && /USB Sniffer Stream 1\.0\.0 protocol 1\r\n/.test(rx.toString()), `version: ${JSON.stringify(rx.toString())}`);
rx = Buffer.alloc(0);
send('e');
run(30e6, () => rx.includes('\n'));
send('e');
run(30e6, () => rx.toString().split('\n').length > 2);
check(/Capture speed changed to Low\r\nCapture speed changed to Full\r\n/.test(rx.toString()), 'speed setting toggles (e)');

console.log('streaming');
rx = Buffer.alloc(0);
profiling = true;
const c0 = mcu.core.cycles;
send('s');
run(160e6); // ~1.3 s: all loops of traffic, at least one STATUS record
send('k'); // a command while streaming: the reply comes as a TEXT record
run(10e6);
send('k');
run(10e6);
const busy = mcu.core.cycles - c0;
profiling = false;
send('p');
run(100e6, () => parse(rx).recs.some((r) => r.type === 8));
const { recs, skipped } = parse(rx);
const types = (t) => recs.filter((r) => r.type === t);
check(skipped === 0, `records parse cleanly (${recs.length} records, ${rx.length} bytes, ${skipped} bytes skipped)`);
check(recs.length > 0 && recs[0].type === 1 && recs[0].p[0] === 1 && recs[0].p[1] === 1, 'HELLO first: protocol 1, full speed');
check(types(2).length === LOOPS * defs.EMU_KEPT, `PACKET records: ${types(2).length} (expected ${LOOPS * defs.EMU_KEPT})`);
check(types(3).length === LOOPS * defs.EMU_RESETS, `RESET records: ${types(3).length}`);
check(types(5).length === 0, 'no LOST records');
check(types(6).length >= 1, `STATUS records: ${types(6).length}`);
check(types(7).some((r) => r.p.toString().includes('Keep idle traffic changed to Enabled')), 'command reply while streaming arrives as a TEXT record');
const end = types(8)[0];
if (end) {
  const u32 = (o) => end.p.readUInt32LE(o);
  check(u32(0) === LOOPS * defs.EMU_FRAMES, `END: frames ${u32(0)}`);
  check(u32(6) === LOOPS * defs.EMU_DROPPED, `END: idle packets dropped ${u32(6)} (expected ${LOOPS * defs.EMU_DROPPED})`);
  check(u32(10) === LOOPS * defs.EMU_KEPT, `END: packets sent ${u32(10)}`);
  check(u32(18) === 0 && u32(22) === 0, 'END: nothing lost');
} else check(false, 'END record');
const answer = types(2).find((r) => r.p.includes(Buffer.from('M 0.5123 V')));
check(!!answer, 'the instrument\'s answer is in a DATA1 record');

let filter = 0, feed = 0;
const rows = [...profile.entries()].sort((a, b) => b[1] - a[1]);
for (const [name, c] of rows) {
  if (/^se_|^emit|^write_record|^room|^make_room|^lost_report|^packet_start|^crc16_usb|^crc5_usb|^rev8|^memcpy|^tx_write|^tx_space|^put/.test(name)) filter += c;
  if (name === 'capture_reply') feed += c;
}
const packets = LOOPS * defs.EMU_PACKETS;
console.log(`profile (${(busy / 1e6).toFixed(0)} M emulated cycles while streaming):`);
for (const [name, c] of rows.filter(([n]) => process.env.EMU_PROFILE_ALL || rows.indexOf(rows.find((r) => r[0] === n)) < 12)) console.log(`  ${name.padEnd(28)} ${(c / 1e6).toFixed(2)} M`);
console.log(`filter + encoder: ${(filter / packets).toFixed(0)} cycles per captured packet (${packets} packets, ${(LOOPS * defs.EMU_DROPPED / packets * 100).toFixed(0)}% idle);` +
  ` the capture loop body (fed by core 0 here): ${(feed / packets).toFixed(0)} cycles per packet`);
console.log(`at 120 MHz that is ${(120e6 / (filter / packets) / 1000).toFixed(0)} k packets/s for core 0 alone`);
console.log(failures ? `${failures} FAILED` : 'all ok');
process.exit(failures ? 1 : 0);
