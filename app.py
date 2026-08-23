"""
ForestGuard - Sistema de monitoreo ambiental
Flujo: ESP32 → API Flask → MySQL → Dashboard
"""
from functools import wraps
from flask import Flask, render_template, request, redirect, url_for, session, jsonify, flash, abort
from flask_session import Session
from werkzeug.security import generate_password_hash, check_password_hash
import database as db
from config import Config
import json
import secrets
import datetime
import re
from math import radians, sin, cos, sqrt, atan2

app = Flask(__name__)
app.config.from_object(Config)
Session(app)

# =====================================================================
# CONSTANTES
# =====================================================================
ESTADO_ESTACION = {'ONLINE': 1, 'OFFLINE': 2, 'ERROR': 3, 'SIN_DATOS': 4, 'PENDIENTE': 5, 'RECHAZADA': 6}
ESTADO_SENSOR = {'ACTIVO': 1, 'ERROR': 2, 'SIN_DATOS': 3}
ESTADO_ALERTA = {'ACTIVA': 1, 'ATENDIDA': 2, 'RESUELTA': 3, 'FALSA': 4}
ROLES_GESTION_ALERTAS = ('ADMINISTRADOR', 'BOMBERO', 'OPERADOR')
ROLES_ADMIN = ('ADMINISTRADOR',)

# =====================================================================
# DECORADORES
# =====================================================================
def login_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if 'usuario_id' not in session:
            flash('Inicia sesión para acceder', 'warning')
            return redirect(url_for('login'))
        return f(*args, **kwargs)
    return decorated

def roles_required(*roles):
    def decorator(f):
        @wraps(f)
        def decorated(*args, **kwargs):
            if session.get('tipo') not in roles:
                flash('No tienes permisos', 'danger')
                return redirect(url_for('dashboard'))
            return f(*args, **kwargs)
        return decorated
    return decorator

admin_required = roles_required('ADMINISTRADOR')
gestion_alertas_required = roles_required(*ROLES_GESTION_ALERTAS)

# =====================================================================
# CONTEXTO GLOBAL
# =====================================================================
@app.context_processor
def inject_alertas_count():
    if 'usuario_id' in session:
        count = db.get_one("SELECT COUNT(*) as count FROM alertas WHERE id_estado_alerta = %s", (ESTADO_ALERTA['ACTIVA'],))
        return {'alertas_activas_count': count['count'] if count else 0}
    return {'alertas_activas_count': 0}

# =====================================================================
# INICIALIZACIÓN
# =====================================================================
def init_db():
    """Crea usuario admin, usuarios de prueba y ajusta tablas."""
    # Admin
    admin = db.get_one("SELECT id_usuario FROM usuarios WHERE nombre_usuario = %s", ('admin',))
    if not admin:
        hash_pass = generate_password_hash('admin123')
        db.execute_query("""
            INSERT INTO usuarios (nombre_usuario, contrasena_hash, email, nombre_completo, id_tipo_usuario)
            VALUES (%s, %s, %s, %s, %s)
        """, ('admin', hash_pass, 'admin@forestguard.com', 'Administrador', 1))
        print("Usuario admin creado (admin / admin123)")

    # Usuarios de prueba
    prueba = [
        ('bombero', 'bombero123', 'bombero@forestguard.com', 'Bombero Test', 2),
        ('operador', 'operador123', 'operador@forestguard.com', 'Operador Test', 3),
        ('observador', 'observador123', 'observador@forestguard.com', 'Observador Test', 4)
    ]
    for user, pwd, email, nombre, tipo in prueba:
        if not db.get_one("SELECT id_usuario FROM usuarios WHERE nombre_usuario = %s", (user,)):
            hash_pass = generate_password_hash(pwd)
            db.execute_query("""
                INSERT INTO usuarios (nombre_usuario, contrasena_hash, email, nombre_completo, id_tipo_usuario)
                VALUES (%s, %s, %s, %s, %s)
            """, (user, hash_pass, email, nombre, tipo))
            print(f"Usuario {user} creado ({user} / {pwd})")

    # Asegurar que latitud y longitud acepten NULL
    try:
        db.execute_query("ALTER TABLE estaciones MODIFY latitud DECIMAL(10,8) NULL")
        db.execute_query("ALTER TABLE estaciones MODIFY longitud DECIMAL(11,8) NULL")
        print("✅ Tabla estaciones actualizada: latitud y longitud aceptan NULL")
    except Exception as e:
        print(f"⚠️ No se pudo modificar la tabla (quizás ya está): {e}")

    # Verificar que exista la configuración de radio_alerta
    if not db.get_one("SELECT clave FROM configuracion WHERE clave = 'radio_alerta'"):
        db.execute_query("INSERT INTO configuracion (clave, valor, descripcion) VALUES ('radio_alerta', '5', 'Radio en kilómetros para alerta prioritaria')")

    # Agregar estados de estación pendiente y rechazada si no existen
    for nombre, desc in [('PENDIENTE', 'Esperando aprobación'), ('RECHAZADA', 'Registro denegado')]:
        if not db.get_one("SELECT id_estado_estacion FROM estados_estacion WHERE nombre = %s", (nombre,)):
            db.execute_query("INSERT INTO estados_estacion (nombre, descripcion) VALUES (%s, %s)", (nombre, desc))
            print(f"✅ Estado '{nombre}' agregado.")

# =====================================================================
# HELPERS DE NEGOCIO
# =====================================================================
def obtener_umbrales(id_estacion=None):
    umbrales = {row['clave']: float(row['valor']) for row in db.get_all("SELECT clave, valor FROM configuracion")}
    if id_estacion:
        override = db.get_one("SELECT * FROM configuracion_estaciones WHERE id_estacion = %s", (id_estacion,))
        if override:
            for clave in ('temp_alta', 'temp_critica', 'hum_baja', 'hum_critica', 'humo_bajo', 'humo_medio', 'humo_alto', 'offline_timeout'):
                if override.get(clave) is not None:
                    umbrales[clave] = float(override[clave])
    return umbrales

def clasificar_humo(cambio_ao, umbrales):
    if cambio_ao is None:
        return None, None
    if cambio_ao >= umbrales['humo_alto']:
        nivel = 3
    elif cambio_ao >= umbrales['humo_medio']:
        nivel = 2
    elif cambio_ao >= umbrales['humo_bajo']:
        nivel = 1
    else:
        nivel = 0
    fila = db.get_one("SELECT id_tipo_humo FROM tipos_humo WHERE nivel = %s", (nivel,))
    return nivel, fila['id_tipo_humo'] if fila else None

def evaluar_riesgo(temp, hum, nivel_humo, umbrales):
    temp_alta = temp is not None and temp >= umbrales['temp_alta']
    temp_critica = temp is not None and temp >= umbrales['temp_critica']
    hum_baja = hum is not None and hum <= umbrales['hum_baja']
    hum_critica = hum is not None and hum <= umbrales['hum_critica']
    humo_bajo = nivel_humo == 1
    humo_medio = nivel_humo == 2
    humo_alto = nivel_humo == 3

    condiciones = []
    if temp_alta: condiciones.append('TEMPERATURA_ALTA')
    if temp_critica: condiciones.append('TEMPERATURA_CRITICA')
    if hum_baja: condiciones.append('HUMEDAD_BAJA')
    if hum_critica: condiciones.append('HUMEDAD_CRITICA')
    if humo_bajo: condiciones.append('HUMO_BAJO')
    if humo_medio: condiciones.append('HUMO_MEDIO')
    if humo_alto: condiciones.append('HUMO_ALTO')

    if (humo_alto or (humo_medio and temp_alta) or (humo_medio and hum_baja) or
        (humo_bajo and temp_critica) or (humo_bajo and hum_critica) or
        (temp_critica and hum_baja)):
        return 'ALERTA', 'POSIBLE_INCENDIO', condiciones
    if (humo_medio or (humo_bajo and temp_alta) or (humo_bajo and hum_baja) or
        (temp_alta and hum_baja)):
        return 'RIESGO_ALTO', 'RIESGO_ALTO', condiciones
    if temp_alta or hum_baja or humo_bajo:
        return 'RIESGO_MODERADO', 'RIESGO_MODERADO', condiciones
    return 'NORMAL', None, condiciones

def registrar_evento(tipo, id_estacion=None, id_usuario=None, mensaje='', datos=None):
    fila = db.get_one("SELECT id_tipo_evento FROM tipos_evento WHERE nombre = %s", (tipo,))
    if fila:
        db.execute_query(
            "INSERT INTO eventos (id_tipo_evento, id_estacion, id_usuario, mensaje, datos_extra) VALUES (%s, %s, %s, %s, %s)",
            (fila['id_tipo_evento'], id_estacion, id_usuario, mensaje, json.dumps(datos) if datos else None)
        )

def cambiar_estado_estacion(id_estacion, nuevo_estado):
    actual = db.get_one("SELECT id_estado_estacion FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    nuevo_id = ESTADO_ESTACION[nuevo_estado]
    if not actual or actual['id_estado_estacion'] == nuevo_id:
        return False
    db.execute_query(
        "INSERT INTO historial_estados_estacion (id_estacion, id_estado_anterior, id_estado_nuevo) VALUES (%s, %s, %s)",
        (id_estacion, actual['id_estado_estacion'], nuevo_id)
    )
    db.execute_query("UPDATE estaciones SET id_estado_estacion = %s WHERE id_estacion = %s", (nuevo_id, id_estacion))
    return True

def crear_o_actualizar_alerta(id_estacion, tipo_alerta, nivel_riesgo, titulo, descripcion, id_medicion=None, id_evaluacion=None):
    tipo = db.get_one("SELECT id_tipo_alerta FROM tipos_alerta WHERE nombre = %s", (tipo_alerta,))
    nivel = db.get_one("SELECT id_nivel_riesgo FROM niveles_riesgo WHERE nombre = %s", (nivel_riesgo,))
    if not tipo or not nivel:
        return None
    existente = db.get_one(
        "SELECT id_alerta FROM alertas WHERE id_estacion = %s AND id_tipo_alerta = %s AND id_estado_alerta = %s",
        (id_estacion, tipo['id_tipo_alerta'], ESTADO_ALERTA['ACTIVA'])
    )
    if existente:
        db.execute_query(
            "UPDATE alertas SET descripcion = %s, id_nivel_riesgo = %s, id_medicion = %s, id_evaluacion = %s WHERE id_alerta = %s",
            (descripcion, nivel['id_nivel_riesgo'], id_medicion, id_evaluacion, existente['id_alerta'])
        )
        return existente['id_alerta']
    nueva_id = db.execute_query(
        "INSERT INTO alertas (id_estacion, id_medicion, id_evaluacion, id_tipo_alerta, id_nivel_riesgo, id_estado_alerta, titulo, descripcion) VALUES (%s, %s, %s, %s, %s, %s, %s, %s)",
        (id_estacion, id_medicion, id_evaluacion, tipo['id_tipo_alerta'], nivel['id_nivel_riesgo'], ESTADO_ALERTA['ACTIVA'], titulo, descripcion)
    )
    registrar_evento('ALERTA_GENERADA', id_estacion=id_estacion, mensaje=descripcion)
    return nueva_id

def verificar_estaciones_offline():
    estaciones = db.get_all(
        "SELECT id_estacion, id_estado_estacion, ultima_conexion, TIMESTAMPDIFF(SECOND, ultima_conexion, NOW()) as segundos FROM estaciones WHERE activa = 1 AND id_estado_estacion IN (%s, %s)",
        (ESTADO_ESTACION['ONLINE'], ESTADO_ESTACION['ERROR'])
    )
    for est in estaciones:
        if not est['ultima_conexion']:
            continue
        umbrales = obtener_umbrales(est['id_estacion'])
        timeout = umbrales.get('offline_timeout', 90)
        if est['segundos'] and est['segundos'] > timeout:
            if cambiar_estado_estacion(est['id_estacion'], 'OFFLINE'):
                registrar_evento('ESTACION_DESCONECTADA', id_estacion=est['id_estacion'], mensaje='Timeout sin datos')
                crear_o_actualizar_alerta(
                    est['id_estacion'], 'ESTACION_OFFLINE', 'RIESGO_MODERADO',
                    'Estación desconectada', 'La estación dejó de reportar datos'
                )

def obtener_estacion_con_ultima_medicion(id_estacion):
    return db.get_one("""
        SELECT e.*, z.nombre as zona, es.nombre as estado_nombre,
            (SELECT temperatura FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_temp,
            (SELECT humedad FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_humedad,
            (SELECT th.nombre FROM mediciones m JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo WHERE m.id_estacion = e.id_estacion ORDER BY m.fecha_hora DESC LIMIT 1) as ult_humo,
            (SELECT cambio_ao FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_cambio_ao,
            (SELECT mq2_ao FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_mq2_ao,
            (SELECT mq2_base FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_mq2_base,
            (SELECT mq2_do FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_mq2_do,
            TIMESTAMPDIFF(SECOND, e.ultima_conexion, NOW()) as segundos_sin_datos
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        WHERE e.id_estacion = %s
    """, (id_estacion,))

def generar_codigo_unico():
    """Genera un código hexadecimal de 8 caracteres que no exista en la BD."""
    while True:
        codigo = secrets.token_hex(4).upper()
        if not db.get_one("SELECT id_estacion FROM estaciones WHERE codigo = %s", (codigo,)):
            return codigo

def calcular_distancia(lat1, lon1, lat2, lon2):
    R = 6371.0
    lat1, lon1, lat2, lon2 = map(radians, [lat1, lon1, lat2, lon2])
    dlon = lon2 - lon1
    dlat = lat2 - lat1
    a = sin(dlat/2)**2 + cos(lat1) * cos(lat2) * sin(dlon/2)**2
    c = 2 * atan2(sqrt(a), sqrt(1-a))
    return R * c

def mapear_nivel_humo_a_id(nivel):
    """Convierte nivel 0-3 a id_tipo_humo según la tabla tipos_humo."""
    if nivel is None:
        return None
    fila = db.get_one("SELECT id_tipo_humo FROM tipos_humo WHERE nivel = %s", (nivel,))
    return fila['id_tipo_humo'] if fila else None

# =====================================================================
# RUTAS PÚBLICAS
# =====================================================================
@app.route('/')
def index():
    return redirect(url_for('dashboard') if 'usuario_id' in session else url_for('login'))

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        usuario = request.form.get('usuario', '').strip()
        contrasena = request.form.get('contrasena', '')
        user = db.get_one("""
            SELECT u.*, t.nombre as tipo_nombre, eu.nombre as estado_nombre
            FROM usuarios u
            JOIN tipos_usuario t ON u.id_tipo_usuario = t.id_tipo_usuario
            JOIN estados_usuario eu ON u.id_estado_usuario = eu.id_estado_usuario
            WHERE u.nombre_usuario = %s
        """, (usuario,))
        exitoso = bool(user and check_password_hash(user['contrasena_hash'], contrasena) and user['estado_nombre'] == 'ACTIVO')
        db.execute_query(
            "INSERT INTO logs_acceso (id_usuario, nombre_usuario_intento, exitoso, ip_origen) VALUES (%s, %s, %s, %s)",
            (user['id_usuario'] if user else None, usuario, exitoso, request.remote_addr)
        )
        if not user or not exitoso:
            flash('Credenciales inválidas o usuario inactivo', 'danger')
        else:
            session['usuario_id'] = user['id_usuario']
            session['usuario'] = user['nombre_usuario']
            session['tipo'] = user['tipo_nombre']
            session['nombre_completo'] = user['nombre_completo']
            db.execute_query("UPDATE usuarios SET ultimo_acceso = NOW() WHERE id_usuario = %s", (user['id_usuario'],))
            registrar_evento('LOGIN', id_usuario=user['id_usuario'], mensaje=f"Inicio de sesión de {usuario}")
            return redirect(url_for('dashboard'))
    return render_template('login.html')

@app.route('/logout')
def logout():
    if 'usuario_id' in session:
        registrar_evento('LOGOUT', id_usuario=session['usuario_id'], mensaje=f"Cierre de sesión de {session.get('usuario')}")
    session.clear()
    flash('Sesión cerrada', 'info')
    return redirect(url_for('login'))

# =====================================================================
# DASHBOARD Y ZONAS
# =====================================================================
@app.route('/dashboard')
@login_required
def dashboard():
    verificar_estaciones_offline()
    ultima_medicion = db.get_one("""
        SELECT m.*, e.nombre as estacion, th.nombre as humo_nombre
        FROM mediciones m
        JOIN estaciones e ON m.id_estacion = e.id_estacion
        LEFT JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo
        ORDER BY m.fecha_hora DESC LIMIT 1
    """)
    riesgo_actual = db.get_one("""
        SELECT nr.nombre, nr.color_hex
        FROM alertas a
        JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
        WHERE a.id_estado_alerta = 1
        ORDER BY nr.severidad DESC LIMIT 1
    """)
    rango = request.args.get('rango', '24h')
    if rango == '1h':
        limite = "INTERVAL 1 HOUR"
    elif rango == '6h':
        limite = "INTERVAL 6 HOUR"
    elif rango == '7d':
        limite = "INTERVAL 7 DAY"
    else:
        limite = "INTERVAL 24 HOUR"
        rango = '24h'
    historico = db.get_all(f"""
        SELECT temperatura, humedad, cambio_ao as humo, fecha_hora
        FROM mediciones
        WHERE fecha_hora > NOW() - {limite}
        ORDER BY fecha_hora ASC
    """)
    estaciones = db.get_all("""
        SELECT e.*, z.nombre as zona, es.nombre as estado_nombre,
            (SELECT temperatura FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_temp,
            (SELECT humedad FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_humedad,
            (SELECT th.nombre FROM mediciones m JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo WHERE m.id_estacion = e.id_estacion ORDER BY m.fecha_hora DESC LIMIT 1) as ult_humo
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        WHERE e.activa = 1
        ORDER BY e.nombre
    """)
    alertas_activas = db.get_all("""
        SELECT a.*, e.nombre as estacion, ta.nombre as tipo, nr.nombre as nivel, nr.color_hex
        FROM alertas a
        JOIN estaciones e ON a.id_estacion = e.id_estacion
        JOIN tipos_alerta ta ON a.id_tipo_alerta = ta.id_tipo_alerta
        JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
        WHERE a.id_estado_alerta = %s
        ORDER BY nr.severidad DESC, a.fecha_creacion DESC
    """, (ESTADO_ALERTA['ACTIVA'],))
    return render_template('dashboard.html',
        estaciones=estaciones,
        total=len(estaciones),
        online=sum(1 for e in estaciones if e['id_estado_estacion'] == ESTADO_ESTACION['ONLINE']),
        offline=sum(1 for e in estaciones if e['id_estado_estacion'] == ESTADO_ESTACION['OFFLINE']),
        alertas_activas=alertas_activas,
        riesgo_alto=sum(1 for a in alertas_activas if a['nivel'] == 'RIESGO_ALTO'),
        posibles_incendios=sum(1 for a in alertas_activas if a['nivel'] == 'ALERTA'),
        ultima_medicion=ultima_medicion,
        riesgo_actual=riesgo_actual,
        historico=historico,
        rango=rango,
        now=datetime.datetime.now()
    )

@app.route('/zonas')
@login_required
def zonas():
    verificar_estaciones_offline()
    zonas = db.get_all("SELECT * FROM zonas ORDER BY nombre")
    estaciones = db.get_all("""
        SELECT e.*, z.nombre as zona, es.nombre as estado_nombre,
            (SELECT temperatura FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_temp,
            (SELECT humedad FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_humedad,
            (SELECT th.nombre FROM mediciones m JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo WHERE m.id_estacion = e.id_estacion ORDER BY m.fecha_hora DESC LIMIT 1) as ult_humo
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        WHERE e.activa = 1
        ORDER BY z.nombre, e.nombre
    """)
    return render_template('zonas.html',
        page="zonas",
        title="Zonas",
        category="MONITOREO GEOGRÁFICO",
        zonas=zonas,
        estaciones=estaciones,
        now=datetime.datetime.now()
    )

# =====================================================================
# GESTIÓN DE ESTACIONES (CRUD) + PENDIENTES
# =====================================================================
@app.route('/estaciones')
@login_required
@admin_required
def listar_estaciones():
    estaciones = db.get_all("""
        SELECT e.*, z.nombre as zona, es.nombre as estado_nombre
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        WHERE e.activa = 1
        ORDER BY e.nombre
    """)
    pendientes_count = db.get_one("SELECT COUNT(*) as count FROM estaciones WHERE id_estado_estacion = %s AND activa = 0", (ESTADO_ESTACION['PENDIENTE'],))
    return render_template('estaciones.html',
        page="estaciones",
        title="Estaciones",
        category="ADMINISTRACIÓN",
        estaciones=estaciones,
        pendientes_count=pendientes_count['count'] if pendientes_count else 0,
        now=datetime.datetime.now()
    )

@app.route('/estaciones/pendientes')
@login_required
@admin_required
def pendientes():
    pendientes = db.get_all("""
        SELECT e.*, z.nombre as zona
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        WHERE e.id_estado_estacion = %s AND e.activa = 0
        ORDER BY e.fecha_registro DESC
    """, (ESTADO_ESTACION['PENDIENTE'],))
    return render_template('pendientes.html',
        page="estaciones",
        title="Estaciones pendientes",
        category="ADMINISTRACIÓN",
        pendientes=pendientes,
        now=datetime.datetime.now()
    )

@app.route('/estaciones/<int:id_estacion>/aprobar', methods=['POST'])
@login_required
@admin_required
def aprobar_estacion(id_estacion):
    est = db.get_one("SELECT * FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if not est:
        flash('Estación no encontrada', 'danger')
        return redirect(url_for('pendientes'))
    # Cambiar a ONLINE y activa=1
    nueva_api_key = secrets.token_hex(16)
    db.execute_query("""
        UPDATE estaciones SET activa = 1, id_estado_estacion = %s, api_key = %s
        WHERE id_estacion = %s
    """, (ESTADO_ESTACION['ONLINE'], nueva_api_key, id_estacion))
    registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {est['nombre']} aprobada")
    flash('Estación aprobada correctamente', 'success')
    return redirect(url_for('pendientes'))

@app.route('/estaciones/<int:id_estacion>/rechazar', methods=['POST'])
@login_required
@admin_required
def rechazar_estacion(id_estacion):
    est = db.get_one("SELECT nombre FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if est:
        db.execute_query("UPDATE estaciones SET id_estado_estacion = %s, activa = 0 WHERE id_estacion = %s",
                         (ESTADO_ESTACION['RECHAZADA'], id_estacion))
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {est['nombre']} rechazada")
        flash('Estación rechazada', 'warning')
    else:
        flash('Estación no encontrada', 'danger')
    return redirect(url_for('pendientes'))

@app.route('/estaciones/<int:id_estacion>/desconectar', methods=['POST'])
@login_required
@admin_required
def desconectar_estacion(id_estacion):
    est = db.get_one("SELECT nombre FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if est:
        db.execute_query("UPDATE estaciones SET activa = 0, id_estado_estacion = %s WHERE id_estacion = %s",
                         (ESTADO_ESTACION['OFFLINE'], id_estacion))
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {est['nombre']} desconectada")
        flash('Estación desconectada', 'info')
    else:
        flash('Estación no encontrada', 'danger')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/<int:id_estacion>/reactivar', methods=['POST'])
@login_required
@admin_required
def reactivar_estacion(id_estacion):
    est = db.get_one("SELECT nombre FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if est:
        db.execute_query("UPDATE estaciones SET activa = 1, id_estado_estacion = %s WHERE id_estacion = %s",
                         (ESTADO_ESTACION['ONLINE'], id_estacion))
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {est['nombre']} reactivada")
        flash('Estación reactivada', 'success')
    else:
        flash('Estación no encontrada', 'danger')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/nueva', methods=['GET', 'POST'])
@login_required
@admin_required
def nueva_estacion():
    if request.method == 'POST':
        nombre = request.form.get('nombre')
        codigo = request.form.get('codigo', '').strip()
        latitud = request.form.get('latitud')
        longitud = request.form.get('longitud')
        id_zona = request.form.get('id_zona')
        descripcion = request.form.get('descripcion')
        if not nombre:
            flash('El nombre es obligatorio', 'danger')
            return redirect(url_for('nueva_estacion'))
        if codigo.upper() == "AUTO":
            codigo = generar_codigo_unico()
        else:
            if db.get_one("SELECT id_estacion FROM estaciones WHERE codigo = %s", (codigo,)):
                flash('El código ya está en uso. Elige otro o escribe "AUTO" para generar uno automático.', 'danger')
                return redirect(url_for('nueva_estacion'))
        api_key = secrets.token_hex(16)
        try:
            if not latitud:
                latitud = 0.0
            if not longitud:
                longitud = 0.0
            db.execute_query("""
                INSERT INTO estaciones (nombre, codigo, api_key, id_zona, latitud, longitud, descripcion, activa, id_estado_estacion)
                VALUES (%s, %s, %s, %s, %s, %s, %s, 1, %s)
            """, (nombre, codigo, api_key, id_zona or None, latitud, longitud, descripcion, ESTADO_ESTACION['ONLINE']))
            flash(f'✅ Estación "{nombre}" creada exitosamente.\nCódigo: {codigo}\nAPI Key: {api_key}', 'success')
            registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {nombre} creada con código {codigo}")
            return redirect(url_for('listar_estaciones'))
        except Exception as e:
            flash(f'Error al crear la estación: {e}', 'danger')
    zonas = db.get_all("SELECT * FROM zonas")
    return render_template('estacion_form.html',
        page="estaciones",
        title="Nueva Estación",
        category="ADMINISTRACIÓN",
        estacion=None,
        zonas=zonas,
        now=datetime.datetime.now()
    )

@app.route('/estaciones/<int:id_estacion>/editar', methods=['GET', 'POST'])
@login_required
@admin_required
def editar_estacion(id_estacion):
    estacion = db.get_one("SELECT * FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if not estacion:
        flash('Estación no encontrada', 'danger')
        return redirect(url_for('listar_estaciones'))
    if request.method == 'POST':
        nombre = request.form.get('nombre')
        codigo = request.form.get('codigo')
        latitud = request.form.get('latitud')
        longitud = request.form.get('longitud')
        id_zona = request.form.get('id_zona')
        descripcion = request.form.get('descripcion')
        activa = 'activa' in request.form
        try:
            db.execute_query("""
                UPDATE estaciones SET nombre=%s, codigo=%s, id_zona=%s, latitud=%s, longitud=%s, descripcion=%s, activa=%s
                WHERE id_estacion=%s
            """, (nombre, codigo, id_zona or None, latitud or None, longitud or None, descripcion, activa, id_estacion))
            flash('Estación actualizada', 'success')
            registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {nombre} editada")
            return redirect(url_for('listar_estaciones'))
        except Exception as e:
            flash(f'Error: {e}', 'danger')
    zonas = db.get_all("SELECT * FROM zonas")
    return render_template('estacion_form.html',
        page="estaciones",
        title="Editar Estación",
        category="ADMINISTRACIÓN",
        estacion=estacion,
        zonas=zonas,
        now=datetime.datetime.now()
    )

@app.route('/estaciones/<int:id_estacion>/eliminar', methods=['POST'])
@login_required
@admin_required
def eliminar_estacion(id_estacion):
    estacion = db.get_one("SELECT nombre FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if estacion:
        db.execute_query("UPDATE estaciones SET activa = 0 WHERE id_estacion = %s", (id_estacion,))
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Estación {estacion['nombre']} eliminada (desactivada)")
        flash('Estación desactivada. Ya no aparecerá en el sistema.', 'success')
    else:
        flash('Estación no encontrada', 'danger')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/<int:id_estacion>/activar', methods=['POST'])
@login_required
@admin_required
def activar_estacion(id_estacion):
    db.execute_query("UPDATE estaciones SET activa = 1 WHERE id_estacion = %s", (id_estacion,))
    flash('Estación activada', 'success')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/<int:id_estacion>/reconectar', methods=['POST'])
@login_required
@admin_required
def reconectar_estacion(id_estacion):
    estacion = db.get_one("SELECT ip FROM estaciones WHERE id_estacion = %s", (id_estacion,))
    if not estacion or not estacion['ip']:
        flash('No se puede reconectar: IP no disponible', 'danger')
        return redirect(url_for('listar_estaciones'))
    cambiar_estado_estacion(id_estacion, 'ONLINE')
    flash('Reconexión exitosa (estado forzado a ONLINE)', 'success')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/<int:id_estacion>/regenerar-api-key', methods=['POST'])
@login_required
@admin_required
def regenerar_api_key(id_estacion):
    nueva = secrets.token_hex(16)
    db.execute_query("UPDATE estaciones SET api_key = %s WHERE id_estacion = %s", (nueva, id_estacion))
    flash('API key regenerada', 'success')
    return redirect(url_for('listar_estaciones'))

@app.route('/estaciones/<int:id_estacion>')
@login_required
def estacion_detalle(id_estacion):
    verificar_estaciones_offline()
    estacion = obtener_estacion_con_ultima_medicion(id_estacion)
    if not estacion or not estacion['activa']:
        flash('Estación no encontrada o desactivada', 'danger')
        return redirect(url_for('dashboard'))
    medicion = db.get_one("SELECT * FROM mediciones WHERE id_estacion = %s ORDER BY fecha_hora DESC LIMIT 1", (id_estacion,))
    sensores = db.get_all("""
        SELECT s.*, ts.nombre as tipo, es.nombre as estado
        FROM sensores s
        JOIN tipos_sensor ts ON s.id_tipo_sensor = ts.id_tipo_sensor
        JOIN estados_sensor es ON s.id_estado_sensor = es.id_estado_sensor
        WHERE s.id_estacion = %s
    """, (id_estacion,))
    historico = db.get_all("""
        SELECT temperatura, humedad, cambio_ao, fecha_hora
        FROM mediciones WHERE id_estacion = %s
        ORDER BY fecha_hora DESC LIMIT 50
    """, (id_estacion,))
    historico.reverse()
    alertas_estacion = db.get_all("""
        SELECT a.*, ta.nombre as tipo, ea.nombre as estado, nr.nombre as nivel
        FROM alertas a
        JOIN tipos_alerta ta ON a.id_tipo_alerta = ta.id_tipo_alerta
        JOIN estados_alerta ea ON a.id_estado_alerta = ea.id_estado_alerta
        JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
        WHERE a.id_estacion = %s ORDER BY a.fecha_creacion DESC LIMIT 20
    """, (id_estacion,))
    componentes = {}
    for s in sensores:
        tipo = s['tipo']
        if tipo not in componentes:
            componentes[tipo] = []
        componentes[tipo].append(s)
    return render_template('estacion_detalle.html',
        page="estacion",
        title=estacion['nombre'],
        category="MONITOREO",
        estacion=estacion,
        medicion=medicion,
        sensores=sensores,
        componentes=componentes,
        historico=historico,
        alertas=alertas_estacion,
        now=datetime.datetime.now()
    )

# =====================================================================
# ALERTAS
# =====================================================================
@app.route('/alertas')
@login_required
def alertas():
    verificar_estaciones_offline()
    filtro = request.args.get('filtro', 'activas')
    estacion_id = request.args.get('estacion', '')
    base = """
        SELECT a.*, e.nombre as estacion, ta.nombre as tipo, ea.nombre as estado,
               nr.nombre as nivel, nr.color_hex, u.nombre_usuario as atendio_por
        FROM alertas a
        JOIN estaciones e ON a.id_estacion = e.id_estacion
        JOIN tipos_alerta ta ON a.id_tipo_alerta = ta.id_tipo_alerta
        JOIN estados_alerta ea ON a.id_estado_alerta = ea.id_estado_alerta
        JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
        LEFT JOIN usuarios u ON a.id_usuario_atendio = u.id_usuario
        WHERE e.activa = 1
    """
    where = []
    params = []
    if filtro == 'activas':
        where.append("a.id_estado_alerta = %s")
        params.append(ESTADO_ALERTA['ACTIVA'])
    elif filtro == 'incendio':
        where.append("nr.nombre = 'ALERTA'")
    elif filtro == 'resueltas':
        where.append("a.id_estado_alerta IN (%s, %s)")
        params.extend([ESTADO_ALERTA['RESUELTA'], ESTADO_ALERTA['FALSA']])
    if estacion_id:
        where.append("a.id_estacion = %s")
        params.append(estacion_id)
    if where:
        base += " AND " + " AND ".join(where)
    base += " ORDER BY nr.severidad DESC, a.fecha_creacion DESC LIMIT 300"
    alerts = db.get_all(base, params)
    estaciones = db.get_all("SELECT id_estacion, nombre FROM estaciones WHERE activa=1 ORDER BY nombre")
    return render_template('alertas.html',
        page="alertas",
        title="Alertas",
        category="SEGURIDAD",
        alertas=alerts,
        filtro=filtro,
        estaciones=estaciones,
        estacion_seleccionada=estacion_id,
        puede_gestionar=session.get('tipo') in ROLES_GESTION_ALERTAS,
        now=datetime.datetime.now()
    )

def _cambiar_estado_alerta(id_alerta, nuevo_estado, tipo_evento, es_falsa=False):
    alerta = db.get_one("SELECT * FROM alertas WHERE id_alerta = %s", (id_alerta,))
    if not alerta:
        return False
    nuevo_id = ESTADO_ALERTA[nuevo_estado]
    campos_fecha = ''
    if nuevo_estado == 'ATENDIDA':
        campos_fecha = ', fecha_atencion = NOW()'
    elif nuevo_estado in ('RESUELTA', 'FALSA'):
        campos_fecha = ', fecha_resolucion = NOW()'
    db.execute_query(
        f"UPDATE alertas SET id_estado_alerta = %s, id_usuario_atendio = %s, es_falsa = %s{campos_fecha} WHERE id_alerta = %s",
        (nuevo_id, session['usuario_id'], 1 if es_falsa else alerta['es_falsa'], id_alerta)
    )
    db.execute_query(
        "INSERT INTO historial_alertas (id_alerta, id_estado_anterior, id_estado_nuevo, id_usuario) VALUES (%s, %s, %s, %s)",
        (id_alerta, alerta['id_estado_alerta'], nuevo_id, session['usuario_id'])
    )
    registrar_evento(tipo_evento, id_estacion=alerta['id_estacion'], id_usuario=session['usuario_id'], mensaje=f"Alerta #{id_alerta} -> {nuevo_estado}")
    return True

@app.route('/alertas/<int:id_alerta>/atender', methods=['POST'])
@login_required
@gestion_alertas_required
def atender_alerta(id_alerta):
    _cambiar_estado_alerta(id_alerta, 'ATENDIDA', 'ALERTA_ATENDIDA')
    flash('Alerta marcada como atendida', 'success')
    return redirect(request.referrer or url_for('alertas'))

@app.route('/alertas/<int:id_alerta>/resolver', methods=['POST'])
@login_required
@gestion_alertas_required
def resolver_alerta(id_alerta):
    _cambiar_estado_alerta(id_alerta, 'RESUELTA', 'ALERTA_RESUELTA')
    flash('Alerta resuelta', 'success')
    return redirect(request.referrer or url_for('alertas'))

@app.route('/alertas/<int:id_alerta>/falsa', methods=['POST'])
@login_required
@gestion_alertas_required
def marcar_falsa_alerta(id_alerta):
    _cambiar_estado_alerta(id_alerta, 'FALSA', 'ALERTA_RESUELTA', es_falsa=True)
    flash('Alerta marcada como falsa', 'success')
    return redirect(request.referrer or url_for('alertas'))

# =====================================================================
# HISTORIAL Y SENSORES
# =====================================================================
@app.route('/historial')
@login_required
def historial():
    estacion_id = request.args.get('estacion', '')
    riesgo = request.args.get('riesgo', '')
    fecha_inicio = request.args.get('fecha_inicio', '')
    fecha_fin = request.args.get('fecha_fin', '')
    query = """
        SELECT m.*, e.nombre as estacion, th.nombre as humo_nombre,
               nr.nombre as nivel_riesgo
        FROM mediciones m
        JOIN estaciones e ON m.id_estacion = e.id_estacion
        LEFT JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo
        LEFT JOIN evaluaciones_riesgo er ON m.id_medicion = er.id_medicion
        LEFT JOIN niveles_riesgo nr ON er.id_nivel_riesgo = nr.id_nivel_riesgo
        WHERE e.activa = 1
    """
    params = []
    if estacion_id:
        query += " AND m.id_estacion = %s"
        params.append(estacion_id)
    if riesgo:
        query += " AND nr.nombre = %s"
        params.append(riesgo)
    if fecha_inicio:
        query += " AND DATE(m.fecha_hora) >= %s"
        params.append(fecha_inicio)
    if fecha_fin:
        query += " AND DATE(m.fecha_hora) <= %s"
        params.append(fecha_fin)
    query += " ORDER BY m.fecha_hora DESC LIMIT 500"
    mediciones = db.get_all(query, params)
    estaciones = db.get_all("SELECT id_estacion, nombre FROM estaciones WHERE activa = 1 ORDER BY nombre")
    niveles = db.get_all("SELECT nombre FROM niveles_riesgo")
    return render_template('historial.html',
        page="historial",
        title="Historial",
        category="DATOS",
        mediciones=mediciones,
        estaciones=estaciones,
        estacion_seleccionada=estacion_id,
        riesgo_seleccionado=riesgo,
        niveles=niveles,
        now=datetime.datetime.now()
    )

@app.route('/sensores')
@login_required
def sensores():
    estaciones = db.get_all("""
        SELECT e.id_estacion, e.nombre, e.ultima_conexion, z.nombre as zona,
               es.nombre as estado_nombre,
               (SELECT temperatura FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_temp,
               (SELECT humedad FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1) as ult_humedad,
               TIMESTAMPDIFF(SECOND, e.ultima_conexion, NOW()) as segundos_desde_ultima
        FROM estaciones e
        LEFT JOIN zonas z ON e.id_zona = z.id_zona
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        WHERE e.activa = 1
        ORDER BY e.nombre
    """)
    for est in estaciones:
        dht = db.get_one("""
            SELECT es.nombre as estado FROM sensores s
            JOIN estados_sensor es ON s.id_estado_sensor = es.id_estado_sensor
            WHERE s.id_estacion = %s AND s.id_tipo_sensor = 1
        """, (est['id_estacion'],))
        est['dht_estado'] = dht['estado'] if dht else 'SIN_DATOS'
        mq2 = db.get_one("""
            SELECT es.nombre as estado FROM sensores s
            JOIN estados_sensor es ON s.id_estado_sensor = es.id_estado_sensor
            WHERE s.id_estacion = %s AND s.id_tipo_sensor = 2
        """, (est['id_estacion'],))
        est['mq2_estado'] = mq2['estado'] if mq2 else 'SIN_DATOS'
    return render_template('sensores.html',
        page="sensores",
        title="Sensores",
        category="INTERNET OF THINGS",
        estaciones=estaciones,
        now=datetime.datetime.now()
    )

# =====================================================================
# CONFIGURACIÓN, USUARIOS, ADMIN, MEDIA, AYUDA
# =====================================================================
@app.route('/configuracion', methods=['GET', 'POST'])
@login_required
@admin_required
def configuracion():
    claves = ['temp_riesgo', 'temp_alta', 'temp_critica', 'hum_riesgo', 'hum_baja', 'hum_critica',
              'humo_bajo', 'humo_medio', 'humo_alto', 'offline_timeout']
    if request.method == 'POST':
        for clave in claves:
            valor = request.form.get(clave)
            if valor:
                db.execute_query("UPDATE configuracion SET valor = %s WHERE clave = %s", (valor, clave))
        session['notificaciones'] = 'notificaciones' in request.form
        session['auto_update'] = 'auto_update' in request.form
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje='Umbrales actualizados')
        flash('Configuración actualizada', 'success')
    config = {row['clave']: row['valor'] for row in db.get_all("SELECT clave, valor FROM configuracion")}
    config['notificaciones'] = session.get('notificaciones', True)
    config['auto_update'] = session.get('auto_update', True)
    return render_template('configuracion.html',
        page="configuracion",
        title="Configuración",
        category="SISTEMA",
        config=config,
        now=datetime.datetime.now()
    )

@app.route('/usuarios')
@login_required
@admin_required
def usuarios():
    filas = db.get_all("""
        SELECT u.*, t.nombre as tipo, eu.nombre as estado
        FROM usuarios u
        JOIN tipos_usuario t ON u.id_tipo_usuario = t.id_tipo_usuario
        JOIN estados_usuario eu ON u.id_estado_usuario = eu.id_estado_usuario
        ORDER BY u.nombre_usuario
    """)
    tipos = db.get_all("SELECT * FROM tipos_usuario")
    return render_template('usuarios.html',
        page="usuarios",
        title="Usuarios",
        category="ADMINISTRACIÓN",
        usuarios=filas,
        tipos=tipos,
        now=datetime.datetime.now()
    )

@app.route('/usuarios/nuevo', methods=['GET', 'POST'])
@login_required
@admin_required
def nuevo_usuario():
    if request.method == 'POST':
        nombre_usuario = request.form.get('nombre_usuario')
        email = request.form.get('email')
        nombre_completo = request.form.get('nombre_completo')
        contrasena = request.form.get('contrasena')
        id_tipo_usuario = request.form.get('id_tipo_usuario')
        if not nombre_usuario or not email or not contrasena or not id_tipo_usuario:
            flash('Todos los campos son obligatorios', 'danger')
            return redirect(url_for('nuevo_usuario'))
        if not re.match(r'^[^@\s]+@[^@\s]+\.[^@\s]+$', email):
            flash('Email inválido', 'danger')
            return redirect(url_for('nuevo_usuario'))
        if db.get_one("SELECT id_usuario FROM usuarios WHERE nombre_usuario = %s", (nombre_usuario,)):
            flash('El nombre de usuario ya existe', 'danger')
            return redirect(url_for('nuevo_usuario'))
        hash_pass = generate_password_hash(contrasena)
        db.execute_query("""
            INSERT INTO usuarios (nombre_usuario, contrasena_hash, email, nombre_completo, id_tipo_usuario)
            VALUES (%s, %s, %s, %s, %s)
        """, (nombre_usuario, hash_pass, email, nombre_completo, id_tipo_usuario))
        flash('Usuario creado correctamente', 'success')
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Usuario {nombre_usuario} creado")
        return redirect(url_for('usuarios'))
    tipos = db.get_all("SELECT * FROM tipos_usuario")
    return render_template('usuario_form.html',
        page="usuarios",
        title="Nuevo Usuario",
        category="ADMINISTRACIÓN",
        tipos=tipos,
        usuario=None,
        now=datetime.datetime.now()
    )

@app.route('/usuarios/<int:id_usuario>/editar', methods=['GET', 'POST'])
@login_required
@admin_required
def editar_usuario(id_usuario):
    usuario = db.get_one("SELECT * FROM usuarios WHERE id_usuario = %s", (id_usuario,))
    if not usuario:
        flash('Usuario no encontrado', 'danger')
        return redirect(url_for('usuarios'))
    if request.method == 'POST':
        email = request.form.get('email')
        nombre_completo = request.form.get('nombre_completo')
        id_tipo_usuario = request.form.get('id_tipo_usuario')
        contrasena = request.form.get('contrasena')
        if not email or not nombre_completo or not id_tipo_usuario:
            flash('Email, nombre y rol son obligatorios', 'danger')
            return redirect(url_for('editar_usuario', id_usuario=id_usuario))
        if not re.match(r'^[^@\s]+@[^@\s]+\.[^@\s]+$', email):
            flash('Email inválido', 'danger')
            return redirect(url_for('editar_usuario', id_usuario=id_usuario))
        if contrasena:
            hash_pass = generate_password_hash(contrasena)
            db.execute_query("""
                UPDATE usuarios SET email=%s, nombre_completo=%s, id_tipo_usuario=%s, contrasena_hash=%s
                WHERE id_usuario=%s
            """, (email, nombre_completo, id_tipo_usuario, hash_pass, id_usuario))
        else:
            db.execute_query("""
                UPDATE usuarios SET email=%s, nombre_completo=%s, id_tipo_usuario=%s
                WHERE id_usuario=%s
            """, (email, nombre_completo, id_tipo_usuario, id_usuario))
        flash('Usuario actualizado', 'success')
        registrar_evento('CONFIGURACION_CAMBIADA', id_usuario=session['usuario_id'], mensaje=f"Usuario {usuario['nombre_usuario']} editado")
        return redirect(url_for('usuarios'))
    tipos = db.get_all("SELECT * FROM tipos_usuario")
    return render_template('usuario_form.html',
        page="usuarios",
        title="Editar Usuario",
        category="ADMINISTRACIÓN",
        tipos=tipos,
        usuario=usuario,
        now=datetime.datetime.now()
    )

@app.route('/usuarios/<int:id_usuario>/estado', methods=['POST'])
@login_required
@admin_required
def cambiar_estado_usuario(id_usuario):
    nuevo_estado = request.form.get('estado')
    fila = db.get_one("SELECT id_estado_usuario FROM estados_usuario WHERE nombre = %s", (nuevo_estado,))
    if fila:
        db.execute_query("UPDATE usuarios SET id_estado_usuario = %s WHERE id_usuario = %s", (fila['id_estado_usuario'], id_usuario))
        flash('Estado de usuario actualizado', 'success')
    return redirect(url_for('usuarios'))

@app.route('/media')
@login_required
def media():
    return render_template('media.html',
        page="media",
        title="Media",
        category="VIDEO",
        now=datetime.datetime.now()
    )

@app.route('/ayuda')
@login_required
def ayuda():
    return render_template('ayuda.html',
        page="ayuda",
        title="Ayuda",
        category="SISTEMA",
        now=datetime.datetime.now()
    )

@app.route('/admin')
@login_required
@admin_required
def admin():
    total_estaciones = db.get_one("SELECT COUNT(*) as total FROM estaciones WHERE activa=1")['total']
    online = db.get_one("SELECT COUNT(*) as total FROM estaciones WHERE id_estado_estacion = %s", (ESTADO_ESTACION['ONLINE'],))['total']
    offline = db.get_one("SELECT COUNT(*) as total FROM estaciones WHERE id_estado_estacion = %s", (ESTADO_ESTACION['OFFLINE'],))['total']
    total_usuarios = db.get_one("SELECT COUNT(*) as total FROM usuarios")['total']
    total_alertas_activas = db.get_one("SELECT COUNT(*) as total FROM alertas WHERE id_estado_alerta = %s", (ESTADO_ALERTA['ACTIVA'],))['total']
    ultimos_eventos = db.get_all("""
        SELECT e.*, te.nombre as tipo, u.nombre_usuario as usuario, es.nombre as estacion
        FROM eventos e
        LEFT JOIN tipos_evento te ON e.id_tipo_evento = te.id_tipo_evento
        LEFT JOIN usuarios u ON e.id_usuario = u.id_usuario
        LEFT JOIN estaciones es ON e.id_estacion = es.id_estacion
        ORDER BY e.fecha DESC LIMIT 20
    """)
    return render_template('admin.html',
        page="admin",
        title="Administración",
        category="SISTEMA",
        total_estaciones=total_estaciones,
        online=online,
        offline=offline,
        total_usuarios=total_usuarios,
        total_alertas_activas=total_alertas_activas,
        ultimos_eventos=ultimos_eventos,
        now=datetime.datetime.now()
    )

# =====================================================================
# API - INGESTA DE DATOS DE LA ESP32 (CON AUTO-REGISTRO Y ESTADO PENDIENTE)
# =====================================================================
@app.route('/api/estaciones/<codigo>/datos', methods=['POST'])
def api_recibir_datos(codigo):
    data = request.get_json(silent=True)
    if not data:
        return jsonify({'error': 'Se esperaba JSON'}), 400
    if 'api_key' not in data:
        return jsonify({'error': 'Falta api_key'}), 400

    # Buscar la estación por código
    estacion = db.get_one("SELECT * FROM estaciones WHERE codigo = %s", (codigo,))

    # ============================================================
    # AUTO-REGISTRO: si no existe, crearla en estado PENDIENTE
    # ============================================================
    if not estacion:
        try:
            # Crear estación con estado PENDIENTE y activa=0
            db.execute_query("""
                INSERT INTO estaciones (nombre, codigo, api_key, id_zona, latitud, longitud, descripcion, activa, id_estado_estacion)
                VALUES (%s, %s, %s, NULL, 0.0, 0.0, %s, 0, %s)
            """, (f"Auto-{codigo}", codigo, data['api_key'], "Estación registrada automáticamente - pendiente de aprobación", ESTADO_ESTACION['PENDIENTE']))

            # Obtener la estación recién creada
            estacion = db.get_one("SELECT * FROM estaciones WHERE codigo = %s", (codigo,))
            print(f"✅ Estación registrada automáticamente (pendiente): {codigo} con API key {data['api_key']}")

            # Registrar evento
            registrar_evento('CONFIGURACION_CAMBIADA', id_estacion=estacion['id_estacion'], mensaje=f"Estación auto-registrada {codigo} (pendiente)")

        except Exception as e:
            print(f"❌ Error al registrar estación automáticamente: {e}")
            return jsonify({'error': f'Error al registrar estación: {str(e)}'}), 500

    # Si después del intento sigue sin existir, error
    if not estacion:
        return jsonify({'error': 'Estación no registrada y no se pudo auto-registrar'}), 404

    # Verificar API key (la que envía la ESP32 debe coincidir con la almacenada)
    if estacion['api_key'] != data['api_key']:
        return jsonify({'error': 'api_key inválida'}), 401

    # Verificar que la estación esté activa (aprobada)
    if not estacion['activa']:
        return jsonify({'error': 'Estación pendiente de aprobación. Contacte al administrador.'}), 403

    id_estacion = estacion['id_estacion']
    temperatura = data.get('temperatura')
    humedad = data.get('humedad')
    mq2_ao = data.get('mq2_ao')
    mq2_base = data.get('mq2_base')
    cambio_ao = data.get('cambio_ao')
    mq2_do = data.get('mq2_do')
    dht_ok = temperatura is not None and humedad is not None
    mq2_ok = mq2_ao is not None and cambio_ao is not None
    if cambio_ao is None and mq2_ao is not None and mq2_base is not None:
        cambio_ao = mq2_ao - mq2_base
        mq2_ok = True

    # Obtener umbrales para evaluación (solo si se requiere)
    umbrales = obtener_umbrales(id_estacion)

    # Determinar id_tipo_humo usando el nivel enviado por la ESP32 si existe
    nivel_humo_enviado = data.get('nivel_humo')
    if nivel_humo_enviado is not None:
        id_tipo_humo = mapear_nivel_humo_a_id(nivel_humo_enviado)
    else:
        # Fallback: recalcular con los umbrales de Flask
        nivel_humo_num, id_tipo_humo = clasificar_humo(cambio_ao if mq2_ok else None, umbrales)

    id_medicion = db.execute_query("""
        INSERT INTO mediciones (id_estacion, temperatura, humedad, dht_ok, mq2_ao, mq2_base, cambio_ao, mq2_do, mq2_ok, id_tipo_humo)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
    """, (id_estacion, temperatura, humedad, dht_ok, mq2_ao, mq2_base, cambio_ao, mq2_do, mq2_ok, id_tipo_humo))

    registrar_evento('LECTURA_RECIBIDA', id_estacion=id_estacion, mensaje='Medición recibida')

    def actualizar_estado_sensor(id_tipo, ok):
        sensor = db.get_one("SELECT * FROM sensores WHERE id_estacion = %s AND id_tipo_sensor = %s", (id_estacion, id_tipo))
        if sensor:
            nuevo = ESTADO_SENSOR['ACTIVO'] if ok else ESTADO_SENSOR['ERROR']
            if sensor['id_estado_sensor'] != nuevo:
                db.execute_query("UPDATE sensores SET id_estado_sensor = %s WHERE id_sensor = %s", (nuevo, sensor['id_sensor']))
                if not ok:
                    registrar_evento('SENSOR_ERROR', id_estacion=id_estacion, mensaje=f"Sensor tipo {id_tipo} en error")

    actualizar_estado_sensor(1, dht_ok)
    actualizar_estado_sensor(2, mq2_ok)

    hubo_error_sensor = not dht_ok or not mq2_ok

    db.execute_query("UPDATE estaciones SET ultima_conexion = NOW(), ip = %s WHERE id_estacion = %s", (request.remote_addr, id_estacion))
    db.execute_query("INSERT INTO historial_conexiones (id_estacion, ip) VALUES (%s, %s)", (id_estacion, request.remote_addr))

    if hubo_error_sensor:
        cambiar_estado_estacion(id_estacion, 'ERROR')
        crear_o_actualizar_alerta(
            id_estacion, 'SENSOR_ERROR', 'RIESGO_MODERADO',
            'Error de sensor', f"DHT22 {'OK' if dht_ok else 'ERROR'}, MQ-2 {'OK' if mq2_ok else 'ERROR'}",
            id_medicion=id_medicion
        )
    else:
        cambio = cambiar_estado_estacion(id_estacion, 'ONLINE')
        if cambio:
            registrar_evento('ESTACION_CONECTADA', id_estacion=id_estacion, mensaje='Estación ONLINE')
            db.execute_query("""
                UPDATE alertas SET id_estado_alerta = %s, fecha_resolucion = NOW()
                WHERE id_estacion = %s AND id_estado_alerta = %s
                AND id_tipo_alerta = (SELECT id_tipo_alerta FROM tipos_alerta WHERE nombre = 'ESTACION_OFFLINE')
            """, (ESTADO_ALERTA['RESUELTA'], id_estacion, ESTADO_ALERTA['ACTIVA']))

    respuesta_riesgo = 'SIN_EVALUAR'
    if not hubo_error_sensor:
        # Usamos el nivel_humo que ya tenemos (id_tipo_humo) para evaluar riesgo
        nivel_humo_num = db.get_one("SELECT nivel FROM tipos_humo WHERE id_tipo_humo = %s", (id_tipo_humo,))
        nivel_humo_num = nivel_humo_num['nivel'] if nivel_humo_num else 0
        nivel_riesgo, tipo_alerta, condiciones = evaluar_riesgo(temperatura, humedad, nivel_humo_num, umbrales)
        respuesta_riesgo = nivel_riesgo
        nivel_id = db.get_one("SELECT id_nivel_riesgo FROM niveles_riesgo WHERE nombre = %s", (nivel_riesgo,))
        id_evaluacion = db.execute_query(
            "INSERT INTO evaluaciones_riesgo (id_medicion, id_estacion, id_nivel_riesgo) VALUES (%s, %s, %s)",
            (id_medicion, id_estacion, nivel_id['id_nivel_riesgo'])
        )
        if tipo_alerta:
            titulos = {
                'POSIBLE_INCENDIO': 'Posible incendio - verificar',
                'RIESGO_ALTO': 'Riesgo alto detectado',
                'RIESGO_MODERADO': 'Riesgo moderado detectado'
            }
            descripciones = {
                'POSIBLE_INCENDIO': 'Condiciones compatibles con incendio. Requiere verificación.',
                'RIESGO_ALTO': 'Combinación de variables eleva el riesgo.',
                'RIESGO_MODERADO': 'Una variable fuera de rango normal.'
            }
            crear_o_actualizar_alerta(
                id_estacion, tipo_alerta, nivel_riesgo,
                titulos.get(tipo_alerta, tipo_alerta),
                descripciones.get(tipo_alerta, ''),
                id_medicion=id_medicion, id_evaluacion=id_evaluacion
            )
        else:
            db.execute_query("""
                UPDATE alertas SET id_estado_alerta = %s, fecha_resolucion = NOW()
                WHERE id_estacion = %s AND id_estado_alerta = %s
                AND id_tipo_alerta IN (SELECT id_tipo_alerta FROM tipos_alerta
                    WHERE nombre IN ('RIESGO_MODERADO', 'RIESGO_ALTO', 'POSIBLE_INCENDIO'))
            """, (ESTADO_ALERTA['RESUELTA'], id_estacion, ESTADO_ALERTA['ACTIVA']))

    return jsonify({
        'status': 'ok',
        'medicion_id': id_medicion,
        'nivel_humo': ['NINGUNO', 'BAJO', 'MEDIO', 'ALTO'][nivel_humo_num] if nivel_humo_num is not None else None,
        'riesgo': respuesta_riesgo,
        'sensores_ok': not hubo_error_sensor
    })

# =====================================================================
# API - DASHBOARD DATA
# =====================================================================
@app.route('/api/dashboard_data')
@login_required
def api_dashboard_data():
    verificar_estaciones_offline()
    estaciones = db.get_all("""
        SELECT e.id_estacion, e.nombre, e.codigo, e.ip, e.ultima_conexion, e.latitud, e.longitud,
               es.nombre as estado,
               m.temperatura, m.humedad, m.cambio_ao, m.mq2_do, th.nombre as humo_nombre,
               (SELECT nr.nombre FROM alertas a JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
                    WHERE a.id_estacion = e.id_estacion AND a.id_estado_alerta = 1
                    ORDER BY nr.severidad DESC LIMIT 1) as nivel_alerta_activa,
               (SELECT nr.color_hex FROM alertas a JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
                    WHERE a.id_estacion = e.id_estacion AND a.id_estado_alerta = 1
                    ORDER BY nr.severidad DESC LIMIT 1) as color_alerta
        FROM estaciones e
        JOIN estados_estacion es ON e.id_estado_estacion = es.id_estado_estacion
        LEFT JOIN mediciones m ON m.id_medicion = (
            SELECT id_medicion FROM mediciones WHERE id_estacion = e.id_estacion ORDER BY fecha_hora DESC LIMIT 1
        )
        LEFT JOIN tipos_humo th ON m.id_tipo_humo = th.id_tipo_humo
        WHERE e.activa = 1
    """)
    for est in estaciones:
        if not est['color_alerta']:
            if est['estado'] == 'ONLINE':
                est['color_alerta'] = '#2ecc71'
            elif est['estado'] == 'OFFLINE':
                est['color_alerta'] = '#95a5a6'
            elif est['estado'] == 'ERROR':
                est['color_alerta'] = '#3498db'
            else:
                est['color_alerta'] = '#f39c12'
    return jsonify(estaciones)

# =====================================================================
# NUEVA API: GUARDAR UBICACIÓN DEL USUARIO
# =====================================================================
@app.route('/api/set_location', methods=['POST'])
@login_required
def api_set_location():
    data = request.get_json()
    if not data:
        return jsonify({'error': 'Se espera JSON'}), 400
    lat = data.get('lat')
    lon = data.get('lon')
    if lat is None or lon is None:
        return jsonify({'error': 'Faltan lat o lon'}), 400
    try:
        session['user_lat'] = float(lat)
        session['user_lon'] = float(lon)
        return jsonify({'status': 'ok'})
    except ValueError:
        return jsonify({'error': 'Lat/Lon inválidos'}), 400

# =====================================================================
# NUEVA API: OBTENER ALERTAS ACTIVAS CON DISTANCIA
# =====================================================================
@app.route('/api/alertas_activas')
@login_required
def api_alertas_activas():
    radio_km = float(db.get_one("SELECT valor FROM configuracion WHERE clave='radio_alerta'")['valor'] or 5.0)
    alerts = db.get_all("""
        SELECT a.*, e.nombre as estacion, e.latitud, e.longitud,
               ta.nombre as tipo, nr.nombre as nivel, nr.color_hex
        FROM alertas a
        JOIN estaciones e ON a.id_estacion = e.id_estacion
        JOIN tipos_alerta ta ON a.id_tipo_alerta = ta.id_tipo_alerta
        JOIN niveles_riesgo nr ON a.id_nivel_riesgo = nr.id_nivel_riesgo
        WHERE a.id_estado_alerta = %s AND e.activa = 1
        ORDER BY nr.severidad DESC, a.fecha_creacion DESC
    """, (ESTADO_ALERTA['ACTIVA'],))
    user_lat = session.get('user_lat')
    user_lon = session.get('user_lon')
    result = []
    for a in alerts:
        dist = None
        cerca = False
        if user_lat is not None and user_lon is not None and a['latitud'] and a['longitud']:
            dist = calcular_distancia(user_lat, user_lon, float(a['latitud']), float(a['longitud']))
            cerca = dist <= radio_km
        result.append({
            **a,
            'distancia_km': round(dist, 2) if dist is not None else None,
            'cerca': cerca,
            'radio_km': radio_km
        })
    return jsonify(result)

# =====================================================================
# ERRORES
# =====================================================================
@app.errorhandler(404)
def not_found(e):
    return render_template('error.html', codigo=404, mensaje='Página no encontrada'), 404

@app.errorhandler(500)
def server_error(e):
    return render_template('error.html', codigo=500, mensaje='Error interno del servidor'), 500

# =====================================================================
# INICIO
# =====================================================================
if __name__ == '__main__':
    init_db()
    app.run(host='0.0.0.0', port=5000, debug=True)