// The player's own paks, kept in the browser's origin private file system
// (OPFS) for the next visit, by this dedicated worker of the page's (bring,
// in client-page.js.in): each at own/<gamedir>/<name>, the name asked for, with
// own/index.json listing each one kept, { src, path, name, checksum, size,
// mtime, stamp, route, stored, lastUsed }: src the file it fills, as asked
// for (<gamedir>/<name>, lowercased), path where it is under own/, name and
// mtime the chosen file's, and stamp the page's of its bytes (stampOf, in
// pak_check.js), which the page checks on later visits; route how it came,
// 'copied' here, and stored and lastUsed when, in ms since 1970. A second
// version of a pak under the same name is kept beside the first, at
// own/<gamedir>/<checksum>/<name>, rather than over it.
//
// A pak the page reads where the player keeps it, in a folder whose handle
// it keeps (client-page.js.in's FOLDERS_DATABASE), is listed too, with route
// 'in-place', folder that handle's key, and no path: nothing of it is here.
// Records are kept as they're read, so a field or a route this page doesn't
// know, a later version's, stays as it was through this one's writes, and
// a record without a path is no file of this store's to list or remove.
//
// A pak is copied in slices and written with a sync access handle, or with
// createWritable where the browser has none, or where sync access handles'
// writes can be lost (SYNC_WRITES_LOST). Its record is taken out of the
// index before the copy and put back only once the whole file is written,
// so a copy cut short, by a failure or by the page going, leaves a file the
// index doesn't list, which is removed. A sync access handle is exclusive:
// each is closed as soon as its file is written, and a copy stops, and lets
// go of its handle, when the page goes. Another page's copy of the same file
// holds a lock (Web Locks) this one waits a moment for and then leaves be.
//
// The worker posts { running } as it starts. The page posts { id, op, ... }
// and gets { id, ok, ... } back, and { id, copied } as a copy goes:
//   list   { sync, files }: whether paks are written with sync access
//          handles, and the index's records, each with file, its File
//   store  { file, src, checksum, stamp } -> { path }, with already if it
//          was there, the same stamp, or { busy } when another page holds src
//   stamp  { path, stamp }: a record kept from before stamps given its own
//   note   { folder, files: [{ src, name, checksum, size, mtime }] }: paks
//          read in place from a kept folder, each one's record made or
//          brought up to date, in one write
//   used   { paths }: the records of the copies at those paths given
//          lastUsed now
//   drop   { path }: its record and file removed
//   tidy   files the index doesn't list removed
// and { op: 'stop' } as it goes: the copy stops, its handle closed, and
// nothing more is done, until { op: 'resume' }, as the page comes back
// from the browser's back/forward cache.

const INDEX = 'index.json';
// how much of a pak is read and written at a time, and how often a copy
// says how far it is
const SLICE = 4 << 20;
// how long a copy waits for another page to let go of a file, and how often
// and how long apart a handle another page's worker holds, as on a reload,
// is asked for again
const LOCK_WAIT = 1500, HANDLE_TRIES = 5, HANDLE_WAIT = 200;
// WebKit (Safari, and every browser on iOS) can lose a file written through
// a sync access handle: one written so, flushed and closed, came back at its
// full size but zero-filled after the browser was killed, where one written
// through createWritable didn't. No probe can see that, as a file reads back
// whole until the kill, so it goes by the engine's name in the user agent:
// AppleWebKit without Chrome's or Chromium's, which Blink's carries too.
// There paks are written through createWritable, where it's there; the
// page's stamp catches a loss wherever else one comes.
const SYNC_WRITES_LOST = /AppleWebKit\//.test(navigator.userAgent) && !/Chrome\/|Chromium\//.test(navigator.userAgent);
let stopping = false;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const own = async () => (await navigator.storage.getDirectory()).getDirectoryHandle('own', { create: true });
// the folder a path under own/ is in, made if create, and its file's name
const place = async (path, create) => {
    const parts = path.split('/');
    let dir = await own();
    for (const part of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create });
    return { dir, name: parts.at(-1) };
};
const handleOf = async (path, create = false) => {
    const { dir, name } = await place(path, create);
    return dir.getFileHandle(name, { create });
};
// the File at a path, or null when it's gone or another page is writing it
const fileAt = (path) => handleOf(path).then((handle) => handle.getFile()).catch(() => null);
const fileLock = (src) => `own-paks:${src}`;
const withIndex = (use) => navigator.locks.request('own-paks:index', use);

// A sync access handle, asked for again a few times while another page's
// worker still holds it, as an old page's can for a moment on a reload
async function access(handle) {
    for (let tries = 1; ; tries++) {
        try {
            return await handle.createSyncAccessHandle();
        } catch (error) {
            if (error?.name !== 'NoModificationAllowedError' || tries >= HANDLE_TRIES) throw error;
            await sleep(HANDLE_WAIT);
        }
    }
}

// Writes slices, an iterable of bytes, sync or async, as the whole of
// handle's file, calling copied with the bytes written so far; the bytes
// written. With whole, it's written whole or not at all (createWritable
// writes beside the file and swaps it in), where the browser can, as it
// is where sync access handles' writes can be lost (SYNC_WRITES_LOST).
async function write(handle, slices, copied = () => {}, whole = false) {
    let at = 0, put, finish, abandon;
    if (handle.createSyncAccessHandle && !(handle.createWritable && (whole || SYNC_WRITES_LOST))) {
        const file = await access(handle);
        try {
            await file.truncate(0);
        } catch (error) {
            await file.close();
            throw error;
        }
        put = (bytes) => {
            for (let done = 0; done < bytes.length;) done += file.write(bytes.subarray(done), { at: at + done });
        };
        finish = async () => {
            try {
                await file.flush();
            } finally {
                await file.close();
            }
        };
        abandon = () => file.close();
    } else {
        const writable = await handle.createWritable();
        put = (bytes) => writable.write(bytes);
        finish = () => writable.close();
        abandon = () => writable.abort();
    }
    try {
        for await (const bytes of slices) {
            if (stopping) throw new Error('the page went');
            await put(bytes);
            at += bytes.length;
            copied(at);
        }
    } catch (error) {
        await Promise.resolve(abandon()).catch(() => {});
        throw error;
    }
    await finish();
    return at;
}

async function* slicesOf(blob) {
    for (let at = 0; at < blob.size; at += SLICE) yield new Uint8Array(await blob.slice(at, at + SLICE).arrayBuffer());
}

// the index, read and written under its lock; one that isn't there, or
// isn't an index, lists nothing, so none of its files is trusted; one the
// browser couldn't read is an error, so it isn't written over with less
async function readIndex() {
    let text;
    try {
        text = await (await (await handleOf(INDEX)).getFile()).text();
    } catch (error) {
        if (error?.name === 'NotFoundError') return { files: [] };
        throw error;
    }
    try {
        const index = JSON.parse(text);
        return Array.isArray(index?.files) ? index : { files: [] };
    } catch {
        return { files: [] };
    }
}
const writeIndex = async (index) => write(await handleOf(INDEX, true), [new TextEncoder().encode(JSON.stringify(index))], undefined, true);
const changeIndex = (change) => withIndex(async () => {
    const index = await readIndex();
    change(index);
    await writeIndex(index);
});

const remove = async (path) => {
    const { dir, name } = await place(path, false);
    await dir.removeEntry(name);
};

// records a server name and timestamp on an own/index.json record (client-side
// only, capped at 32 servers, matching sources.dat's limit)
const MAX_SERVERS = 32;
const noteServer = (record, server, now) => {
    if (typeof server != 'string') return;
    const clean = server.replace(/[\x00-\x1f\x7f]/g, '').trim().slice(0, 64);
    if (!clean) return;
    const list = Array.isArray(record.servers) ? record.servers.filter((s) => s && typeof s.name == 'string') : [];
    const existing = list.find((s) => s.name.toLowerCase() === clean.toLowerCase());
    if (existing) {
        existing.lastUsed = now;
    } else {
        if (list.length >= MAX_SERVERS) {
            list.sort((a, b) => (a.lastUsed ?? 0) - (b.lastUsed ?? 0));
            list.splice(0, list.length - MAX_SERVERS + 1);
        }
        list.push({ name: clean, lastUsed: now });
    }
    record.servers = list;
};

const ops = {
    list: () => withIndex(async () => {
        const records = (await readIndex()).files;
        // a file another page is writing, or that's gone, isn't listed
        const files = await Promise.all(records.map(async (record) => ({ ...record, file: await fileAt(record.path) })));
        const sync = typeof FileSystemSyncAccessHandle == 'function'
            && !(SYNC_WRITES_LOST && 'createWritable' in FileSystemFileHandle.prototype);
        return { sync, files: files.filter(({ file }) => file) };
    }),

    // all records for the storage list, both copied files that exist in OPFS
    // and in-place records from kept folders
    index: () => withIndex(async () => {
        const records = (await readIndex()).files;
        const checked = await Promise.all(records.map(async (record) => {
            if (!record) return null;
            if (typeof record.path == 'string' && record.path) return (await fileAt(record.path)) ? record : null;
            return record.route === 'in-place' && typeof record.src == 'string' ? record : null;
        }));
        return { records: checked.filter(Boolean) };
    }),

    store: ({ file, src, checksum, stamp, server }, id) => navigator.locks.request(fileLock(src),
        { signal: AbortSignal.timeout(LOCK_WAIT) }, async () => {
            const [gamedir, name] = src.split('/');
            // Kept already, as by another page that held src a moment ago;
            // or this version's place: src's own, unless another version
            // holds it, in a folder named for its checksum. Its record is
            // out of the index until it's written.
            const { kept, path } = await withIndex(async () => {
                const index = await readIndex();
                // the copies of src, not the records of it read in place
                const versions = index.files.filter((record) => record.src === src && typeof record.path == 'string');
                const same = versions.find((record) => record.checksum === checksum);
                if (same?.size === file.size && same.stamp === stamp && (await fileAt(same.path))?.size === same.size) return { kept: same };
                const path = same?.path ?? (versions.length ? `${gamedir}/${checksum ?? 'unchecked'}/${name}` : src);
                index.files = index.files.filter((record) => record.path !== path);
                await writeIndex(index);
                return { path };
            });
            if (kept) return { path: kept.path, already: true };
            try {
                let said = 0;
                const size = await write(await handleOf(path, true), slicesOf(file), (copied) => {
                    if (copied - said < SLICE * 4 && copied < file.size) return;
                    said = copied;
                    postMessage({ id, copied });
                });
                if (size !== file.size) throw new Error(`wrote ${size} of its ${file.size} bytes`);
            } catch (error) {
                await remove(path).catch(() => {});
                throw error;
            }
            const now = Date.now();
            await changeIndex((index) => {
                const record = { src, path, name: file.name, checksum, size: file.size, mtime: file.lastModified, stamp,
                    route: 'copied', stored: now, lastUsed: now };
                noteServer(record, server, now);
                index.files.push(record);
            });
            return { path };
        }).catch((error) => {
        if (error?.name === 'AbortError' || error?.name === 'TimeoutError') return { busy: true };
        throw error;
    }),

    // only a record that has none, so a stamp taken before another page
    // stored the pak again isn't put on the new one's
    stamp: async ({ path, stamp }) => {
        await changeIndex((index) => {
            const record = index.files.find((each) => each.path === path);
            if (record && record.stamp === undefined) record.stamp = stamp;
        });
        return {};
    },

    note: async ({ folder, files, server }) => {
        const now = Date.now();
        await changeIndex((index) => {
            for (const { src, name, checksum, size, mtime } of files) {
                let record = index.files.find((each) => each.src === src && each.route === 'in-place' && each.folder === folder);
                if (record) Object.assign(record, { name, checksum, size, mtime, lastUsed: now });
                else {
                    record = { src, name, checksum, size, mtime, route: 'in-place', folder, stored: now, lastUsed: now };
                    index.files.push(record);
                }
                noteServer(record, server, now);
            }
        });
        return {};
    },

    used: async ({ paths, server }) => {
        const now = Date.now(), used = new Set(paths);
        await changeIndex((index) => {
            for (const record of index.files) {
                if (typeof record.path == 'string' && used.has(record.path)) {
                    record.lastUsed = now;
                    noteServer(record, server, now);
                }
            }
        });
        return {};
    },

    drop: async ({ path, src }) => {
        if (typeof path != 'string' || !path) return {};
        const run = async () => {
            await changeIndex((index) => {
                index.files = index.files.filter((record) => record.path !== path);
            });
            await remove(path).catch(() => {});
        };
        if (typeof src == 'string' && src) await navigator.locks.request(fileLock(src.toLowerCase()), run);
        else await run();
        return {};
    },

    dropRecord: async ({ src, folder }) => {
        let remainingForFolder = 0;
        await changeIndex((index) => {
            index.files = index.files.filter((record) => !(record.src === src && record.route === 'in-place' && record.folder === folder));
            if (folder) {
                remainingForFolder = index.files.filter((record) => record.route === 'in-place' && record.folder === folder).length;
            }
        });
        return { remainingForFolder };
    },

    // files the index doesn't list, as a copy cut short leaves, that no
    // other page is copying
    tidy: () => withIndex(async () => {
        const listed = new Map((await readIndex()).files.map((record) => [record.path, record.src]));
        const unlisted = [];
        const walk = async (dir, prefix) => {
            for await (const [name, entry] of dir.entries()) {
                const path = prefix ? `${prefix}/${name}` : name;
                if (entry.kind === 'directory') await walk(entry, path);
                else if (path !== INDEX && !listed.has(path)) unlisted.push({ dir, name, path });
            }
        };
        await walk(await own(), '');
        const removed = [];
        for (const { dir, name, path } of unlisted)
            await navigator.locks.request(fileLock(`${path.split('/')[0]}/${name}`.toLowerCase()), { ifAvailable: true }, async (lock) => {
                if (lock && await dir.removeEntry(name).then(() => true, () => false)) removed.push(path);
            });
        return { removed };
    }),
};

onmessage = async ({ data: { id, op, ...args } }) => {
    if (op === 'stop' || op === 'resume') {
        stopping = op === 'stop';
        return;
    }
    if (stopping) {
        postMessage({ id, ok: false, error: 'the page went' });
        return;
    }
    try {
        postMessage({ id, ok: true, ...await ops[op](args, id) });
    } catch (error) {
        postMessage({ id, ok: false, error: `${error?.name ?? 'Error'}: ${error?.message ?? error}` });
    }
};

// the page times its first answer from here
postMessage({ running: true });
