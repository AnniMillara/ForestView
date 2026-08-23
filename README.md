# ForestGuard - Sistema de Monitoreo Ambiental

ForestGuard es una aplicación Flask que recibe datos de una ESP32 con sensores DHT22 y MQ-2, los almacena en MySQL, y los visualiza en un dashboard con mapa interactivo, alertas automáticas y gestión de usuarios.

## Requisitos

- Python 3.8+
- MySQL 5.7+
- ESP32 con Arduino IDE
- Librerías: ver `requirements.txt`

## Instalación

1. Clonar el repositorio.
2. Crear entorno virtual (opcional):
   ```bash
   python -m venv venv
   source venv/bin/activate  # Linux/Mac
   venv\Scripts\activate     # Windows