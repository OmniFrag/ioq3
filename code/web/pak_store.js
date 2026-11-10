// The player's own paks, kept in the browser's origin private file system
// (OPFS) for the next visit, by this dedicated worker of the page's (bring,
// in client.html.in): each at own/<gamedir>/<name>, the name asked for, with
// own/index.json listing each one kept, { src, path, name, checksum, size,
// mtime }: src the file it fills, as asked for (<gamedir>/<name>,
// lowercased), path where it is under own/, and name and mtime the chosen
// file's. A second version of a pak under the same name is kept beside the
// first, at own/<gamedir>/<checksum>/<name>, rather than over it.
//
// A pak is copied in slices and written with a sync access handle, or with
// createWritable where the browser has none. Its record is taken out of the
// index before the copy and put back only once the whole file is written, so
// a copy cut short, by a failure or by the page going, leaves a file the
// index doesn't list, which is removed. A sync access handle is exclusive:
// each is closed as soon as its file is written, and a copy stops, and lets
// go of its handle, when the page goes. Another page's copy of the same file
// holds a lock (Web Locks) this one waits a moment for and then leaves be.
//
// The worker posts { running } as it starts. The page posts { id, op, ... }
// and gets { id, ok, ... } back, and { id, copied } as a copy goes:
//   list   { sync, files }: whether the browser has sync access handles, and
//          the index's records, each with file, its File
//   store  { file, src, checksum } -> { path }, with already if it was
//          there, or { busy } when another page holds src
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
// writes beside the file and swaps it in), where the browser can.
async function write(handle, slices, copied = () => {}, whole = false) {
    let at = 0, put, finish, abandon;
    if (handle.createSyncAccessHandle && !(whole && handle.createWritable)) {
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

const ops = {
    list: () => withIndex(async () => {
        const records = (await readIndex()).files;
        // a file another page is writing, or that's gone, isn't listed
        const files = await Promise.all(records.map(async (record) => ({ ...record, file: await fileAt(record.path) })));
        return { sync: typeof FileSystemSyncAccessHandle == 'function', files: files.filter(({ file }) => file) };
    }),

    store: ({ file, src, checksum }, id) => navigator.locks.request(fileLock(src),
        { signal: AbortSignal.timeout(LOCK_WAIT) }, async () => {
            const [gamedir, name] = src.split('/');
            // Kept already, as by another page that held src a moment ago;
            // or this version's place: src's own, unless another version
            // holds it, in a folder named for its checksum. Its record is
            // out of the index until it's written.
            const { kept, path } = await withIndex(async () => {
                const index = await readIndex();
                const versions = index.files.filter((record) => record.src === src);
                const same = versions.find((record) => record.checksum === checksum);
                if (same?.size === file.size && (await fileAt(same.path))?.size === same.size) return { kept: same };
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
            await changeIndex((index) => {
                index.files.push({ src, path, name: file.name, checksum, size: file.size, mtime: file.lastModified });
            });
            return { path };
        }).catch((error) => {
        if (error?.name === 'AbortError' || error?.name === 'TimeoutError') return { busy: true };
        throw error;
    }),

    drop: async ({ path }) => {
        await changeIndex((index) => {
            index.files = index.files.filter((record) => record.path !== path);
        });
        await remove(path).catch(() => {});
        return {};
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
