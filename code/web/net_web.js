// The page's half of a web client's connections to servers (net_web.c,
// P7): a WebRTC peer connection each, to the server's gateway, with the
// two data channels the gateway agrees by id, "q3" unordered and never
// resent, "oob" reliable.
//
// A connection's offer goes to its signal as fields rather than SDP
// (docs/contracts.md section 9): the ICE username fragment and password,
// the DTLS certificate's SHA-256, and the candidates. The answer comes back
// as the same fields, from which the remote description is written; the
// gateway is the DTLS client. The signal is a function: rtc_connect's
// posts to a path on the page's own server (NetWeb.post), and joining by
// code's (the game's own join_web.js) asks the rendezvous and checks the
// answer's signature, with this file unchanged.
//
// Packets the engine sends before both channels open wait, a few, and go
// then. Received ones wait in one queue for the engine's next frame; past
// its length the newest are dropped, as a socket's buffer would.
//
// A gateway that lets a connection go tells the server first, which drops
// the player at once, so the server's own disconnect usually reaches the
// page before anything else. A quarter of a second later the gateway says
// why, in one message on oob, "\xff\xff\xff\xffomnifrag-close <word>", then
// closes the connection: idle when the page was silent too long, as a
// hidden tab's is, shutdown when the gateway stops, and replaced when
// another connection took its place; any other word, or none (ICE
// failing, the network gone, the computer asleep), is a plain loss. The
// engine never sees the message, which this file takes as it arrives: the
// connection counts as lost from it on. A page that shows read the
// server's disconnect first, and its own disconnect closed the connection;
// a hidden page, whose engine runs no frames, still holds that disconnect
// unread, and finds the connection lost when it reads it. One lost as
// idle, while the page was hidden, or within AWAY_MS of the page being
// shown again after HIDDEN_MS or more hidden while it was open, was lost
// while the player was away, which the engine words as such and rejoins;
// a replaced one it doesn't rejoin.

addToLibrary({
  $NetWeb: {
    peers: {},
    next: 1,
    // received packets, read from head on; emptied once all are read
    received: [],
    head: 0,
    // the queue's length, and the packets kept per connection before it
    // opens
    MAX_RECEIVED: 1024,
    // the engine's MAX_MSGLEN, the most it reads of one (net_web.c)
    MAX_MESSAGE: 16384,
    MAX_PENDING: 16,
    // bytes waiting on q3 past which a send is refused, as a full socket's
    BUFFERED_MAX: 65536,
    // how long gathering may take before the offer goes, and the channels
    // to open once the answer is in
    GATHER_MS: 3000,
    OPEN_MS: 10000,
    // a loss this soon after the page was shown again, hidden at least
    // HIDDEN_MS while the connection was open, was the hidden time's: a
    // gateway lets a silent connection go after 30 seconds, and the
    // browser may take as long again to say the connection failed
    AWAY_MS: 60000,
    HIDDEN_MS: 10000,
    watching: false,
    // the close message's start, and each word's words and how it was
    // lost: 1 more than a netLoss_t, lost (1), away (2) or for good (3)
    CLOSE: '\xff\xff\xff\xffomnifrag-close ',
    CLOSED: {
      idle: { why: 'The server let the connection go while the game was hidden.', loss: 2 },
      shutdown: { why: 'The server\'s gateway stopped.', loss: 1 },
      replaced: { why: 'The game connected to the server again from elsewhere.', loss: 3 },
    },

    // A connection whose offer goes to signal, a function of the offer's
    // fields that resolves to the answer's, or to { refused: why }. Its
    // errors end the connection, an Error with plain set in its own words.
    // oobAbove: a connectionless packet longer than this goes on oob, as
    // the gateway's replies do: an unreliable message that long is split
    // by SCTP, and lost with either part. iceServers: RTCIceServers for
    // the browser's own candidates. The connection's id, or 0 without
    // WebRTC
    openWith: function(signal, oobAbove, iceServers) {
      if (typeof RTCPeerConnection != 'function' || NetWeb.next > 65535) return 0;
      if (!NetWeb.watching) {
        NetWeb.watching = true;
        document.addEventListener('visibilitychange', NetWeb.visibility);
      }
      var peer = { id: NetWeb.next++, oobAbove: oobAbove, pending: [], open: false, failure: '', closed: false,
        loss: 1, reason: '', hiddenAt: document.hidden ? Date.now() : 0, awayUntil: 0 };
      NetWeb.peers[peer.id] = peer;
      NetWeb.start(peer, signal, iceServers || []).catch(function(e) {
        NetWeb.fail(peer, e && e.plain ? e.message : 'The connection to the server failed: ' + (e && e.message || e) + '.');
      });
      return peer.id;
    },

    // a message from the server, queued for the engine; one longer than
    // the engine reads isn't kept, or a server could fill the queue with
    // the largest messages SCTP takes, and nor is a text one, whose length
    // a string's isn't. The close word is read on oob alone, as the gateway
    // writes it; on q3 the same bytes are a packet. A full queue drops its
    // oldest: a hidden page reads nothing, and the newest carry what the
    // server said last, a kick's disconnect among them, as every packet
    // carries the reliable commands not acknowledged yet. An older one
    // lost is a packet the netchan counts as dropped
    take: function(peer, data, oob) {
      if (!(data instanceof ArrayBuffer) || data.byteLength > NetWeb.MAX_MESSAGE) return;
      if (oob && NetWeb.closing(peer, new Uint8Array(data))) return;
      if (NetWeb.received.length - NetWeb.head >= NetWeb.MAX_RECEIVED) NetWeb.received.splice(NetWeb.head, 1);
      NetWeb.received.push({ id: peer.id, data: new Uint8Array(data) });
    },

    // A POST of body to url, on the page's own server, or the shared site's
    // page's server (its centralServer when it has its own origin, or the
    // page's own origin for a code join), within OPEN_MS: its response's
    // status, ok and json(), the body read within the time too. The URL
    // parser reads a backslash as a slash and drops tabs, so the origin is
    // checked after it, not on the text
    request: async function(url, body) {
      url = new URL(url, location.href);
      var server = Module.centralServer;
      var origin = (server && server.origin) || location.origin;
      if (url.origin != origin || (server && server.code && url.pathname !== '/j/' + server.code + '/rtc')) {
        throw new Error('its signal is on another server');
      }
      // a signal that never answers fails, as no route would, and so does
      // one whose headers come and whose body doesn't
      var abort = new AbortController();
      var timer = setTimeout(function() { abort.abort(); }, NetWeb.OPEN_MS);
      try {
        var response = await fetch(url, { method: 'POST', body: body, signal: abort.signal });
        var text = await response.text();
        return { status: response.status, ok: response.ok, json: async function() { return JSON.parse(text); } };
      } catch (e) {
        throw abort.signal.aborted ? new Error("its signal didn't answer") : e;
      } finally {
        clearTimeout(timer);
      }
    },

    // the signal of a path on the page's own server, which answers the
    // offer's fields with the answer's
    post: function(path) {
      return async function(offer) {
        var response = await NetWeb.request(path, JSON.stringify(offer));
        if (!response.ok) throw new Error('its signal answered ' + response.status);
        return response.json();
      };
    },

    start: async function(peer, signal, iceServers) {
      var pc = peer.pc = new RTCPeerConnection({ iceServers: iceServers });
      peer.q3 = pc.createDataChannel('q3', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
      peer.oob = pc.createDataChannel('oob', { negotiated: true, id: 1 });
      [peer.q3, peer.oob].forEach(function(channel) {
        channel.binaryType = 'arraybuffer';
        channel.onmessage = function(e) { NetWeb.take(peer, e.data, channel === peer.oob); };
        channel.onopen = function() { NetWeb.opened(peer); };
        channel.onclose = function() { NetWeb.fail(peer, 'The server closed the connection.'); };
      });
      pc.onconnectionstatechange = function() {
        if (pc.connectionState == 'failed') {
          NetWeb.fail(peer, 'The connection to the server was lost.');
        }
      };

      await pc.setLocalDescription(await pc.createOffer());
      await Promise.race([
        new Promise(function(done) {
          if (pc.iceGatheringState == 'complete') done();
          pc.onicegatheringstatechange = function() { if (pc.iceGatheringState == 'complete') done(); };
        }),
        new Promise(function(done) { setTimeout(done, NetWeb.GATHER_MS); }),
      ]);
      if (peer.closed) return;

      var answer = await signal(NetWeb.offer(pc.localDescription.sdp));
      if (peer.closed) return;
      // a server for its local network alone (sv_visibility local) says so
      if (answer && answer.refused == 'local') {
        throw new Error('the server is for players on its local network only');
      }
      if (answer && typeof answer.refused == 'string') {
        throw new Error('the server turned it down (' + answer.refused.slice(0, 40) + ')');
      }
      await pc.setRemoteDescription({ type: 'answer', sdp: NetWeb.answer(answer) });
      setTimeout(function() {
        if (!peer.open) NetWeb.fail(peer, 'No route to the server: your network may block this game.');
      }, NetWeb.OPEN_MS);
    },

    // the fields of the offer's SDP the gateway needs. Host candidates the
    // browser names by mDNS are left out: the gateway can't resolve them,
    // and finds the browser's address from its checks instead
    offer: function(sdp) {
      var get = function(re) { return (sdp.match(re) || [])[1]; };
      var candidates = [];
      // any case: Firefox writes the transport as UDP
      for (var m of sdp.matchAll(/a=candidate:\S+ 1 udp \d+ (\S+) (\d+) typ (\w+)/gi)) {
        if (m[1].endsWith('.local')) continue;
        candidates.push({ kind: m[3], address: m[1].includes(':') ? '[' + m[1] + ']:' + m[2] : m[1] + ':' + m[2] });
      }
      return {
        ufrag: get(/a=ice-ufrag:(\S+)/),
        pwd: get(/a=ice-pwd:(\S+)/),
        fingerprint: (get(/a=fingerprint:sha-256 (\S+)/i) || '').replace(/:/g, '').toLowerCase(),
        candidates: candidates,
      };
    },

    // the answer's SDP, from the gateway's fields, each checked first so
    // nothing else gets into it
    answer: function(a) {
      var ice = /^[A-Za-z0-9+\/]{4,256}$/;
      var where = /^(\d{1,3}(\.\d{1,3}){3}|\[[0-9A-Fa-f:.]{2,45}\]):(\d{1,5})$/;
      var preference = { host: 126, prflx: 110, srflx: 100, relay: 0 };
      if (!a || !ice.test(a.ufrag) || !ice.test(a.pwd) || !/^[0-9a-f]{64}$/.test(a.fingerprint) ||
          !Array.isArray(a.candidates) || a.candidates.length > 16 ||
          !a.candidates.every(function(c) { return c && preference.hasOwnProperty(c.kind) && where.test(c.address); })) {
        throw new Error("the server's answer was malformed");
      }
      var lines = [
        'v=0', 'o=- ' + Date.now() + ' 2 IN IP4 127.0.0.1', 's=-', 't=0 0', 'a=group:BUNDLE 0',
        'm=application 9 UDP/DTLS/SCTP webrtc-datachannel', 'c=IN IP4 0.0.0.0',
        'a=ice-ufrag:' + a.ufrag, 'a=ice-pwd:' + a.pwd,
        'a=fingerprint:sha-256 ' + a.fingerprint.toUpperCase().match(/../g).join(':'),
        'a=setup:active', 'a=mid:0', 'a=sctp-port:5000', 'a=max-message-size:262144',
      ];
      a.candidates.forEach(function(c, i) {
        var at = c.address.lastIndexOf(':');
        var ip = c.address.slice(0, at).replace(/^\[|\]$/g, ''), port = c.address.slice(at + 1);
        var priority = (preference[c.kind] << 24 | (65535 - i) << 8 | 255) >>> 0;
        // Firefox drops a candidate other than a host one without its
        // related address; the answer doesn't carry the gateway's base, and
        // the unspecified address of the candidate's family, port 0, is
        // the form a browser itself writes to hide one
        lines.push('a=candidate:' + (i + 1) + ' 1 udp ' + priority + ' ' + ip + ' ' + port + ' typ ' + c.kind
          + (c.kind == 'host' ? '' : ' raddr ' + (ip.includes(':') ? '::' : '0.0.0.0') + ' rport 0'));
      });
      lines.push('a=end-of-candidates');
      return lines.join('\r\n') + '\r\n';
    },

    // both channels open: what waited goes
    opened: function(peer) {
      if (peer.open || peer.closed || peer.q3.readyState != 'open' || peer.oob.readyState != 'open') return;
      peer.open = true;
      var pending = peer.pending;
      peer.pending = [];
      pending.forEach(function(data) { NetWeb.send(peer.id, data); });
    },

    send: function(id, data) {
      var peer = NetWeb.peers[id];
      // gone, as a datagram to nobody
      if (!peer || peer.failure) return 1;
      if (!peer.open) {
        if (peer.pending.length < NetWeb.MAX_PENDING) peer.pending.push(data);
        return 1;
      }
      var connectionless = data.length >= 4 && data[0] == 255 && data[1] == 255 && data[2] == 255 && data[3] == 255;
      var channel = connectionless && data.length > peer.oobAbove ? peer.oob : peer.q3;
      if (channel.bufferedAmount > NetWeb.BUFFERED_MAX) return 0;
      try {
        channel.send(data);
      } catch (e) {
        // closing: its onclose says so
      }
      return 1;
    },

    // a note to the gateway that the page won't send for a while, seconds
    // at most: the busy note, on oob as a connectionless packet's bytes
    // start, which the gateway takes itself and never forwards; nothing for
    // a connection not open
    busy: function(id, seconds) {
      var peer = NetWeb.peers[id];
      if (!peer || !peer.open || peer.failure || peer.oob.readyState != 'open') return;
      var text = 'omnifrag-busy ' + seconds;
      var data = new Uint8Array(4 + text.length);
      data.set([255, 255, 255, 255]);
      for (var i = 0; i < text.length; i++) data[4 + i] = text.charCodeAt(i);
      try {
        peer.oob.send(data);
      } catch (e) {
        // closing: its onclose says so
      }
    },

    // each open connection's hidden stretch, and the time after it that a
    // loss still counts as the player's being away
    visibility: function() {
      var now = Date.now();
      Object.values(NetWeb.peers).forEach(function(peer) {
        if (document.hidden) {
          peer.hiddenAt = now;
        } else if (peer.hiddenAt) {
          if (now - peer.hiddenAt >= NetWeb.HIDDEN_MS) peer.awayUntil = now + NetWeb.AWAY_MS;
          peer.hiddenAt = 0;
        }
      });
    },

    // the gateway's word on why it lets the connection go, which ends it
    // here at once; a server's disconnect that came before it and is still
    // unread, as a hidden page's is, then finds it lost
    closing: function(peer, bytes) {
      var head = NetWeb.CLOSE.length;
      if (bytes.length <= head || bytes.length > head + 32) return false;
      for (var i = 0; i < head; i++) if (bytes[i] != NetWeb.CLOSE.charCodeAt(i)) return false;
      peer.reason = String.fromCharCode.apply(null, bytes.subarray(head)).trim();
      NetWeb.fail(peer, 'The server closed the connection.');
      return true;
    },

    fail: function(peer, why) {
      if (peer.failure || peer.closed) return;
      var closed = Object.hasOwn(NetWeb.CLOSED, peer.reason) ? NetWeb.CLOSED[peer.reason] : null;
      peer.failure = closed ? closed.why : why;
      peer.loss = closed && closed.loss != 1 ? closed.loss :
        document.hidden || Date.now() < peer.awayUntil ? 2 : 1;
      if (peer.pc) peer.pc.close();
    },

    close: function(id) {
      var peer = NetWeb.peers[id];
      if (!peer) return;
      delete NetWeb.peers[id];
      peer.closed = true;
      // after a moment, so the disconnect sent just before goes
      setTimeout(function() { if (peer.pc) peer.pc.close(); }, 1000);
    },
  },

  NetWeb_Open__deps: ['$NetWeb', '$UTF8ToString'],
  NetWeb_Open: function(signal, oobAbove) {
    return NetWeb.openWith(NetWeb.post(UTF8ToString(signal)), oobAbove);
  },

  NetWeb_Send__deps: ['$NetWeb'],
  NetWeb_Send: function(id, data, length) {
    // a copy: the heap can move as it grows
    return NetWeb.send(id, HEAPU8.slice(data, data + length));
  },

  NetWeb_Receive__deps: ['$NetWeb'],
  NetWeb_Receive: function(buffer, size, id) {
    for (;;) {
      if (NetWeb.head == NetWeb.received.length) {
        NetWeb.received = [];
        NetWeb.head = 0;
        return -1;
      }
      var packet = NetWeb.received[NetWeb.head++];
      if (packet.data.length > size) continue;
      HEAPU8.set(packet.data, buffer);
      HEAP32[id >> 2] = packet.id;
      return packet.data.length;
    }
  },

  NetWeb_Failure__deps: ['$NetWeb', '$stringToUTF8'],
  NetWeb_Failure: function(id, buffer, size) {
    var peer = NetWeb.peers[id];
    var failure = peer ? peer.failure : 'The connection to the server is closed.';
    if (!failure) return 0;
    stringToUTF8(failure, buffer, size);
    return peer ? peer.loss : 1;
  },

  NetWeb_Busy__deps: ['$NetWeb'],
  NetWeb_Busy: function(id, seconds) {
    NetWeb.busy(id, seconds);
  },

  NetWeb_Close__deps: ['$NetWeb'],
  NetWeb_Close: function(id) {
    NetWeb.close(id);
  },
});
