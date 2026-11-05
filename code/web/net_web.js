// The page's half of a web client's connections to servers (net_web.c,
// P7): a WebRTC peer connection each, to the server's gateway, with the
// two data channels the gateway agrees by id, "q3" unordered and never
// resent, "oob" reliable.
//
// A connection's offer goes to its signal, a path on the page's own server,
// as fields rather than SDP (docs/contracts.md section 9): the ICE username
// fragment and password, the DTLS certificate's SHA-256, and the
// candidates. The answer comes back as the same fields, from which the
// remote description is written; the gateway is the DTLS client. Joining
// by code finds the answer through the rendezvous instead, and only that
// part changes.
//
// Packets the engine sends before both channels open wait, a few, and go
// then. Received ones wait in one queue for the engine's next frame; past
// its length the newest are dropped, as a socket's buffer would.

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
    MAX_PENDING: 16,
    // bytes waiting on q3 past which a send is refused, as a full socket's
    BUFFERED_MAX: 65536,
    // how long gathering may take before the offer goes, and the channels
    // to open once the answer is in
    GATHER_MS: 3000,
    OPEN_MS: 10000,

    // oobAbove: a connectionless packet longer than this goes on oob, as
    // the gateway's replies do: an unreliable message that long is split
    // by SCTP, and lost with either part
    open: function(signal, oobAbove) {
      if (typeof RTCPeerConnection != 'function' || NetWeb.next > 65535) return 0;
      var peer = { id: NetWeb.next++, oobAbove: oobAbove, pending: [], open: false, failure: '', closed: false };
      NetWeb.peers[peer.id] = peer;
      NetWeb.start(peer, signal).catch(function(e) {
        NetWeb.fail(peer, 'The connection to the server failed: ' + (e && e.message || e) + '.');
      });
      return peer.id;
    },

    start: async function(peer, signal) {
      var pc = peer.pc = new RTCPeerConnection();
      peer.q3 = pc.createDataChannel('q3', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
      peer.oob = pc.createDataChannel('oob', { negotiated: true, id: 1 });
      [peer.q3, peer.oob].forEach(function(channel) {
        channel.binaryType = 'arraybuffer';
        channel.onmessage = function(e) {
          if (NetWeb.received.length - NetWeb.head < NetWeb.MAX_RECEIVED) {
            NetWeb.received.push({ id: peer.id, data: new Uint8Array(e.data) });
          }
        };
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

      // on the page's own server only: the URL parser reads a backslash as
      // a slash and drops tabs, so "/\host" would be another server's
      var url = new URL(signal, location.href);
      if (url.origin != location.origin) throw new Error('its signal is on another server');
      // and a signal that never answers fails, as no route would
      var abort = new AbortController();
      var timer = setTimeout(function() { abort.abort(); }, NetWeb.OPEN_MS);
      var answer;
      try {
        var response = await fetch(url, { method: 'POST', body: JSON.stringify(NetWeb.offer(pc.localDescription.sdp)), signal: abort.signal });
        if (!response.ok) throw new Error('its signal answered ' + response.status);
        answer = await response.json();
      } catch (e) {
        throw abort.signal.aborted ? new Error("its signal didn't answer") : e;
      } finally {
        clearTimeout(timer);
      }
      if (peer.closed) return;
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

    fail: function(peer, why) {
      if (peer.failure || peer.closed) return;
      peer.failure = why;
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
    return NetWeb.open(UTF8ToString(signal), oobAbove);
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
    return 1;
  },

  NetWeb_Close__deps: ['$NetWeb'],
  NetWeb_Close: function(id) {
    NetWeb.close(id);
  },
});
