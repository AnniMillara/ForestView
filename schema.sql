-- =====================================================================
-- FORESTGUARD - BASE DE DATOS COMPLETA
-- =====================================================================

DROP DATABASE IF EXISTS forestguard;
CREATE DATABASE forestguard CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE forestguard;

-- =====================================================================
-- 1. CATÁLOGOS
-- =====================================================================

CREATE TABLE tipos_usuario (
    id_tipo_usuario INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE estados_usuario (
    id_estado_usuario INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE zonas (
    id_zona INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(60) NOT NULL,
    descripcion VARCHAR(200)
);

CREATE TABLE estados_estacion (
    id_estado_estacion INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE tipos_sensor (
    id_tipo_sensor INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE estados_sensor (
    id_estado_sensor INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE tipos_humo (
    id_tipo_humo INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL,
    nivel INT NOT NULL  -- 0=NINGUNO, 1=BAJO, 2=MEDIO, 3=ALTO
);

CREATE TABLE niveles_riesgo (
    id_nivel_riesgo INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(30) UNIQUE NOT NULL,
    descripcion VARCHAR(200) NOT NULL,
    severidad INT NOT NULL,
    color_hex VARCHAR(7) NOT NULL
);

CREATE TABLE tipos_alerta (
    id_tipo_alerta INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(30) UNIQUE NOT NULL,
    descripcion VARCHAR(200) NOT NULL
);

CREATE TABLE estados_alerta (
    id_estado_alerta INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(20) UNIQUE NOT NULL,
    descripcion VARCHAR(150) NOT NULL
);

CREATE TABLE tipos_evento (
    id_tipo_evento INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(40) UNIQUE NOT NULL,
    descripcion VARCHAR(200) NOT NULL
);

-- =====================================================================
-- 2. CONFIGURACIÓN
-- =====================================================================

CREATE TABLE configuracion (
    id_config INT PRIMARY KEY AUTO_INCREMENT,
    clave VARCHAR(50) UNIQUE NOT NULL,
    valor VARCHAR(50) NOT NULL,
    descripcion VARCHAR(150)
);

-- =====================================================================
-- 3. USUARIOS Y SEGURIDAD
-- =====================================================================

CREATE TABLE usuarios (
    id_usuario INT PRIMARY KEY AUTO_INCREMENT,
    nombre_usuario VARCHAR(50) UNIQUE NOT NULL,
    contrasena_hash VARCHAR(255) NOT NULL,
    email VARCHAR(120) UNIQUE NOT NULL,
    nombre_completo VARCHAR(120) NOT NULL,
    id_tipo_usuario INT NOT NULL,
    id_estado_usuario INT NOT NULL DEFAULT 1,
    fecha_creacion TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    ultimo_acceso TIMESTAMP NULL,
    FOREIGN KEY (id_tipo_usuario) REFERENCES tipos_usuario(id_tipo_usuario),
    FOREIGN KEY (id_estado_usuario) REFERENCES estados_usuario(id_estado_usuario)
);

CREATE TABLE logs_acceso (
    id_log_acceso INT PRIMARY KEY AUTO_INCREMENT,
    id_usuario INT NULL,
    nombre_usuario_intento VARCHAR(50),
    exitoso TINYINT(1) NOT NULL DEFAULT 0,
    ip_origen VARCHAR(45),
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_usuario) REFERENCES usuarios(id_usuario) ON DELETE SET NULL
);

CREATE TABLE logs_sistema (
    id_log INT PRIMARY KEY AUTO_INCREMENT,
    id_usuario INT NULL,
    accion VARCHAR(100) NOT NULL,
    descripcion VARCHAR(255),
    ip_origen VARCHAR(45),
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_usuario) REFERENCES usuarios(id_usuario) ON DELETE SET NULL
);

-- =====================================================================
-- 4. ESTACIONES
-- =====================================================================

CREATE TABLE estaciones (
    id_estacion INT PRIMARY KEY AUTO_INCREMENT,
    nombre VARCHAR(60) UNIQUE NOT NULL,
    codigo VARCHAR(20) UNIQUE NOT NULL,
    api_key VARCHAR(64) UNIQUE NOT NULL,
    id_estado_estacion INT NOT NULL DEFAULT 4,
    id_zona INT NULL,
    latitud DECIMAL(10,8) NOT NULL,
    longitud DECIMAL(11,8) NOT NULL,
    ip VARCHAR(45),
    ultima_conexion TIMESTAMP NULL,
    fecha_registro TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    descripcion VARCHAR(255),
    activa TINYINT(1) NOT NULL DEFAULT 1,
    FOREIGN KEY (id_estado_estacion) REFERENCES estados_estacion(id_estado_estacion),
    FOREIGN KEY (id_zona) REFERENCES zonas(id_zona) ON DELETE SET NULL
);

CREATE TABLE configuracion_estaciones (
    id_config_estacion INT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT UNIQUE NOT NULL,
    temp_alta DECIMAL(5,2) NULL,
    temp_critica DECIMAL(5,2) NULL,
    hum_baja DECIMAL(5,2) NULL,
    hum_critica DECIMAL(5,2) NULL,
    humo_bajo INT NULL,
    humo_medio INT NULL,
    humo_alto INT NULL,
    offline_timeout INT NULL,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE
);

CREATE TABLE historial_estados_estacion (
    id_historial INT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT NOT NULL,
    id_estado_anterior INT NULL,
    id_estado_nuevo INT NOT NULL,
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE,
    FOREIGN KEY (id_estado_anterior) REFERENCES estados_estacion(id_estado_estacion),
    FOREIGN KEY (id_estado_nuevo) REFERENCES estados_estacion(id_estado_estacion)
);

CREATE TABLE historial_conexiones (
    id_conexion INT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT NOT NULL,
    ip VARCHAR(45),
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE
);

-- =====================================================================
-- 5. SENSORES
-- =====================================================================

CREATE TABLE sensores (
    id_sensor INT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT NOT NULL,
    id_tipo_sensor INT NOT NULL,
    id_estado_sensor INT NOT NULL DEFAULT 3,
    pin_gpio INT,               -- Número GPIO
    canal VARCHAR(20),          -- DATA, AO, DO, ROJO, VERDE, AZUL
    descripcion VARCHAR(100),
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE,
    FOREIGN KEY (id_tipo_sensor) REFERENCES tipos_sensor(id_tipo_sensor),
    FOREIGN KEY (id_estado_sensor) REFERENCES estados_sensor(id_estado_sensor)
);

CREATE TABLE historial_estados_sensor (
    id_historial INT PRIMARY KEY AUTO_INCREMENT,
    id_sensor INT NOT NULL,
    id_estado_anterior INT NULL,
    id_estado_nuevo INT NOT NULL,
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_sensor) REFERENCES sensores(id_sensor) ON DELETE CASCADE,
    FOREIGN KEY (id_estado_anterior) REFERENCES estados_sensor(id_estado_sensor),
    FOREIGN KEY (id_estado_nuevo) REFERENCES estados_sensor(id_estado_sensor)
);

-- =====================================================================
-- 6. MEDICIONES
-- =====================================================================

CREATE TABLE mediciones (
    id_medicion BIGINT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT NOT NULL,
    temperatura DECIMAL(5,2) NULL,
    humedad DECIMAL(5,2) NULL,
    dht_ok TINYINT(1) NOT NULL DEFAULT 1,
    mq2_ao INT NULL,
    mq2_base INT NULL,
    cambio_ao INT NULL,
    mq2_do TINYINT(1) NULL,
    mq2_ok TINYINT(1) NOT NULL DEFAULT 1,
    id_tipo_humo INT NULL,
    fecha_hora TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE,
    FOREIGN KEY (id_tipo_humo) REFERENCES tipos_humo(id_tipo_humo)
);

-- =====================================================================
-- 7. EVALUACIÓN DE RIESGO
-- =====================================================================

CREATE TABLE evaluaciones_riesgo (
    id_evaluacion BIGINT PRIMARY KEY AUTO_INCREMENT,
    id_medicion BIGINT NOT NULL,
    id_estacion INT NOT NULL,
    id_nivel_riesgo INT NOT NULL,
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_medicion) REFERENCES mediciones(id_medicion) ON DELETE CASCADE,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE,
    FOREIGN KEY (id_nivel_riesgo) REFERENCES niveles_riesgo(id_nivel_riesgo)
);

-- =====================================================================
-- 8. ALERTAS
-- =====================================================================

CREATE TABLE alertas (
    id_alerta BIGINT PRIMARY KEY AUTO_INCREMENT,
    id_estacion INT NOT NULL,
    id_medicion BIGINT NULL,
    id_evaluacion BIGINT NULL,
    id_tipo_alerta INT NOT NULL,
    id_nivel_riesgo INT NOT NULL,
    id_estado_alerta INT NOT NULL DEFAULT 1,
    id_usuario_atendio INT NULL,
    titulo VARCHAR(80) NOT NULL,
    descripcion VARCHAR(255) NOT NULL,
    fecha_creacion TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    fecha_atencion TIMESTAMP NULL,
    fecha_resolucion TIMESTAMP NULL,
    es_falsa TINYINT(1) NOT NULL DEFAULT 0,
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE CASCADE,
    FOREIGN KEY (id_medicion) REFERENCES mediciones(id_medicion) ON DELETE SET NULL,
    FOREIGN KEY (id_evaluacion) REFERENCES evaluaciones_riesgo(id_evaluacion) ON DELETE SET NULL,
    FOREIGN KEY (id_tipo_alerta) REFERENCES tipos_alerta(id_tipo_alerta),
    FOREIGN KEY (id_nivel_riesgo) REFERENCES niveles_riesgo(id_nivel_riesgo),
    FOREIGN KEY (id_estado_alerta) REFERENCES estados_alerta(id_estado_alerta),
    FOREIGN KEY (id_usuario_atendio) REFERENCES usuarios(id_usuario) ON DELETE SET NULL
);

CREATE TABLE historial_alertas (
    id_historial BIGINT PRIMARY KEY AUTO_INCREMENT,
    id_alerta BIGINT NOT NULL,
    id_estado_anterior INT NULL,
    id_estado_nuevo INT NOT NULL,
    id_usuario INT NULL,
    comentario VARCHAR(255),
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_alerta) REFERENCES alertas(id_alerta) ON DELETE CASCADE,
    FOREIGN KEY (id_estado_anterior) REFERENCES estados_alerta(id_estado_alerta),
    FOREIGN KEY (id_estado_nuevo) REFERENCES estados_alerta(id_estado_alerta),
    FOREIGN KEY (id_usuario) REFERENCES usuarios(id_usuario) ON DELETE SET NULL
);

-- =====================================================================
-- 9. EVENTOS
-- =====================================================================

CREATE TABLE eventos (
    id_evento BIGINT PRIMARY KEY AUTO_INCREMENT,
    id_tipo_evento INT NOT NULL,
    id_estacion INT NULL,
    id_usuario INT NULL,
    mensaje VARCHAR(255) NOT NULL,
    datos_extra JSON NULL,
    fecha TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (id_tipo_evento) REFERENCES tipos_evento(id_tipo_evento),
    FOREIGN KEY (id_estacion) REFERENCES estaciones(id_estacion) ON DELETE SET NULL,
    FOREIGN KEY (id_usuario) REFERENCES usuarios(id_usuario) ON DELETE SET NULL
);

-- =====================================================================
-- 10. PREFERENCIAS DE USUARIO (NUEVA TABLA)
-- =====================================================================

CREATE TABLE preferencias_usuario (
    id_preferencia INT PRIMARY KEY AUTO_INCREMENT,
    id_usuario INT NOT NULL UNIQUE,
    notificaciones TINYINT(1) DEFAULT 1,
    auto_update TINYINT(1) DEFAULT 1,
    ubicacion TINYINT(1) DEFAULT 1,
    FOREIGN KEY (id_usuario) REFERENCES usuarios(id_usuario) ON DELETE CASCADE
);

-- =====================================================================
-- 11. DATOS INICIALES
-- =====================================================================

INSERT INTO tipos_usuario (nombre, descripcion) VALUES
('ADMINISTRADOR', 'Control total del sistema'),
('BOMBERO', 'Personal de emergencia'),
('OPERADOR', 'Monitoreo diario'),
('OBSERVADOR', 'Solo visualización');

INSERT INTO estados_usuario (nombre, descripcion) VALUES
('ACTIVO', 'Cuenta habilitada'),
('INACTIVO', 'Cuenta deshabilitada'),
('BLOQUEADO', 'Cuenta bloqueada');

INSERT INTO zonas (nombre, descripcion) VALUES
('Zona Norte', 'Sector norte del área forestal'),
('Zona Centro', 'Sector central'),
('Zona Sur', 'Sector sur');

INSERT INTO estados_estacion (nombre, descripcion) VALUES
('ONLINE', 'Comunicación activa'),
('OFFLINE', 'Sin comunicación reciente'),
('ERROR', 'Error en sensores'),
('SIN_DATOS', 'Nunca ha enviado datos');

INSERT INTO tipos_sensor (nombre, descripcion) VALUES
('DHT22', 'Temperatura y humedad'),
('MQ-2', 'Sensor de gases/humo'),
('LED_RGB', 'Indicador visual');

INSERT INTO estados_sensor (nombre, descripcion) VALUES
('ACTIVO', 'Funcionando correctamente'),
('ERROR', 'Lectura errónea'),
('SIN_DATOS', 'Sin medición');

INSERT INTO tipos_humo (nombre, descripcion, nivel) VALUES
('NINGUNO', 'Sin humo detectable', 0),
('BAJO', 'Humo leve', 1),
('MEDIO', 'Humo moderado', 2),
('ALTO', 'Humo crítico', 3);

INSERT INTO niveles_riesgo (nombre, descripcion, severidad, color_hex) VALUES
('NORMAL', 'Condiciones dentro de rangos normales', 0, '#2ecc71'),
('RIESGO_MODERADO', 'Una variable fuera de rango', 1, '#f1c40f'),
('RIESGO_ALTO', 'Combinación de variables', 2, '#e67e22'),
('ALERTA', 'Posible incendio', 3, '#e74c3c');

INSERT INTO tipos_alerta (nombre, descripcion) VALUES
('POSIBLE_INCENDIO', 'Condiciones compatibles con incendio'),
('RIESGO_ALTO', 'Alta probabilidad de incendio'),
('RIESGO_MODERADO', 'Riesgo medio'),
('ANOMALIA', 'Comportamiento inusual'),
('SENSOR_ERROR', 'Falla en sensor'),
('ESTACION_OFFLINE', 'Estación sin comunicación');

INSERT INTO estados_alerta (nombre, descripcion) VALUES
('ACTIVA', 'Sin resolver'),
('ATENDIDA', 'En proceso'),
('RESUELTA', 'Solucionada'),
('FALSA', 'Falsa alarma');

INSERT INTO tipos_evento (nombre, descripcion) VALUES
('ESTACION_CONECTADA', 'Estación ONLINE'),
('ESTACION_DESCONECTADA', 'Estación OFFLINE'),
('SENSOR_ERROR', 'Error en sensor'),
('LECTURA_RECIBIDA', 'Medición recibida'),
('ALERTA_GENERADA', 'Nueva alerta'),
('ALERTA_ATENDIDA', 'Alerta atendida'),
('ALERTA_RESUELTA', 'Alerta resuelta'),
('CONFIGURACION_CAMBIADA', 'Configuración modificada'),
('LOGIN', 'Inicio de sesión'),
('LOGOUT', 'Cierre de sesión');

INSERT INTO configuracion (clave, valor, descripcion) VALUES
('temp_alta', '32', 'Temperatura alta (°C)'),
('temp_critica', '38', 'Temperatura crítica (°C)'),
('hum_baja', '30', 'Humedad baja (%)'),
('hum_critica', '15', 'Humedad crítica (%)'),
('humo_bajo', '100', 'Cambio AO para humo bajo'),
('humo_medio', '300', 'Cambio AO para humo medio'),
('humo_alto', '550', 'Cambio AO para humo alto'),
('offline_timeout', '90', 'Segundos sin datos para OFFLINE'),
('radio_alerta', '5', 'Radio en kilómetros para alerta prioritaria');

-- =====================================================================
-- 12. ESTACIÓN DE EJEMPLO
-- =====================================================================
INSERT INTO estaciones (nombre, codigo, api_key, id_zona, latitud, longitud, descripcion) VALUES
('ForestGuard-01', 'FG001', 'fg_demo_key_change_me', 2, -33.4569, -70.6483, 'Estación de ejemplo');

INSERT INTO sensores (id_estacion, id_tipo_sensor, pin_gpio, canal, descripcion) VALUES
(1, 1, 13, 'DATA', 'DHT22'),
(1, 2, 35, 'AO', 'MQ-2 analógico'),
(1, 2, 33, 'DO', 'MQ-2 digital'),
(1, 3, 26, 'ROJO', 'LED RGB rojo'),
(1, 3, 27, 'VERDE', 'LED RGB verde'),
(1, 3, 25, 'AZUL', 'LED RGB azul');

-- Agregar estados de estación PENDIENTE y RECHAZADA
INSERT INTO estados_estacion (nombre, descripcion) VALUES
('PENDIENTE', 'Esperando aprobación'),
('RECHAZADA', 'Registro denegado');