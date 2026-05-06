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

// Fields to plot in the trend chart, keyed by sensor source name.
// Sources not listed here fall back to DEFAULT_CHART_FIELDS.
const SOURCE_CHART_FIELDS = {
  esp8266:     ['soil_humidity', 'on_threshold_percent'],
  raspberrypi: ['temperature', 'humidity', 'pressure'],
};
const DEFAULT_CHART_FIELDS = ['temperature', 'humidity'];

function getChartFields(source) {
  return SOURCE_CHART_FIELDS[source] || DEFAULT_CHART_FIELDS;
}

function initTrendChart(data) {
  const canvas = document.getElementById('trend-chart');
  if (!canvas || !data || data.length === 0) return;

  // Group by field for the legend
  const fields = [...new Set(data.map(r => r.field))];
  const palette = ['#4caf50', '#1e88e5', '#ff8f00', '#e53935', '#6a1b9a', '#6d4c41'];

  const datasets = fields.map((field, i) => ({
    label: fieldLabel(field),
    data: data
      .filter(r => r.field === field)
      .map(r => ({ x: r.recorded_at, y: r.value })),
    borderColor: palette[i % palette.length],
    backgroundColor: palette[i % palette.length] + '22',
    tension: 0.3,
    pointRadius: 3,
    fill: false,
  }));

  if (trendChart) {
    trendChart.destroy();
    trendChart = null;
  }

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
          title: { display: true, text: 'Timestamp (UTC-6)' },
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

// How many hours of history to display in the trend chart.
const CHART_HOURS = 24;
// Safety cap: 24 h × 120 readings/h (one every 30 s) per field + margin.
const CHART_LIMIT = 3000;

/**
 * Fetches the last CHART_HOURS hours of data for each field relevant to the
 * given sensor source, then re-renders the trend chart.
 *
 * @param {string} source  The active sensor source (e.g. "esp8266" or
 *                         "raspberrypi").  When falsy, the chart is cleared.
 */
async function loadTrendChart(source) {
  if (!source) return;

  const chartFields = getChartFields(source);

  // Update the chart section heading to reflect the active source / window.
  const titleEl = document.getElementById('chart-title');
  if (titleEl) {
    const fieldNames = chartFields.map(fieldLabel).join(', ');
    titleEl.textContent =
      `Tendencias – ${fieldNames} — ${source.toUpperCase()} (últimas ${CHART_HOURS} horas)`;
  }

  try {
    const responses = await Promise.all(
      chartFields.map(f =>
        fetch(
          `/api/history?source=${encodeURIComponent(source)}&field=${encodeURIComponent(f)}&hours=${CHART_HOURS}&limit=${CHART_LIMIT}`
        )
      )
    );
    const arrays = await Promise.all(
      responses.map((r, i) => {
        if (!r.ok) {
          console.error(`loadTrendChart: API returned ${r.status} for ${chartFields[i]}`);
          return [];
        }
        return r.json();
      })
    );
    const data = arrays.flat();
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
