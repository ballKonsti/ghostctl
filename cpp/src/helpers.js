(() => {
  // A small stand-in for Playwright selectors, used by the C++ side:
  //   "role=button[name='Not now']"   role + accessible name (exact or /regex/i)
  //   "text='Click to view'"          exact visible text   ("text=/re/i" for regex)
  //   "css selector:visible"          CSS, optionally visible-only
  //   "A >> B"                        B searched inside matches of A
  if (window.__gc) return true;

  const IMPLICIT = {
    button: 'button, input[type=button], input[type=submit]',
    heading: 'h1, h2, h3, h4, h5, h6',
    textbox: 'input:not([type]), input[type=text], input[type=email], input[type=tel], input[type=password], textarea',
    searchbox: 'input[type=search]',
    list: 'ul, ol',
    listitem: 'li',
    link: 'a[href]',
  };

  const visible = (el) => {
    if (!el || !el.isConnected) return false;
    const r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    const s = getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none' && +s.opacity !== 0;
  };

  const accName = (el) => {
    const label = el.getAttribute('aria-label');
    if (label) return label.trim();
    const by = el.getAttribute('aria-labelledby');
    if (by) {
      const t = by.split(/\s+/).map(id => document.getElementById(id)).filter(Boolean)
        .map(e => e.textContent.trim()).join(' ');
      if (t) return t;
    }
    if (el.labels && el.labels.length) return el.labels[0].textContent.trim();
    if (el.placeholder) return el.placeholder.trim();
    if (el.title) return el.title.trim();
    return (el.textContent || '').replace(/\s+/g, ' ').trim();
  };

  const matcher = (spec) => {
    const re = spec.match(/^\/(.*)\/([a-z]*)$/s);
    if (re) { const r = new RegExp(re[1], re[2]); return (s) => r.test(s); }
    const lit = spec.replace(/^['"]|['"]$/g, '');
    return (s) => s === lit;
  };

  const one = (sel, roots) => {
    sel = sel.trim();
    let onlyVisible = false;
    if (sel.endsWith(':visible')) { onlyVisible = true; sel = sel.slice(0, -8); }
    let out = [];
    if (sel.startsWith('role=')) {
      const m = sel.match(/^role=([a-z]+)(?:\[name=(.*)\])?$/s);
      const [_, role, nameSpec] = m;
      const css = `[role="${role}"]` + (IMPLICIT[role] ? ', ' + IMPLICIT[role] : '');
      const test = nameSpec ? matcher(nameSpec) : null;
      for (const root of roots)
        for (const el of root.querySelectorAll(css)) {
          const r = el.getAttribute('role');
          if (r && r !== role) continue;
          if (test && !test(accName(el))) continue;
          out.push(el);
        }
    } else if (sel.startsWith('text=')) {
      const test = matcher(sel.slice(5));
      for (const root of roots)
        for (const el of root.querySelectorAll('*')) {
          if (!el.childElementCount || [...el.childNodes].every(n => n.nodeType !== 3)) {
            if (test((el.textContent || '').trim())) out.push(el);
          } else if ([...el.childNodes].some(n => n.nodeType === 3 && test(n.textContent.trim()))) {
            out.push(el);
          }
        }
    } else {
      for (const root of roots) out.push(...root.querySelectorAll(sel));
    }
    if (onlyVisible) out = out.filter(visible);
    return out;
  };

  const all = (sel) => {
    let roots = [document];
    for (const part of sel.split('>>')) roots = one(part, roots);
    return roots;
  };

  const first = (sel, wantVisible = true) => {
    const els = all(sel);
    return (wantVisible ? els.find(visible) : els[0]) || null;
  };

  window.__gc = {
    visible, accName, all, first,
    count: (sel) => all(sel).length,
    isVisible: (sel) => !!first(sel, true),
    texts: (sel) => all(sel).filter(visible).map(e => (e.innerText || '').trim()).filter(Boolean),
    // Centre of the first visible match, scrolled into view (for real mouse events).
    point: (sel, nth = 0) => {
      const el = all(sel).filter(visible)[nth];
      if (!el) return null;
      el.scrollIntoView({block: 'center', inline: 'center'});
      const r = el.getBoundingClientRect();
      return {x: r.x + r.width / 2, y: r.y + r.height / 2, w: r.width, h: r.height};
    },
    rect: (sel) => {
      const el = first(sel, true);
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return {x: r.x, y: r.y, w: r.width, h: r.height};
    },
    value: (sel) => { const el = first(sel, false); return el ? (el.value ?? el.innerText ?? '') : null; },
    url: () => location.href,
  };
  return true;
})()
