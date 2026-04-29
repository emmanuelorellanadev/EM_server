/* dashboard.js – client-side helpers for the EM Server dashboard */

'use strict';

// ------------------------------------------------------------------ //
// Field metadata
// ------------------------------------------------------------------ //
const FIELD_META = {
  temperature:          { label: 'Temperatura',              icon: '🌡️'  },
  humidity:             { label: 'Humedad Ambiental',         icon: '💧'  },
  soil_humidity:        { label: 'Humedad de Suelo',          icon: '🌱'  },
  light:                { label: 'Iluminación',               icon: '☀️'  },
  pressure:             { label: 'Presión Atmosférica',       icon: '🌀'  },
  watering:             { label: 'Riego Activo',              icon: '🚿'  },
  on_threshold_percent: { label: 'Umbral de Activación (%)', icon: '🎯'  },
  relay_on_time_s:      { label: 'Duración de Riego (s)',    icon: '⏱️'  },
};

// Fields rendered as on/off rather than a number
const BOOLEAN_FIELDS = new Set(['watering']);

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
      if (valEl) {
        if (BOOLEAN_FIELDS.has(r.field)) {
          const on = r.value !== 0;
          valEl.className = `card-value status-badge ${on ? 'status-on' : 'status-off'}`;
          valEl.textContent = on ? 'Activo' : 'Inactivo';
        } else {
          valEl.className = 'card-value';
          valEl.innerHTML = `${parseFloat(r.value).toFixed(1)} <small>${r.unit}</small>`;
        }
      }
      if (timeEl) timeEl.textContent = r.recorded_at;
    });
  });
}

// ------------------------------------------------------------------ //
// Remote watering command — sends POST /api/command/water
// ------------------------------------------------------------------ //

/**
 * sendWaterCommand — Sends a manual irrigation command to the ESP8266.
 *
 * Flow:
 *  1. The dashboard calls POST /api/command/water on the Flask server.
 *  2. Flask publishes {"action":"water"} to "commands/esp8266" via MQTT.
 *  3. The ESP8266 subscribes to that topic; upon receiving the message its
 *     mqttCallback() calls startWatering(force=true), which:
 *       • cancels any active cooldown,
 *       • opens the solenoid valve (relay HIGH),
 *       • starts the DURACION_RIEGO_MS countdown.
 *  4. The dashboard shows a success or error badge next to the button.
 *
 * @param {HTMLElement} btn  The button element that triggered the action.
 */
async function sendWaterCommand(btn) {
  const statusEl = document.getElementById('water-cmd-status');
  btn.disabled = true;
  if (statusEl) statusEl.textContent = 'Enviando…';

  try {
    const resp = await fetch('/api/command/water', { method: 'POST' });
    const data = await resp.json();
    if (resp.ok) {
      if (statusEl) {
        statusEl.textContent = '✅ Comando enviado';
        statusEl.className = 'cmd-status cmd-ok';
      }
    } else {
      if (statusEl) {
        statusEl.textContent = '❌ Error: ' + (data.error || resp.status);
        statusEl.className = 'cmd-status cmd-err';
      }
    }
  } catch (e) {
    if (statusEl) {
      statusEl.textContent = '❌ Error de red';
      statusEl.className = 'cmd-status cmd-err';
    }
  }

  btn.disabled = false;
  // Limpiar el mensaje de estado después de 6 segundos.
  setTimeout(() => {
    if (statusEl) {
      statusEl.textContent = '';
      statusEl.className = 'cmd-status';
    }
  }, 6000);
}

// ------------------------------------------------------------------ //
// Load trend chart from history API
// ------------------------------------------------------------------ //

/**
 * Fetches the last 50 readings from /api/history, sorts them
 * chronologically, and renders the trend chart.
 */
async function loadTrendChart() {
  try {
    const resp = await fetch('/api/history?limit=50');
    if (!resp.ok) {
      console.error('loadTrendChart: API returned', resp.status);
      return;
    }
    const data = await resp.json();
    // Sort oldest-first so time flows left→right on the x-axis
    data.sort((a, b) => a.recorded_at.localeCompare(b.recorded_at));
    initTrendChart(data);
  } catch (err) {
    console.error('loadTrendChart: network error –', err);
  }
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
