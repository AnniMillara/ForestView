/* =========================================================
   FORESTGUARD DASHBOARD - JavaScript real con datos de la API
========================================================= */

// Variables globales
let dashboardMap = null;
let zonesMap = null;
let environmentChart = null;
let updateInterval = null;

// Elementos del DOM
const temperatureElement = document.getElementById("temperature");
const humidityElement = document.getElementById("humidity");
const smokeElement = document.getElementById("smoke");
const riskElement = document.getElementById("risk");
const lastUpdateElement = document.getElementById("lastUpdate");
const sensorConnectionStatus = document.getElementById("sensorConnectionStatus");

/* =========================================================
   OBTENER DATOS DE LA API (con filtro por estación)
========================================================= */

async function fetchDashboardData() {
    try {
        const urlParams = new URLSearchParams(window.location.search);
        const estacion = urlParams.get('estacion');
        let apiUrl = '/api/dashboard_data';
        if (estacion) {
            apiUrl += `?estacion=${estacion}`;
        }
        const response = await fetch(apiUrl);
        if (!response.ok) throw new Error('Error en la API');
        const data = await response.json();
        return data;
    } catch (error) {
        console.error('Error al obtener datos del dashboard:', error);
        return null;
    }
}

/* =========================================================
   ACTUALIZAR DASHBOARD CON DATOS REALES
========================================================= */

function updateDashboardWithData(data) {
    if (!data || data.length === 0) {
        if (temperatureElement) temperatureElement.textContent = '--';
        if (humidityElement) humidityElement.textContent = '--';
        if (smokeElement) smokeElement.textContent = '--';
        if (riskElement) riskElement.textContent = 'NORMAL';
        if (sensorConnectionStatus) sensorConnectionStatus.textContent = '0/0 estaciones conectadas';
        return;
    }

    let total = data.length;
    let online = data.filter(e => e.estado === 'ONLINE').length;
    let offline = data.filter(e => e.estado === 'OFFLINE').length;

    if (sensorConnectionStatus) {
        sensorConnectionStatus.textContent = `${online}/${total} estaciones conectadas`;
    }

    // Última medición (la más reciente entre las estaciones filtradas)
    let ultimaMedicion = null;
    let ultimaFecha = null;
    data.forEach(est => {
        if (est.ultima_conexion) {
            let fecha = new Date(est.ultima_conexion);
            if (!ultimaFecha || fecha > ultimaFecha) {
                ultimaFecha = fecha;
                ultimaMedicion = est;
            }
        }
    });

    if (data.length === 1) {
        ultimaMedicion = data[0];
    }

    if (ultimaMedicion) {
        if (temperatureElement) {
            let temp = ultimaMedicion.temperatura;
            temperatureElement.textContent = temp !== null && temp !== undefined ? temp.toFixed(1) : '--';
        }
        if (humidityElement) {
            let hum = ultimaMedicion.humedad;
            humidityElement.textContent = hum !== null && hum !== undefined ? Math.round(hum) : '--';
        }
        if (smokeElement) {
            let humo = ultimaMedicion.humo_nombre || 'NINGUNO';
            smokeElement.textContent = humo;
        }
    }

    // Riesgo máximo
    let maxSeveridad = 0;
    let maxNombre = 'NORMAL';
    let maxColor = '#159447';
    data.forEach(est => {
        if (est.nivel_alerta_activa) {
            let severidad = { 'NORMAL': 0, 'RIESGO_MODERADO': 1, 'RIESGO_ALTO': 2, 'ALERTA': 3 }[est.nivel_alerta_activa] || 0;
            if (severidad > maxSeveridad) {
                maxSeveridad = severidad;
                maxNombre = est.nivel_alerta_activa;
                maxColor = est.color_alerta || '#159447';
            }
        }
    });
    if (riskElement) {
        riskElement.textContent = maxNombre;
        riskElement.style.color = maxColor;
    }
    const indicator = document.querySelector(".risk-indicator");
    if (indicator) {
        indicator.style.background = maxColor;
        indicator.style.boxShadow = `0 0 0 5px ${maxColor}22`;
    }

    updateLastUpdate();

    // Actualizar mapa
    if (document.getElementById('dashboardMap') && dashboardMap) {
        updateMapMarkers(dashboardMap, data);
    }
    if (document.getElementById('zonesMap') && zonesMap) {
        updateMapMarkers(zonesMap, data);
    }
}

/* =========================================================
   ACTUALIZAR MAPA CON MARCADORES
========================================================= */

function updateMapMarkers(map, data) {
    // Limpiar marcadores anteriores
    map.eachLayer((layer) => {
        if (layer instanceof L.Marker || layer instanceof L.Popup) {
            map.removeLayer(layer);
        }
    });

    data.forEach(est => {
        if (!est.latitud || !est.longitud) return;
        let color = est.color_alerta || '#22c55e';
        if (!est.color_alerta) {
            if (est.estado === 'ONLINE') color = '#22c55e';
            else if (est.estado === 'OFFLINE') color = '#94a3b8';
            else if (est.estado === 'ERROR') color = '#3b82f6';
            else color = '#f59e0b';
        }
        let icon = L.divIcon({
            className: 'forestguard-marker',
            html: `<div class="forestguard-marker-content" style="background:${color};"><i class="fa-solid fa-tree"></i></div>`,
            iconSize: [30, 30],
            iconAnchor: [15, 15],
            popupAnchor: [0, -15]
        });
        L.marker([est.latitud, est.longitud], { icon: icon })
            .addTo(map)
            .bindPopup(`
                <div class="forestguard-popup">
                    <h3><i class="fa-solid fa-location-dot"></i> ${est.nombre}</h3>
                    <p><strong>Estado:</strong> ${est.estado}</p>
                    <p><strong>Temperatura:</strong> ${est.temperatura !== null && est.temperatura !== undefined ? est.temperatura.toFixed(1) : 'N/A'} °C</p>
                    <p><strong>Humedad:</strong> ${est.humedad !== null && est.humedad !== undefined ? Math.round(est.humedad) : 'N/A'} %</p>
                    <p><strong>Humo:</strong> ${est.humo_nombre || 'N/A'}</p>
                    ${est.nivel_alerta_activa ? `<p><strong>Riesgo:</strong> <span class="risk-${est.nivel_alerta_activa.toLowerCase()}">${est.nivel_alerta_activa}</span></p>` : ''}
                </div>
            `);
    });
}

/* =========================================================
   OTRAS FUNCIONES
========================================================= */

function updateLastUpdate() {
    if (!lastUpdateElement) return;
    const now = new Date();
    const hours = String(now.getHours()).padStart(2, "0");
    const minutes = String(now.getMinutes()).padStart(2, "0");
    const seconds = String(now.getSeconds()).padStart(2, "0");
    lastUpdateElement.textContent = `${hours}:${minutes}:${seconds}`;
}

/* =========================================================
   INICIALIZAR MAPA DASHBOARD
========================================================= */

function initializeDashboardMap() {
    const mapElement = document.getElementById("dashboardMap");
    if (!mapElement || dashboardMap) return;

    dashboardMap = L.map("dashboardMap").setView([-34.5, -71.0], 5);
    L.tileLayer('https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png', {
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>, &copy; CartoDB'
    }).addTo(dashboardMap);

    fetchDashboardData().then(data => {
        if (data) updateMapMarkers(dashboardMap, data);
    });

    window.addEventListener('resize', () => {
        if (dashboardMap) dashboardMap.invalidateSize();
    });
}

/* =========================================================
   INICIALIZAR MAPA ZONAS
========================================================= */

function initializeZonesMap() {
    const mapElement = document.getElementById("zonesMap");
    if (!mapElement || zonesMap) return;

    zonesMap = L.map("zonesMap").setView([-34.5, -71.0], 6);
    L.tileLayer('https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png', {
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>, &copy; CartoDB'
    }).addTo(zonesMap);

    const legend = L.control({ position: "bottomright" });
    legend.onAdd = function() {
        const div = L.DomUtil.create("div", "map-legend");
        div.innerHTML = `
            <div class="map-legend-title">Nivel de riesgo</div>
            <div class="legend-item"><span class="legend-dot legend-low"></span> Bajo</div>
            <div class="legend-item"><span class="legend-dot legend-medium"></span> Medio</div>
            <div class="legend-item"><span class="legend-dot legend-high"></span> Alto</div>
        `;
        return div;
    };
    legend.addTo(zonesMap);

    fetchDashboardData().then(data => {
        if (data) updateMapMarkers(zonesMap, data);
    });

    window.addEventListener('resize', () => {
        if (zonesMap) zonesMap.invalidateSize();
    });
}

/* =========================================================
   GRÁFICO
========================================================= */

function initializeEnvironmentChart(historico) {
    const canvas = document.getElementById("environmentChart");
    if (!canvas) return;
    const ctx = canvas.getContext("2d");

    if (!historico || historico.length === 0) {
        historico = [];
    }

    const labels = historico.map(h => h.fecha_hora || '');
    const temps = historico.map(h => h.temperatura !== null && h.temperatura !== undefined ? h.temperatura : null);
    const hums = historico.map(h => h.humedad !== null && h.humedad !== undefined ? h.humedad : null);
    const humo = historico.map(h => h.humo !== null && h.humo !== undefined ? h.humo : 0);

    if (environmentChart) {
        environmentChart.destroy();
    }

    environmentChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: labels.length ? labels : ['Sin datos'],
            datasets: [
                {
                    label: 'Temperatura (°C)',
                    data: temps.length ? temps : [0],
                    borderColor: '#16833d',
                    backgroundColor: 'rgba(22, 131, 61, 0.10)',
                    tension: 0.35,
                    fill: true,
                    pointRadius: 4,
                    pointHoverRadius: 6
                },
                {
                    label: 'Humedad (%)',
                    data: hums.length ? hums : [0],
                    borderColor: '#2985c7',
                    tension: 0.35,
                    fill: false,
                    pointRadius: 4,
                    pointHoverRadius: 6
                },
                {
                    label: 'Humo (cambio AO)',
                    data: humo.length ? humo : [0],
                    borderColor: '#e5a500',
                    tension: 0.35,
                    fill: false,
                    pointRadius: 4,
                    pointHoverRadius: 6,
                    yAxisID: 'y1'
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: 'index', intersect: false },
            plugins: {
                legend: {
                    position: 'top',
                    align: 'end',
                    labels: {
                        usePointStyle: true,
                        boxWidth: 8,
                        font: { family: 'Poppins', size: 10 }
                    }
                },
                tooltip: {
                    backgroundColor: '#062b16',
                    titleFont: { family: 'Poppins' },
                    bodyFont: { family: 'Poppins' },
                    padding: 10,
                    cornerRadius: 8
                }
            },
            scales: {
                x: {
                    grid: { display: false },
                    ticks: {
                        color: '#9aa69f',
                        font: { family: 'Poppins', size: 9 },
                        maxRotation: 0,
                        autoSkip: true,
                        maxTicksLimit: 8
                    }
                },
                y: {
                    beginAtZero: false,
                    suggestedMin: 0,
                    suggestedMax: 100,
                    grid: { color: '#e5ebe7' },
                    ticks: {
                        color: '#9aa69f',
                        font: { family: 'Poppins', size: 9 }
                    }
                },
                y1: {
                    position: 'right',
                    grid: { drawOnChartArea: false },
                    ticks: {
                        color: '#9aa69f',
                        font: { family: 'Poppins', size: 9 }
                    }
                }
            }
        }
    });
}

function startPeriodicUpdate() {
    updateInterval = setInterval(async () => {
        const data = await fetchDashboardData();
        if (data) {
            updateDashboardWithData(data);
        }
    }, 15000);
}

document.addEventListener("DOMContentLoaded", function() {
    initializeDashboardMap();
    initializeZonesMap();
    fetchDashboardData().then(data => {
        if (data) {
            updateDashboardWithData(data);
        }
    });
    startPeriodicUpdate();
    setInterval(updateLastUpdate, 1000);
    console.log("ForestGuard Dashboard inicializado correctamente.");
});

window.initializeChart = function(historico) {
    initializeEnvironmentChart(historico);
};