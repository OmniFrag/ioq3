// The page's pak reader: the player's own paks, stored in the browser's
// private files (OPFS) by pak_store.js, read by the engine from there, so
// the page never holds them (downloadGame, in client.html.in). Each becomes
// a file node of Emscripten's file system with no contents and its own
// stream_ops.read, which a worker (pak_reader_worker.js) answers.
//
// The engine reads synchronously, on the main thread, which can't
// Atomics.wait: a read the cache can't answer posts a request in a
// SharedArrayBuffer, spins on Atomics.load until the worker answers, and
// copies the bytes into the engine's heap, which stays unshared. A cache of
// 64 KB blocks, 4 MB in all, answers the zip directory's many small reads
// and unzip's 16 KB ones; a read of more than 256 KB goes straight through.
//
// It's on only where the page is cross-origin isolated and has
// SharedArrayBuffer, and the worker opens every pak and reads each one's end
// and the start of its zip directory within PROVE_MS (a second tab's worker
// in WebKit never answered a read while the first held the paks).
// Otherwise the page reads the paks into memory. Once on, a read the worker
// doesn't answer within READ_LIMIT_MS is a page error, never an unreadable
// pak to the engine.
import { lastEndRecord } from './pak_check.js';

// the shared buffer's data area: the most one round trip carries
const DATA_BYTES = 2 ** 20, DATA_OFFSET = 64;
const BLOCK = 65536, CACHE_BYTES = 4 * 2 ** 20, DIRECT = 2 ** 18;
// how long the worker may take to start, as on a slow link, and to open the
// paks once it runs, and how long a read may take while the reader proves
// the paks, and then while the game runs; the page gives the end of a stored
// pak it reads into memory READ_LIMIT_MS too
const START_MS = 15000, OPEN_MS = 2000, PROVE_MS = 2000;
export const READ_LIMIT_MS = 10000;
const EPERM = 63, ENODEV = 43;
// a zip's end record and comment, at most, which hold where its directory is
const END_MOST = 0xffff + 22;

// A failed read: the page's error, never an errno for the engine
class ReaderError extends Error {}

// The worker, started, and given the paks' paths once it runs, within the
// time limits: its answer, or { error }
async function start(url, sab, paths) {
    const worker = new Worker(url);
    let timer;
    const answer = await new Promise((resolve) => {
        const late = (ms, what) => {
            clearTimeout(timer);
            timer = setTimeout(() => resolve({ error: `${what} within ${ms / 1000} s` }), ms);
        };
        late(START_MS, "its worker didn't start");
        worker.onmessage = ({ data }) => {
            if (!data.running) return resolve(data);
            late(OPEN_MS, "its worker didn't open the paks");
            worker.postMessage({ sab, dataOffset: DATA_OFFSET, paks: paths });
        };
        worker.onerror = (event) => resolve({ error: event.message || "its worker didn't start" });
    });
    clearTimeout(timer);
    if (!answer.ok) worker.terminate();
    return { worker, answer };
}

// Opens paks, each { src, dst, stored }, stored the path pak_store.js keeps
// it at, for FS; mark(why) is told why a read failed, 'other-tab' or
// 'unanswered'. Returns { on, why, other }, other whether another page holds
// the paks: on, each pak is a node in FS read through the reader, which has
// modes, stats(), held() and close(); off, none is, and the page reads them
// into memory.
export async function openReader(FS, paks, workerURL, mark) {
    if (!globalThis.crossOriginIsolated) return { on: false, why: "the page isn't cross-origin isolated" };
    if (typeof SharedArrayBuffer != 'function') return { on: false, why: 'there is no SharedArrayBuffer' };
    const sab = new SharedArrayBuffer(DATA_OFFSET + DATA_BYTES);
    const ctl = new Int32Array(sab, 0, 8);
    const at = new Float64Array(sab, 32, 1);
    const shared = new Uint8Array(sab, DATA_OFFSET);
    // kept: a Worker nothing refers to may be collected, and the worker,
    // idle in Atomics.wait, ended with it
    const { worker, answer } = await start(workerURL, sab, paks.map(({ stored }) => stored));
    if (!answer.ok) return { on: false, why: answer.error };
    const { files } = answer;
    const other = files.some(({ taken }) => taken);
    const stats = { reads: 0, roundTrips: 0, hits: 0, misses: 0, direct: 0, spinMs: 0 };
    let limit = PROVE_MS, closed = false;
    const failed = (index, what) => new ReaderError(`${paks[index].src} ${what}${other ? ': it may be open in another tab' : ''}`);

    // a request to the worker, woken from its wait: length bytes of pak
    // index from where, or with index -1, to close its handles and end
    const post = (index, length, where) => {
        const request = (Atomics.load(ctl, 0) + 1) | 0;
        ctl[2] = index;
        ctl[3] = length;
        at[0] = where;
        Atomics.store(ctl, 0, request);
        Atomics.notify(ctl, 0);
        return request;
    };
    // one round trip, into shared: the bytes read
    const fetchBytes = (index, where, length) => {
        if (closed) throw new ReaderError('the page is going');
        const begun = performance.now();
        const request = post(index, length, where);
        for (let spins = 1; Atomics.load(ctl, 1) !== request; spins++)
            if ((spins & 0xffff) === 0 && performance.now() - begun > limit) throw failed(index, `wasn't read within ${limit / 1000} s`);
        stats.roundTrips++;
        stats.spinMs += performance.now() - begun;
        if (ctl[4] < 0) throw failed(index, "couldn't be read");
        return ctl[4];
    };

    // the block cache: key to bytes, in the order last used
    const cache = new Map();
    const MAX_BLOCKS = CACHE_BYTES / BLOCK, MAX_RUN = DATA_BYTES / BLOCK;
    // a small integer, as a pak of a 32-bit zip has fewer blocks than 2^17
    const key = (index, block) => index * 2 ** 17 + block;
    // block of pak index, and the misses after it up to last, in one round
    // trip; the first's bytes
    const load = (index, size, block, last) => {
        let end = block + 1;
        const stop = Math.min(Math.ceil(size / BLOCK), last + 1, block + MAX_RUN);
        while (end < stop && !cache.has(key(index, end))) end++;
        const from = block * BLOCK, to = Math.min(size, end * BLOCK);
        if (fetchBytes(index, from, to - from) !== to - from) throw failed(index, 'ended early');
        for (let b = block; b < end; b++) cache.set(key(index, b), shared.slice((b - block) * BLOCK, Math.min(to - from, (b - block + 1) * BLOCK)));
        while (cache.size > MAX_BLOCKS) cache.delete(cache.keys().next().value);
        return cache.get(key(index, block));
    };

    // length bytes of pak index at position into heap, a Uint8Array
    const readInto = (index, heap, length, position) => {
        stats.reads++;
        const { size } = files[index];
        if (position >= size || length <= 0) return 0;
        length = Math.min(length, size - position);
        if (length > DIRECT) {
            stats.direct++;
            for (let done = 0; done < length;) {
                const n = fetchBytes(index, position + done, Math.min(DATA_BYTES, length - done));
                if (n <= 0) throw failed(index, 'ended early');
                heap.set(shared.subarray(0, n), done);
                done += n;
            }
            return length;
        }
        const last = Math.floor((position + length - 1) / BLOCK);
        for (let done = 0; done < length;) {
            const where = position + done, block = Math.floor(where / BLOCK), within = where - block * BLOCK;
            const k = key(index, block);
            let bytes = cache.get(k);
            if (bytes) {
                stats.hits++;
                cache.delete(k);
                cache.set(k, bytes);
            } else {
                stats.misses++;
                bytes = load(index, size, block, last);
            }
            const n = Math.min(bytes.length - within, length - done);
            heap.set(bytes.subarray(within, within + n), done);
            done += n;
        }
        return length;
    };
    const readBytes = (index, position, length) => {
        const bytes = new Uint8Array(length);
        readInto(index, bytes, length, position);
        return new DataView(bytes.buffer);
    };

    // Each pak proved, its end and the start of its zip directory, where the
    // end record says, as unzip finds it, read within PROVE_MS each; the
    // reads the engine begins with, so they're in the cache for it
    try {
        files.forEach(({ size }, index) => {
            const from = Math.max(0, size - END_MOST);
            const tail = readBytes(index, from, size - from);
            const end = lastEndRecord(tail);
            if (end < 0) throw failed(index, 'has no end record');
            const directory = tail.getUint32(end + 12, true), begins = from + end - directory;
            if (directory > 0 && begins >= 0) readBytes(index, begins, Math.min(directory, BLOCK));
        });
    } catch (error) {
        worker.terminate();
        return { on: false, why: error.message, other };
    }
    limit = READ_LIMIT_MS;

    // the worker lets go of the paks, as the page goes: woken from its wait,
    // it closes each handle and ends
    const close = () => {
        if (closed) return;
        closed = true;
        post(-1, 0, 0);
    };

    const nodes = [];
    try {
        paks.forEach((pak, index) => nodes.push(makeNode(pak, index)));
    } catch (error) {
        // a node that can't be made, as where a file is there already: none
        // is, and the worker lets go, so the page reads the paks into memory
        for (const node of nodes) {
            try {
                FS.unlink(FS.getPath(node));
            } catch {
                // gone already
            }
        }
        close();
        return { on: false, why: `a pak's file couldn't be made (${error.message})`, other };
    }

    function makeNode(pak, index) {
        const { size } = files[index];
        FS.mkdirTree(pak.dst);
        const node = FS.createFile(pak.dst, pak.src.match(/[^/]+$/)[0], {}, true, false);
        // no contents: its size is the pak's (getattr, and llseek's SEEK_END),
        // which a truncate can't change
        node.contents = undefined;
        Object.defineProperty(node, 'usedBytes', { configurable: true, get: () => size,
            set: () => { throw new FS.ErrnoError(EPERM); } });
        node.stream_ops = {
            ...node.stream_ops,
            read: (stream, buffer, offset, length, position) => {
                try {
                    return readInto(index, new Uint8Array(buffer.buffer, buffer.byteOffset + offset, length), length, position);
                } catch (error) {
                    // never an errno: the engine would take the pak as unreadable
                    if (error instanceof ReaderError) {
                        mark(other ? 'other-tab' : 'unanswered');
                        console.error(`Pak reader: stopped, as ${error.message}`);
                    }
                    throw error;
                }
            },
            write: () => { throw new FS.ErrnoError(EPERM); },
            mmap: () => { throw new FS.ErrnoError(ENODEV); },
            msync: () => { throw new FS.ErrnoError(ENODEV); },
        };
        return node;
    }

    return {
        on: true,
        modes: [...new Set(files.map(({ mode }) => mode))].join(', '),
        stats: () => ({ ...stats, spinMs: Math.round(stats.spinMs) }),
        // bytes the page holds of the paks: none while their nodes have no
        // contents
        held: () => nodes.reduce((sum, node) => sum + (node.contents?.length ?? 0), 0),
        close,
    };
}
