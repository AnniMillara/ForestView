import mysql.connector
from mysql.connector import Error
from config import Config

def get_connection():
    try:
        conn = mysql.connector.connect(
            host=Config.DB_HOST,
            database=Config.DB_NAME,
            user=Config.DB_USER,
            password=Config.DB_PASSWORD,
            port=Config.DB_PORT
        )
        return conn
    except Error as e:
        print(f"Error de conexión a BD: {e}")
        return None

def execute_query(query, params=None, fetch=False):
    conn = get_connection()
    if not conn:
        return None if not fetch else []
    cursor = conn.cursor(dictionary=True)
    try:
        cursor.execute(query, params or ())
        if fetch:
            result = cursor.fetchall()
        else:
            conn.commit()
            result = cursor.lastrowid
        return result
    except Error as e:
        print(f"Error en query: {e}")
        conn.rollback()
        return None if not fetch else []
    finally:
        cursor.close()
        conn.close()

def get_one(query, params=None):
    result = execute_query(query, params, fetch=True)
    return result[0] if result else None

def get_all(query, params=None):
    return execute_query(query, params, fetch=True) or []