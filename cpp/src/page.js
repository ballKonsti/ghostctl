(SEL) => {
  if (window.__ghostctl) return window.__ghostctl.ok;
  // innerText keeps line breaks but applies CSS text-transform (sender names are
  // uppercased); use it for message text only and textContent for labels.
  const txt = (e) => (e ? (e.innerText || e.textContent || '').trim() : '');
  const raw = (e) => (e ? (e.textContent || '').replace(/\s+/g, ' ').trim() : '');

  function readFeed() {
    const feed = document.querySelector(SEL.feed);
    if (!feed) return null;
    const rows = [];
    for (const row of feed.querySelectorAll(SEL.feed_row)) {
      const title = row.querySelector(SEL.feed_row_title);
      if (!title) continue;
      const id = title.id.slice('title-'.length);
      const status = row.querySelector(SEL.feed_row_status);
      const time = row.querySelector(SEL.feed_row_time);
      // Extras: text after the time ("941 🔥"); badge: text on the avatar (friend emoji).
      const known = [title, status, time].filter(Boolean);
      const extras = [], badges = [];
      const seen = new Set();
      const walker = document.createTreeWalker(row, NodeFilter.SHOW_TEXT);
      for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const el = n.parentElement;
        const t = raw(el);
        if (!t || t === '·' || t === 'View' || seen.has(el) || known.some(k => k.contains(n))) continue;
        seen.add(el);  // one entry per element, so "941" + "🔥" stay together
        const beforeTitle = title.compareDocumentPosition(n) & Node.DOCUMENT_POSITION_PRECEDING;
        (beforeTitle ? badges : extras).push(t);
      }
      rows.push({
        id, name: raw(title), status: raw(status),
        time: time ? time.getAttribute('datetime') : null,
        extras, badge: badges.join(' '),
        group: row.querySelectorAll(SEL.feed_row_avatar).length > 1,
        top: row.getBoundingClientRect().top,
      });
    }
    rows.sort((a, b) => a.top - b.top);
    return rows;
  }

  function readMessage(li, ctx) {
    const header = li.querySelector(SEL.msg_header);
    if (header) {
      const t = header.querySelector('time[datetime]');
      ctx.sender = raw(header).replace(t ? raw(t) : '', '').trim();
      ctx.time = t ? t.getAttribute('datetime') : null;
      ctx.idx = 0;
    }
    const body = [...li.children].find(c => c.tagName === 'DIV');
    const reactions = li.querySelector(SEL.msg_reactions);
    const m = {kind: 'msg', sender: ctx.sender, time: ctx.time, text: '', quote_sender: '',
               quote_text: '', media: [], snap_status: '', snap_new: false, reactions: [],
               not_supported: false, duration: 0};
    if (reactions) {
      const imgs = [...reactions.querySelectorAll(SEL.msg_reaction_img)];
      m.reactions = imgs.length
        ? imgs.map(i => i.alt.replace(/^Reaction /, '').replace(/ from /, ' · '))
        : (raw(reactions) ? [raw(reactions)] : []);
    }
    if (body) {
      const qul = body.querySelector(SEL.msg_quote_list);
      const quote = qul ? qul.parentElement : null;
      if (quote) {
        m.quote_text = txt(qul);
        m.quote_sender = raw(quote).replace(raw(qul), '').trim();
      }
      m.text = [...body.querySelectorAll(SEL.msg_text)]
        .filter(s => !(quote && quote.contains(s))).map(txt).join('\n');
      for (const v of body.querySelectorAll('video')) m.media.push('video');
      for (const a of body.querySelectorAll('audio')) {  // voice note
        m.media.push('audio');
        m.duration = isFinite(a.duration) ? a.duration : 0;
      }
      if (!m.text) {  // a bare link renders as <a>, not as a text span
        const links = [...body.querySelectorAll('a[href]')].filter(l => !(quote && quote.contains(l)));
        if (links.length) m.text = links.map(l => raw(l) || l.href).join('\n');
      }
      for (const i of body.querySelectorAll('img')) if (!(quote && quote.contains(i))) m.media.push('image');
      const all = raw(body);
      if (all.includes(SEL.__not_supported)) { m.not_supported = true; m.text = ''; }
      else if (!m.text && !m.media.length) {
        m.snap_new = all.includes('Click to view');
        m.snap_status = all.replace('Click to view', '').trim();
      }
    }
    m.key = `${ctx.time}|${ctx.sender}|${ctx.idx++}`;
    return [m, li];
  }

  // Walk the open conversation. Returns [items, Map(key -> li element)].
  function walk() {
    const ul = document.querySelector(SEL.conv_list);
    if (!ul) return null;
    const items = [], els = new Map();
    for (const li of ul.querySelectorAll(SEL.conv_item)) {
      const msgs = li.querySelectorAll(SEL.msg_item);
      if (msgs.length) {
        const ctx = {sender: '', time: null, idx: 0};
        for (const el of msgs) { const [m, e] = readMessage(el, ctx); items.push(m); els.set(m.key, e); }
        continue;
      }
      const t = li.querySelector('time');
      if (t && !raw(li).replace(raw(t), '').trim()) { items.push({kind: 'date', label: raw(t)}); continue; }
      const n = txt(li);
      if (n) items.push({kind: 'notice', text: n});
    }
    return [ul, items, els];
  }

  function readActivity(ul) {
    const region = ul.parentElement.querySelector(':scope > ul[id^="cv-"] + div') || ul.nextElementSibling;
    if (!region) return {seen_by: [], activity: '', typing: false};
    const chips = [...region.querySelectorAll('[role=link]')].map(raw).filter(Boolean);
    let rest = raw(region);
    for (const c of chips) rest = rest.replace(c, '');
    rest = rest.trim();
    return {seen_by: chips, activity: rest, typing: /typing/i.test(rest)};
  }

  function readConversation() {
    const w = walk();
    if (!w) return null;
    const [ul, items] = w;
    return {id: ul.id.slice('cv-'.length), items, ...readActivity(ul)};
  }

  // Tag one message element so Playwright can act on it.
  function target(key) {
    document.querySelectorAll('[data-ghostctl-target]').forEach(e => e.removeAttribute('data-ghostctl-target'));
    const w = walk();
    const el = w && w[2].get(key);
    if (!el) return false;
    el.setAttribute('data-ghostctl-target', '1');
    el.scrollIntoView({block: 'center'});
    return true;
  }

  // Find the snap/story viewer media and tag it. Prefers the "media content"
  // region; falls back to the largest image/video outside the chat panes.
  function findViewer() {
    document.querySelectorAll('[data-ghostctl-viewer]').forEach(e => e.removeAttribute('data-ghostctl-viewer'));
    let best = [...document.querySelectorAll(SEL.viewer_media)].find(e => e.getBoundingClientRect().width > 0);
    let exact = !!best;
    if (!best) {
      const vw = innerWidth, vh = innerHeight;
      let bestArea = 0;
      for (const el of document.querySelectorAll('img, video, canvas')) {
        if (el.closest(SEL.feed) || el.closest(SEL.conv_list) || el.alt === '') continue;
        const r = el.getBoundingClientRect();
        if (r.width < 120 || r.height < 120 || r.bottom < 0 || r.top > vh) continue;
        const area = r.width * r.height;
        if (area > bestArea && area > 0.12 * vw * vh) { best = el; bestArea = area; }
      }
    }
    if (!best) return null;
    best.setAttribute('data-ghostctl-viewer', '1');
    // Sender name and time shown above the media.
    let sender = '', time = null;
    for (let e = best.closest('[aria-label="media content"]') || best; e && e !== document.body; e = e.parentElement) {
      const t = e.querySelector('time[datetime]');
      if (t && !t.closest(SEL.feed) && !t.closest(SEL.conv_list)) {
        time = t.getAttribute('datetime');
        let row = t.parentElement;
        while (row && row.parentElement && !raw(row.parentElement).replace(raw(row), '').trim()) row = row.parentElement;
        sender = row && row.parentElement ? raw(row.parentElement).replace(raw(row), '').trim() : '';
        break;
      }
    }
    return {tag: best.tagName.toLowerCase(), src: best.currentSrc || best.src || '', exact, sender, time};
  }

  // Current camera frame (or any video/img/canvas) as a JPEG data URL, scaled to maxW.
  function frame(sel, maxW) {
    const v = document.querySelector(sel);
    if (!v) return null;
    const w0 = v.videoWidth || v.naturalWidth || v.width, h0 = v.videoHeight || v.naturalHeight || v.height;
    if (!w0 || !h0) return null;
    const scale = Math.min(1, maxW / w0);
    const c = document.createElement('canvas');
    c.width = Math.round(w0 * scale); c.height = Math.round(h0 * scale);
    c.getContext('2d').drawImage(v, 0, 0, c.width, c.height);
    return c.toDataURL('image/jpeg', 0.8);
  }

  // Recipients in the "Send To" picker. Tags each li with data-ghostctl-rcpt.
  function readRecipients(mark) {
    const form = document.querySelector(SEL.sendto_form);
    if (!form) return null;
    const out = [];
    let section = '', idx = 0;
    const walker = document.createTreeWalker(form, NodeFilter.SHOW_ELEMENT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
      if (n.tagName === 'H2') { section = raw(n); continue; }
      if (n.tagName !== 'LI' || n.querySelector('h2') || n.querySelector('li')) continue;
      const texts = [];
      const tw = document.createTreeWalker(n, NodeFilter.SHOW_TEXT);
      for (let t = tw.nextNode(); t; t = tw.nextNode()) { const x = t.textContent.trim(); if (x) texts.push(x); }
      if (!texts.length) continue;
      n.setAttribute('data-ghostctl-rcpt', String(idx));
      out.push({idx: idx++, section, name: texts[0], extra: texts.slice(1).join(' '),
                // Every row has the checkmark; only selected rows show it.
                selected: [...n.querySelectorAll(`img[alt="${mark}"], [aria-label="${mark}"]`)]
                  .some(m => getComputedStyle(m).visibility === 'visible')});
    }
    return out;
  }

  async function fetchSrc(src) {
    const r = await fetch(src);
    const buf = new Uint8Array(await r.arrayBuffer());
    let s = '';
    for (let i = 0; i < buf.length; i += 0x8000) s += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
    return {mime: r.headers.get('content-type') || '', b64: btoa(s)};
  }

  // Media elements inside the tagged message (excluding quotes and reactions).
  function targetMedia() {
    const el = document.querySelector('[data-ghostctl-target]');
    if (!el) return [];
    const body = [...el.children].find(c => c.tagName === 'DIV');
    if (!body) return [];
    const q = body.querySelector(SEL.msg_quote_list);
    return [...body.querySelectorAll('img, video, audio')]
      .filter(m => !(q && q.parentElement.contains(m)))
      .map(m => ({tag: m.tagName.toLowerCase(),
                  src: m.currentSrc || m.src || (m.querySelector('source') || {}).src || ''}));
  }

  // Push changes: debounce per region, then hand a fresh read to Python.
  const pending = new Set();
  let timer = null;
  const flush = () => {
    timer = null;
    for (const region of pending) {
      try {
        const data = region === 'feed' ? readFeed() : readConversation();
        if (data) window.__ghostctl_emit(region, JSON.stringify(data));
      } catch (e) { window.__ghostctl_emit('error', String(e)); }
    }
    pending.clear();
  };
  const obs = new MutationObserver((records) => {
    for (const r of records) {
      const el = r.target.nodeType === 1 ? r.target : r.target.parentElement;
      if (!el) continue;
      if (el.closest(SEL.feed)) pending.add('feed');
      else if (el.closest(SEL.conv_list) || el.querySelector?.(SEL.conv_list)
               || el.closest('[id^="cv-"] + div')) pending.add('conv');
    }
    if (pending.size && !timer) timer = setTimeout(flush, 250);
  });
  obs.observe(document.body, {childList: true, subtree: true, characterData: true});

  window.__ghostctl = {ok: true, readFeed, readConversation, target, findViewer, fetchSrc, targetMedia, frame, readRecipients};
  return true;
}
