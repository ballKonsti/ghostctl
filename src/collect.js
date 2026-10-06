() => {
  const hooks = ['role','aria-label','aria-labelledby','aria-selected','aria-expanded',
                 'aria-live','title','data-testid','placeholder','contenteditable','alt','type','name'];
  const out = [];
  const pathOf = (el) => {
    const parts = [];
    for (let e = el; e && e.nodeType === 1 && parts.length < 6; e = e.parentElement) {
      let p = e.tagName.toLowerCase();
      const r = e.getAttribute('role'); if (r) p += `[role=${r}]`;
      const a = e.getAttribute('aria-label'); if (a) p += `[aria-label="${a.slice(0,30)}"]`;
      parts.unshift(p);
    }
    return parts.join(' > ');
  };
  for (const el of document.querySelectorAll('*')) {
    const attrs = {};
    for (const h of hooks) if (el.hasAttribute(h)) attrs[h] = el.getAttribute(h);
    if (!Object.keys(attrs).length && !['BUTTON','INPUT','TEXTAREA','VIDEO','IMG','CANVAS'].includes(el.tagName)) continue;
    const rect = el.getBoundingClientRect();
    out.push({
      tag: el.tagName.toLowerCase(),
      attrs,
      cls: (typeof el.className === 'string' ? el.className : '').slice(0, 80),
      text: (el.innerText || '').trim().replace(/\s+/g, ' ').slice(0, 80),
      visible: rect.width > 0 && rect.height > 0,
      box: [Math.round(rect.x), Math.round(rect.y), Math.round(rect.width), Math.round(rect.height)],
      path: pathOf(el),
    });
  }
  return out;
}
