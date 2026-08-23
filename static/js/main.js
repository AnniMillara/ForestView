document.addEventListener('DOMContentLoaded', () => {
    // --- Mapa Leaflet (si existe el contenedor) ---
    const mapContainer = document.getElementById('map');
    if (mapContainer) {
        const map = L.map('map').setView([-33.4489, -70.6693], 6);
        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '© OpenStreetMap contributors'
        }).addTo(map);

        // Los marcadores se actualizan desde dashboard.html con fetch,
        // pero podemos poner unos de ejemplo para que se vea algo.
        // En realidad, dashboard.html ya los pinta con /api/dashboard_data.
        // Este bloque es solo por si se usa en otras páginas.
    }

    // --- Gráfico Chart.js (si existe el canvas) ---
    const chartCanvas = document.getElementById('chart');
    if (chartCanvas) {
        // Si el gráfico se usa en dashboard, se puede llenar con datos reales desde Jinja.
        // Pero dejamos un ejemplo estático; en dashboard.html se puede sobrescribir.
        new Chart(chartCanvas, {
            type: 'line',
            data: {
                labels: ['17:30:20', '17:30:35', '17:30:50', '17:31:05', '17:31:20', '17:31:36', '17:31:50'],
                datasets: [{
                    label: 'Temperatura (°C)',
                    data: [20, 20.2, 20.1, 20.9, 20.5, 20.3, 20.9],
                    borderColor: '#10b981',
                    tension: 0.3,
                    fill: false
                }, {
                    label: 'Humedad (%)',
                    data: [60, 58, 57, 58, 56, 55, 58],
                    borderColor: '#3b82f6',
                    tension: 0.3,
                    fill: false
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false
            }
        });
    }
});