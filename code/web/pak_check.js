// The player's own paks, checked before the page takes them (bring, in
// client.html.in): each one's checksum, as the engine computes it when it
// opens a pak (PakDir_Read in qcommon/pak_dir.c, Com_BlockChecksum in
// qcommon/md4.c), from the zip's end record and directory alone, read with
// Blob.slice and nothing inflated; and what each chosen file is, against the
// files the page asks for.
//
// The build writes the game's own checksums in as it copies this file beside
// the page (deploy_shell_files, cmake/platforms/emscripten.cmake), from
// files.c's tables, so they have one source; this copy, not written in,
// doesn't load.

// The game's own paks' checksums, by game directory, each directory's from
// pak0 on: Quake III Arena 1.32's, Team Arena's and the demo's; none in a
// standalone build
export const GAME_PAKS = JSON.parse('@WEB_GAME_PAKS@');

// what a known checksum of a game the page doesn't ask for is
const OTHER_GAMES = { demoq3: 'demo', missionpack: 'team-arena' };

// MD4 (RFC 1320), which WebCrypto doesn't have: the digest's four words
export function md4(bytes) {
    const blocks = ((bytes.length + 8) >>> 6) + 1;
    const x = new Uint32Array(16 * blocks);
    for (let i = 0; i < bytes.length; i++) x[i >> 2] |= bytes[i] << (8 * (i & 3));
    x[bytes.length >> 2] |= 0x80 << (8 * (bytes.length & 3));
    // the length in bits, which a pak's directory keeps under 2^32
    x[16 * blocks - 2] = bytes.length * 8;
    const SHIFTS = [[3, 7, 11, 19], [3, 5, 9, 13], [3, 9, 11, 15]];
    const ORDER = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
        [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15],
        [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]];
    const ADD = [0, 0x5a827999, 0x6ed9eba1];
    const h = new Uint32Array([0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]);
    for (let block = 0; block < x.length; block += 16) {
        let [a, b, c, d] = h;
        for (let round = 0; round < 3; round++) {
            for (let i = 0; i < 16; i++) {
                const f = round === 0 ? (b & c) | (~b & d) : round === 1 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
                const t = (a + f + x[block + ORDER[round][i]] + ADD[round]) | 0;
                const s = SHIFTS[round][i & 3];
                // each step's result goes to the next step's b: a, d, c, b
                // in turn, as RFC 1320's [abcd k s], [dabc k s] and so on
                [a, b, c, d] = [d, (t << s) | (t >>> (32 - s)), b, c];
            }
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    return h;
}

// A pak's checksum from its files' CRC-32s, those of the files over 0 bytes
// in the directory's order: MD4 over them as little-endian words, its four
// words XORed (Com_BlockChecksum)
export function checksumOf(crcs) {
    const bytes = new Uint8Array(4 * crcs.length);
    const view = new DataView(bytes.buffer);
    crcs.forEach((crc, i) => view.setUint32(4 * i, crc, true));
    const [a, b, c, d] = md4(bytes);
    return (a ^ b ^ c ^ d) >>> 0;
}

const END_RECORD = 0x06054b50, ENTRY = 0x02014b50, ZIP64_LOCATOR = 0x07064b50;
const END_SIZE = 22, ENTRY_SIZE = 46, ZIP64_LOCATOR_SIZE = 20;
// the most a zip's comment takes, within which unzip looks for the end
// record (unzlocal_SearchCentralDir); and how far back the first look goes,
// as a pak seldom has a comment
const COMMENT_MAX = 0xffff, FIRST_LOOK = 1024;
// a directory larger than any pak's is damaged, rather than read whole:
// pak0's, of 3,539 files, is about 300 KB
const DIRECTORY_MAX = 32 << 20;

const read = async (blob, start, end) => new DataView(await blob.slice(start, end).arrayBuffer());

// A pak's checksum, { checksum }, or why it can't be read, { damaged }:
// "no end record", "zip64", "spanned" (across disks), "empty", "directory
// past the end", "directory too large", "unreadable entry" or "unreadable"
// (the browser couldn't read the file). It reads the records unzip reads,
// where it reads them: the end record, never zip64's, and each directory
// entry's fixed fields and name. Every entry the end record claims must be
// there, where the engine would take the ones before the first that isn't;
// a zip with no files is refused. So is zip64, which the engine doesn't
// read either: an end record that leaves the sizes to zip64's records
// (0xffffffff) can't be opened, and one that holds them still has zip64's
// records between it and the directory, which unzip takes for bytes before
// the zip (byte_before_the_zipfile), so it reads none of the entries.
export const checkPak = (blob) => checksumOfPak(blob).catch(() => ({ damaged: 'unreadable' }));

// The last end record's signature in a file's last back bytes, the one
// unzip finds: the bytes read from from, tail, and its place there, at, -1
// for none
async function findEnd(blob, back) {
    const from = Math.max(0, blob.size - back);
    const tail = await read(blob, from, blob.size);
    return { from, tail, at: lastEndRecord(tail) };
}

// Where in view, a DataView of a zip's last bytes, its last end record
// begins, as unzip finds it, if all of the record is there; -1 if not
export function lastEndRecord(view) {
    let at = view.byteLength - 4;
    while (at >= 0 && view.getUint32(at, true) !== END_RECORD) at--;
    return at >= 0 && at + END_SIZE <= view.byteLength ? at : -1;
}

async function checksumOfPak(blob) {
    let { from, tail, at } = await findEnd(blob, FIRST_LOOK);
    if (at < 0 && from > 0) ({ from, tail, at } = await findEnd(blob, COMMENT_MAX));
    if (at < 0) return { damaged: 'no end record' };
    const field16 = (offset) => tail.getUint16(at + offset, true), field32 = (offset) => tail.getUint32(at + offset, true);
    const [disk, directoryDisk, entries, allEntries] = [field16(4), field16(6), field16(8), field16(10)];
    const [size, offset] = [field32(12), field32(16)];
    if (size === 0xffffffff || offset === 0xffffffff) return { damaged: 'zip64' };
    if (disk !== 0 || directoryDisk !== 0 || entries !== allEntries) return { damaged: 'spanned' };
    if (entries === 0) return { damaged: 'empty' };
    const end = from + at;
    if (end < offset + size) return { damaged: 'directory past the end' };
    if (size > DIRECTORY_MAX) return { damaged: 'directory too large' };
    // just before the end record, wherever its offset says, as unzip reads
    // it: whatever comes before the zip, as in a self-extracting one, is
    // skipped. An entry's name may run past the directory, as unzip reads it,
    // but not past the file.
    const directory = await read(blob, end - size, blob.size);
    const unreadable = { damaged: at >= ZIP64_LOCATOR_SIZE && tail.getUint32(at - ZIP64_LOCATOR_SIZE, true) === ZIP64_LOCATOR
        ? 'zip64' : 'unreadable entry' };
    const crcs = [];
    for (let i = 0, p = 0; i < entries; i++) {
        if (p + ENTRY_SIZE > directory.byteLength || directory.getUint32(p, true) !== ENTRY) return unreadable;
        const name = directory.getUint16(p + 28, true);
        if (p + ENTRY_SIZE + name > directory.byteLength) return unreadable;
        if (directory.getUint32(p + 24, true) > 0) crcs.push(directory.getUint32(p + 16, true));
        p += ENTRY_SIZE + name + directory.getUint16(p + 30, true) + directory.getUint16(p + 32, true);
    }
    return { checksum: checksumOf(crcs) };
}

// fn of each of items, at most limit at once, in items' order: a whole
// install's folder is checked a few files at a time, not with hundreds of
// reads under way together
export async function mapLimited(items, limit, fn) {
    const results = new Array(items.length);
    let next = 0;
    const run = async () => {
        while (next < items.length) {
            const i = next++;
            results[i] = await fn(items[i]);
        }
    };
    await Promise.all(Array.from({ length: Math.min(limit, items.length) }, run));
    return results;
}

const parts = (entry) => entry.src.toLowerCase().split('/');
const gameOf = (entry) => parts(entry)[0];
const nameOf = (entry) => parts(entry).at(-1);

// The checksum an entry of the config's own list must have: its own
// checksum, which wins, or the game's for a pak<n>.pk3 of a game directory
// in table; undefined for neither
export function expectedChecksum(entry, table = GAME_PAKS) {
    const { checksum } = entry;
    if (Number.isInteger(checksum) && checksum >= -0x80000000 && checksum <= 0xffffffff) return checksum >>> 0;
    const path = parts(entry);
    const number = path.length === 2 ? /^pak(\d)\.pk3$/.exec(path[1])?.[1] : undefined;
    return number !== undefined && Object.hasOwn(table, path[0]) ? table[path[0]][Number(number)] : undefined;
}

// What each checked file is, for entries, the config's own list ({ src },
// and maybe checksum), of which the page has had already; files are
// { name, folder }, folder the one it was chosen in, lowercased, if the
// player chose a folder, with checkPak's checksum or damaged for a .pk3.
// Each file's { verdict, entry }, in files' order:
// - good: the checksum an entry must have, whatever the file's name, and
//   that entry, which it fills; one file an entry
// - unchecked: an entry with no checksum to have, by its name, as the page
//   took them before it checked; never a damaged file
// - damaged: a .pk3 whose directory can't be read
// - duplicate: the checksum of a file kept already
// - demo, team-arena: the demo's pak, or one of Team Arena's, not asked for
// - other-release: an entry's name, another checksum
// - ignored: anything else, a .pk3 or not
// A file that isn't good, unchecked or a duplicate names the entry still
// wanted that it's named as, in its folder's game directory, if any.
// Within an entry's candidates, a file in its game directory's folder comes
// first.
export function judge(entries, had, files, table = GAME_PAKS) {
    const verdicts = new Map();
    const wanted = entries.filter((entry) => !had.has(entry));
    const inGame = (entry, file) => file.folder === gameOf(entry);
    const keptSums = new Set(entries.filter((entry) => had.has(entry)).map((entry) => expectedChecksum(entry, table)));
    for (const entry of wanted) {
        const sum = expectedChecksum(entry, table);
        const candidates = files.filter((file) => !verdicts.has(file) && (sum === undefined
            ? !file.damaged && file.name.toLowerCase() === nameOf(entry) : file.checksum === sum));
        const file = candidates.find((candidate) => inGame(entry, candidate)) ?? candidates[0];
        if (!file) continue;
        verdicts.set(file, { verdict: sum === undefined ? 'unchecked' : 'good', entry });
        keptSums.add(file.checksum);
    }
    keptSums.delete(undefined);
    const filled = new Set([...verdicts.values()].map(({ entry }) => entry));
    const named = (file) => wanted.find((entry) => !filled.has(entry) && nameOf(entry) === file.name.toLowerCase()
        && (file.folder === undefined || inGame(entry, file)));
    // a game the page asks for has its own entries, so isn't another game
    const asked = new Set(entries.map(gameOf));
    const gameWith = (sum) => Object.keys(table).find((game) => !asked.has(game) && table[game].includes(sum));
    return files.map((file) => {
        if (verdicts.has(file)) return verdicts.get(file);
        if (!file.damaged && file.checksum === undefined) return { verdict: 'ignored' };
        if (keptSums.has(file.checksum)) return { verdict: 'duplicate' };
        const entry = named(file);
        const verdict = file.damaged ? 'damaged' : OTHER_GAMES[gameWith(file.checksum)] ?? (entry ? 'other-release' : 'ignored');
        return { verdict, entry };
    });
}
