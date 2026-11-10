// The worker that reads the player's stored paks from the browser's private
// files (OPFS) for the page's pak reader (pak_reader.js), whose engine reads
// synchronously on the page's main thread, which can't wait for an
// asynchronous read. It reads each pak, at own/<path> as pak_store.js keeps
// it, with a sync access handle, asked for again over about a second while
// another page's worker still holds it (as an old page's can on a reload;
// pak_store.js's access() does the same), or else with FileReaderSync on the
// file's Blob.
//
// It posts { running } as it starts. The page posts { sab, dataOffset,
// paks: [path] } once; the worker opens them and answers { ok, files:
// [{ size, mode, taken }] } (taken: another page held the sync access
// handle, still, when the worker gave up asking), or { ok: false, error },
// and then serves reads, blocked in
// Atomics.wait between them, in the shared buffer:
//   ctl[0]  request number, the page's: raised by one for each request
//   ctl[1]  answer number, the worker's: the request it has answered
//   ctl[2]  which pak, or -1: close every handle and end, which the page
//           asks as it goes (pagehide)
//   ctl[3]  how many bytes
//   ctl[4]  how many it read, or -1 for an error
//   at[0]   (a Float64 at byte 32) where in the pak
//   bytes   from dataOffset: the bytes read

const HANDLE_TRIES = 5, HANDLE_WAIT = 200;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let ctl, at, bytes;
// where a browser won't let a sync access handle read into the shared
// buffer itself, a buffer of the worker's own, copied from
let scratch = null;

async function open(own, path) {
    const parts = path.split('/');
    let dir = own;
    for (const part of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(part);
    const handle = await dir.getFileHandle(parts.at(-1));
    let taken = false;
    for (let tries = 1; handle.createSyncAccessHandle && tries <= HANDLE_TRIES; tries++) {
        try {
            const access = await handle.createSyncAccessHandle();
            const read = (where, length) => {
                if (!scratch) {
                    try {
                        return access.read(bytes.subarray(0, length), { at: where });
                    } catch (error) {
                        if (!(error instanceof TypeError)) throw error;
                        scratch = new Uint8Array(bytes.length);
                    }
                }
                const n = access.read(scratch.subarray(0, length), { at: where });
                bytes.set(scratch.subarray(0, n));
                return n;
            };
            // the handle's ours: no other page holds the pak now
            return { size: access.getSize(), mode: 'sync access handle', taken: false, read, close: () => access.close() };
        } catch (error) {
            if (error?.name !== 'NoModificationAllowedError') break;
            taken = true;
            if (tries < HANDLE_TRIES) await sleep(HANDLE_WAIT);
        }
    }
    if (typeof FileReaderSync != 'function') throw new Error('no sync access handle and no FileReaderSync');
    const file = await handle.getFile();
    const reader = new FileReaderSync();
    const read = (where, length) => {
        const data = new Uint8Array(reader.readAsArrayBuffer(file.slice(where, where + length)));
        bytes.set(data);
        return data.length;
    };
    return { size: file.size, mode: 'FileReaderSync', taken, read, close: () => {} };
}

// Answers each request as it comes, blocked between them, until the page
// asks it to close
function serve(paks) {
    // request 0 is none: the page asks only once it has the answer
    let seen = 0;
    for (;;) {
        Atomics.wait(ctl, 0, seen);
        seen = Atomics.load(ctl, 0);
        if (ctl[2] < 0) {
            for (const pak of paks) {
                try {
                    pak.close();
                } catch {
                    // closed already
                }
            }
            close();
            return;
        }
        let n;
        try {
            n = paks[ctl[2]].read(at[0], ctl[3]);
        } catch {
            n = -1;
        }
        ctl[4] = n;
        Atomics.store(ctl, 1, seen);
    }
}

onmessage = async ({ data: { sab, dataOffset, paks } }) => {
    ctl = new Int32Array(sab, 0, 8);
    at = new Float64Array(sab, 32, 1);
    bytes = new Uint8Array(sab, dataOffset);
    const opening = (async () => {
        const own = await (await navigator.storage.getDirectory()).getDirectoryHandle('own');
        return Promise.allSettled(paks.map((path) => open(own, path)));
    })();
    let opened;
    try {
        opened = await opening;
    } catch (error) {
        opened = [{ status: 'rejected', reason: error }];
    }
    const failed = opened.find(({ status }) => status === 'rejected');
    if (failed) {
        opened.forEach(({ value }) => value?.close());
        const error = failed.reason;
        postMessage({ ok: false, error: `${error?.name ?? 'Error'}: ${error?.message ?? error}` });
        return;
    }
    const files = opened.map(({ value }) => value);
    postMessage({ ok: true, files: files.map(({ size, mode, taken }) => ({ size, mode, taken })) });
    serve(files);
};

postMessage({ running: true });
