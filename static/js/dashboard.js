/* dashboard.js – client-side helpers for the EM Server dashboard */

'use strict';

// ------------------------------------------------------------------ //
// Field metadata
// ------------------------------------------------------------------ //
const FIELD_META = {
  temperature:   { label: 'Temperatura',         icon: '🌡️'  },
  humidity:      { label: 'Humedad Ambiental',    icon: '💧'  },
  soil_humidity: { label: 'Humedad de Suelo',     icon: '🌱'  },
  light:         { label: 'Iluminación',          icon: '☀️'  },
  pressure:      { label: 'Presión Atmosférica',  icon: '🌀'  },
};

function fieldLabel(field) {
  return (FIELD_META[field] || {}).label || field;
}

function fieldIcon(field) {
  return (FIELD_META[field] || {}).icon || '📊';
}

// ------------------------------------------------------------------ //
// Trend chart
// ------------------------------------------------------------------ //
let trendChart = null;

function initTrendChart(latestData) {
  const canvas = document.getElementById('trend-chart');
  if (!canvas || !latestData || latestData.length === 0) return;

  // Group by field for the legend
  const fields = [...new Set(latestData.map(r => r.field))];
  const palette = ['#4caf50', '#1e88e5', '#ff8f00', '#e53935', '#6a1b9a', '#6d4c41'];

  const datasets = fields.map((field, i) => ({
    label: fieldLabel(field),
    data: latestData
      .filter(r => r.field === field)
      .map(r => ({ x: r.recorded_at, y: r.value })),
    borderColor: palette[i % palette.length],
    backgroundColor: palette[i % palette.length] + '22',
    tension: 0.3,
    pointRadius: 3,
    fill: false,
  }));

  trendChart = new Chart(canvas, {
    type: 'line',
    data: { datasets },
    options: {
      responsive: true,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { position: 'top' },
        tooltip: { callbacks: {
          label: ctx => `${ctx.dataset.label}: ${ctx.parsed.y.toFixed(2)}`,
        }},
      },
      scales: {
        x: {
          type: 'category',
          title: { display: true, text: 'Timestamp (UTC)' },
          ticks: { maxRotation: 30, maxTicksLimit: 10 },
        },
        y: {
          title: { display: true, text: 'Valor' },
        },
      },
    },
  });
}

// ------------------------------------------------------------------ //
// Live card update (called after /api/latest refresh)
// ------------------------------------------------------------------ //
function updateCards(data) {
  // Re-render only the value + time inside each existing card
  data.forEach(r => {
    const panel = document.getElementById(`panel-${r.source}`);
    if (!panel) return;

    const cards = panel.querySelectorAll(`.card.${r.field}`);
    cards.forEach(card => {
      const valEl = card.querySelector('.card-value');
      const timeEl = card.querySelector('.card-time');
      if (valEl) valEl.innerHTML = `${parseFloat(r.value).toFixed(1)} <small>${r.unit}</small>`;
      if (timeEl) timeEl.textContent = r.recorded_at;
    });
  });
}

// ------------------------------------------------------------------ //
// Auto-refresh every 30 seconds
// ------------------------------------------------------------------ //
setInterval(async () => {
  try {
    const resp = await fetch('/api/latest');
    if (!resp.ok) return;
    const data = await resp.json();
    updateCards(data);
  } catch (_) { /* network error – ignore */ }
}, 30000);
