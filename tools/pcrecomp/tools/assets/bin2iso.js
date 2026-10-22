#!/usr/bin/env node
// Convert a raw 2352-byte BIN data track to a standard 2048-byte sector ISO.
// Mode 1:        12 sync + 4 header                  + 2048 data + 288 EDC/ECC
// Mode 2 Form 1: 12 sync + 4 header + 8 XA subheader + 2048 data + 280 EDC/ECC
// The mode is byte 15 of each sector, read per sector: the cue sheet's track
// type is easy to lose, and Win95-era PC discs are often Mode 2 (CD-ROM XA).
// Mode 2 Form 2 sectors (video/audio streams) carry no ISO9660 data and are
// not expected on a PC data track.

const fs = require('fs');
const path = require('path');

const binPath = process.argv[2];
const isoPath = process.argv[3] || binPath.replace(/\.bin$/i, '.iso');

const SECTOR_RAW = 2352;
const SECTOR_DATA = 2048;

const stat = fs.statSync(binPath);
const sectorCount = Math.floor(stat.size / SECTOR_RAW);

console.log(`BIN file: ${binPath}`);
console.log(`Size: ${stat.size} bytes`);
console.log(`Sectors: ${sectorCount}`);
console.log(`Output: ${isoPath}`);

const fd_in = fs.openSync(binPath, 'r');
const fd_out = fs.openSync(isoPath, 'w');
const buf = Buffer.alloc(SECTOR_RAW);

for (let i = 0; i < sectorCount; i++) {
    fs.readSync(fd_in, buf, 0, SECTOR_RAW, i * SECTOR_RAW);
    const header = buf[15] === 2 ? 24 : 16;
    fs.writeSync(fd_out, buf, header, SECTOR_DATA);
}

fs.closeSync(fd_in);
fs.closeSync(fd_out);

console.log(`Done! ISO written (${sectorCount * SECTOR_DATA} bytes)`);
