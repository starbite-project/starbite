/**
 * The Project Starbite mark: iOS on a Samsung.
 *   - an iOS app-icon squircle (a true superellipse, |x|^5 + |y|^5 = 1),
 *   - in Samsung blue fading into violet,
 *   - a white five-point star (star-lte), softly rounded,
 *   - a round bite out of the icon's top-right corner (star-bite).
 * starbiteMark(x, y, s, id) returns SVG for an s x s mark at (x, y); `id`
 * keeps gradient/mask ids unique when it is drawn more than once.
 */
const squircle = (x, y, s, n = 5, steps = 240) => {
  const pts = [];
  const r = s / 2;
  for (let i = 0; i < steps; i++) {
    const t = (i / steps) * Math.PI * 2;
    const c = Math.cos(t), sn = Math.sin(t);
    const px = Math.sign(c) * Math.abs(c) ** (2 / n);
    const py = Math.sign(sn) * Math.abs(sn) ** (2 / n);
    pts.push(`${(x + r + r * px).toFixed(2)},${(y + r + r * py).toFixed(2)}`);
  }
  return `M${pts.join('L')}Z`;
};

const starPoints = (cx, cy, R, inner = 0.48) => {
  const pts = [];
  for (let i = 0; i < 10; i++) {
    const r = i % 2 ? R * inner : R;
    const a = -Math.PI / 2 + (i * Math.PI) / 5;
    pts.push(`${(cx + r * Math.cos(a)).toFixed(2)},${(cy + r * Math.sin(a)).toFixed(2)}`);
  }
  return pts.join(' ');
};

export function starbiteMark(x, y, s, id = 'sb') {
  // The bite: a circle centred just outside the top-right corner.
  const bx = x + s * 0.95, by = y + s * 0.07, br = s * 0.22;
  // The star sits a little low and left, clear of the bite.
  const sx = x + s * 0.47, sy = y + s * 0.54, sr = s * 0.3;
  return (
    `<defs>` +
    `<linearGradient id="${id}-bg" x1="0" y1="0" x2="1" y2="1">` +
    `<stop offset="0" stop-color="#2a7bff"/><stop offset="0.55" stop-color="#3f4df0"/><stop offset="1" stop-color="#7a3cf0"/></linearGradient>` +
    `<radialGradient id="${id}-hi" cx="0.25" cy="0.15" r="0.75"><stop offset="0" stop-color="#ffffff" stop-opacity="0.28"/><stop offset="1" stop-color="#ffffff" stop-opacity="0"/></radialGradient>` +
    `<linearGradient id="${id}-st" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#e6e9ff"/></linearGradient>` +
    `<mask id="${id}-m" maskUnits="userSpaceOnUse" x="${x - s}" y="${y - s}" width="${s * 3}" height="${s * 3}">` +
    `<rect x="${x - s}" y="${y - s}" width="${s * 3}" height="${s * 3}" fill="#fff"/>` +
    `<circle cx="${bx}" cy="${by}" r="${br}" fill="#000"/></mask>` +
    `<filter id="${id}-sh" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="${(s * 0.025).toFixed(2)}"/></filter>` +
    `</defs>` +
    `<g mask="url(#${id}-m)">` +
    `<path d="${squircle(x, y, s)}" fill="url(#${id}-bg)"/>` +
    `<path d="${squircle(x, y, s)}" fill="url(#${id}-hi)"/>` +
    `<polygon points="${starPoints(sx, sy + s * 0.025, sr)}" fill="#1b1f6b" opacity="0.35" stroke="#1b1f6b" stroke-width="${(s * 0.06).toFixed(2)}" stroke-linejoin="round" filter="url(#${id}-sh)"/>` +
    `<polygon points="${starPoints(sx, sy, sr)}" fill="url(#${id}-st)" stroke="url(#${id}-st)" stroke-width="${(s * 0.06).toFixed(2)}" stroke-linejoin="round"/>` +
    `</g>`
  );
}
