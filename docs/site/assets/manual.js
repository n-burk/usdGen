// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT
(() => {
  const dialog = document.querySelector('.search-dialog');
  const input = document.querySelector('#manual-search');
  const results = document.querySelector('.search-results');
  const data = JSON.parse(document.querySelector('#search-data').textContent);
  let active = -1;
  let matches = [];

  function words(value) { return value.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean); }
  function score(item, terms) {
    const title = item.title.toLocaleLowerCase();
    const kind = item.kind.toLocaleLowerCase();
    const body = item.text.toLocaleLowerCase();
    if (!terms.every(term => title.includes(term) || kind.includes(term) || body.includes(term))) return -1;
    return terms.reduce((total, term) => total + (title.startsWith(term) ? 20 : title.includes(term) ? 12 : 0) + (kind.includes(term) ? 3 : 0) + (body.includes(term) ? 1 : 0), 0);
  }
  function render() {
    const terms = words(input.value);
    results.replaceChildren();
    active = -1;
    if (!terms.length) { results.innerHTML = '<p>Search operators, settings, and guides.</p>'; matches = []; return; }
    matches = data.map(item => ({...item, rank: score(item, terms)})).filter(item => item.rank >= 0).sort((a,b) => b.rank - a.rank || a.title.localeCompare(b.title)).slice(0, 16);
    if (!matches.length) { results.innerHTML = '<p>No results. Try an operator name or a different setting.</p>'; return; }
    for (const item of matches) {
      const a = document.createElement('a'); a.href = item.url; a.setAttribute('role', 'option');
      const strong = document.createElement('strong'); strong.textContent = item.title;
      const small = document.createElement('small'); small.textContent = item.kind;
      a.append(strong, small); results.append(a);
    }
  }
  function open() { dialog.showModal(); input.value = ''; render(); setTimeout(() => input.focus(), 0); }
  document.querySelectorAll('[data-search-open]').forEach(button => button.addEventListener('click', open));
  document.querySelector('[data-search-close]').addEventListener('click', () => dialog.close());
  input.addEventListener('input', render);
  input.addEventListener('keydown', event => {
    const rows = [...results.querySelectorAll('a')];
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault(); if (!rows.length) return;
      active = (active + (event.key === 'ArrowDown' ? 1 : -1) + rows.length) % rows.length;
      rows.forEach((row, index) => row.classList.toggle('is-active', index === active));
      rows[active].scrollIntoView({block:'nearest'});
    } else if (event.key === 'Enter' && rows.length) {
      event.preventDefault(); window.location.href = rows[Math.max(active,0)].href;
    }
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && dialog.open) { dialog.close(); return; }
    const target = event.target;
    const typing = target instanceof HTMLElement && (target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName));
    if (((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') || (!typing && !dialog.open && event.key === '/')) { event.preventDefault(); if (!dialog.open) open(); }
  });
  const nav = document.querySelector('#sidebar');
  const toggle = document.querySelector('[data-nav-toggle]');
  toggle.addEventListener('click', () => { const isOpen = nav.classList.toggle('is-open'); toggle.setAttribute('aria-expanded', String(isOpen)); toggle.setAttribute('aria-label', isOpen ? 'Close navigation' : 'Open navigation'); });
  document.addEventListener('click', event => { if (nav.classList.contains('is-open') && !nav.contains(event.target) && !toggle.contains(event.target)) { nav.classList.remove('is-open'); toggle.setAttribute('aria-expanded','false'); } });
  const select = document.querySelector('#gallery-filter');
  if (select) select.addEventListener('change', () => {
    const category = select.value;
    document.querySelectorAll('.gallery-card').forEach(card => { card.hidden = category !== 'all' && card.querySelector('.gallery-card__body span').textContent !== category; });
  });
})();
