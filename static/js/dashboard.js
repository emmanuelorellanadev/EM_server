/* Client logic for dashboard cards, commands, and trend charts. */

'use strict';

// Field metadata used in cards and chart labels.
const FIELD_META = {
  ambient_temperature:  { label: 'Temperatura Ambiental',     icon: '🌡️'  },
  ambient_humidity:     { label: 'Humedad Ambiental',         icon: '💧'  },
  soil_humidity:        { label: 'Humedad de Suelo',          icon: '🌱'  },
  online:               { label: 'Conectado MQTT',            icon: '📡'  },
  light:                { label: 'Luz Ambiental',             icon: '☀️'  },
  pressure:             { label: 'Presión Atmosférica',       icon: '🌀'  },
  watering:             { label: 'Riego Activo',              icon: '🚿'  },
  on_threshold_soil_vwc: { label: 'Umbral de Activación (%)', icon: '🎯'  },
  relay_on_time_s:      { label: 'Duración de Riego (s)',    icon: '⏱️'  },
};

// Fields rendered as boolean badges.
const BOOLEAN_FIELDS = new Set(['watering', 'online']);
const LAST_WATERING_FIELD = 'last_watering_at_epoch';
const LAST_WATERED_SEC_FIELD = 'last_watered_sec';
const RELAY_ON_TIME_FIELD = 'relay_on_time_s';
const ON_THRESHOLD_FIELD = 'on_threshold_soil_vwc';

function fieldLabel(field) {
  return (FIELD_META[field] || {}).label || field;
}

function fieldIcon(field) {
  return (FIELD_META[field] || {}).icon || '📊';
}

/**
 * Format last watering timestamp from epoch seconds.
 * @param {number} epochSeconds - Unix timestamp in seconds.
 * @returns {string} Formatted datetime string or "sin registro".
 */
function formatLastWatering(epochSeconds) {
  if (!Number.isFinite(epochSeconds) || epochSeconds <= 0) {
    return 'sin registro';
  }
  const dt = new Date(epochSeconds * 1000);
  return dt.toLocaleString('es-GT', { hour12: false, timeZone: 'America/Guatemala' });
}

// Read persisted epoch rendered by server for fallback display.
function readPersistedEpoch(card) {
  const node = card.querySelector('.last-watering-time');
  if (!node) return Number.NaN;
  const raw = node.getAttribute('data-last-watering-epoch');
  const epoch = Number(raw);
  if (Number.isFinite(epoch) && epoch > 0) return epoch;
  return Number.NaN;
}

// Convert recorded_at ISO text to epoch seconds.
function recordedAtToEpochSeconds(recordedAt) {
  if (!recordedAt) return Number.NaN;
  const ms = Date.parse(recordedAt);
  if (!Number.isFinite(ms)) return Number.NaN;
  return ms / 1000;
}

/**
 * Format relay duration for display.
 * @param {number} secondsValue - Duration in seconds.
 * @returns {string} Formatted duration string or "sin dato".
 */
function formatRelayDuration(secondsValue) {
  const seconds = Number(secondsValue);
  if (!Number.isFinite(seconds) || seconds < 0) {
    return 'sin dato';
  }
  if (Number.isInteger(seconds)) {
    return `${seconds} s`;
  }
  return `${seconds.toFixed(1)} s`;
}

/**
 * Format watering threshold percentage for display.
 * @param {number} value - Threshold percentage.
 * @returns {string} Formatted threshold string or "sin dato".
 */
function formatThreshold(value) {
  const threshold = Number(value);
  if (!Number.isFinite(threshold) || threshold < 0) {
    return 'sin dato';
  }
  if (Number.isInteger(threshold)) {
    return `${threshold} %`;
  }
  return `${threshold.toFixed(1)} %`;
}

// Trend chart state and helpers.
let trendChart = null;
const raspberryTrendCharts = {
  temperature: null,
  humidity: null,
  pressure: null,
};
const esp32AmbientTrendCharts = {
  ambient_temperature: null,
  ambient_humidity: null,
  light: null,
};

// ESP32 sources that use ambient trend layout.
const ESP32_AMBIENT_SOURCES = new Set(['esp32_01', 'esp32_02']);
const TREND_COLORS = {
  soil_humidity: '#2e7d32',
  on_threshold_soil_vwc: '#d32f2f',
  ambient_temperature: '#e53935',
  ambient_humidity: '#1e88e5',
  pressure: '#6a1b9a',
  light: '#f9a825',
};
const FIELD_UNITS = {
  soil_humidity: '%',
  on_threshold_soil_vwc: '%',
  ambient_temperature: '°C',
  ambient_humidity: '%',
  pressure: 'hPa',
  light: '%',
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
  Object.keys(esp32AmbientTrendCharts).forEach(field => {
    destroyChartInstance(esp32AmbientTrendCharts[field]);
    esp32AmbientTrendCharts[field] = null;
  });
}

function trendChartLayoutForSource(source) {
  const single = document.getElementById('trend-chart-single-wrapper');
  const multi = document.getElementById('trend-chart-rasp-wrapper');
  const esp32 = document.getElementById('trend-chart-esp32-wrapper');
  if (!single || !multi || !esp32) return;

  if (source === 'raspberrypi') {
    single.classList.add('hidden');
    multi.classList.remove('hidden');
    esp32.classList.add('hidden');
  } else if (ESP32_AMBIENT_SOURCES.has(source)) {
    single.classList.add('hidden');
    multi.classList.add('hidden');
    esp32.classList.remove('hidden');
  } else {
    multi.classList.add('hidden');
    single.classList.remove('hidden');
    esp32.classList.add('hidden');
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
    borderWidth: field === 'on_threshold_soil_vwc' ? 2 : 3,
    borderDash: field === 'on_threshold_soil_vwc' ? [8, 6] : [],
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

function initEsp32AmbientTrendCharts(trendPayload) {
  ['ambient_temperature', 'ambient_humidity', 'light'].forEach(field => {
    const canvas = document.getElementById(`trend-chart-esp32-${field}`);
    if (!canvas) return;

    let datasets = [];
    let yTitle = '';

    if (field === 'ambient_humidity') {
      // En la grafica de humedad ambiental tambien trazamos humedad de suelo.
      datasets = [
        buildDataset('ambient_humidity', trendPayload.datasets.ambient_humidity || []),
        buildDataset('soil_humidity', trendPayload.datasets.soil_humidity || [], 1),
      ];
      yTitle = 'Humedad (%)';
    } else {
      const unit = FIELD_UNITS[field] || '';
      yTitle = `${fieldLabel(field)}${unit ? ` (${unit})` : ''}`;
      datasets = [buildDataset(field, trendPayload.datasets[field] || [])];
    }

    esp32AmbientTrendCharts[field] = new Chart(canvas, {
      type: 'line',
      data: { datasets },
      options: buildTrendOptions(yTitle, (field === 'light') ? { yMin: 0, yMax: 100 } : {}),
    });
  });
}

// Build or rebuild trend chart based on source type.
function initTrendChart(trendPayload) {
  if (!trendPayload || !trendPayload.datasets) return;

  const source = trendPayload.source || trendSource;
  destroyAllTrendCharts();
  trendChartLayoutForSource(source);

  if (source === 'raspberrypi') {
    initRaspberryTrendCharts(trendPayload);
    return;
  }

  if (ESP32_AMBIENT_SOURCES.has(source)) {
    initEsp32AmbientTrendCharts(trendPayload);
    return;
  }

  initEspTrendChart(trendPayload);
}

/**
 * Update dashboard cards with latest sensor readings.
 * Iterates over /api/latest response and updates each card's value,
 * timestamp, and metadata (watering duration, threshold, last watering).
 * @param {Array<{source:string, field:string, value:number, unit:string, recorded_at:string}>} data
 */
function updateCards(data) {
  const bySource = {};
  data.forEach(r => {
    if (!bySource[r.source]) bySource[r.source] = {};
    bySource[r.source][r.field] = r;
  });

  // Update value and timestamp in each existing card.
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
          valEl.textContent = on ? 'Conectado' : 'Desconectado';
        } else {
          valEl.className = 'card-value';
          valEl.textContent = `${parseFloat(r.value).toFixed(1)} `;
          const unitEl = document.createElement('small');
          unitEl.textContent = r.unit || '';
          valEl.appendChild(unitEl);
        }
      }
      if (timeEl) timeEl.textContent = r.recorded_at;
    });

    const soilCard = panel.querySelector('.card.soil_humidity');
    if (soilCard) {
      if (r.field === 'soil_humidity' || r.field === 'percent' || r.field === 'soil_vwc') {
        const valEl = soilCard.querySelector('.soil-humidity-value');
        const timeEl = soilCard.querySelector('.soil-time');
        if (valEl) {
          valEl.textContent = `${parseFloat(r.value).toFixed(1)} %`;
        }
        if (timeEl) timeEl.textContent = r.recorded_at;
      }

      const sourceSnapshot = bySource[r.source] || {};

      const wateringEl = soilCard.querySelector('.soil-watering-value');
      if (wateringEl) {
        const wateringReading = sourceSnapshot.watering;
        if (wateringReading) {
          const on = wateringReading.value !== 0;
          wateringEl.className = `soil-watering-value status-badge ${on ? 'status-on' : 'status-off'}`;
          wateringEl.textContent = on ? 'Activo' : 'Inactivo';
        }
      }

      const durationEl = soilCard.querySelector('.watering-duration');
      if (durationEl) {
        durationEl.textContent = formatRelayDuration(sourceSnapshot[RELAY_ON_TIME_FIELD]?.value);
      }

      const thresholdEl = soilCard.querySelector('.watering-threshold');
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

      if (!Number.isFinite(lastEpoch) || lastEpoch <= 0) {
        lastEpoch = readPersistedEpoch(soilCard);
      }

      const extraEl = soilCard.querySelector('.last-watering-time');
      if (extraEl) {
        extraEl.textContent = formatLastWatering(lastEpoch);
        if (Number.isFinite(lastEpoch) && lastEpoch > 0) {
          extraEl.setAttribute('data-last-watering-epoch', String(lastEpoch));
        }
      }
    }

    const atmosphereCard = panel.querySelector('.card.atmosphere');
    if (atmosphereCard) {
      const snapshot = bySource[r.source] || {};
      const temp = snapshot.ambient_temperature;
      const hum = snapshot.ambient_humidity;
      const light = snapshot.light;

      const tempEl = atmosphereCard.querySelector('.atmo-temp');
      if (tempEl) {
        tempEl.textContent = temp ? `${parseFloat(temp.value).toFixed(1)} °C` : '--';
      }

      const humEl = atmosphereCard.querySelector('.atmo-hum');
      if (humEl) {
        humEl.textContent = hum ? `${parseFloat(hum.value).toFixed(1)} %` : '--';
      }

      const lightEl = atmosphereCard.querySelector('.atmo-light');
      if (lightEl) {
        lightEl.textContent = light ? `${parseFloat(light.value).toFixed(1)} %` : '--';
      }

      const timeEl = atmosphereCard.querySelector('.atmo-time');
      if (timeEl) {
        const times = [
          temp ? String(temp.recorded_at || '') : '',
          hum ? String(hum.recorded_at || '') : '',
          light ? String(light.recorded_at || '') : '',
        ];
        timeEl.textContent = times.reduce((a, b) => a > b ? a : b);
      }
    }

    const onlineCard = panel.querySelector('.card.online');
    if (onlineCard) {
      const snapshot = bySource[r.source] || {};
      const online = snapshot.online;
      const statusEl = onlineCard.querySelector('.online-status-value');
      if (statusEl) {
        const on = online && online.value !== 0;
        statusEl.className = `soil-info-value online-status-value status-badge ${on ? 'status-on' : 'status-off'}`;
        statusEl.textContent = on ? 'Conectado' : 'Desconectado';
      }
      const timeEl = onlineCard.querySelector('.online-time');
      if (timeEl) {
        timeEl.textContent = online ? String(online.recorded_at || '') : '--';
      }
    }
  });
}

/**
 * Send manual watering command via POST /api/command/water.
 * Disables the button during the request and shows success/error status.
 * @param {HTMLElement} btn - The button element that triggered the action.
 * @param {string} [source='esp8266'] - Device source name.
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
  // Clear command status after 6 seconds.
  setTimeout(() => {
    if (statusEl) {
      statusEl.textContent = '';
      statusEl.className = 'cmd-status';
    }
  }, 6000);
}

// Load trend chart from /api/trend.

const DEFAULT_TREND_SOURCE = 'esp8266';
let trendSource = DEFAULT_TREND_SOURCE;
const TREND_RANGE_SELECT_ID = 'trend-range';
const TREND_TITLE_BY_SOURCE = {
  esp8266: 'Tendencia de Riego',
  esp32_01: 'Tendencia Ambiental',
  esp32_02: 'Tendencia Ambiental',
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

// Return selected trend range with 1h fallback.
function getSelectedTrendRange() {
  const select = document.getElementById(TREND_RANGE_SELECT_ID);
  if (!select) return '1h';
  return select.value || '1h';
}

/**
 * Fetch trend data from /api/trend and render charts for the active source.
 * @param {string} [rangeKey='1h'] - One of: 1h, 1d, 1w, 1m, 1y.
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

// Range selector handler.
function onTrendRangeChange() {
  loadTrendChart(getSelectedTrendRange());
}

// Nota: el refresco periodico fue removido.
// El dashboard se actualiza unicamente cuando el usuario presiona
// el boton "Actualizar" (ver refreshLatest() en templates/index.html).
