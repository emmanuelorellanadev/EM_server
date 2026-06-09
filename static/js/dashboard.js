/* dashboard.js – client-side helpers for the EM Server dashboard */

'use strict';

// ------------------------------------------------------------------ //
// Field metadata
// ------------------------------------------------------------------ //
const FIELD_META = {
  temperature:          { label: 'Temperatura',              icon: '🌡️'  },
  humidity:             { label: 'Humedad Ambiental',         icon: '💧'  },
  soil_humidity:        { label: 'Humedad de Suelo',          icon: '🌱'  },
  online:               { label: 'Conectado MQTT',            icon: '📡'  },
  light:                { label: 'Iluminación',               icon: '☀️'  },
  pressure:             { label: 'Presión Atmosférica',       icon: '🌀'  },
  watering:             { label: 'Riego Activo',              icon: '🚿'  },
  on_threshold_percent: { label: 'Umbral de Activación (%)', icon: '🎯'  },
  relay_on_time_s:      { label: 'Duración de Riego (s)',    icon: '⏱️'  },
};

// Fields rendered as on/off rather than a number
const BOOLEAN_FIELDS = new Set(['watering', 'online']);
const LAST_WATERING_FIELD = 'last_watering_at_epoch';
const LAST_WATERED_SEC_FIELD = 'last_watered_sec';
const RELAY_ON_TIME_FIELD = 'relay_on_time_s';
const ON_THRESHOLD_FIELD = 'on_threshold_percent';

function fieldLabel(field) {
  return (FIELD_META[field] || {}).label || field;
}

function fieldIcon(field) {
  return (FIELD_META[field] || {}).icon || '📊';
}

/**
 * Formats "ultimo riego" from unix epoch seconds.
 */
function formatLastWatering(epochSeconds) {
  if (!Number.isFinite(epochSeconds) || epochSeconds <= 0) {
    return 'Ultimo riego: sin registro';
  }
  const dt = new Date(epochSeconds * 1000);
  return `Ultimo riego: ${dt.toLocaleString('es-GT', { hour12: false, timeZone: 'America/Guatemala' })}`;
}

/**
 * Reads persisted epoch from a card (server-rendered bootstrap value).
 */
function readPersistedEpoch(card) {
  const node = card.querySelector('.last-watering-time');
  if (!node) return Number.NaN;
  const raw = node.getAttribute('data-last-watering-epoch');
  const epoch = Number(raw);
  if (Number.isFinite(epoch) && epoch > 0) return epoch;
  return Number.NaN;
}

/**
 * Converts API recorded_at ISO string to epoch seconds.
 */
function recordedAtToEpochSeconds(recordedAt) {
  if (!recordedAt) return Number.NaN;
  const ms = Date.parse(recordedAt);
  if (!Number.isFinite(ms)) return Number.NaN;
  return ms / 1000;
}

function formatRelayDuration(secondsValue) {
  const seconds = Number(secondsValue);
  if (!Number.isFinite(seconds) || seconds < 0) {
    return 'Duracion de riego: sin dato';
  }
  if (Number.isInteger(seconds)) {
    return `Duracion de riego: ${seconds} s`;
  }
  return `Duracion de riego: ${seconds.toFixed(1)} s`;
}

function formatThreshold(value) {
  const threshold = Number(value);
  if (!Number.isFinite(threshold) || threshold < 0) {
    return 'Umbral de activacion: sin dato';
  }
  if (Number.isInteger(threshold)) {
    return `Umbral de activacion: ${threshold} %`;
  }
  return `Umbral de activacion: ${threshold.toFixed(1)} %`;
}

// ------------------------------------------------------------------ //
// Trend chart
// ------------------------------------------------------------------ //
let trendChart = null;
const raspberryTrendCharts = {
  temperature: null,
  humidity: null,
  pressure: null,
};
const TREND_COLORS = {
  soil_humidity: '#2e7d32',
  on_threshold_percent: '#d32f2f',
  temperature: '#e53935',
  humidity: '#1e88e5',
  pressure: '#6a1b9a',
};
const FIELD_UNITS = {
  soil_humidity: '%',
  on_threshold_percent: '%',
  temperature: '°C',
  humidity: '%',
  pressure: 'hPa',
};

function destroyChartInstance(chart) {
  if (chart) chart.destroy();
}

function destroyAllTrendCharts() {
  destroyChartInstance(trendChart);
  trendChart = null;
  Object.keys(raspberryTrendCharts).forEach(field => {
    destroyChartInstance(raspberryTrendCharts[field]);
    raspberryTrendCharts[field] = null;
  });
}

function trendChartLayoutForSource(source) {
  const single = document.getElementById('trend-chart-single-wrapper');
  const multi = document.getElementById('trend-chart-rasp-wrapper');
  if (!single || !multi) return;

  if (source === 'raspberrypi') {
    single.classList.add('hidden');
    multi.classList.remove('hidden');
  } else {
    multi.classList.add('hidden');
    single.classList.remove('hidden');
  }
}

function formatTrendDate(value) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return new Intl.DateTimeFormat('es-GT', {
    year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit',
  }).format(date);
}

function formatTrendTooltipLabel(ctx) {
  const unit = FIELD_UNITS[ctx.dataset.fieldKey] || '';
  return `${ctx.dataset.label}: ${ctx.parsed.y.toFixed(1)}${unit ? ` ${unit}` : ''}`;
}

function buildTrendOptions(yTitle, { yMin = null, yMax = null } = {}) {
  const yConfig = {
    title: { display: true, text: yTitle },
  };
  if (Number.isFinite(yMin)) yConfig.min = yMin;
  if (Number.isFinite(yMax)) yConfig.max = yMax;

  return {
    responsive: true,
    interaction: { mode: 'index', intersect: false },
    plugins: {
      legend: { position: 'top' },
      tooltip: {
        callbacks: {
          title: items => (items.length > 0 ? formatTrendDate(items[0].parsed.x) : ''),
          label: formatTrendTooltipLabel,
        },
      },
    },
    scales: {
      x: {
        type: 'time',
        time: { tooltipFormat: 'dd/MM/yyyy HH:mm' },
        title: { display: true, text: 'Fecha y hora (America/Guatemala)' },
        ticks: { maxRotation: 30, maxTicksLimit: 10 },
      },
      y: yConfig,
    },
  };
}

function buildDataset(field, data, index = 0) {
  const colorPalette = ['#4caf50', '#1e88e5', '#ff8f00'];
  const color = TREND_COLORS[field] || colorPalette[index % colorPalette.length];
  return {
    label: fieldLabel(field),
    fieldKey: field,
    data,
    borderColor: color,
    backgroundColor: color + '22',
    tension: 0.3,
    pointRadius: 2,
    borderWidth: field === 'on_threshold_percent' ? 2 : 3,
    borderDash: field === 'on_threshold_percent' ? [8, 6] : [],
    fill: false,
  };
}

function initEspTrendChart(trendPayload) {
  const canvas = document.getElementById('trend-chart');
  if (!canvas) return;

  const datasetsByField = trendPayload.datasets;
  const fields = Object.keys(datasetsByField);
  const datasets = fields.map((field, i) => buildDataset(field, datasetsByField[field] || [], i));

  trendChart = new Chart(canvas, {
    type: 'line',
    data: { datasets },
    options: buildTrendOptions('Humedad (%)', { yMin: 0, yMax: 100 }),
  });
}

function initRaspberryTrendCharts(trendPayload) {
  ['temperature', 'humidity', 'pressure'].forEach(field => {
    const canvas = document.getElementById(`trend-chart-${field}`);
    if (!canvas) return;

    const unit = FIELD_UNITS[field] || '';
    const yTitle = `${fieldLabel(field)}${unit ? ` (${unit})` : ''}`;
    const dataset = buildDataset(field, trendPayload.datasets[field] || []);

    raspberryTrendCharts[field] = new Chart(canvas, {
      type: 'line',
      data: { datasets: [dataset] },
      options: buildTrendOptions(yTitle),
    });
  });
}

/**
 * Builds (or rebuilds) the irrigation trend chart.
 *
 * @param {object} trendPayload Response from GET /api/trend.
 */
function initTrendChart(trendPayload) {
  if (!trendPayload || !trendPayload.datasets) return;

  const source = trendPayload.source || trendSource;
  destroyAllTrendCharts();
  trendChartLayoutForSource(source);

  if (source === 'raspberrypi') {
    initRaspberryTrendCharts(trendPayload);
    return;
  }

  initEspTrendChart(trendPayload);
}

// ------------------------------------------------------------------ //
// Live card update (called after /api/latest refresh)
// ------------------------------------------------------------------ //
function updateCards(data) {
  const bySource = {};
  data.forEach(r => {
    if (!bySource[r.source]) bySource[r.source] = {};
    bySource[r.source][r.field] = r;
  });

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

      // En la tarjeta compuesta de "Riego Activo" actualizamos metadatos
      // relacionados para mantener una sola fuente visual de verdad.
      if (r.field === 'watering') {
        const sourceSnapshot = bySource[r.source] || {};

        const durationEl = card.querySelector('.watering-duration');
        if (durationEl) {
          durationEl.textContent = formatRelayDuration(sourceSnapshot[RELAY_ON_TIME_FIELD]?.value);
        }

        const thresholdEl = card.querySelector('.watering-threshold');
        if (thresholdEl) {
          thresholdEl.textContent = formatThreshold(sourceSnapshot[ON_THRESHOLD_FIELD]?.value);
        }

        let lastEpoch = Number(sourceSnapshot[LAST_WATERING_FIELD]?.value);
        if (!Number.isFinite(lastEpoch) || lastEpoch <= 0) {
          const secValue = Number(sourceSnapshot[LAST_WATERED_SEC_FIELD]?.value);
          if (Number.isFinite(secValue) && secValue >= 0) {
            const secRecordedAt = sourceSnapshot[LAST_WATERED_SEC_FIELD]?.recorded_at;
            const recEpoch = recordedAtToEpochSeconds(secRecordedAt);
            if (Number.isFinite(recEpoch)) {
              lastEpoch = recEpoch - secValue;
            }
          }
        }

        // Si no hay dato nuevo (ej. -1 tras reinicio del ESP), conservamos
        // el ultimo valor valido ya renderizado en el DOM.
        if (!Number.isFinite(lastEpoch) || lastEpoch <= 0) {
          lastEpoch = readPersistedEpoch(card);
        }

        const extraEl = card.querySelector('.last-watering-time');
        if (extraEl) {
          extraEl.textContent = formatLastWatering(lastEpoch);
          if (Number.isFinite(lastEpoch) && lastEpoch > 0) {
            extraEl.setAttribute('data-last-watering-epoch', String(lastEpoch));
          }
        }
      }
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
async function sendWaterCommand(btn, source = 'esp8266') {
  const statusEl = document.getElementById(`water-cmd-status-${source}`);
  btn.disabled = true;
  if (statusEl) statusEl.textContent = 'Enviando…';

  try {
    const resp = await fetch('/api/command/water', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ source }),
    });
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

const DEFAULT_TREND_SOURCE = 'esp8266';
let trendSource = DEFAULT_TREND_SOURCE;
const TREND_RANGE_SELECT_ID = 'trend-range';
const TREND_TITLE_BY_SOURCE = {
  esp8266: 'Tendencia de Riego',
  esp32_01: 'Tendencia de Riego',
  raspberrypi: 'Tendencia Ambiental',
};

function getActiveSourcePanelId() {
  const panel = document.querySelector('.source-panel.active');
  return panel ? panel.id : '';
}

function sourceFromPanelId(panelId) {
  const prefix = 'panel-';
  if (!panelId || !panelId.startsWith(prefix)) return '';
  return panelId.slice(prefix.length);
}

function updateTrendSourceLabel() {
  const title = document.getElementById('trend-title');
  if (title) {
    title.innerHTML = `${TREND_TITLE_BY_SOURCE[trendSource] || 'Tendencia'} (<span id="trend-source-label">${trendSource.toUpperCase()}</span>)`;
    return;
  }

  const label = document.getElementById('trend-source-label');
  if (!label) return;
  label.textContent = trendSource.toUpperCase();
}

function setTrendSource(source, { reload = true } = {}) {
  if (!source) return;
  trendSource = source;
  updateTrendSourceLabel();
  if (reload) {
    loadTrendChart(getSelectedTrendRange());
  }
}

function syncTrendSourceWithActivePanel({ reload = true } = {}) {
  const source = sourceFromPanelId(getActiveSourcePanelId());
  if (!source) {
    updateTrendSourceLabel();
    return;
  }
  setTrendSource(source, { reload });
}

/**
 * Reads the selected range from the selector.
 * Falls back to "ultimos" when the selector is not present.
 */
function getSelectedTrendRange() {
  const select = document.getElementById(TREND_RANGE_SELECT_ID);
  if (!select) return '1h';
  return select.value || '1h';
}

/**
 * Loads trend data for the selected range and renders the chart.
 *
 * @param {string} rangeKey One of: 1h|1d|1w|1m|1y
 */
async function loadTrendChart(rangeKey = '1h') {
  try {
    const url = `/api/trend?source=${encodeURIComponent(trendSource)}&range=${encodeURIComponent(rangeKey)}`;
    const resp = await fetch(url);
    if (!resp.ok) {
      console.error(`loadTrendChart: API returned ${resp.status}`);
      return;
    }
    const payload = await resp.json();
    initTrendChart(payload);
  } catch (err) {
    console.error('loadTrendChart: network error –', err);
  }
}

/**
 * Handler for the range selector change event.
 */
function onTrendRangeChange() {
  loadTrendChart(getSelectedTrendRange());
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
    syncTrendSourceWithActivePanel({ reload: false });
    await loadTrendChart(getSelectedTrendRange());
  } catch (_) { /* network error – ignore */ }
}, 30000);
