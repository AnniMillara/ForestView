/* =========================================================
   FORESTGUARD DASHBOARD - JavaScript real con datos de la API
========================================================= */

// Variables globales
let dashboardMap = null;
let zonesMap = null;
let environmentChart = null;
let sensorHistory = [];
let selectedChartPeriod = 24;
let updateInterval = null;

// Elementos del DOM
const temperatureElement = document.getElementById("temperature");
const humidityElement = document.getElementById("humidity");
const smokeElement = document.getElementById("smoke");
const riskElement = document.getElementById("risk");
const lastUpdateElement = document.getElementById("lastUpdate");
const sensorConnectionStatus = document.getElementById("sensorConnectionStatus");


/* =========================================================
   OBTENER DATOS DE LA API
========================================================= */

async function fetchDashboardData() {
    try {
        const response = await fetch('/api/dashboard_data');
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
    if (!data || data.length === 0) return;

    // Calcular estadísticas generales
    let total = data.length;
    let online = data.filter(e => e.estado === 'ONLINE').length;
    let offline = data.filter(e => e.estado === 'OFFLINE').length;
    let error = data.filter(e => e.estado === 'ERROR').length;

    // Actualizar estado de conexión
    if (sensorConnectionStatus) {
        sensorConnectionStatus.textContent = `${online}/${total} estaciones conectadas`;
    }

    // Buscar la última medición global (la más reciente de todas)
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

    if (ultimaMedicion) {
        // Temperatura
        if (temperatureElement) {
            let temp = ultimaMedicion.temperatura;
            temperatureElement.textContent = temp !== null && temp !== undefined ? temp.toFixed(1) : '--';
        }
        // Humedad
        if (humidityElement) {
            let hum = ultimaMedicion.humedad;
            humidityElement.textContent = hum !== null && hum !== undefined ? Math.round(hum) : '--';
        }
        // Humo
        if (smokeElement) {
            let humo = ultimaMedicion.humo_nombre || 'NINGUNO';
            smokeElement.textContent = humo;
        }
        // Riesgo (tomar el mayor nivel de alerta activa)
        let maxSeveridad = 0;
        let maxNombre = 'NORMAL';
        let maxColor = '#159447';
        data.forEach(est => {
            if (est.nivel_alerta_activa) {
                // Podríamos mapear nombres a severidad, pero mejor usamos el color o un orden fijo
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
        // Indicador de riesgo
        const indicator = document.querySelector(".risk-indicator");
        if (indicator) {
            indicator.style.background = maxColor;
            indicator.style.boxShadow = `0 0 0 5px ${maxColor}22`;
        }
    }

    // Actualizar hora de última actualización
    updateLastUpdate();

    // Actualizar mapa (si está visible)
    if (document.getElementById('dashboardMap') && dashboardMap) {
        updateMapMarkers(dashboardMap, data);
    }
    if (document.getElementById('zonesMap') && zonesMap) {
        updateMapMarkers(zonesMap, data);
    }

    // Actualizar alertas recientes (en dashboard)
    updateRecentAlerts(data);

    // Actualizar lista de dispositivos (en dashboard)
    updateDeviceList(data);
}


/* =========================================================
   ACTUALIZAR MAPA CON MARCADORES
========================================================= */

function updateMapMarkers(map, data) {
    // Limpiar capas existentes (excepto el tile layer)
    map.eachLayer((layer) => {
        if (layer instanceof L.Marker || layer instanceof L.Popup) {
            map.removeLayer(layer);
        }
    });

    data.forEach(est => {
        if (!est.latitud || !est.longitud) return;
        let color = est.color_alerta || '#159447';
        // Si no hay alerta, usar color según estado
        if (!est.color_alerta) {
            if (est.estado === 'ONLINE') color = '#22c55e';
            else if (est.estado === 'OFFLINE') color = '#9ca3af';
            else if (est.estado === 'ERROR') color = '#3b82f6';
            else color = '#f59e0b';
        }
        let icon = L.divIcon({
            className: 'forestguard-marker',
            html: `<div class="forestguard-marker-content" style="background:${color};"><i class="fa-solid fa-tree"></i></div>`,
            iconSize: [38, 38],
            iconAnchor: [19, 19],
            popupAnchor: [0, -19]
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
   ACTUALIZAR ALERTAS RECIENTES (en dashboard)
========================================================= */

function updateRecentAlerts(data) {
    const container = document.querySelector('.alerts-list');
    if (!container) return;

    // Obtener alertas activas de la API (ya vienen en data)
    // Pero la API no devuelve alertas directamente, solo el nivel. 
    // Para simplificar, usamos los datos de alertas que se pasan desde Flask en el renderizado inicial.
    // En el dashboard, se renderizan con Jinja, y la actualización periódica no las cambia.
    // Dejamos que el renderizado inicial las muestre, y no las actualizamos dinámicamente.
    // Pero podemos recargar la página completa si queremos, o mejor no hacer nada aquí.
}


/* =========================================================
   ACTUALIZAR LISTA DE DISPOSITIVOS (dashboard)
========================================================= */

function updateDeviceList(data) {
    const container = document.querySelector('.device-list');
    if (!container) return;

    // Similar a las alertas, los dispositivos se renderizan desde Flask con Jinja.
    // No actualizamos dinámicamente para no perder el diseño.
}


/* =========================================================
   ACTUALIZAR HORA DE ÚLTIMA ACTUALIZACIÓN
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
    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19,
        attribution: "&copy; OpenStreetMap contributors"
    }).addTo(dashboardMap);

    // Cargar datos iniciales para el mapa
    fetchDashboardData().then(data => {
        if (data) updateMapMarkers(dashboardMap, data);
    });
}


/* =========================================================
   INICIALIZAR MAPA ZONAS
========================================================= */

function initializeZonesMap() {
    const mapElement = document.getElementById("zonesMap");
    if (!mapElement || zonesMap) return;

    zonesMap = L.map("zonesMap").setView([-34.5, -71.0], 6);
    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19,
        attribution: "&copy; OpenStreetMap contributors"
    }).addTo(zonesMap);

    // Agregar leyenda
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

    // Cargar datos iniciales
    fetchDashboardData().then(data => {
        if (data) updateMapMarkers(zonesMap, data);
    });
}


/* =========================================================
   GRÁFICO - Inicializar con datos reales desde Flask
========================================================= */

function initializeEnvironmentChart(historico) {
    const canvas = document.getElementById("environmentChart");
    if (!canvas) return;

    const ctx = canvas.getContext("2d");

    // Si no hay historico, usar datos vacíos
    if (!historico || historico.length === 0) {
        historico = [];
    }

    // Preparar datos
    const labels = historico.map(h => h.fecha_hora || '');
    const temps = historico.map(h => h.temperatura !== null && h.temperatura !== undefined ? h.temperatura : null);
    const hums = historico.map(h => h.humedad !== null && h.humedad !== undefined ? h.humedad : null);
    const humo = historico.map(h => h.humo !== null && h.humo !== undefined ? h.humo : 0);

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
            interaction: {
                mode: 'index',
                intersect: false
            },
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


/* =========================================================
   SELECTOR DE PERÍODO DEL GRÁFICO
========================================================= */

function initializeChartPeriod() {
    const chartPeriod = document.getElementById("chartPeriod");
    if (!chartPeriod) return;

    chartPeriod.addEventListener("change", function() {
        const value = this.value;
        // Recargar la página con el parámetro de rango
        window.location.href = `/dashboard?rango=${value}`;
    });
}


/* =========================================================
   ACTUALIZACIÓN PERIÓDICA (cada 15 segundos)
========================================================= */

function startPeriodicUpdate() {
    // Actualizar cada 15 segundos con datos de la API
    updateInterval = setInterval(async () => {
        const data = await fetchDashboardData();
        if (data) {
            updateDashboardWithData(data);
            // También actualizar el gráfico si es necesario (recargar página o hacer fetch específico)
            // Para simplificar, no actualizamos el gráfico en tiempo real, solo recargamos la página si es necesario.
        }
    }, 15000);
}


/* =========================================================
   INICIALIZACIÓN COMPLETA
========================================================= */

document.addEventListener("DOMContentLoaded", function() {

    // Obtener historico desde la variable de Jinja (se pasa en el renderizado)
    // Nota: en las plantillas, se inyecta como variable 'historico'
    // Para que funcione, se debe pasar en el contexto de Flask.
    // En el dashboard.html, ya se pasa 'historico' como una lista de diccionarios.
    // Lo tomamos desde una variable global que podemos definir en la plantilla.
    // Pero como no podemos acceder directamente a Jinja desde JS, lo haremos mediante un script en la plantilla.
    // En dashboard.html, añadiremos un script que defina window.historicoData.
    // Por ahora, inicializamos con datos vacíos y luego se actualizará con la API.

    // Inicializar gráfico con datos vacíos (se llenará desde la plantilla)
    // En la plantilla, se inyecta 'historico' y se llama a initializeEnvironmentChart(historico)
    // Por eso, dejamos esta función para ser llamada desde la plantilla.

    // Inicializar mapas
    initializeDashboardMap();
    initializeZonesMap();

    // Inicializar selector de período
    initializeChartPeriod();

    // Cargar datos iniciales para el dashboard
    fetchDashboardData().then(data => {
        if (data) {
            updateDashboardWithData(data);
        }
    });

    // Iniciar actualización periódica
    startPeriodicUpdate();

    // Actualizar la hora cada segundo
    setInterval(updateLastUpdate, 1000);

    console.log("ForestGuard Dashboard inicializado correctamente.");
});


/* =========================================================
   FUNCIONES EXPORTADAS PARA USO DESDE PLANTILLAS
========================================================= */

// Función para inicializar el gráfico desde la plantilla
window.initializeChart = function(historico) {
    initializeEnvironmentChart(historico);
};